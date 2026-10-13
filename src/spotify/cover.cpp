// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/cover.hpp"

#include <exception>

#include "HTTPClient.h"
#include "platform/ps5/system.hpp"
#include "third_party/stb/stb_image.h"

namespace spotify
{

CoverLoader::CoverLoader()
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
    if (url == last_requested_)
        return;
    last_requested_ = url;
    wanted_ = url;
    wake_.notify_all();
}

std::optional<Cover> CoverLoader::take()
{
    std::scoped_lock guard(lock_);
    std::optional<Cover> cover;
    cover.swap(done_);
    return cover;
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
            url.swap(wanted_);
        }

        std::vector<std::uint8_t> bytes;
        try
        {
            bytes = bell::HTTPClient::get(url)->bytes();
        }
        catch (const std::exception &e)
        {
            hui::sys::log("[APP] cover download failed: %s", e.what());
            continue;
        }

        int width = 0, height = 0, channels = 0;
        std::uint8_t *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                     &width, &height, &channels, 4);
        if (!pixels)
        {
            hui::sys::log("[APP] cover not decodable (%zu bytes): %s", bytes.size(),
                          stbi_failure_reason());
            continue;
        }
        Cover cover;
        cover.url = url;
        cover.width = width;
        cover.height = height;
        cover.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        cover.palette = screens::style::palette_from_pixels(cover.rgba.data(), width, height);
        hui::sys::log("[APP] cover %dx%d (%zu KB)", width, height, bytes.size() / 1024);

        std::scoped_lock guard(lock_);
        // A newer request supersedes this one; its cover will follow.
        if (wanted_.empty())
            done_ = std::move(cover);
    }
}

} // namespace spotify
