// ps5-spotify - Spotify side of the app, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the connection state machine and makes every call into cspot, so cspot
// only ever sees this one thread. The UI thread talks to it through two
// thread-safe doors: send(Command) in, status() out.
//
// States:
//   Connecting   -> Ready          login worked
//   Connecting   -> Pairing        nothing saved: wait for the phone (zeroconf)
//   Pairing      -> Connecting     the phone sent its login
//   Connecting   -> Error          no config, no network, rejected login
//   Ready        -> Reconnecting   cspot lost the connection (it retries itself)
//   Reconnecting -> Ready          cspot got it back
//   Reconnecting -> Error          not back within kReconnectGiveUpMs
//   Error        -> Connecting     Command::Retry (config and login re-read)

#pragma once

#include <pthread.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "audio/player.hpp"
#include "screens/model.hpp"
#include "spotify/pairing.hpp"
#include "spotify/session.hpp"

namespace app
{

enum class State
{
    Connecting,
    Pairing,
    Ready,
    Reconnecting,
    Error,
};

const char *state_name(State state);

// What the UI may ask for: screens::Command (retry, play_pause, next,
// previous; quit is handled by the app itself).
using screens::Command;

// A copy of everything the UI shows, taken under one lock.
struct Status
{
    State state = State::Connecting;
    screens::Step step = screens::Step::certificates; // loader progress
    std::string message; // why we are in Error/Reconnecting
    std::string username; // the signed-in account, once connected
    NowPlaying now_playing;
};

class Controller
{
  public:
    Controller() = default;
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    // Starts the controller thread. Returns false if it could not start.
    bool start();

    void send(Command command);
    Status status();

    // Control Room's settings. Quality and volume apply to the running
    // session; a new device name reconnects so the phone sees it.
    void apply(const screens::Settings &settings);

  private:
    static void *thread_entry(void *self);
    void run();
    void handle_commands();
    void apply_settings();
    void connect();
    void start_pairing();
    void step_pairing();
    void start_session(std::shared_ptr<cspot::LoginBlob> login);
    void step_ready();
    void step_reconnecting();
    void set_state(State next, const std::string &message = {});
    void set_step(screens::Step step);
    void publish_now_playing();

    // Controller thread only.
    std::unique_ptr<spotify::Session> session_;
    std::unique_ptr<spotify::Pairing> pairing_; // only in Pairing (and the login after it)
    std::int64_t reconnect_started_ms_ = 0;

    screens::Settings settings_; // what the session runs with (controller thread)

    // Shared with the UI thread, under lock_.
    std::mutex lock_;
    Status status_;
    std::deque<Command> commands_;
    std::optional<screens::Settings> pending_settings_;
};

} // namespace app
