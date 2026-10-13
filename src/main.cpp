// ps5-spotify - Console login test: cspot logs in from saved credentials.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Milestone before audio: prove the account side of cspot on the PS5.
//   1. read /app0/config.json ({"client_id": "..."}) and
//      /app0/cspot-creds.json (saved by cspotcli -c on the PC)
//   2. connect to a Spotify access point and authenticate (Shannon session)
//   3. fetch one track's metadata over Mercury
//   4. get a Web API access token via login5 (client token + hashcash)
// Each step is logged ([LOGIN] lines in the app log and klog). Secrets are
// never logged, only their sizes.

#include <cstdarg>
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
#include "pb_decode.h"
#include "protobuf/metadata.pb.h"

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceLoadExec(const char *path, const char **arguments);
}

namespace
{

// A public track (used in Spotify's API docs): base62 4uLU6hMCjMI75M1A2tKUQC.
constexpr const char *kTrackGid = "93bc414a606747b2b612491ef83d5a3e";

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
