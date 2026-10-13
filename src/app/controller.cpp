// ps5-spotify - Spotify side of the app, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>

#include "platform/ps5/system.hpp"

namespace app
{

namespace
{

constexpr std::int64_t kReconnectGiveUpMs = 30000;
// Home: refetched hourly, by hand at most once a minute, and a failed fetch is
// not retried for ten minutes (bursts of calls get an account blocked).
constexpr std::int64_t kHomeMaxAgeMs = 3600 * 1000;
constexpr std::int64_t kManualRefreshGapMs = 60 * 1000;
constexpr std::int64_t kRetryGapMs = 10 * 60 * 1000;
// Add the next window of a long context when this many tracks are left.
constexpr std::size_t kQueueLowWater = 10;

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Home's fetch time is wall-clock (it is saved in home.json).
std::int64_t wall_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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

void Controller::play(const screens::PlayRequest &request)
{
    std::scoped_lock guard(lock_);
    pending_play_ = request;
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
            std::remove(paths::kHome.c_str()); // that account's Home
            {
                std::scoped_lock guard(lock_);
                status_.home = nullptr;
            }
            if (std::remove(paths::kCredentials.c_str()) == 0)
                hui::sys::log("[APP] saved login forgotten");
            else
                hui::sys::log("[APP] could not delete %s (nothing saved?)", paths::kCredentials.c_str());
            session_.reset();
            pairing_.reset();
            set_state(State::Connecting);
            break;
        case Command::refresh_home:
            if (state == State::Ready && now_ms() - home_asked_ms_ >= kManualRefreshGapMs)
                refresh_home("asked");
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
    home_asked_ms_ = 0; // a new session checks Home's age at once
    rest_.clear();
    set_state(State::Ready);
}

void Controller::step_ready()
{
    session_->pump(); // waits up to 200 ms for a packet
    publish_now_playing();
    step_browser();
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

void Controller::step_browser()
{
    spotify::Browser &browser = session_->browser();
    if (auto page = browser.take_page())
    {
        std::scoped_lock guard(lock_);
        status_.home = std::move(page);
    }
    if (std::optional<spotify::Resolved> resolved = browser.take_resolved())
    {
        rest_.clear();
        if (!resolved->error.empty())
            set_notice(resolved->error);
        else if (session_->play(*resolved, rest_))
            rest_context_ = resolved->context_uri;
        else
            set_notice("Couldn't start playback");
    }
    std::optional<screens::PlayRequest> request;
    {
        std::scoped_lock guard(lock_);
        request.swap(pending_play_);
    }
    if (request)
        browser.resolve(*request);

    // Page in the rest of a long context before the queue runs dry. The
    // locked count is checked first; the context only when it is low.
    if (!rest_.empty() && session_->tracks_remaining() <= kQueueLowWater)
    {
        if (session_->context_uri() != rest_context_)
        {
            rest_.clear(); // the phone started something else
        }
        else
        {
            const std::size_t count = std::min(rest_.size(), spotify::Session::kQueueWindow);
            session_->append({rest_.begin(), rest_.begin() + static_cast<std::ptrdiff_t>(count)});
            rest_.erase(rest_.begin(), rest_.begin() + static_cast<std::ptrdiff_t>(count));
            hui::sys::log("[APP] queued %zu more tracks, %zu still to come", count, rest_.size());
        }
    }

    // Hourly refresh; the first check comes right after login.
    const std::int64_t age = wall_ms() - browser.fetched_ms();
    if (age > kHomeMaxAgeMs && !browser.busy() &&
        (home_asked_ms_ == 0 || now_ms() - home_asked_ms_ > kRetryGapMs))
        refresh_home(browser.fetched_ms() == 0 ? "first time" : "over an hour old");

    std::scoped_lock guard(lock_);
    status_.home_loading = browser.busy();
}

void Controller::refresh_home(const char *why)
{
    hui::sys::log("[APP] Home refresh (%s)", why);
    home_asked_ms_ = now_ms();
    session_->browser().refresh();
}

void Controller::set_notice(const std::string &text)
{
    hui::sys::log("[APP] notice: %s", text.c_str());
    std::scoped_lock guard(lock_);
    status_.notice = text;
    ++status_.notice_id;
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
