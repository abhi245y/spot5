// spot5 - One logged-in Spotify Connect session (cspot + player).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Owns everything cspot needs for one login: context, Mercury session,
// spirc handler and the audio player. Destroying it shuts all of that down,
// so "retry" is simply: destroy, then start() again.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "app/config.hpp"
#include "audio/player.hpp"
#include "service/browser.hpp"

namespace cspot
{
struct Context;
class LoginBlob;
class SpircHandler;
} // namespace cspot

namespace service
{

class Session
{
  public:
    enum class Stage
    {
        connecting, // to an access point, then logging in
        starting,   // Spotify Connect and the audio output
    };

    // The login saved by an earlier session, or nullptr if there is none
    // (the console needs pairing). Throws if the file is unreadable JSON.
    static std::shared_ptr<cspot::LoginBlob> saved_login(const std::string &device_name);

    // How start()'s error begins when the network, not the login, failed.
    static constexpr const char *kUnreachable = "Could not reach Spotify";

    // Logs in (saved login or one from pairing), saves the refreshed
    // credential and starts Spotify Connect, reporting each stage. Returns
    // nullptr and sets `error` (a message for the user) on failure.
    static std::unique_ptr<Session> start(const app::Config &config,
                                          const screens::Settings &settings, const std::string &timezone,
                                          std::shared_ptr<cspot::LoginBlob> login, std::string &error,
                                          const std::function<void(Stage)> &progress);

    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Handles one queued Spotify packet (waits up to 200 ms). Call in a loop.
    void pump();

    // False while cspot is reconnecting after a lost connection.
    bool connected() const;

    NowPlaying now_playing() { return player_->now_playing(); }

    // Control Room: quality applies from the next track, volume at once.
    void set_quality(int quality);
    void set_volume(int level, bool follow_phone);
    const std::string &device_name() const { return device_name_; }
    std::string username() const;

    // Home and what to play from it (fetching runs on the browser's thread).
    Browser &browser() { return *browser_; }
    // Starts a resolved track list on this console, as a phone would. Only
    // the first kQueueWindow tracks are queued (a Spirc frame carries the
    // whole queue); the rest are returned for append() as the queue runs low.
    static constexpr std::size_t kQueueWindow = 100;
    bool play(const Resolved &resolved, std::vector<std::string> &rest, std::size_t start = 0);
    void append(const std::vector<std::string> &tracks);
    // Tracks queued after the current one, and the context being played.
    std::size_t tracks_remaining() const;
    std::string context_uri() const;
    // Seek within the playing track; play queue position `index`.
    void seek(std::uint32_t ms);
    bool jump(int index);
    // The queued track URIs and the playing position (-1: none).
    std::vector<std::string> queue(int &index) const;

    // Playback commands, as if pressed on the phone.
    void set_paused(bool paused);
    // See Player::hold_play / take_held_play.
    void hold_play(bool hold);
    bool take_held_play();
    void next();
    void previous();

  private:
    Session() = default;

    std::shared_ptr<cspot::Context> ctx_;
    std::shared_ptr<cspot::SpircHandler> handler_;
    std::unique_ptr<Player> player_;
    std::unique_ptr<Browser> browser_; // destroyed first: it uses ctx_
    std::string device_name_;
};

} // namespace service
