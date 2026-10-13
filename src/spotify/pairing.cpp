// ps5-spotify - Zeroconf pairing: the phone sends its login to the console.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/pairing.hpp"

#include <algorithm>
#include <exception>

#include "BellHTTPServer.h"
#include "LoginBlob.h"
#include "MDNSService.h"
#include "platform/ps5/system.hpp"

namespace spotify
{

namespace
{

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

    // TXT record as cspot's CLI: tells the phone where to GET/POST.
    pairing->mdns_ = bell::MDNSService::registerService(
        device_name, "_spotify-connect", "_tcp", "", kPort,
        {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});
    if (!pairing->mdns_)
    {
        error = "Could not advertise on the network (mDNS)";
        return nullptr;
    }
    hui::sys::log("[APP] pairing: advertising \"%s\" on port %d", device_name.c_str(), kPort);
    return pairing;
}

Pairing::~Pairing()
{
    if (mdns_)
        mdns_->unregisterService();
    // Stop the server (joins its threads) before the handlers' `this` goes.
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
