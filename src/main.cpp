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
#include <mutex>
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

// tremor reads the decrypted stream through these, as cspot's TrackPlayer does.
size_t vorbis_read(void *ptr, size_t size, size_t nmemb, void *file)
{
    return static_cast<cspot::CDNAudioFile *>(file)->readBytes(static_cast<uint8_t *>(ptr),
                                                             size * nmemb);
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

    Resampler resampler(info->rate, 48000);
    std::vector<int16_t> decoded(4096), pending;
    std::size_t played = 0, read_at = 0;
    long slowest_ms = 0;
    int errors = 0, section = 0;
    while (played < static_cast<std::size_t>(kPlaySeconds) * 48000)
    {
        // Top up the 48 kHz buffer, then play whole grains from it.
        while (pending.size() - read_at < kGrain * 2)
        {
            auto start = std::chrono::steady_clock::now();
            long bytes = ov_read(&vorbis, reinterpret_cast<char *>(decoded.data()),
                                 static_cast<int>(decoded.size() * 2), &section);
            long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
            slowest_ms = ms > slowest_ms ? ms : slowest_ms;
            if (bytes <= 0)
            {
                trace("[LOGIN] step6: decoder stopped rc=%ld", bytes);
                played = SIZE_MAX - 1;
                break;
            }
            resampler.process(decoded.data(), static_cast<std::size_t>(bytes) / 4, pending);
        }
        if (pending.size() - read_at < kGrain * 2)
            break;
        if (sceAudioOutOutput(handle, pending.data() + read_at) < 0)
            errors++;
        read_at += kGrain * 2;
        played += kGrain;
        if (read_at > 1 << 16) // drop played samples now and then
        {
            pending.erase(pending.begin(), pending.begin() + static_cast<long>(read_at));
            read_at = 0;
        }
    }
    sceAudioOutOutput(handle, nullptr);
    sceAudioOutClose(handle);
    ov_clear(&vorbis);
    trace("[LOGIN] step6: played %.1f s, output errors=%d, slowest decode %ld ms",
          played > SIZE_MAX / 2 ? -1.0 : played / 48000.0, errors, slowest_ms);
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
    for (std::size_t i = 0; i < 32; i += 2)
        ref.gid.push_back(static_cast<uint8_t>(std::stoi(std::string(kTrackGid + i, 2), nullptr, 16)));
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
