// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Worker threads with a small queue: request(url) adds to it, and when it is
// full the oldest request is dropped (Now Playing uses a queue of one: only
// the latest track's cover matters; Home uses a long one so the launch screen
// can fetch every cover). take() hands over finished covers.
//
// Downloads use bell's HTTPS client, so certificates are verified like every
// other connection. With a cache directory, the downloaded file is kept there
// and read back next time (no network). JPEG/PNG decode with stb_image, WebP
// with libwebp; covers wider than kMaxSide are halved, so the drawing thread
// only uploads pixels.

#pragma once

#include <pthread.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
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
    // `cache_dir` empty: no disk cache.
    explicit CoverLoader(std::size_t max_queued = 1, int workers = 1, std::string cache_dir = {});
    ~CoverLoader();
    CoverLoader(const CoverLoader &) = delete;
    CoverLoader &operator=(const CoverLoader &) = delete;

    // Asks for this cover (no-op if it is already queued, being fetched, was
    // the last asked, cannot be decoded, or failed to download moments ago).
    void request(const std::string &url);
    // A finished cover, once each; empty while nothing new is ready.
    std::optional<Cover> take();

    // Larger than this is halved before upload. 2048 keeps the original
    // album art (~2000 px) whole, for big canvases drawn at 4K.
    static constexpr int kMaxSide = 2048;

    // True once this cover will not come (cannot be decoded, or its download
    // failed moments ago): a launch screen waiting for covers stops counting it.
    bool given_up(const std::string &url);

  private:
    static void *entry(void *self);
    void run();
    std::vector<std::uint8_t> fetch(const std::string &url);
    void finished(const std::string &url);

    std::size_t max_queued_;
    std::string cache_dir_;
    std::vector<pthread_t> threads_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<std::string> wanted_;
    std::string last_requested_;
    std::vector<std::string> in_flight_;
    std::vector<std::string> undecodable_; // never asked again
    std::map<std::string, std::int64_t> retry_at_ms_; // failed downloads wait a while
    std::deque<Cover> done_;
    bool stop_ = false;
};

} // namespace spotify
