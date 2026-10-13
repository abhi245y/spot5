// ps5-spotify - Spotify side of the app, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/controller.hpp"

#include <chrono>
#include <cstdio>
#include <exception>

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
    case State::Pairing:
        return "Pairing";
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

void Controller::apply(const screens::Settings &settings)
{
    std::scoped_lock guard(lock_);
    pending_settings_ = settings;
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
        apply_settings();
        handle_commands();
        switch (status().state)
        {
        case State::Connecting:
            connect();
            break;
        case State::Pairing:
            step_pairing();
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
        const State state = status().state;
        switch (command)
        {
        case Command::retry:
            if (state == State::Error)
            {
                session_.reset();
                pairing_.reset();
                set_state(State::Connecting);
            }
            break;
        case Command::play_pause:
            if (state == State::Ready)
                session_->set_paused(!session_->now_playing().paused);
            break;
        case Command::next:
            if (state == State::Ready)
                session_->next();
            break;
        case Command::previous:
            if (state == State::Ready)
                session_->previous();
            break;
        case Command::forget_login:
            // Delete the saved login and end the session; connecting again
            // then finds nothing saved and waits for the phone (Pairing).
            std::remove(paths::kCredentials.c_str());
            hui::sys::log("[APP] saved login forgotten");
            session_.reset();
            pairing_.reset();
            set_state(State::Connecting);
            break;
        case Command::quit:
            break; // the app quits itself
        }
    }
}

void Controller::apply_settings()
{
    std::optional<screens::Settings> next;
    {
        std::scoped_lock guard(lock_);
        next.swap(pending_settings_);
    }
    if (!next)
        return;
    const bool renamed = next->device_name != settings_.device_name;
    settings_ = *next;
    if (pairing_ && renamed)
    {
        // Advertise under the new name.
        pairing_.reset();
        set_state(State::Connecting);
        return;
    }
    if (!session_)
        return; // used by the next connect()
    session_->set_quality(settings_.quality);
    session_->set_volume(settings_.volume, settings_.phone_volume);
    if (renamed)
    {
        // The name is part of the Connect identity: reconnect under it.
        hui::sys::log("[APP] device renamed to \"%s\"; reconnecting", settings_.device_name.c_str());
        session_.reset();
        set_state(State::Connecting);
    }
}

void Controller::connect()
{
    set_step(screens::Step::login);
    std::shared_ptr<cspot::LoginBlob> login;
    try
    {
        login = spotify::Session::saved_login(settings_.device_name);
    }
    catch (const std::exception &)
    {
        set_state(State::Error, "The saved login is unreadable. Forget it in Settings > Account.");
        return;
    }
    if (login)
        start_session(std::move(login));
    else
        start_pairing();
}

void Controller::start_pairing()
{
    std::string error;
    pairing_ = spotify::Pairing::start(settings_.device_name, error);
    if (!pairing_)
    {
        set_state(State::Error, error);
        return;
    }
    {
        std::scoped_lock guard(lock_);
        status_.username.clear();
    }
    set_state(State::Pairing);
}

void Controller::step_pairing()
{
    std::string error;
    std::shared_ptr<cspot::LoginBlob> login = pairing_->take_login(error);
    if (!error.empty())
        hui::sys::log("[APP] pairing: %s", error.c_str()); // the phone can try again
    if (!login)
    {
        hui::sys::sleep_us(100000);
        return;
    }
    set_state(State::Connecting);
    start_session(std::move(login));
    // ponytail: the phone may check /spotify_info while we log in, so the
    // advertisement stays up until the login is done.
    pairing_.reset();
}

void Controller::start_session(std::shared_ptr<cspot::LoginBlob> login)
{
    Config config;
    std::string error;
    if (!load_config(config, error))
    {
        set_state(State::Error, error);
        return;
    }
    session_ = spotify::Session::start(config, settings_, std::move(login), error,
                                       [this](spotify::Session::Stage stage) {
                                           set_step(stage == spotify::Session::Stage::connecting
                                                        ? screens::Step::connect
                                                        : screens::Step::start);
                                       });
    if (!session_)
    {
        set_state(State::Error, error);
        return;
    }
    set_step(screens::Step::done);
    {
        std::scoped_lock guard(lock_);
        status_.username = session_->username();
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

void Controller::set_step(screens::Step step)
{
    std::scoped_lock guard(lock_);
    status_.step = step;
}

void Controller::publish_now_playing()
{
    NowPlaying now = session_->now_playing();
    std::scoped_lock guard(lock_);
    const NowPlaying &was = status_.now_playing;
    if (!now.title.empty() && (now.track_id != was.track_id || now.paused != was.paused))
        hui::sys::log("[APP] %s: %s - %s", now.paused ? "paused" : "playing", now.artist.c_str(),
                      now.title.c_str());
    status_.now_playing = now;
}

} // namespace app
