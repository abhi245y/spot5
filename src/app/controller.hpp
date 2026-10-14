// spot5 - Spotify side of the app, on its own thread.
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
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio/player.hpp"
#include "screens/model.hpp"
#include "service/pairing.hpp"
#include "service/session.hpp"

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
    std::shared_ptr<const screens::HomePage> home; // kept across reconnects
    std::shared_ptr<const screens::PlaylistPage> page;
    std::shared_ptr<const screens::Queue> queue;
    std::shared_ptr<const screens::Library> library; // kept across reconnects
    bool library_loading = false;
    std::shared_ptr<const screens::ArtistPage> artist;
    bool home_loading = false;
    int artwork_done = 0; // reported by the app while Step::artwork
    int artwork_total = 0;
    std::string notice; // see screens::Model::notice
    std::string busy;   // see screens::Model::busy
    std::uint32_t notice_id = 0;
};

class Controller
{
  public:
    Controller() = default;
    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    // The zone Automatic stands for (from the console's offset). Call before start().
    void set_auto_timezone(std::string iana) { auto_timezone_ = std::move(iana); }
    // Starts the controller thread. Returns false if it could not start.
    bool start();

    void send(Command command);
    Status status();

    // Control Room's settings. Quality and volume apply to the running
    // session; a new device name reconnects so the phone sees it.
    void apply(const screens::Settings &settings);

    // Plays, opens pages, seeks... (screens::Request; done on the controller thread).
    void request(const screens::Request &request);

    // Launch: how many of Home's covers are loaded (the app loads them).
    void report_artwork(int done, int total);

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
    void step_browser();
    void handle_request(const screens::Request &request);
    void publish_page();
    void step_queue();
    void refresh_home(const char *why);
    void set_notice(const std::string &text);
    // The busy pill: what is under way, and what ends it.
    enum class Until
    {
        track_change, // a new track reaches the player
        restart,      // a new track, or this one back at its start
        page_loaded,  // the open page has its track list
        artist_loaded, // the open artist page has arrived
    };
    void set_busy(const std::string &text, Until until);
    void step_busy();
    std::string title_of(const std::string &uri);

    // Controller thread only.
    std::unique_ptr<service::Session> session_;
    std::unique_ptr<service::Pairing> pairing_; // only in Pairing (and the login after it)
    std::int64_t reconnect_started_ms_ = 0;
    std::int64_t home_asked_ms_ = 0; // last refresh request, to keep calls rare
    std::int64_t artwork_since_ms_ = 0; // launch: when Step::artwork began
    std::int64_t library_asked_ms_ = 0; // last library request, to keep calls rare
    std::string artist_uri_;            // the artist page asked for last
    // A long context started from Home: tracks not queued yet, added a
    // window at a time as the queue runs low (dropped if the phone takes over).
    std::vector<std::string> rest_;
    std::string rest_context_;
    // Pages and queue (controller thread): track details by URI, and the
    // URIs already asked for (so a row on screen is asked once).
    std::map<std::string, screens::TrackRow> details_;
    std::set<std::string> described_;
    screens::PlaylistPage page_;
    std::vector<std::string> page_uris_;
    std::vector<std::string> queue_uris_;
    int queue_index_ = -1;
    bool queue_dirty_ = false;
    // Busy pill (controller thread).
    Until busy_until_ = Until::track_change;
    std::string busy_track_; // the track playing when it began
    std::int64_t busy_since_ms_ = 0;

    screens::Settings settings_; // what the session runs with (controller thread)
    std::string auto_timezone_ = "Etc/UTC";
    // The IANA zone Home is asked for: the chosen one, or Automatic's.
    const std::string &timezone() const { return settings_.timezone.empty() ? auto_timezone_ : settings_.timezone; }

    // Shared with the UI thread, under lock_.
    std::mutex lock_;
    Status status_;
    std::deque<Command> commands_;
    std::optional<screens::Settings> pending_settings_;
    std::deque<screens::Request> requests_;
};

} // namespace app
