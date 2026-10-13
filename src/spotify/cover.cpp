// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/cover.hpp"

#include <algorithm>
#include <exception>

#include "HTTPClient.h"
#include "platform/ps5/system.hpp"
#include "third_party/stb/stb_image.h"

namespace spotify
{

CoverLoader::CoverLoader(std::size_t max_queued) : max_queued_(std::max<std::size_t>(1, max_queued))
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20); // TLS handshake + JPEG decoder
    started_ = pthread_create(&thread_, &attr, &CoverLoader::entry, this) == 0;
    pthread_attr_destroy(&attr);
    if (!started_)
        hui::sys::log("[APP] cover loader thread failed");
}

CoverLoader::~CoverLoader()
{
    {
        std::scoped_lock guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (started_)
        pthread_join(thread_, nullptr);
}

void CoverLoader::request(const std::string &url)
{
    if (url.empty())
        return;
    std::scoped_lock guard(lock_);
    if (url == last_requested_ || url == in_flight_ ||
        std::find(wanted_.begin(), wanted_.end(), url) != wanted_.end() ||
        std::find(failed_.begin(), failed_.end(), url) != failed_.end())
        return;
    last_requested_ = url;
    wanted_.push_back(url);
    if (wanted_.size() > max_queued_)
        wanted_.pop_front(); // superseded
    wake_.notify_all();
}

std::optional<Cover> CoverLoader::take()
{
    std::scoped_lock guard(lock_);
    if (done_.empty())
        return std::nullopt;
    std::optional<Cover> cover(std::move(done_.front()));
    done_.pop_front();
    return cover;
}

void CoverLoader::forget(const std::string &url)
{
    std::scoped_lock guard(lock_);
    in_flight_.clear();
    failed_.push_back(url);
}

void *CoverLoader::entry(void *self)
{
    static_cast<CoverLoader *>(self)->run();
    return nullptr;
}

void CoverLoader::run()
{
    for (;;)
    {
        std::string url;
        {
            std::unique_lock guard(lock_);
            wake_.wait(guard, [&] { return stop_ || !wanted_.empty(); });
            if (stop_)
                return;
            url = std::move(wanted_.front());
            wanted_.pop_front();
            in_flight_ = url;
        }

        std::vector<std::uint8_t> bytes;
        try
        {
            bytes = bell::HTTPClient::get(url)->bytes();
        }
        catch (const std::exception &e)
        {
            hui::sys::log("[APP] cover download failed: %s", e.what());
            forget(url);
            continue;
        }

        int width = 0, height = 0, channels = 0;
        std::uint8_t *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                     &width, &height, &channels, 4);
        if (!pixels)
        {
            hui::sys::log("[APP] cover not decodable (%zu bytes): %s", bytes.size(),
                          stbi_failure_reason());
            forget(url);
            continue;
        }
        Cover cover;
        cover.url = url;
        cover.width = width;
        cover.height = height;
        cover.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        cover.palette = screens::style::palette_from_pixels(cover.rgba.data(), width, height);

        std::scoped_lock guard(lock_);
        in_flight_.clear();
        done_.push_back(std::move(cover));
    }
}

} // namespace spotify
