// ps5-spotify - Spotify side of the app, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runs the connection state machine and makes every call into cspot, so cspot
// only ever sees this one thread. The UI thread talks to it through two
// thread-safe doors: send(Command) in, status() out.
//
// States:
//   Connecting   -> Ready          login worked
//   Connecting   -> Error          no config/login, no network, rejected login
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
#include <string>

#include "audio/player.hpp"
#include "spotify/session.hpp"

namespace app
{

enum class State
{
    Connecting,
    Ready,
    Reconnecting,
    Error,
};

const char *state_name(State state);

enum class Command
{
    Retry,
};

// A copy of everything the UI shows, taken under one lock.
struct Status
{
    State state = State::Connecting;
    std::string message; // why we are in Error/Reconnecting
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

  private:
    static void *thread_entry(void *self);
    void run();
    void handle_commands();
    void connect();
    void step_ready();
    void step_reconnecting();
    void set_state(State next, const std::string &message = {});
    void publish_now_playing();

    // Controller thread only.
    std::unique_ptr<spotify::Session> session_;
    std::int64_t reconnect_started_ms_ = 0;

    // Shared with the UI thread, under lock_.
    std::mutex lock_;
    Status status_;
    std::deque<Command> commands_;
};

} // namespace app
