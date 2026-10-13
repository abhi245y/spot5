// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/cover.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>

#include "HTTPClient.h"
#include "app/config.hpp"
#include "platform/ps5/system.hpp"
#include "third_party/stb/stb_image.h"
#include "webp/decode.h"

namespace spotify
{

namespace
{

constexpr std::int64_t kRetryAfterMs = 30000; // a failed download, asked again

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// File name for a URL in the cache (FNV-1a, 64 bit).
std::string cache_name(const std::string &url)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (char c : url)
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
    return name;
}

bool is_webp(const std::vector<std::uint8_t> &bytes)
{
    return bytes.size() > 12 && std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") &&
           std::equal(bytes.begin() + 8, bytes.begin() + 12, "WEBP");
}

// Halves an RGBA image (2x2 box filter) until it fits kMaxSide.
void shrink(std::vector<std::uint8_t> &rgba, int &width, int &height, int max_side)
{
    while (width > max_side || height > max_side)
    {
        const int w = width / 2, h = height / 2;
        std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const auto at = [&](int sx, int sy) {
                        return rgba[(static_cast<std::size_t>(sy) * width + sx) * 4 + c];
                    };
                    out[(static_cast<std::size_t>(y) * w + x) * 4 + c] = static_cast<std::uint8_t>(
                        (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) +
                         at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
        rgba.swap(out);
        width = w;
        height = h;
    }
}

} // namespace

CoverLoader::CoverLoader(std::size_t max_queued, int workers, std::string cache_dir)
    : max_queued_(std::max<std::size_t>(1, max_queued)), cache_dir_(std::move(cache_dir))
{
    if (!cache_dir_.empty())
        mkdir(cache_dir_.c_str(), 0755); // exists already after the first run
    for (int i = 0; i < std::max(1, workers); ++i)
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 1 << 20); // TLS handshake + image decoders
        pthread_t thread;
        if (pthread_create(&thread, &attr, &CoverLoader::entry, this) == 0)
            threads_.push_back(thread);
        pthread_attr_destroy(&attr);
    }
    if (threads_.empty())
        hui::sys::log("[APP] cover loader thread failed");
}

CoverLoader::~CoverLoader()
{
    {
        std::scoped_lock guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    for (pthread_t thread : threads_)
        pthread_join(thread, nullptr);
}

void CoverLoader::request(const std::string &url)
{
    if (url.empty())
        return;
    std::scoped_lock guard(lock_);
    const auto known = [&](const auto &list) {
        return std::find(list.begin(), list.end(), url) != list.end();
    };
    if (url == last_requested_ || known(in_flight_) || known(wanted_) || known(undecodable_))
        return;
    if (const auto retry = retry_at_ms_.find(url); retry != retry_at_ms_.end() && now_ms() < retry->second)
        return;
    last_requested_ = url;
    wanted_.push_back(url);
    if (wanted_.size() > max_queued_)
        wanted_.pop_front(); // superseded
    wake_.notify_one();
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

void *CoverLoader::entry(void *self)
{
    static_cast<CoverLoader *>(self)->run();
    return nullptr;
}

void CoverLoader::finished(const std::string &url)
{
    in_flight_.erase(std::remove(in_flight_.begin(), in_flight_.end(), url), in_flight_.end());
}

// The cover's bytes: from the cache if there, else downloaded (and cached).
// Empty if the download failed.
std::vector<std::uint8_t> CoverLoader::fetch(const std::string &url)
{
    const std::string path = cache_dir_.empty() ? std::string() : cache_dir_ + "/" + cache_name(url);
    if (!path.empty())
    {
        const std::string cached = app::read_file(path);
        if (!cached.empty())
            return {cached.begin(), cached.end()};
    }
    std::vector<std::uint8_t> bytes;
    try
    {
        auto response = bell::HTTPClient::get(url);
        bytes = response->bytes();
        if (response->statusCode() != 200)
        {
            hui::sys::log("[APP] cover HTTP %d: %s", response->statusCode(), url.c_str());
            return {};
        }
    }
    catch (const std::exception &e)
    {
        hui::sys::log("[APP] cover download failed: %s", e.what());
        return {};
    }
    if (!path.empty() && !bytes.empty())
    {
        // Temp + rename: a crash never leaves half a file that would then
        // fail to decode on every launch.
        const std::string temporary = path + ".tmp";
        if (app::write_file(temporary, std::string(bytes.begin(), bytes.end())))
            std::rename(temporary.c_str(), path.c_str());
    }
    return bytes;
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
            in_flight_.push_back(url);
        }

        const std::vector<std::uint8_t> bytes = fetch(url);
        if (bytes.empty())
        {
            std::scoped_lock guard(lock_);
            finished(url);
            retry_at_ms_[url] = now_ms() + kRetryAfterMs;
            continue;
        }

        Cover cover;
        cover.url = url;
        if (is_webp(bytes))
        {
            if (std::uint8_t *pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &cover.width, &cover.height))
            {
                cover.rgba.assign(pixels, pixels + static_cast<std::size_t>(cover.width) * cover.height * 4);
                WebPFree(pixels);
            }
        }
        else
        {
            int channels = 0;
            if (std::uint8_t *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                             &cover.width, &cover.height, &channels, 4))
            {
                cover.rgba.assign(pixels, pixels + static_cast<std::size_t>(cover.width) * cover.height * 4);
                stbi_image_free(pixels);
            }
        }
        if (cover.rgba.empty())
        {
            hui::sys::log("[APP] cover not decodable (%zu bytes): %s", bytes.size(), url.c_str());
            std::scoped_lock guard(lock_);
            finished(url);
            undecodable_.push_back(url);
            if (!cache_dir_.empty())
                std::remove((cache_dir_ + "/" + cache_name(url)).c_str());
            continue;
        }
        shrink(cover.rgba, cover.width, cover.height, kMaxSide);
        cover.palette = screens::style::palette_from_pixels(cover.rgba.data(), cover.width, cover.height);

        std::scoped_lock guard(lock_);
        finished(url);
        done_.push_back(std::move(cover));
    }
}

} // namespace spotify
