// ps5-spotify - Console network test: bell + mbedTLS + DNS on the PS5.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the first network steps cspot takes, each logged ([NET] lines in the
// app log and klog):
//   1. DNS: getaddrinfo (our sceNetResolver version) for apresolve.spotify.com
//   2. HTTPS GET apresolve.spotify.com (bell HTTPClient, TLS verified against
//      the bundled roots), which lists Spotify's access points
//   3. plain TCP connect to the first access point (cspot's next step)
//   4. two TLS connections that MUST fail (wrong host name, self-signed), to
//      prove certificate verification is on
// Then the app closes itself.

#include <arpa/inet.h>
#include <netdb.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "BellLogger.h"
#include "HTTPClient.h"
#include "TCPSocket.h"
#include "X509Bundle.h"

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceLoadExec(const char *path, const char **arguments);
}

namespace
{

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

std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> data;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return data;
    uint8_t buf[16384];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

// Fetches url; returns the body, or "" after logging the error.
std::string https_get(const char *name, const std::string &url)
{
    try
    {
        auto response = bell::HTTPClient::get(url);
        auto body = response->body();
        trace("[NET] %s: OK, %zu bytes", name, body.size());
        return std::string(body);
    }
    catch (const std::exception &error)
    {
        trace("[NET] %s: FAILED (%s)", name, error.what());
        return "";
    }
}

[[noreturn]] void quit()
{
    // The log is readable over FTP only while the title runs.
    trace("[NET] all tests finished; closing in 60 s");
    sceKernelUsleep(60000000);
    std::fflush(nullptr);
    int refused = sceSystemServiceLoadExec("exit", nullptr);
    trace("[NET] quit refused rc=0x%x", static_cast<unsigned>(refused));
    for (;;)
        sceKernelUsleep(100000);
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
    trace("[NET] libc++ abort");
    for (;;)
        sceKernelUsleep(100000);
}
} // namespace __1
} // namespace std

int main()
{
    trace("[NET] network test start");
    bell::setDefaultLogger();

    auto bundle = read_file("/app0/assets/x509_bundle.bin");
    try
    {
        bell::X509Bundle::init(bundle.data(), bundle.size());
        trace("[NET] cert bundle: %zu bytes, verify=%d", bundle.size(),
              bell::X509Bundle::shouldVerify());
    }
    catch (const std::exception &error)
    {
        trace("[NET] cert bundle: FAILED (%s); stopping, TLS would be unverified",
              error.what());
        quit();
    }

    // 1. DNS
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    int rc = getaddrinfo("apresolve.spotify.com", "443", &hints, &result);
    if (rc == 0 && result)
    {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr, ip,
                  sizeof(ip));
        trace("[NET] test1 dns apresolve.spotify.com: %s", ip);
        freeaddrinfo(result);
    }
    else
        trace("[NET] test1 dns: FAILED rc=%d", rc);

    // 2. HTTPS, verified
    std::string body =
        https_get("test2 https apresolve", "https://apresolve.spotify.com/?type=accesspoint");
    trace("[NET] test2 body: %.200s", body.c_str());

    // 3. TCP to the first access point, e.g. "ap-gew4.spotify.com:4070"
    std::size_t start = body.find("\"ap");
    std::size_t colon = body.find(':', start);
    std::size_t end = body.find('"', start + 1);
    if (start != std::string::npos && colon < end && end != std::string::npos)
    {
        std::string host = body.substr(start + 1, colon - start - 1);
        int port = std::stoi(body.substr(colon + 1, end - colon - 1));
        try
        {
            bell::TCPSocket socket;
            socket.open(host, port);
            trace("[NET] test3 tcp %s:%d: connected", host.c_str(), port);
            socket.close();
        }
        catch (const std::exception &error)
        {
            trace("[NET] test3 tcp %s:%d: FAILED (%s)", host.c_str(), port, error.what());
        }
    }
    else
        trace("[NET] test3 tcp: no access point in test2 body");

    // 4. Must fail: proves bad certificates are rejected
    bool wrong_host = https_get("test4a bad host name (must FAIL)",
                                "https://wrong.host.badssl.com/")
                          .empty();
    bool self_signed =
        https_get("test4b self-signed (must FAIL)", "https://self-signed.badssl.com/").empty();
    trace("[NET] test4 verification %s", wrong_host && self_signed ? "WORKS" : "BROKEN");

    quit();
}
