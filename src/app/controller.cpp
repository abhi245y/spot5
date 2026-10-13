// ps5-spotify - Spotify side of the app, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/controller.hpp"

#include <chrono>

#include "platform/ps5/system.hpp"

namespace app
{

namespace
{

constexpr std::int64_t kReconnectGiveUpMs = 30000;

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

const char *state_name(State state)
{
    switch (state)
    {
    case State::Connecting:
        return "Connecting";
    case State::Ready:
        return "Ready";
    case State::Reconnecting:
        return "Reconnecting";
    case State::Error:
        return "Error";
    }
    return "?";
}

bool Controller::start()
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20); // cspot login + JSON parsing
    pthread_t thread;
    int result = pthread_create(&thread, &attr, &Controller::thread_entry, this);
    pthread_attr_destroy(&attr);
    if (result != 0)
        return false;
    pthread_detach(thread); // runs for the life of the app
    return true;
}

void Controller::send(Command command)
{
    std::scoped_lock guard(lock_);
    commands_.push_back(command);
}

Status Controller::status()
{
    std::scoped_lock guard(lock_);
    return status_;
}

void *Controller::thread_entry(void *self)
{
    static_cast<Controller *>(self)->run();
    return nullptr;
}

void Controller::run()
{
    for (;;)
    {
        handle_commands();
        switch (status().state)
        {
        case State::Connecting:
            connect();
            break;
        case State::Ready:
            step_ready();
            break;
        case State::Reconnecting:
            step_reconnecting();
            break;
        case State::Error:
            hui::sys::sleep_us(50000); // waiting for Command::Retry
            break;
        }
    }
}

void Controller::handle_commands()
{
    std::deque<Command> commands;
    {
        std::scoped_lock guard(lock_);
        commands.swap(commands_);
    }
    for (Command command : commands)
    {
        switch (command)
        {
        case Command::Retry:
            if (status().state == State::Error)
            {
                session_.reset();
                set_state(State::Connecting);
            }
            break;
        }
    }
}

void Controller::connect()
{
    Config config;
    std::string error;
    if (!load_config(config, error))
    {
        set_state(State::Error, error);
        return;
    }
    session_ = spotify::Session::start(config, error);
    if (!session_)
    {
        set_state(State::Error, error);
        return;
    }
    set_state(State::Ready);
}

void Controller::step_ready()
{
    session_->pump(); // waits up to 200 ms for a packet
    publish_now_playing();
    if (!session_->connected())
    {
        reconnect_started_ms_ = now_ms();
        set_state(State::Reconnecting, "Connection lost, reconnecting");
    }
}

void Controller::step_reconnecting()
{
    session_->pump();
    if (session_->connected())
        set_state(State::Ready);
    else if (now_ms() - reconnect_started_ms_ > kReconnectGiveUpMs)
    {
        session_.reset(); // shuts cspot down; Retry starts a fresh session
        set_state(State::Error, "Lost connection to Spotify");
    }
}

void Controller::set_state(State next, const std::string &message)
{
    std::scoped_lock guard(lock_);
    hui::sys::log("[APP] state: %s -> %s%s%s", state_name(status_.state), state_name(next),
                  message.empty() ? "" : ": ", message.c_str());
    status_.state = next;
    status_.message = message;
    if (next != State::Ready && next != State::Reconnecting)
        status_.now_playing = {};
}

void Controller::publish_now_playing()
{
    NowPlaying now = session_->now_playing();
    std::scoped_lock guard(lock_);
    if (now == status_.now_playing)
        return;
    if (!now.title.empty())
        hui::sys::log("[APP] %s: %s - %s", now.paused ? "paused" : "playing", now.artist.c_str(),
                      now.title.c_str());
    status_.now_playing = now;
}

} // namespace app
