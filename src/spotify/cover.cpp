// ps5-spotify - Downloads and decodes album covers off the main threads.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/cover.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "HTTPClient.h"
#include "app/config.hpp"
#include "platform/ps5/system.hpp"
#include "third_party/stb/stb_image.h"
#include "webp/decode.h"

extern "C" std::size_t hui_heap_capacity(void); // runtime/app_heap.c
extern "C" void hui_heap_stats(std::size_t *live, std::size_t *peak, std::size_t *blocks, std::size_t *failures);

namespace spotify
{

namespace
{

constexpr std::int64_t kRetryAfterMs = 30000; // a failed download, asked again
constexpr std::size_t kMaxWaiting = 16;       // decoded covers not yet uploaded
// What decoding costs, worked out before anything is allocated (a failed
// allocation ends the app on this console): stb_image keeps the output
// (4 bytes a pixel) and the colour planes (3), plus 16-bit coefficients for
// progressive JPEGs (6); we add the halved copy. An original-size cover that
// would need more than a third of the heap falls back to the 640 px one
// (the app itself peaked at ~60 MB of 256: a third still leaves ~100 MB).
// Only one big image decodes at a time (all loaders share the lock).
bool is_progressive_jpeg(const std::vector<std::uint8_t> &bytes)
{
    // Walk the JPEG markers up to the frame header: SOF2 means progressive.
    std::size_t i = 2;
    while (i + 4 <= bytes.size() && bytes[i] == 0xFF)
    {
        const std::uint8_t marker = bytes[i + 1];
        if (marker == 0xC2)
            return true;
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xDA)
            return false;
        i += 2 + ((static_cast<std::size_t>(bytes[i + 2]) << 8) | bytes[i + 3]);
    }
    return false;
}

bool fits_decode_budget(long long width, long long height, bool progressive)
{
    constexpr long long kMargin = 48ll << 20; // for everything else running meanwhile
    const long long pixels = width * height;
    const long long cost = pixels * (4 + 3 + (progressive ? 6 : 0)) + pixels * 4; // + our copy
    std::size_t live = 0;
    hui_heap_stats(&live, nullptr, nullptr, nullptr);
    const long long capacity = static_cast<long long>(hui_heap_capacity());
    const bool fits = cost <= capacity / 3 && cost <= capacity - static_cast<long long>(live) - kMargin;
    if (!fits)
        hui::sys::log("[APP] cover %lldx%lld needs %lld MB, heap %zu of %lld MB in use: using the smaller one",
                      width, height, cost >> 20, live >> 20, capacity >> 20);
    return fits;
}
constexpr int kBigPixels = 1000 * 1000;
std::mutex g_big_decode;
// Every saved cover's name goes into index.txt (listing a directory under
// /app0 returns nothing on the console, so clearing needs its own list).
std::mutex g_index;
constexpr const char *kIndex = "index.txt";

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
// Returns false (and leaves `out` empty) when there is no memory for it.
bool take_pixels(const std::uint8_t *pixels, int width, int height, int max_side, Cover &out)
{
    int step = 1;
    while (width / step > max_side || height / step > max_side)
        step *= 2;
    const std::size_t size = static_cast<std::size_t>(width / step) * (height / step) * 4;
    // A failed C++ allocation ends the app on this console; malloc can say no.
    void *room = std::malloc(size);
    if (!room)
    {
        hui::sys::log("[APP] cover: no memory for %zu MB", size >> 20);
        return false;
    }
    std::free(room);
    out.width = width / step;
    out.height = height / step;
    out.rgba.resize(size);
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
    return true;
}

} // namespace

int clear_cover_cache(const std::string &dir)
{
    std::scoped_lock guard(g_index);
    int removed = 0;
    const std::string index = dir + "/" + kIndex;
    if (FILE *list = std::fopen(index.c_str(), "r"))
    {
        char name[64];
        while (std::fscanf(list, "%63s", name) == 1)
            removed += std::remove((dir + "/" + name).c_str()) == 0;
        std::fclose(list);
    }
    // Covers saved before there was an index, where listing works.
    if (DIR *folder = opendir(dir.c_str()))
    {
        while (const dirent *entry = readdir(folder))
            if (entry->d_name[0] != '.' && std::string(entry->d_name) != kIndex)
                removed += std::remove((dir + "/" + entry->d_name).c_str()) == 0;
        closedir(folder);
    }
    std::remove(index.c_str());
    hui::sys::log("[APP] cache: %d covers deleted", removed);
    return removed;
}

CoverLoader::CoverLoader(std::size_t max_queued, int workers, std::string cache_dir)
    : max_queued_(std::max<std::size_t>(1, max_queued)), cache_dir_(std::move(cache_dir))
{
    if (!cache_dir_.empty())
        mkdir(cache_dir_.c_str(), 0777); // exists already after the first run; 0777 so FTP can clear it
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
    // Also skip covers decoded and waiting for upload: asked again every
    // frame until uploaded, they were decoded again and again (the cause of
    // a 200 MB heap spike and of the out-of-memory crash).
    const bool waiting = std::any_of(done_.begin(), done_.end(), [&](const Cover &c) { return c.url == url; });
    if (url == last_requested_ || waiting || known(in_flight_) || known(wanted_) || known(undecodable_))
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
    wake_.notify_all(); // room for another decoded cover
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
    save(url, bytes);
    return bytes;
}

// Keeps the bytes in the cache under the URL's name, and lists it in the
// index (the only way Clear cache can find it again).
void CoverLoader::save(const std::string &url, const std::vector<std::uint8_t> &bytes)
{
    if (cache_dir_.empty() || bytes.empty())
        return;
    // Temp + rename: a crash never leaves half a file that would then fail to
    // decode on every launch. The temp name is this thread's own: two
    // loaders may fetch the same cover at once.
    const std::string path = cache_dir_ + "/" + cache_name(url);
    const std::string temporary = path + ".tmp" + std::to_string(reinterpret_cast<std::uintptr_t>(pthread_self()));
    if (!app::write_file(temporary, std::string(bytes.begin(), bytes.end())) ||
        std::rename(temporary.c_str(), path.c_str()) != 0)
        return;
    std::scoped_lock guard(g_index);
    if (FILE *index = std::fopen((cache_dir_ + "/" + kIndex).c_str(), "a"))
    {
        std::fprintf(index, "%s\n", cache_name(url).c_str());
        std::fclose(index);
    }
}

void CoverLoader::run()
{
    for (;;)
    {
        std::string url;
        {
            std::unique_lock guard(lock_);
            // Decoded covers wait here until the drawing thread uploads them
            // (a few a frame): at most kMaxWaiting, or memory piles up.
            wake_.wait(guard, [&] { return stop_ || (!wanted_.empty() && done_.size() < kMaxWaiting); });
            if (stop_)
                return;
            url = std::move(wanted_.front());
            wanted_.pop_front();
            in_flight_.push_back(url);
        }

        std::vector<std::uint8_t> bytes = fetch(url);
        // A big image (over a megapixel) that does not fit in memory now: an
        // original-size album cover becomes the 640 px one (cached under the
        // asked-for name, so this happens once); anything else waits.
        // Ordinary covers (a few MB to decode) are never held back.
        int info_w = 0, info_h = 0, info_n = 0;
        const bool webp = is_webp(bytes);
        bool wait_for_memory = false;
        if (!bytes.empty() && !webp &&
            stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &info_w, &info_h, &info_n) &&
            static_cast<long long>(info_w) * info_h > kBigPixels &&
            !fits_decode_budget(info_w, info_h, is_progressive_jpeg(bytes)))
        {
            const std::size_t at = url.find("ab67616d000082c1");
            if (at != std::string::npos)
            {
                std::string smaller = url;
                smaller.replace(at, 16, "ab67616d0000b273");
                bytes = fetch(smaller);
                save(url, bytes);
            }
            else
            {
                wait_for_memory = true;
            }
        }
        if (bytes.empty() || wait_for_memory)
        {
            std::scoped_lock guard(lock_);
            finished(url);
            retry_at_ms_[url] = now_ms() + kRetryAfterMs;
            continue;
        }

        Cover cover;
        cover.url = url;
        bool short_of_memory = false;
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
                    short_of_memory = !take_pixels(pixels, width, height, kMaxSide, cover);
                    WebPFree(pixels);
                }
            }
            else if (!bytes.empty())
            {
                if (std::uint8_t *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                                 &width, &height, &channels, 4))
                {
                    short_of_memory = !take_pixels(pixels, width, height, kMaxSide, cover);
                    stbi_image_free(pixels);
                }
            }
        }
        if (short_of_memory)
        {
            std::scoped_lock guard(lock_);
            finished(url);
            retry_at_ms_[url] = now_ms() + kRetryAfterMs; // later, when memory is back
            continue;
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
