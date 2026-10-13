// ps5-spotify - Zeroconf pairing: the phone sends its login to the console.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/pairing.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <exception>

#include "BellHTTPServer.h"
#include "LoginBlob.h"
#include "app/config.hpp"
#include "platform/ps5/system.hpp"

namespace spotify
{

namespace
{

// Apps may not bind the mDNS port (EACCES), payloads may: elfldr runs the
// helper (helper/mdns_helper.c), which advertises us while this server
// answers and exits by itself afterwards.
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

constexpr int kPort = 7864; // same as cspot's CLI
// A real login is about 1 KB; refuse anything far larger from the network.
constexpr long long kMaxBody = 16 * 1024;

// Spotify's "OK, logging in" reply to the phone's POST.
constexpr const char *kPostReply = R"({"status":101,"spotifyError":0,"statusString":"ERROR-OK"})";

} // namespace

std::unique_ptr<Pairing> Pairing::start(const std::string &device_name, std::string &error)
{
    std::unique_ptr<Pairing> pairing(new Pairing());
    try
    {
        pairing->blob_ = std::make_shared<cspot::LoginBlob>(device_name);
        pairing->info_json_ = pairing->blob_->buildZeroconfInfo();
        pairing->server_ = std::make_unique<bell::BellHTTPServer>(kPort);
    }
    catch (const std::exception &e)
    {
        error = std::string("Pairing could not start (") + e.what() + ")";
        return nullptr;
    }

    Pairing *self = pairing.get();
    auto &server = *pairing->server_;
    server.registerGet("/spotify_info", [self](mg_connection *) {
        return self->server_->makeJsonResponse(self->info_json_);
    });
    server.registerPost("/spotify_info", [self](mg_connection *conn) {
        const mg_request_info *request = mg_get_request_info(conn);
        if (request->content_length <= 0 || request->content_length > kMaxBody)
            return self->server_->makeJsonResponse(R"({"status":102})", 400);

        std::string body(static_cast<size_t>(request->content_length), '\0');
        body.resize(static_cast<size_t>(std::max(0, mg_read(conn, body.data(), body.size()))));
        mg_header fields[10];
        const int count = mg_split_form_urlencoded(body.data(), fields, 10);
        std::map<std::string, std::string> query;
        for (int i = 0; i < count; i++)
            query[fields[i].name] = fields[i].value ? fields[i].value : "";

        hui::sys::log("[APP] pairing: login received from phone");
        {
            std::scoped_lock guard(self->lock_);
            self->query_ = std::move(query);
        }
        return self->server_->makeJsonResponse(kPostReply);
    });

    if (!send_helper(error))
        return nullptr;
    hui::sys::log("[APP] pairing: \"%s\" on port %d, mDNS helper sent", device_name.c_str(), kPort);
    return pairing;
}

Pairing::~Pairing()
{
    // The helper sees the server gone and stops advertising. Stop the server (joins its threads) before the handlers' `this` goes.
    if (server_)
        server_->close();
}

std::shared_ptr<cspot::LoginBlob> Pairing::take_login(std::string &error)
{
    std::optional<std::map<std::string, std::string>> query;
    {
        std::scoped_lock guard(lock_);
        query.swap(query_);
    }
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

} // namespace spotify
