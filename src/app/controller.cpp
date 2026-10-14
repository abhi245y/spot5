// spot5 - Spotify side of the app, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/controller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <random>

#include "platform/ps5/system.hpp"
#include "service/home.hpp"

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
// Launch waits for Home's covers at most this long (a dead link must not
// keep the app on the loader).
constexpr std::int64_t kArtworkWaitMs = 45000;
// After a pairing login, how long the phone's first "play" is held back.
constexpr std::int64_t kPairingHoldMs = 15000;
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

void Controller::request(const screens::Request &request)
{
    std::scoped_lock guard(lock_);
    requests_.push_back(request);
}

void Controller::report_artwork(int done, int total)
{
    std::scoped_lock guard(lock_);
    status_.artwork_done = done;
    status_.artwork_total = total;
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
            {
                set_busy("Skipping", Until::track_change);
                session_->next();
            }
            break;
        case Command::previous:
            if (state == State::Ready)
            {
                set_busy("Going back", Until::restart);
                session_->previous();
            }
            break;
        case Command::forget_login:
            // Delete the saved login and end the session; connecting again
            // then finds nothing saved and waits for the phone (Pairing).
            std::remove(paths::kHome.c_str()); // that account's Home and Library
            std::remove(paths::kLibrary.c_str());
            {
                std::scoped_lock guard(lock_);
                status_.home = nullptr;
                status_.library = nullptr;
                status_.artist = nullptr;
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
        case Command::refresh_library:
            if (state == State::Ready && now_ms() - library_asked_ms_ >= kManualRefreshGapMs)
            {
                library_asked_ms_ = now_ms();
                session_->browser().refresh_library();
                std::scoped_lock guard(lock_);
                status_.library_loading = true;
            }
            break;
        case Command::clear_cache:
            // The app clears covers itself; here the saved Home and what was
            // learned about tracks, then a fresh Home.
            std::remove(paths::kHome.c_str());
            std::remove(paths::kLibrary.c_str());
            library_asked_ms_ = 0; // fetched again with its art
            if (session_)
                session_->browser().forget_library();
            {
                std::scoped_lock guard(lock_);
                status_.library = nullptr;
                status_.artist = nullptr;
            }
            details_.clear();
            described_.clear();
            if (state == State::Ready)
            {
                home_asked_ms_ = now_ms();
                session_->browser().refresh(true);
            }
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
    const bool rezoned = next->timezone != settings_.timezone;
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
    if (rezoned)
        session_->browser().set_timezone(timezone()); // the next Home refresh uses it
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
        login = service::Session::saved_login(settings_.device_name);
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
    pairing_ = service::Pairing::start(settings_.device_name, error);
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
    if (pairing_->lost())
    {
        pairing_.reset();
        set_state(State::Error, "Pairing stopped. Try again.");
        return;
    }
    if (!error.empty())
        hui::sys::log("[APP] pairing: %s", error.c_str()); // the phone can try again
    if (!login)
    {
        hui::sys::sleep_us(100000);
        return;
    }
    set_state(State::Connecting);
    start_session(std::move(login));
    if (session_)
    {
        // Picking this console on the phone also moves the phone's music
        // here, playing: let it arrive paused (Play continues it).
        session_->hold_play(true);
        hold_play_until_ms_ = now_ms() + kPairingHoldMs;
    }
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
    // The first connect after launch often fails (the console's network is
    // not ready yet) and works a moment later: retry quietly before saying so.
    constexpr int kConnectTries = 3;
    for (int attempt = 1;; ++attempt)
    {
        error.clear();
        session_ = service::Session::start(config, settings_, timezone(), login, error,
                                           [this](service::Session::Stage stage) {
                                               set_step(stage == service::Session::Stage::connecting
                                                            ? screens::Step::connect
                                                            : screens::Step::start);
                                           });
        if (session_ || attempt == kConnectTries || error.rfind(service::Session::kUnreachable, 0) != 0)
            break;
        hui::sys::log("[APP] connect attempt %d of %d failed (%s); retrying", attempt, kConnectTries,
                      error.c_str());
        hui::sys::sleep_us(2000000);
    }
    if (!session_)
    {
        set_state(State::Error, error);
        return;
    }
    // Logged in; the loader goes on with Home and its covers (step_browser).
    set_step(screens::Step::home);
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
    if (hold_play_until_ms_ != 0)
    {
        const bool held = session_->take_held_play();
        if (held)
        {
            session_->set_paused(true); // so the phone shows it paused here too
            hui::sys::log("[APP] music moved here at pairing: paused");
        }
        if (held || now_ms() > hold_play_until_ms_) // only that first play; later ones are wanted
        {
            session_->hold_play(false);
            hold_play_until_ms_ = 0;
        }
    }
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
    service::Browser &browser = session_->browser();
    if (auto page = browser.take_page())
    {
        std::scoped_lock guard(lock_);
        status_.home = std::move(page);
    }
    if (std::optional<service::Resolved> resolved = browser.take_resolved())
    {
        rest_.clear();
        if (!resolved->error.empty())
            set_notice(resolved->error);
        else if (session_->play(*resolved, rest_))
            rest_context_ = resolved->context_uri;
        else
            set_notice("Couldn't start playback");
    }
    std::deque<screens::Request> requests;
    {
        std::scoped_lock guard(lock_);
        requests.swap(requests_);
    }
    for (const screens::Request &request : requests)
        handle_request(request);

    if (auto library = browser.take_library())
    {
        std::scoped_lock guard(lock_);
        status_.library = std::move(library);
        status_.library_loading = false;
    }
    if (auto artist = browser.take_artist())
    {
        if (artist->card.uri == artist_uri_)
        {
            if (!artist->error.empty())
                set_notice(artist->error);
            std::scoped_lock guard(lock_);
            status_.artist = std::move(artist);
        }
    }
    // Library a minute after the launch is done (Home's calls first, so the
    // two never burst together), then hourly.
    constexpr std::int64_t kLibraryAfterLaunchMs = 60 * 1000;
    if (status().step == screens::Step::done && now_ms() - artwork_since_ms_ > kLibraryAfterLaunchMs &&
        wall_ms() - browser.library_fetched_ms() > kHomeMaxAgeMs && !browser.busy() &&
        (library_asked_ms_ == 0 || now_ms() - library_asked_ms_ > kRetryGapMs))
    {
        hui::sys::log("[APP] Library refresh");
        library_asked_ms_ = now_ms();
        browser.refresh_library();
        std::scoped_lock guard(lock_);
        status_.library_loading = true;
    }

    // A page's track list, then details as they arrive.
    if (std::optional<service::Resolved> opened = browser.take_opened())
    {
        if (opened->context_uri == page_.card.uri)
        {
            page_uris_ = opened->tracks;
            page_.loading = false;
            page_.error = opened->tracks.empty() ? opened->error : std::string();
            publish_page();
        }
    }
    if (std::vector<screens::TrackRow> rows = browser.take_described(); !rows.empty())
    {
        for (screens::TrackRow &row : rows)
            details_[row.uri] = std::move(row);
        publish_page();
        queue_dirty_ = true;
    }
    step_queue();
    step_busy();

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
            const std::size_t count = std::min(rest_.size(), service::Session::kQueueWindow);
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

    // Launch: Home first (if it was refreshed above), then its covers.
    const screens::Step step = status().step;
    if (step == screens::Step::home && !browser.busy())
    {
        artwork_since_ms_ = now_ms();
        set_step(screens::Step::artwork);
    }
    else if (step == screens::Step::artwork)
    {
        const Status now = status();
        const bool loaded = now.artwork_total > 0 && now.artwork_done >= now.artwork_total;
        if (loaded || !now.home || now_ms() - artwork_since_ms_ > kArtworkWaitMs)
        {
            hui::sys::log("[APP] launch: %d/%d covers loaded", now.artwork_done, now.artwork_total);
            set_step(screens::Step::done);
        }
    }

    std::scoped_lock guard(lock_);
    status_.home_loading = browser.busy();
}

void Controller::handle_request(const screens::Request &request)
{
    using Kind = screens::Request::Kind;
    service::Browser &browser = session_->browser();
    switch (request.kind)
    {
    case Kind::play:
        // A list of tracks given with the request (an artist's Popular).
        if (!request.play.tracks.empty())
        {
            service::Resolved resolved;
            resolved.context_uri = request.play.uri;
            resolved.tracks = request.play.tracks;
            rest_.clear();
            if (session_->play(resolved, rest_, static_cast<std::size_t>(std::max(0, request.play.index))))
                rest_context_ = resolved.context_uri;
            else
                set_notice("Couldn't start playback");
        }
        // From an open page: its track list is here already (start anywhere).
        else if (request.play.uri == page_.card.uri && !page_uris_.empty())
        {
            service::Resolved resolved;
            resolved.context_uri = service::home::playable_uri(page_.card.uri, session_->username());
            resolved.tracks = page_uris_;
            resolved.shuffle = request.play.shuffle;
            std::size_t start = static_cast<std::size_t>(std::max(0, request.play.index));
            if (resolved.shuffle)
            {
                std::minstd_rand random(static_cast<std::uint32_t>(now_ms()));
                std::shuffle(resolved.tracks.begin(), resolved.tracks.end(), random);
                start = 0;
            }
            rest_.clear();
            if (session_->play(resolved, rest_, start))
                rest_context_ = resolved.context_uri;
            else
                set_notice("Couldn't start playback");
        }
        else
        {
            browser.resolve(request.play);
        }
        {
            const std::string title = title_of(request.play.uri);
            set_busy((request.play.shuffle ? "Shuffling " : "Starting ") + (title.empty() ? std::string("playback") : title),
                     Until::track_change);
        }
        break;
    case Kind::open:
        if (request.card.uri.rfind("spotify:artist:", 0) == 0)
        {
            auto loading = std::make_shared<screens::ArtistPage>();
            loading->card = request.card;
            artist_uri_ = request.card.uri;
            {
                std::scoped_lock guard(lock_);
                status_.artist = std::move(loading);
            }
            browser.artist(request.card.uri);
            set_busy("Loading " + request.card.title, Until::artist_loaded);
            break;
        }
        page_ = {};
        page_.card = request.card;
        page_uris_.clear();
        publish_page();
        browser.open(request.card.uri);
        set_busy("Loading " + request.card.title, Until::page_loaded);
        break;
    case Kind::describe:
    {
        std::vector<std::string> wanted;
        for (const std::string &uri : request.uris)
            if (!uri.empty() && !details_.count(uri) && described_.insert(uri).second)
                wanted.push_back(uri);
        if (!wanted.empty())
            browser.describe(wanted);
        break;
    }
    case Kind::seek:
        session_->seek(request.ms);
        break;
    case Kind::jump:
        if (request.index >= 0 && request.index < static_cast<int>(queue_uris_.size()))
        {
            const auto found = details_.find(queue_uris_[static_cast<std::size_t>(request.index)]);
            set_busy(found != details_.end() ? "Jumping to " + found->second.title : std::string("Jumping"),
                     Until::track_change);
        }
        if (!session_->jump(request.index))
            set_notice("That track is no longer in the queue");
        break;
    }
}

// The open page with every known detail filled in.
void Controller::publish_page()
{
    auto page = std::make_shared<screens::PlaylistPage>(page_);
    page->tracks.reserve(page_uris_.size());
    for (const std::string &uri : page_uris_)
    {
        const auto found = details_.find(uri);
        page->tracks.push_back(found != details_.end() ? found->second : screens::TrackRow{uri});
    }
    std::scoped_lock guard(lock_);
    status_.page = std::move(page);
}

// Now Playing's queue: a window around the playing track, rebuilt when the
// queue moves or details arrive.
void Controller::step_queue()
{
    int index = -1;
    std::vector<std::string> uris = session_->queue(index);
    if (index == queue_index_ && uris == queue_uris_ && !queue_dirty_)
        return;
    queue_index_ = index;
    queue_uris_ = std::move(uris);
    queue_dirty_ = false;
    auto queue = std::make_shared<screens::Queue>();
    if (index >= 0 && index < static_cast<int>(queue_uris_.size()))
    {
        constexpr int kBefore = 1, kAfter = 50;
        queue->first = std::max(0, index - kBefore);
        const int end = std::min(static_cast<int>(queue_uris_.size()), index + kAfter + 1);
        for (int i = queue->first; i < end; ++i)
        {
            const std::string &uri = queue_uris_[static_cast<std::size_t>(i)];
            const auto found = details_.find(uri);
            queue->tracks.push_back(found != details_.end() ? found->second : screens::TrackRow{uri});
        }
        queue->playing = index - queue->first;
    }
    std::scoped_lock guard(lock_);
    status_.queue = queue->tracks.empty() ? nullptr : std::move(queue);
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
    status_.busy.clear(); // a notice is how a busy action ends when it fails
}

void Controller::set_busy(const std::string &text, Until until)
{
    std::scoped_lock guard(lock_);
    status_.busy = text;
    busy_until_ = until;
    busy_track_ = status_.now_playing.track_id;
    busy_since_ms_ = now_ms();
}

// Ends the busy pill once its action has visibly happened (or given up).
void Controller::step_busy()
{
    constexpr std::int64_t kGiveUpMs = 25000;
    std::scoped_lock guard(lock_);
    if (status_.busy.empty())
        return;
    const NowPlaying &now = status_.now_playing;
    const bool new_track = !now.track_id.empty() && now.track_id != busy_track_;
    bool done = false;
    switch (busy_until_)
    {
    case Until::track_change:
        done = new_track;
        break;
    case Until::restart:
        done = new_track || (now_ms() - busy_since_ms_ > 1200 && now.position_ms < 2500);
        break;
    case Until::page_loaded:
        done = !page_.loading;
        break;
    case Until::artist_loaded:
        done = status_.artist && !status_.artist->loading;
        break;
    }
    if (done || now_ms() - busy_since_ms_ > kGiveUpMs)
        status_.busy.clear();
}

// A card's title from the open page or Home (for the busy pill).
std::string Controller::title_of(const std::string &uri)
{
    if (uri == page_.card.uri)
        return page_.card.title;
    const std::shared_ptr<const screens::HomePage> home = status().home;
    if (home)
        for (const screens::Shelf &shelf : home->shelves)
            for (const screens::Card &card : shelf.cards)
                if (card.uri == uri)
                    return card.title;
    return {};
}

void Controller::set_state(State next, const std::string &message)
{
    std::scoped_lock guard(lock_);
    hui::sys::log("[APP] state: %s -> %s%s%s", state_name(status_.state), state_name(next),
                  message.empty() ? "" : ": ", message.c_str());
    status_.state = next;
    status_.message = message;
    if (next != State::Ready && next != State::Reconnecting)
    {
        status_.now_playing = {};
        status_.busy.clear();
    }
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
