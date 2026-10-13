// ps5-spotify - One logged-in Spotify Connect session (cspot + player).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/session.hpp"

#include <exception>

#include "CSpotContext.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "SpircHandler.h"
#include "platform/ps5/system.hpp"

namespace spotify
{

namespace
{
constexpr const char *kDeviceName = "PS5"; // name in the phone's Connect list
}

std::unique_ptr<Session> Session::start(const app::Config &config, std::string &error)
{
    std::string creds = app::read_file(app::paths::kCredentials);
    if (creds.empty())
    {
        error = "No saved login (" + app::paths::kCredentials + ")";
        return nullptr;
    }

    std::unique_ptr<Session> session(new Session());
    try
    {
        auto blob = std::make_shared<cspot::LoginBlob>(kDeviceName);
        blob->loadJson(creds);
        session->ctx_ = cspot::Context::createFromBlob(blob);
        auto &ctx = *session->ctx_;
        ctx.config.clientId = config.client_id;
        ctx.config.audioFormat = AudioFormat_OGG_VORBIS_320;
        ctx.config.volume = 65535; // reported to the phone; applied in software

        ctx.session->connectWithRandomAp();
        ctx.config.authData = ctx.session->authenticate(blob);
        if (ctx.config.authData.empty())
        {
            error = "Spotify rejected the saved login";
            return nullptr;
        }
    }
    catch (const std::exception &e)
    {
        error = std::string("Could not reach Spotify (") + e.what() + ")";
        return nullptr;
    }

    // Spotify returns a fresh reusable credential at each login; save it now
    // (not at exit: Close app ends the process without warning).
    if (!app::write_file(app::paths::kCredentials, session->ctx_->getCredentialsJson()))
        hui::sys::log("[APP] note: could not save refreshed login to %s",
                      app::paths::kCredentials.c_str());

    // Same order as cspot's CLI: spirc, Mercury task, then the player.
    session->handler_ = std::make_shared<cspot::SpircHandler>(session->ctx_);
    session->ctx_->session->startTask();
    session->player_ = std::make_unique<Player>(session->handler_);
    if (!session->player_->ok())
    {
        error = "Audio output could not be opened";
        return nullptr;
    }
    hui::sys::log("[APP] Spotify Connect running as \"%s\"", kDeviceName);
    return session;
}

Session::~Session()
{
    // Stop cspot's tasks first (no more audio callbacks), then the output.
    if (handler_)
        handler_->disconnect();
    else if (ctx_)
        ctx_->session->disconnect();
    player_.reset();
}

void Session::pump()
{
    ctx_->session->handlePacket();
}

bool Session::connected() const
{
    return ctx_->session->isConnected();
}

} // namespace spotify
