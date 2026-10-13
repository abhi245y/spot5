// ps5-spotify - Fetches Home and resolves what to play, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Web calls take seconds (one TLS connection each), so they never run on the
// controller thread, which must keep answering the phone. Requests go in,
// results come out under a lock; the controller hands a resolved track list
// to cspot itself (cspot only ever sees the controller thread for playback).
//
// Calls are kept few on purpose: bursts of requests got a research account
// temporarily blocked (SPOTIFY_MOBILE_RESEARCH.md §11.4). Home is cached in
// home.json, and metadata is only fetched for URIs not seen in the last day.

#pragma once

#include <pthread.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "screens/model.hpp"
#include "spotify/home.hpp"

namespace cspot
{
struct Context;
class AccessKeyFetcher;
} // namespace cspot

namespace spotify
{

// A track list ready for Session::play, or why there is none.
struct Resolved
{
    std::string context_uri;
    std::vector<std::string> tracks; // all of them, in play order (shuffled if asked)
    bool shuffle = false;
    std::string error; // for the user; set when tracks is empty
};

class Browser
{
  public:
    // Loads the cached Home (if any) at once; nothing is fetched until asked.
    // `timezone` (IANA, e.g. "Asia/Kolkata") shapes Home's time-of-day shelves.
    Browser(std::shared_ptr<cspot::Context> ctx, std::string timezone);
    // Waits for a running request to finish.
    ~Browser();
    Browser(const Browser &) = delete;
    Browser &operator=(const Browser &) = delete;

    // Fetch Home again (the caller decides how often). `forget`: drop the
    // metadata kept in memory first, so every title and cover is fetched anew.
    void refresh(bool forget = false);
    // Resolve a Home card into tracks (to play them).
    void resolve(const screens::PlayRequest &request);
    // Resolve a card's tracks for its page (take_opened()).
    void open(const std::string &uri);
    // Names, artists, covers and durations for these tracks (take_described()).
    void describe(const std::vector<std::string> &uris);

    // The newest page, once (after construction from the cache, and after
    // each refresh); null when nothing new.
    std::shared_ptr<const screens::HomePage> take_page();
    std::optional<Resolved> take_resolved();
    std::optional<Resolved> take_opened();
    std::vector<screens::TrackRow> take_described();
    bool busy();
    // When Home was last fetched successfully (ms since epoch), 0 if never.
    std::int64_t fetched_ms();

  private:
    struct Job
    {
        enum class Kind { refresh, resolve, open, describe } kind = Kind::refresh;
        bool forget = false;                // refresh
        screens::PlayRequest play;          // resolve
        std::string uri;                    // open
        std::vector<std::string> uris;      // describe
    };

    static void *entry(void *self);
    void run();
    void fetch_home();
    Resolved fetch_tracks(const screens::PlayRequest &request);
    void fetch_details(const std::vector<std::string> &uris);

    std::shared_ptr<cspot::Context> ctx_;
    std::string timezone_;
    std::unique_ptr<cspot::AccessKeyFetcher> keys_; // worker thread only

    // Worker thread only.
    std::vector<home::Section> sections_;
    home::Entities entities_;

    pthread_t thread_{};
    bool started_ = false;
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool working_ = false;
    bool stop_ = false;
    std::shared_ptr<const screens::HomePage> page_;
    std::optional<Resolved> resolved_;
    std::optional<Resolved> opened_;
    std::vector<screens::TrackRow> described_;
    std::int64_t fetched_ms_ = 0;
};

} // namespace spotify
