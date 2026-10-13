// ps5-spotify - App lifecycle: connect, play, report errors, retry on ✕.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// States:
//   Connecting   -> Ready          login worked
//   Connecting   -> Error          no config/login, no network, rejected login
//   Ready        -> Reconnecting   cspot lost the connection (it retries itself)
//   Reconnecting -> Ready          cspot got it back
//   Reconnecting -> Error          not back within kReconnectGiveUpMs
//   Error        -> Connecting     user pressed ✕ (config and login re-read)
// The app runs until the system closes it (Close app); there is no quit.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/config.hpp"
#include "audio/player.hpp"
#include "platform/pad.hpp"
#include "spotify/session.hpp"

namespace app
{

class App
{
  public:
    [[noreturn]] void run();

  private:
    enum class State
    {
        Connecting,
        Ready,
        Reconnecting,
        Error,
    };

    void connect();
    void step_ready();
    void step_reconnecting();
    void step_error();
    void set_state(State next, const std::string &message = {});
    void log_now_playing();

    State state_ = State::Connecting;
    std::string error_; // shown to the user in State::Error
    std::int64_t reconnect_started_ms_ = 0;
    std::unique_ptr<spotify::Session> session_;
    platform::Pad pad_;
    NowPlaying last_logged_;
};

} // namespace app
