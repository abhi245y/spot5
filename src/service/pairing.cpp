// spot5 - Zeroconf pairing: the phone sends its login to the console.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/pairing.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "civetweb.h"
#include "LoginBlob.h"
#include "app/config.hpp"
#include "platform/ps5/system.hpp"

namespace service
{

namespace
{

// Apps may not bind the mDNS port, and a media app may not listen at all
// (EACCES); payloads may: elfldr runs the helper (helper/mdns_helper.c),
// which advertises us and answers the phone, and talks to us over the
// control link below.
constexpr const char *kHelper = "/app0/assets/mdns-helper.elf";
constexpr int kElfLoaderPort = 9021;

bool send_helper(std::string &error)
{
    const std::string elf = app::read_file(kHelper);
    if (elf.empty())
    {
        error = std::string("Missing ") + kHelper;
        return false;
    }
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in loader{};
    loader.sin_len = sizeof(loader);
    loader.sin_family = AF_INET;
    loader.sin_port = htons(kElfLoaderPort);
    loader.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&loader), sizeof(loader)) != 0)
    {
        if (fd >= 0)
            close(fd);
        error = "Pairing needs the ELF loader running (port 9021)";
        return false;
    }
    size_t sent = 0;
    while (sent < elf.size())
    {
        const ssize_t now = send(fd, elf.data() + sent, elf.size() - sent, 0);
        if (now <= 0)
            break;
        sent += static_cast<size_t>(now);
    }
    close(fd); // end of file: elfldr starts the helper
    if (sent != elf.size())
    {
        error = "Could not hand the pairing helper to the ELF loader";
        return false;
    }
    return true;
}

constexpr int kControlPort = 7865; // the helper's lock port
// A real login is about 1 KB; the helper refuses anything far larger.
constexpr std::size_t kMaxBody = 32 * 1024;

int connect_control()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in helper{};
    helper.sin_len = sizeof(helper);
    helper.sin_family = AF_INET;
    helper.sin_port = htons(kControlPort);
    helper.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr *>(&helper), sizeof(helper)) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    return -1;
}

// Connects to the helper elfldr just started and waits for its "READY".
int open_control()
{
    for (int attempt = 0; attempt < 30; ++attempt) // about 6 s
    {
        const int fd = connect_control();
        if (fd >= 0)
        {
            timeval timeout{1, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            char ready[6];
            if (recv(fd, ready, sizeof(ready), MSG_WAITALL) == 6 && std::memcmp(ready, "READY\n", 6) == 0)
                return fd;
            close(fd); // a helper on its way out
        }
        hui::sys::sleep_us(200000);
    }
    return -1;
}

} // namespace

std::unique_ptr<Pairing> Pairing::start(const std::string &device_name, std::string &error)
{
    std::unique_ptr<Pairing> pairing(new Pairing());
    try
    {
        pairing->blob_ = std::make_shared<cspot::LoginBlob>(device_name);
        pairing->info_json_ = pairing->blob_->buildZeroconfInfo();
    }
    catch (const std::exception &e)
    {
        error = std::string("Pairing could not start (") + e.what() + ")";
        return nullptr;
    }
    if (!send_helper(error))
        return nullptr;
    pairing->control_ = open_control();
    const std::string hello = "INFO " + std::to_string(pairing->info_json_.size()) + "\n" + pairing->info_json_;
    if (pairing->control_ < 0 ||
        send(pairing->control_, hello.data(), hello.size(), 0) != static_cast<ssize_t>(hello.size()))
    {
        error = "The pairing helper did not answer";
        return nullptr;
    }
    fcntl(pairing->control_, F_SETFL, fcntl(pairing->control_, F_GETFL) | O_NONBLOCK);
    hui::sys::log("[APP] pairing: \"%s\" served by the helper", device_name.c_str());
    return pairing;
}

Pairing::~Pairing()
{
    if (control_ < 0)
        return;
    // The helper sees the link close and exits; wait until its lock port is
    // free, so a new pairing (after a rename) gets a new helper.
    close(control_);
    for (int attempt = 0; attempt < 15; ++attempt)
    {
        const int fd = connect_control();
        if (fd < 0)
            break;
        close(fd);
        hui::sys::sleep_us(200000);
    }
}

// The next "LOGIN <length>\n<form>" from the helper, decoded, if whole.
std::optional<std::map<std::string, std::string>> Pairing::next_query()
{
    char chunk[4096];
    for (;;)
    {
        const ssize_t got = recv(control_, chunk, sizeof(chunk), 0);
        if (got > 0)
            inbox_.append(chunk, static_cast<std::size_t>(got));
        else
        {
            if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                lost_ = true;
            break;
        }
    }
    const std::size_t line = inbox_.find('\n');
    if (line == std::string::npos || inbox_.compare(0, 6, "LOGIN ") != 0)
        return std::nullopt;
    const std::size_t length = std::strtoul(inbox_.c_str() + 6, nullptr, 10);
    if (length == 0 || length > kMaxBody)
    {
        lost_ = true; // not the helper we know
        return std::nullopt;
    }
    if (inbox_.size() < line + 1 + length)
        return std::nullopt;
    std::string form = inbox_.substr(line + 1, length);
    inbox_.erase(0, line + 1 + length);
    mg_header fields[10];
    const int count = mg_split_form_urlencoded(form.data(), fields, 10);
    std::map<std::string, std::string> query;
    for (int i = 0; i < count; i++)
        query[fields[i].name] = fields[i].value ? fields[i].value : "";
    hui::sys::log("[APP] pairing: login received from phone");
    return query;
}

std::shared_ptr<cspot::LoginBlob> Pairing::take_login(std::string &error)
{
    std::optional<std::map<std::string, std::string>> query = next_query();
    if (!query)
        return nullptr;
    // ponytail: cspot trusts the blob's inner lengths once decrypted; a hostile
    // device on the LAN could crash the app here. Check sizes in LoginBlob if
    // pairing ever stays on outside the Pairing screen.
    if ((*query)["blob"].size() < 64 || (*query)["clientKey"].empty() || (*query)["userName"].empty())
    {
        error = "The phone's login was incomplete";
        return nullptr;
    }
    try
    {
        blob_->loadZeroconfQuery(*query); // key exchange + decrypt
    }
    catch (const std::exception &e)
    {
        error = std::string("The phone's login could not be read (") + e.what() + ")";
        return nullptr;
    }
    if (blob_->username.empty())
    {
        error = "The phone's login could not be read";
        return nullptr;
    }
    return blob_;
}

} // namespace service
