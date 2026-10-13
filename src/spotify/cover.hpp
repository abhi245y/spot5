// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One worker thread per loader with a small queue: request(url) adds to it,
// and when it is full the oldest request is dropped (Now Playing uses a queue
// of one: only the latest track's cover matters; Home's cards use a few, so
// covers scrolled past are not fetched). take() hands over finished covers.
// Downloads use bell's HTTPS client, so certificates are verified like every
// other connection; decoding (stb_image) happens here too, so the drawing
// thread only uploads pixels.

#pragma once

#include <pthread.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "screens/style.hpp"

namespace spotify
{

struct Cover
{
    std::string url;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // top row first
    screens::style::AlbumPalette palette;
};

class CoverLoader
{
  public:
    explicit CoverLoader(std::size_t max_queued = 1);
    ~CoverLoader();
    CoverLoader(const CoverLoader &) = delete;
    CoverLoader &operator=(const CoverLoader &) = delete;

    // Asks for this cover (no-op if it is already queued or was the last asked).
    void request(const std::string &url);
    // A finished cover, once each; empty while nothing new is ready.
    std::optional<Cover> take();

  private:
    static void *entry(void *self);
    void run();
    void forget(const std::string &url); // a download that failed

    pthread_t thread_{};
    bool started_ = false;
    std::mutex lock_;
    std::condition_variable wake_;
    std::size_t max_queued_;
    std::deque<std::string> wanted_;
    std::string last_requested_;
    std::string in_flight_;
    std::vector<std::string> failed_; // not asked again (ponytail: grows with failures only)
    std::deque<Cover> done_;
    bool stop_ = false;
};

} // namespace spotify
