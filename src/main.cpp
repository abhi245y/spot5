// ps5-spotify - Console login test: cspot logs in from saved credentials.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone before audio: prove the account side of cspot on the PS5.
//   1. read /app0/config.json ({"client_id": "..."}) and
//      /app0/cspot-creds.json (saved by cspotcli -c on the PC)
//   2. connect to a Spotify access point and authenticate (Shannon session)
//   3. fetch one track's metadata over Mercury
//   4. get a Web API access token via login5 (client token + hashcash)
//   5. load the track's audio: file id, AES key, CDN URL (cspot QueuedTrack)
//   6. stream + decrypt (CDNAudioFile), decode (tremor), resample 44.1->48 kHz
//      and play 30 s through sceAudioOut
// Each step is logged ([LOGIN] lines in the app log and klog). Secrets are
// never logged, only their sizes.

#include <chrono>
#include <cstdarg>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <functional>
#include <pthread.h>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "AccessKeyFetcher.h"
#include "BellLogger.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "WrappedSemaphore.h"
#include "X509Bundle.h"
#include "nlohmann/json.hpp"
#include "CDNAudioFile.h"
#include "TrackQueue.h"
#include "TrackReference.h"
#include "audio/resampler.hpp"
#include "ivorbisfile.h"
#include "pb_decode.h"
#include "protobuf/metadata.pb.h"

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceLoadExec(const char *path, const char **arguments);
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
}

namespace
{

void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));

// A public track (used in Spotify's API docs): base62 4uLU6hMCjMI75M1A2tKUQC.
constexpr const char *kTrackGid = "93bc414a606747b2b612491ef83d5a3e";
constexpr int kPlaySeconds = 30;
constexpr int kGrain = 256;

// Spotify track id (base62, 22 chars; a full open.spotify.com/track/ link or
// spotify:track: URI also works) -> 16-byte gid. Empty if malformed.
std::vector<uint8_t> gid_from_track(std::string id)
{
    static const std::string digits =
        "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (id.size() != 22)
    {
        std::size_t at = id.rfind("track");
        if (at != std::string::npos)
            id = id.substr(at + 6); // after "track/" or "track:"
        id = id.substr(0, id.find('?'));
    }
    std::vector<uint8_t> gid(16, 0);
    if (id.size() != 22)
        return {};
    for (char c : id)
    {
        std::size_t d = digits.find(c);
        if (d == std::string::npos)
            return {};
        // gid = gid * 62 + d, big-endian
        unsigned carry = static_cast<unsigned>(d);
        for (int i = 15; i >= 0; i--)
        {
            unsigned v = gid[i] * 62u + carry;
            gid[i] = static_cast<uint8_t>(v & 0xff);
            carry = v >> 8;
        }
    }
    return gid;
}

// tremor reads the decrypted stream through these, as cspot's TrackPlayer does.
// Time spent in CDN reads (network + decrypt) vs total, to tell a slow
// network from a slow decoder. Only the decoder thread touches these.
long g_read_us = 0, g_slow_reads = 0;
std::size_t g_read_bytes = 0;

size_t vorbis_read(void *ptr, size_t size, size_t nmemb, void *file)
{
    auto start = std::chrono::steady_clock::now();
    size_t n = static_cast<cspot::CDNAudioFile *>(file)->readBytes(static_cast<uint8_t *>(ptr),
                                                                 size * nmemb);
    long us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    g_read_us += us;
    g_slow_reads += us > 100000;
    g_read_bytes += n;
    return n;
}
int vorbis_seek(void *file, ogg_int64_t offset, int whence)
{
    auto *f = static_cast<cspot::CDNAudioFile *>(file);
    if (whence == 1)
        offset += f->getPosition();
    else if (whence == 2)
        offset += f->getSize();
    f->seek(offset);
    return 0;
}
int vorbis_close(void *)
{
    return 0;
}
long vorbis_tell(void *file)
{
    return static_cast<long>(static_cast<cspot::CDNAudioFile *>(file)->getPosition());
}

// Streams, decodes, resamples and plays the first kPlaySeconds of a track.
void play(std::shared_ptr<cspot::CDNAudioFile> file)
{
    file->openStream();
    OggVorbis_File vorbis{};
    ov_callbacks callbacks = {vorbis_read, vorbis_seek, vorbis_close, vorbis_tell};
    int rc = ov_open_callbacks(file.get(), &vorbis, nullptr, 0, callbacks);
    if (rc != 0)
    {
        trace("[LOGIN] step6: ov_open_callbacks FAILED rc=%d", rc);
        return;
    }
    vorbis_info *info = ov_info(&vorbis, -1);
    trace("[LOGIN] step6: vorbis %ld Hz, %d channels, %ld kbps", info->rate, info->channels,
          info->bitrate_nominal / 1000);

    int init = sceAudioOutInit();
    int handle = sceAudioOutOpen(0xff, 0, 0, kGrain, 48000, 1 /* S16 stereo */);
    trace("[LOGIN] step6: sceAudioOutInit=0x%08x open=0x%08x", static_cast<unsigned>(init),
          static_cast<unsigned>(handle));
    if (handle < 0 || info->channels != 2)
    {
        ov_clear(&vorbis);
        return;
    }

    // Decoder thread fills a few seconds of 48 kHz audio; this thread only
    // moves grains to sceAudioOut. The port holds just one grain (~5 ms),
    // so decoding or a CDN fetch (up to ~350 ms) on this thread leaves gaps.
    std::mutex lock;
    std::condition_variable changed;
    std::deque<int16_t> buffer;
    constexpr std::size_t kCapacity = 4 * 48000 * 2; // 4 s
    constexpr std::size_t kPrebuffer = 2 * 48000 * 2; // start after 2 s
    bool finished = false, stop = false;
    long slowest_ms = 0;
    long decode_us = 0; // total time inside ov_read, CDN reads included

    // pthread with an explicit 1 MiB stack: tremor allocas per packet, and
    // the platform default stack size is not known to be large enough.
    std::function<void()> decode = [&] {
        Resampler resampler(info->rate, 48000);
        std::vector<int16_t> decoded(4096), out;
        int section = 0;
        for (;;)
        {
            auto start = std::chrono::steady_clock::now();
            long bytes = ov_read(&vorbis, reinterpret_cast<char *>(decoded.data()),
                                 static_cast<int>(decoded.size() * 2), &section);
            long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            out.clear();
            if (bytes > 0)
                resampler.process(decoded.data(), static_cast<std::size_t>(bytes) / 4, out);
            std::unique_lock<std::mutex> guard(lock);
            slowest_ms = ms > slowest_ms ? ms : slowest_ms;
            decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
            if (bytes <= 0 || stop)
            {
                finished = true;
                changed.notify_all();
                return;
            }
            changed.wait(guard, [&] { return stop || buffer.size() + out.size() <= kCapacity; });
            buffer.insert(buffer.end(), out.begin(), out.end());
            changed.notify_all();
        }
    };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_t decoder;
    int created = pthread_create(
        &decoder, &attr,
        [](void *fn) -> void * {
            (*static_cast<std::function<void()> *>(fn))();
            return nullptr;
        },
        &decode);
    pthread_attr_destroy(&attr);
    if (created != 0)
    {
        trace("[LOGIN] step6: decoder thread FAILED rc=%d", created);
        sceAudioOutClose(handle);
        ov_clear(&vorbis);
        return;
    }

    {
        std::unique_lock<std::mutex> guard(lock);
        changed.wait(guard, [&] { return finished || buffer.size() >= kPrebuffer; });
    }
    auto play_start = std::chrono::steady_clock::now();
    std::size_t played = 0;
    int errors = 0, underruns = 0;
    int16_t grain[kGrain * 2];
    while (played < static_cast<std::size_t>(kPlaySeconds) * 48000)
    {
        {
            std::unique_lock<std::mutex> guard(lock);
            if (buffer.size() >= kGrain * 2)
            {
                std::copy(buffer.begin(), buffer.begin() + kGrain * 2, grain);
                buffer.erase(buffer.begin(), buffer.begin() + kGrain * 2);
                changed.notify_all();
            }
            else if (finished)
                break;
            else
            {
                std::fill(grain, grain + kGrain * 2, 0);
                underruns++;
            }
        }
        if (sceAudioOutOutput(handle, grain) < 0)
            errors++;
        played += kGrain;
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        stop = true;
        changed.notify_all();
    }
    pthread_join(decoder, nullptr);
    sceAudioOutOutput(handle, nullptr);
    sceAudioOutClose(handle);
    ov_clear(&vorbis);
    trace("[LOGIN] step6: played %.1f s, output errors=%d, underrun grains=%d, "
          "slowest decode %ld ms",
          played / 48000.0, errors, underruns, slowest_ms);
    long wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - play_start)
                       .count();
    trace("[LOGIN] step6: wall %ld ms; in ov_read %ld ms = CDN reads %ld ms (%zu KB, "
          "%ld reads >100 ms) + decode %ld ms",
          wall_ms, decode_us / 1000, g_read_us / 1000, g_read_bytes / 1024, g_slow_reads,
          (decode_us - g_read_us) / 1000);
}

void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
void trace(const char *format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    std::strcat(line, "\n");
    std::fputs(line, stdout);
    sceKernelDebugOutText(0, line);
}

std::string read_text(const char *path)
{
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

[[noreturn]] void quit()
{
    // The log is readable over FTP only while the title runs.
    trace("[LOGIN] finished; closing in 60 s");
    sceKernelUsleep(60000000);
    std::fflush(nullptr);
    int refused = sceSystemServiceLoadExec("exit", nullptr);
    trace("[LOGIN] quit refused rc=0x%x", static_cast<unsigned>(refused));
    for (;;)
        sceKernelUsleep(100000);
}

void run()
{
    auto bundle = read_text("/app0/assets/x509_bundle.bin");
    bell::X509Bundle::init(reinterpret_cast<const uint8_t *>(bundle.data()), bundle.size());

    // 1. Config and saved login, both supplied on the console only.
    auto config = nlohmann::json::parse(read_text("/app0/config.json"), nullptr, false);
    std::string creds = read_text("/app0/cspot-creds.json");
    if (config.is_discarded() || !config.contains("client_id") || creds.empty())
    {
        trace("[LOGIN] step1: missing or invalid /app0/config.json or /app0/cspot-creds.json");
        return;
    }
    auto blob = std::make_shared<cspot::LoginBlob>("PS5");
    blob->loadJson(creds);
    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.clientId = config["client_id"].get<std::string>();
    trace("[LOGIN] step1: config + creds loaded (user name %zu chars)",
          ctx->config.username.size());

    // 2. Access point + authentication
    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(blob);
    if (ctx->config.authData.empty())
    {
        trace("[LOGIN] step2: authentication FAILED");
        return;
    }
    trace("[LOGIN] step2: authenticated (reusable credential %zu bytes)",
          ctx->config.authData.size());
    ctx->session->startTask();

    // 3. Track metadata over Mercury
    auto done = std::make_shared<bell::WrappedSemaphore>(0);
    std::string name;
    ctx->session->execute(cspot::MercurySession::RequestType::GET,
                          std::string("hm://metadata/3/track/") + kTrackGid,
                          [&name, done](cspot::MercurySession::Response &res) {
                              if (!res.parts.empty())
                              {
                                  Track track = Track_init_zero;
                                  pb_istream_t stream = pb_istream_from_buffer(
                                      res.parts[0].data(), res.parts[0].size());
                                  if (pb_decode(&stream, Track_fields, &track) && track.name)
                                      name = track.name;
                                  pb_release(Track_fields, &track);
                              }
                              done->give();
                          });
    // The app drives Mercury: each handlePacket() call processes one queued
    // packet (waiting up to 200 ms), like the PC CLI's main loop.
    bool answered = false;
    for (int i = 0; i < 75 && !answered; i++)
    {
        ctx->session->handlePacket();
        answered = done->twait(0) == 0;
    }
    if (!answered)
        trace("[LOGIN] step3: metadata TIMEOUT");
    else
        trace("[LOGIN] step3: metadata %s (track name: %s)", name.empty() ? "FAILED" : "OK",
              name.c_str());

    // 4. login5 access token
    cspot::AccessKeyFetcher fetcher(ctx);
    std::string token = fetcher.getAccessKey();
    trace("[LOGIN] step4: access token %s (%zu chars)", token.empty() ? "FAILED" : "OK",
          token.size());
    if (token.empty())
        return;

    // 5. Audio file, key and CDN URL, stepped like TrackQueue::processTrack
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
    cspot::TrackReference ref;
    std::string track_id = config.value("track", std::string());
    if (!track_id.empty())
    {
        ref.gid = gid_from_track(track_id);
        if (ref.gid.empty())
        {
            trace("[LOGIN] step5: config \"track\" is not a Spotify track link or id");
            return;
        }
    }
    else
        for (std::size_t i = 0; i < 32; i += 2)
            ref.gid.push_back(
                static_cast<uint8_t>(std::stoi(std::string(kTrackGid + i, 2), nullptr, 16)));
    auto track = std::make_shared<cspot::QueuedTrack>(ref, ctx);
    Track pb_track = Track_init_zero;
    Episode pb_episode = Episode_init_zero;
    std::mutex list_mutex;
    auto update = std::make_shared<bell::WrappedSemaphore>(0);
    for (int i = 0; i < 150 && track->state != cspot::QueuedTrack::State::READY &&
                    track->state != cspot::QueuedTrack::State::FAILED;
         i++)
    {
        switch (track->state)
        {
        case cspot::QueuedTrack::State::QUEUED:
            track->stepLoadMetadata(&pb_track, &pb_episode, list_mutex, update);
            break;
        case cspot::QueuedTrack::State::KEY_REQUIRED:
            track->stepLoadAudioFile(list_mutex, update);
            break;
        case cspot::QueuedTrack::State::CDN_REQUIRED:
            track->stepLoadCDNUrl(token);
            break;
        default:
            break;
        }
        ctx->session->handlePacket();
    }
    pb_release(Track_fields, &pb_track);
    pb_release(Episode_fields, &pb_episode);
    if (track->state != cspot::QueuedTrack::State::READY)
    {
        trace("[LOGIN] step5: track load FAILED (state %d)", static_cast<int>(track->state));
        return;
    }
    trace("[LOGIN] step5: audio file ready: %s", track->trackInfo.name.c_str());

    // 6. Play
    play(track->getAudioFile());
}

} // namespace

// libc++ calls this where it cannot throw. Log and park (exit() crashes a
// native title).
namespace std
{
inline namespace __1
{
[[noreturn]] void __libcpp_verbose_abort(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stdout, format, arguments);
    va_end(arguments);
    trace("[LOGIN] libc++ abort");
    for (;;)
        sceKernelUsleep(100000);
}
} // namespace __1
} // namespace std

int main()
{
    trace("[LOGIN] login test start");
    bell::setDefaultLogger();
    try
    {
        run();
    }
    catch (const std::exception &error)
    {
        trace("[LOGIN] FAILED with exception: %s", error.what());
    }
    quit();
}
