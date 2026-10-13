// ps5-spotify - One logged-in Spotify Connect session (cspot + player).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Owns everything cspot needs for one login: context, Mercury session,
// spirc handler and the audio player. Destroying it shuts all of that down,
// so "retry" is simply: destroy, then start() again.

#pragma once

#include <memory>
#include <string>

#include "app/config.hpp"
#include "audio/player.hpp"

namespace cspot
{
struct Context;
class SpircHandler;
} // namespace cspot

namespace spotify
{

class Session
{
  public:
    // Logs in with the saved credentials, saves the refreshed credential and
    // starts Spotify Connect. Returns nullptr and sets `error` (a message for
    // the user) on failure.
    static std::unique_ptr<Session> start(const app::Config &config, std::string &error);

    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Handles one queued Spotify packet (waits up to 200 ms). Call in a loop.
    void pump();

    // False while cspot is reconnecting after a lost connection.
    bool connected() const;

    NowPlaying now_playing() { return player_->now_playing(); }

  private:
    Session() = default;

    std::shared_ptr<cspot::Context> ctx_;
    std::shared_ptr<cspot::SpircHandler> handler_;
    std::unique_ptr<Player> player_;
};

} // namespace spotify
