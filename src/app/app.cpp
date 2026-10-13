// ps5-spotify - App lifecycle: connect, play, report errors, retry on ✕.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include <chrono>

#include "BellLogger.h"
#include "X509Bundle.h"
#include "platform/log.hpp"
#include "platform/system.hpp"

namespace app
{

namespace
{

constexpr std::int64_t kReconnectGiveUpMs = 30000;

const char *name(int state)
{
    static const char *names[] = {"Connecting", "Ready", "Reconnecting", "Error"};
    return names[state];
}

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

void App::run()
{
    platform::log("[APP] start");
    bell::setDefaultLogger();
    pad_.open();

    // TLS certificate checks need the root bundle; without it bell would
    // accept any certificate, so refuse to go on.
    std::string bundle = read_file(paths::kCertBundle);
    try
    {
        bell::X509Bundle::init(reinterpret_cast<const uint8_t *>(bundle.data()), bundle.size());
    }
    catch (const std::exception &e)
    {
        set_state(State::Error, std::string("Certificate bundle unusable: ") + e.what());
        platform::park(); // shipped with the app: retrying cannot fix it
    }

    for (;;)
    {
        switch (state_)
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
            step_error();
            break;
        }
    }
}

void App::connect()
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

void App::step_ready()
{
    session_->pump(); // waits up to 200 ms for a packet
    log_now_playing();
    if (!session_->connected())
    {
        reconnect_started_ms_ = now_ms();
        set_state(State::Reconnecting, "Connection lost, reconnecting");
    }
}

void App::step_reconnecting()
{
    session_->pump();
    if (session_->connected())
        set_state(State::Ready);
    else if (now_ms() - reconnect_started_ms_ > kReconnectGiveUpMs)
    {
        session_.reset(); // shuts cspot down; ✕ starts a fresh session
        set_state(State::Error, "Lost connection to Spotify");
    }
}

void App::step_error()
{
    if (!pad_.is_open())
        pad_.open();
    if (pad_.poll_pressed() & platform::button::kCross)
    {
        session_.reset();
        set_state(State::Connecting);
        return;
    }
    platform::sleep_ms(50);
}

void App::set_state(State next, const std::string &message)
{
    platform::log("[APP] state: %s -> %s%s%s", name(static_cast<int>(state_)),
                  name(static_cast<int>(next)), message.empty() ? "" : ": ", message.c_str());
    if (next == State::Error)
    {
        error_ = message;
        platform::log("[APP] press X to retry");
    }
    state_ = next;
}

void App::log_now_playing()
{
    NowPlaying now = session_->now_playing();
    if (now == last_logged_ || now.title.empty())
        return;
    platform::log("[APP] %s: %s - %s", now.paused ? "paused" : "playing", now.artist.c_str(),
                  now.title.c_str());
    last_logged_ = now;
}

} // namespace app
