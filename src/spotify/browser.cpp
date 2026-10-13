// ps5-spotify - Fetches Home and resolves what to play, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "spotify/browser.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <random>

#include "AccessKeyFetcher.h"
#include "CSpotContext.h"
#include "HTTPClient.h"
#include "app/config.hpp"
#include "nlohmann/json.hpp"
#include "platform/ps5/system.hpp"

namespace spotify
{

namespace
{

constexpr const char *kSpclient = "https://spclient.wg.spotify.com/";
constexpr std::size_t kTraitsBatch = 20;              // URIs per metadata request
constexpr std::int64_t kEntityMaxAgeMs = 24 * 3600 * 1000; // refetch metadata after a day

std::int64_t now_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

struct Reply
{
    int status = 0;
    std::string body;
};

// One authorised spclient call. Status 0 means it never got an answer.
Reply call(cspot::AccessKeyFetcher &keys, const std::string &path, const char *accept,
           const std::string *post_body = nullptr)
{
    Reply reply;
    try
    {
        const std::string token = keys.getAccessKey();
        if (token.empty())
            return reply;
        bell::HTTPClient::Headers headers = {{"Authorization", "Bearer " + token}};
        if (accept)
            headers.push_back({"Accept", accept});
        std::unique_ptr<bell::HTTPClient::Response> response;
        if (post_body)
        {
            headers.push_back({"Content-Type", "application/protobuf"});
            response = bell::HTTPClient::post(kSpclient + path, headers,
                                              std::vector<std::uint8_t>(post_body->begin(), post_body->end()));
        }
        else
        {
            response = bell::HTTPClient::get(kSpclient + path, headers);
        }
        reply.body = std::string(response->body());
        reply.status = response->statusCode();
    }
    catch (const std::exception &e)
    {
        hui::sys::log("[HOME] %s: %s", path.substr(0, path.find('?')).c_str(), e.what());
    }
    if (reply.status != 200)
        hui::sys::log("[HOME] %s -> HTTP %d", path.substr(0, path.find('?')).c_str(), reply.status);
    return reply;
}

} // namespace

Browser::Browser(std::shared_ptr<cspot::Context> ctx, std::string timezone)
    : ctx_(std::move(ctx)), timezone_(std::move(timezone))
{
    keys_ = std::make_unique<cspot::AccessKeyFetcher>(ctx_);
    if (home::from_json(app::read_file(app::paths::kHome), sections_, entities_, fetched_ms_))
    {
        page_ = std::make_shared<screens::HomePage>(home::build_page(sections_, entities_));
        hui::sys::log("[HOME] cached page: %zu shelves", page_->shelves.size());
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20); // TLS handshake + JSON
    started_ = pthread_create(&thread_, &attr, &Browser::entry, this) == 0;
    pthread_attr_destroy(&attr);
}

Browser::~Browser()
{
    {
        std::scoped_lock guard(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (started_)
        pthread_join(thread_, nullptr);
}

void Browser::refresh()
{
    Job job;
    job.kind = Job::Kind::refresh;
    std::scoped_lock guard(lock_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

void Browser::resolve(const screens::PlayRequest &request)
{
    Job job;
    job.kind = Job::Kind::resolve;
    job.play = request;
    std::scoped_lock guard(lock_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

void Browser::open(const std::string &uri)
{
    Job job;
    job.kind = Job::Kind::open;
    job.uri = uri;
    std::scoped_lock guard(lock_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

void Browser::describe(const std::vector<std::string> &uris)
{
    Job job;
    job.kind = Job::Kind::describe;
    job.uris = uris;
    std::scoped_lock guard(lock_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

std::optional<Resolved> Browser::take_opened()
{
    std::scoped_lock guard(lock_);
    return std::exchange(opened_, std::nullopt);
}

std::vector<screens::TrackRow> Browser::take_described()
{
    std::scoped_lock guard(lock_);
    return std::exchange(described_, {});
}

std::shared_ptr<const screens::HomePage> Browser::take_page()
{
    std::scoped_lock guard(lock_);
    return std::exchange(page_, nullptr);
}

std::optional<Resolved> Browser::take_resolved()
{
    std::scoped_lock guard(lock_);
    return std::exchange(resolved_, std::nullopt);
}

bool Browser::busy()
{
    std::scoped_lock guard(lock_);
    return working_ || !jobs_.empty();
}

std::int64_t Browser::fetched_ms()
{
    std::scoped_lock guard(lock_);
    return fetched_ms_;
}

void *Browser::entry(void *self)
{
    static_cast<Browser *>(self)->run();
    return nullptr;
}

void Browser::run()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock guard(lock_);
            wake_.wait(guard, [this] { return stop_ || !jobs_.empty(); });
            if (stop_)
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            working_ = true;
        }
        // Replies are network input: a surprise in one must not end the app.
        try
        {
            switch (job.kind)
            {
            case Job::Kind::refresh:
                fetch_home();
                break;
            case Job::Kind::resolve:
            {
                Resolved resolved = fetch_tracks(job.play);
                std::scoped_lock guard(lock_);
                resolved_ = std::move(resolved);
                break;
            }
            case Job::Kind::open:
            {
                Resolved opened = fetch_tracks({job.uri, false});
                opened.context_uri = job.uri; // the card's own URI (Liked Songs)
                std::scoped_lock guard(lock_);
                opened_ = std::move(opened);
                break;
            }
            case Job::Kind::describe:
                fetch_details(job.uris);
                break;
            }
        }
        catch (const std::exception &e)
        {
            hui::sys::log("[HOME] job %d failed: %s", static_cast<int>(job.kind), e.what());
            Resolved failed;
            failed.context_uri = job.kind == Job::Kind::open ? job.uri : job.play.uri;
            failed.error = "Couldn't load it. Try again in a moment.";
            std::scoped_lock guard(lock_);
            if (job.kind == Job::Kind::resolve)
                resolved_ = std::move(failed);
            else if (job.kind == Job::Kind::open)
                opened_ = std::move(failed);
        }
        std::scoped_lock guard(lock_);
        working_ = false;
    }
}

void Browser::fetch_home()
{
    const Reply reply = call(*keys_,
                             "casita/v1/home/default?timezone=" + timezone_ +
                                 "&locale=en&mobile-only-dsa-enabled=false",
                             "application/protobuf");
    if (reply.status != 200)
        return; // keep showing what we have; never cache an error
    std::vector<home::Section> sections = home::parse_home(reply.body);
    if (sections.empty())
    {
        hui::sys::log("[HOME] reply had no sections (%zu bytes)", reply.body.size());
        return;
    }

    // Metadata only for URIs we have not seen in the last day.
    const std::int64_t now = now_ms();
    std::vector<std::string> wanted;
    for (const home::Section &section : sections)
        for (const std::string &uri : section.uris)
        {
            const auto found = entities_.find(uri);
            if ((found == entities_.end() || now - found->second.fetched_ms > kEntityMaxAgeMs) &&
                std::find(wanted.begin(), wanted.end(), uri) == wanted.end())
                wanted.push_back(uri);
        }
    for (std::size_t first = 0; first < wanted.size(); first += kTraitsBatch)
    {
        {
            std::scoped_lock guard(lock_);
            if (stop_)
                return; // the session is closing
        }
        if (first > 0)
            hui::sys::sleep_us(300000); // spaced out, like a person scrolling
        const std::vector<std::string> batch(
            wanted.begin() + static_cast<std::ptrdiff_t>(first),
            wanted.begin() + static_cast<std::ptrdiff_t>(std::min(wanted.size(), first + kTraitsBatch)));
        const std::string request = home::metadata_request(batch, ctx_->config.countryCode);
        const Reply traits = call(*keys_, "extended-metadata/v0/extended-metadata",
                                  "application/protobuf", &request);
        if (traits.status != 200 || !home::parse_traits(traits.body, entities_, now))
            break; // show what resolved; the next refresh asks again
    }

    sections_ = std::move(sections);
    auto page = std::make_shared<screens::HomePage>(home::build_page(sections_, entities_));
    hui::sys::log("[HOME] fetched: %zu shelves, metadata for %zu new URIs", page->shelves.size(),
                  wanted.size());
    if (!app::write_file(app::paths::kHome, home::to_json(sections_, entities_, now)))
        hui::sys::log("[HOME] note: could not save %s", app::paths::kHome.c_str());
    std::scoped_lock guard(lock_);
    page_ = std::move(page);
    fetched_ms_ = now;
}

void Browser::fetch_details(const std::vector<std::string> &uris)
{
    for (std::size_t first = 0; first < uris.size(); first += kTraitsBatch)
    {
        {
            std::scoped_lock guard(lock_);
            if (stop_)
                return;
        }
        if (first > 0)
            hui::sys::sleep_us(300000);
        const std::vector<std::string> batch(
            uris.begin() + static_cast<std::ptrdiff_t>(first),
            uris.begin() + static_cast<std::ptrdiff_t>(std::min(uris.size(), first + kTraitsBatch)));
        const std::string request = home::metadata_request(batch, ctx_->config.countryCode, {home::kTrackV4});
        const Reply reply = call(*keys_, "extended-metadata/v0/extended-metadata", "application/protobuf", &request);
        std::map<std::string, screens::TrackRow> rows;
        if (reply.status != 200 || !home::parse_tracks(reply.body, rows))
            return;
        std::scoped_lock guard(lock_);
        for (auto &[uri, row] : rows)
            described_.push_back(std::move(row));
    }
}

Resolved Browser::fetch_tracks(const screens::PlayRequest &request)
{
    Resolved resolved;
    resolved.context_uri = home::playable_uri(request.uri, ctx_->config.username);
    resolved.shuffle = request.shuffle;
    // Artists and similar resolve to their top tracks / radio on the server.
    const Reply reply = call(*keys_, "context-resolve/v1/" + resolved.context_uri, nullptr);
    const auto json = nlohmann::json::parse(reply.body, nullptr, false);
    if (reply.status == 200 && json.is_object() && json.contains("pages"))
    {
        for (const auto &page : json["pages"])
            for (const auto &track : page.value("tracks", nlohmann::json::array()))
            {
                const std::string uri = track.value("uri", "");
                if (uri.rfind("spotify:track:", 0) == 0 || uri.rfind("spotify:episode:", 0) == 0)
                    resolved.tracks.push_back(uri);
            }
    }
    if (resolved.shuffle)
    {
        std::minstd_rand random(static_cast<std::uint32_t>(now_ms()));
        std::shuffle(resolved.tracks.begin(), resolved.tracks.end(), random);
    }
    if (resolved.tracks.empty())
        resolved.error = reply.status == 200 ? "This can't be played here yet"
                                             : "Couldn't load it. Try again in a moment.";
    hui::sys::log("[HOME] resolved %s: %zu tracks%s", resolved.context_uri.c_str(),
                  resolved.tracks.size(), resolved.shuffle ? " (shuffled)" : "");
    return resolved;
}

} // namespace spotify
