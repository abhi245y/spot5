// ps5-spotify - One logged-in Spotify Connect session (cspot + player).
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/session.hpp"

#include <algorithm>
#include <exception>

#include "CSpotContext.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "SpircHandler.h"
#include "platform/ps5/system.hpp"

namespace spotify
{

std::shared_ptr<cspot::LoginBlob> Session::saved_login(const std::string &device_name)
{
    std::string creds = app::read_file(app::paths::kCredentials);
    if (creds.empty())
        return nullptr;
    auto blob = std::make_shared<cspot::LoginBlob>(device_name);
    blob->loadJson(creds);
    return blob;
}

std::unique_ptr<Session> Session::start(const app::Config &config,
                                        const screens::Settings &settings,
                                        std::shared_ptr<cspot::LoginBlob> blob, std::string &error,
                                        const std::function<void(Stage)> &progress)
{
    std::unique_ptr<Session> session(new Session());
    session->device_name_ = settings.device_name; // the name in the phone's Connect list
    try
    {
        session->ctx_ = cspot::Context::createFromBlob(blob);
        auto &ctx = *session->ctx_;
        ctx.config.clientId = config.client_id;
        ctx.config.audioFormat = static_cast<AudioFormat>(settings.quality);
        ctx.config.volume = 65535; // reported to the phone; applied in software

        progress(Stage::connecting);
        ctx.session->connectWithRandomAp();
        ctx.config.authData = ctx.session->authenticate(blob);
        if (ctx.config.authData.empty())
        {
            error = "Spotify rejected the login";
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
    progress(Stage::starting);
    session->handler_ = std::make_shared<cspot::SpircHandler>(session->ctx_);
    session->ctx_->session->startTask();
    session->player_ = std::make_unique<Player>(session->handler_);
    if (!session->player_->ok())
    {
        error = "Audio output could not be opened";
        return nullptr;
    }
    session->player_->set_volume(settings.volume, settings.phone_volume);
    session->browser_ = std::make_unique<Browser>(session->ctx_, config.timezone);
    hui::sys::log("[APP] Spotify Connect running as \"%s\"", settings.device_name.c_str());
    return session;
}

Session::~Session()
{
    browser_.reset(); // finishes its request; uses ctx_
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

void Session::set_quality(int quality)
{
    // AudioFormat_OGG_VORBIS_96/160/320 are 0/1/2, the same as the setting.
    // QueuedTrack reads it when it picks the next track's file.
    ctx_->config.audioFormat = static_cast<AudioFormat>(std::clamp(quality, 0, 2));
}

void Session::set_volume(int level, bool follow_phone)
{
    player_->set_volume(level, follow_phone);
}

std::string Session::username() const
{
    return ctx_->config.username;
}

bool Session::play(const Resolved &resolved, std::vector<std::string> &rest)
{
    const std::size_t first = std::min(resolved.tracks.size(), kQueueWindow);
    const std::vector<std::string> window(resolved.tracks.begin(),
                                          resolved.tracks.begin() + static_cast<std::ptrdiff_t>(first));
    rest.assign(resolved.tracks.begin() + static_cast<std::ptrdiff_t>(first), resolved.tracks.end());
    return handler_->loadContext(resolved.context_uri, window, 0, resolved.shuffle);
}

void Session::append(const std::vector<std::string> &tracks)
{
    handler_->appendToContext(tracks);
}

std::size_t Session::tracks_remaining() const
{
    return handler_->getTrackQueue()->tracksRemaining();
}

std::string Session::context_uri() const
{
    return handler_->contextUri();
}

void Session::set_paused(bool paused)
{
    handler_->setPause(paused);
}

void Session::next()
{
    handler_->nextSong();
}

void Session::previous()
{
    handler_->previousSong();
}

bool Session::connected() const
{
    return ctx_->session->isConnected();
}

} // namespace spotify
