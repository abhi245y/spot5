// ps5-spotify - Spotify Connect receiver on the PS5 (phone-control test).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Logs in from /app0/cspot-creds.json with the client id from
// /app0/config.json, then runs cspot's spirc: the console appears in the
// phone's Spotify Connect list and plays what the phone picks. Runs for
// kRunMinutes, then closes itself. Progress lines are [APP] (app log + klog);
// cspot's own log goes to the app log.

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "BellLogger.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "SpircHandler.h"
#include "X509Bundle.h"
#include "audio/player.hpp"
#include "nlohmann/json.hpp"

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceLoadExec(const char *path, const char **arguments);
}

namespace
{

constexpr int kRunMinutes = 20;
constexpr const char *kDeviceName = "PS5";

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
    trace("[APP] closing in 60 s");
    sceKernelUsleep(60000000);
    std::fflush(nullptr);
    int refused = sceSystemServiceLoadExec("exit", nullptr);
    trace("[APP] quit refused rc=0x%x", static_cast<unsigned>(refused));
    for (;;)
        sceKernelUsleep(100000);
}

void run()
{
    auto bundle = read_text("/app0/assets/x509_bundle.bin");
    bell::X509Bundle::init(reinterpret_cast<const uint8_t *>(bundle.data()), bundle.size());

    auto config = nlohmann::json::parse(read_text("/app0/config.json"), nullptr, false);
    std::string creds = read_text("/app0/cspot-creds.json");
    if (config.is_discarded() || !config.contains("client_id") || creds.empty())
    {
        trace("[APP] missing or invalid /app0/config.json or /app0/cspot-creds.json");
        return;
    }
    auto blob = std::make_shared<cspot::LoginBlob>(kDeviceName);
    blob->loadJson(creds);
    auto ctx = cspot::Context::createFromBlob(blob);
    ctx->config.clientId = config["client_id"].get<std::string>();
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
    ctx->config.volume = 65535; // reported to the phone; applied in software

    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(blob);
    if (ctx->config.authData.empty())
    {
        trace("[APP] login FAILED");
        return;
    }
    trace("[APP] logged in; starting Spotify Connect as \"%s\"", kDeviceName);

    // Same order as cspot's CLI: spirc, Mercury task, player, packet loop.
    auto handler = std::make_shared<cspot::SpircHandler>(ctx);
    ctx->session->startTask();
    Player player(handler);
    if (!player.ok())
    {
        trace("[APP] audio output FAILED");
        return;
    }
    trace("[APP] ready: pick \"%s\" in the phone's Connect list (running %d min)", kDeviceName,
          kRunMinutes);

    // The app drives Mercury: each call handles one packet (waits <= 200 ms).
    for (long i = 0; i < kRunMinutes * 60L * 5; i++)
        ctx->session->handlePacket();

    handler->disconnect();
    trace("[APP] time limit reached");
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
    trace("[APP] libc++ abort");
    for (;;)
        sceKernelUsleep(100000);
}
} // namespace __1
} // namespace std

int main()
{
    trace("[APP] start");
    bell::setDefaultLogger();
    try
    {
        run();
    }
    catch (const std::exception &error)
    {
        trace("[APP] FAILED with exception: %s", error.what());
    }
    quit();
}
