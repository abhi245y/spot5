// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One worker thread: request(url) replaces whatever was asked before (only
// the latest track's cover matters), take() hands over a finished cover.
// Downloads use bell's HTTPS client, so certificates are verified like every
// other connection; decoding (stb_image) happens here too, so the drawing
// thread only uploads pixels.

#pragma once

#include <pthread.h>

#include <condition_variable>
#include <cstdint>
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
    CoverLoader();
    ~CoverLoader();
    CoverLoader(const CoverLoader &) = delete;
    CoverLoader &operator=(const CoverLoader &) = delete;

    // Asks for this cover (no-op if it is already the latest request).
    void request(const std::string &url);
    // A finished cover, once; empty while nothing new is ready.
    std::optional<Cover> take();

  private:
    static void *entry(void *self);
    void run();

    pthread_t thread_{};
    bool started_ = false;
    std::mutex lock_;
    std::condition_variable wake_;
    std::string wanted_;
    std::string last_requested_;
    std::optional<Cover> done_;
    bool stop_ = false;
};

} // namespace spotify
