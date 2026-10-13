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
// The app heap is 128 MB and stb_image needs ~7 bytes a pixel while decoding:
// original-size covers above this fall back to the 640 px one, and only one
// big image decodes at a time (all loaders share the lock).
constexpr int kMaxDecodeSide = 2048;
constexpr int kBigPixels = 1000 * 1000;
std::mutex g_big_decode;

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

// Copies decoded RGBA into `out`, halving (2x2 box filter) while it is
// larger than max_side; reads the decoder's buffer directly (no full copy).
void take_pixels(const std::uint8_t *pixels, int width, int height, int max_side, Cover &out)
{
    int step = 1;
    while (width / step > max_side || height / step > max_side)
        step *= 2;
    out.width = width / step;
    out.height = height / step;
    out.rgba.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    const int area = step * step;
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x)
            for (int c = 0; c < 4; ++c)
            {
                int sum = 0;
                for (int dy = 0; dy < step; ++dy)
                    for (int dx = 0; dx < step; ++dx)
                        sum += pixels[((static_cast<std::size_t>(y) * step + dy) * width + x * step + dx) * 4 + c];
                out.rgba[(static_cast<std::size_t>(y) * out.width + x) * 4 + c] =
                    static_cast<std::uint8_t>((sum + area / 2) / area);
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

bool CoverLoader::given_up(const std::string &url)
{
    std::scoped_lock guard(lock_);
    if (std::find(undecodable_.begin(), undecodable_.end(), url) != undecodable_.end())
        return true;
    const auto retry = retry_at_ms_.find(url);
    return retry != retry_at_ms_.end() && now_ms() < retry->second;
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
        // fail to decode on every launch. The temp name is this thread's own:
        // two loaders may fetch the same cover at once.
        const std::string temporary =
            path + ".tmp" + std::to_string(reinterpret_cast<std::uintptr_t>(pthread_self()));
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

        std::vector<std::uint8_t> bytes = fetch(url);
        // An original-size cover too big for the heap: the 640 px one instead
        // (cached under the asked-for name, so this happens once).
        int info_w = 0, info_h = 0, info_n = 0;
        bool no_smaller = false; // too big, and no smaller size exists
        const bool webp = is_webp(bytes);
        if (!bytes.empty() && !webp &&
            stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &info_w, &info_h, &info_n) &&
            std::max(info_w, info_h) > kMaxDecodeSide)
        {
            const std::size_t at = url.find("ab67616d000082c1");
            if (at != std::string::npos)
            {
                std::string smaller = url;
                smaller.replace(at, 16, "ab67616d0000b273");
                bytes = fetch(smaller);
                if (!bytes.empty() && !cache_dir_.empty())
                    app::write_file(cache_dir_ + "/" + cache_name(url), std::string(bytes.begin(), bytes.end()));
            }
            else
            {
                bytes.clear();
                no_smaller = true; // treated as undecodable below
            }
        }
        if (bytes.empty() && !no_smaller)
        {
            std::scoped_lock guard(lock_);
            finished(url);
            retry_at_ms_[url] = now_ms() + kRetryAfterMs;
            continue;
        }

        Cover cover;
        cover.url = url;
        {
            const bool big = static_cast<long long>(info_w) * info_h > kBigPixels;
            std::unique_lock<std::mutex> one_at_a_time(g_big_decode, std::defer_lock);
            if (big || webp)
                one_at_a_time.lock();
            int width = 0, height = 0, channels = 0;
            if (webp)
            {
                if (std::uint8_t *pixels = WebPDecodeRGBA(bytes.data(), bytes.size(), &width, &height))
                {
                    take_pixels(pixels, width, height, kMaxSide, cover);
                    WebPFree(pixels);
                }
            }
            else if (!bytes.empty())
            {
                if (std::uint8_t *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                                 &width, &height, &channels, 4))
                {
                    take_pixels(pixels, width, height, kMaxSide, cover);
                    stbi_image_free(pixels);
                }
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
        cover.palette = screens::style::palette_from_pixels(cover.rgba.data(), cover.width, cover.height);

        std::scoped_lock guard(lock_);
        finished(url);
        done_.push_back(std::move(cover));
    }
}

} // namespace spotify
