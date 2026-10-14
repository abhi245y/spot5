// spot5 - Fetches Home and resolves what to play, on its own thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/browser.hpp"

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
#include "service/library.hpp"
#include "platform/ps5/system.hpp"

namespace service
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

// One pathfinder query (the web player's GraphQL service): our login5 token,
// the client-token and the web player's app-platform header.
Reply pathfinder(cspot::AccessKeyFetcher &keys, const std::string &body)
{
    Reply reply;
    try
    {
        const std::string token = keys.getAccessKey();
        const std::string client = keys.getClientToken();
        if (token.empty() || client.empty())
            return reply;
        bell::HTTPClient::Headers headers = {{"Authorization", "Bearer " + token},
                                             {"client-token", client},
                                             {"app-platform", "WebPlayer"},
                                             {"Content-Type", "application/json"},
                                             {"Accept", "application/json"}};
        auto response = bell::HTTPClient::post("https://api-partner.spotify.com/pathfinder/v1/query", headers,
                                               std::vector<std::uint8_t>(body.begin(), body.end()));
        reply.body = std::string(response->body());
        reply.status = response->statusCode();
    }
    catch (const std::exception &e)
    {
        hui::sys::log("[HOME] pathfinder: %s", e.what());
    }
    if (reply.status != 200)
        hui::sys::log("[HOME] pathfinder -> HTTP %d", reply.status);
    return reply;
}

} // namespace

Browser::Browser(std::shared_ptr<cspot::Context> ctx, std::string timezone)
    : ctx_(std::move(ctx)), timezone_(std::move(timezone))
{
    keys_ = std::make_unique<cspot::AccessKeyFetcher>(ctx_);
    {
        screens::Library cached;
        long long fetched = 0;
        if (library::from_json(app::read_file(app::paths::kLibrary), cached, fetched))
        {
            library_kept_ = cached;
            library_ = std::make_shared<screens::Library>(std::move(cached));
            library_fetched_ms_ = fetched;
        }
    }
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

void Browser::refresh(bool forget)
{
    Job job;
    job.kind = Job::Kind::refresh;
    job.forget = forget;
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

void Browser::refresh_library()
{
    Job job;
    job.kind = Job::Kind::library;
    std::scoped_lock guard(lock_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

void Browser::artist(const std::string &uri)
{
    Job job;
    job.kind = Job::Kind::artist;
    job.uri = uri;
    std::scoped_lock guard(lock_);
    jobs_.push_back(std::move(job));
    wake_.notify_all();
}

void Browser::forget_library()
{
    std::scoped_lock guard(lock_);
    library_ = nullptr;
    library_fetched_ms_ = 0;
    // library_kept_ belongs to the worker; a fresh fetch replaces it, and its
    // wide art is still valid (only URLs).
}

std::shared_ptr<const screens::Library> Browser::take_library()
{
    std::scoped_lock guard(lock_);
    return std::exchange(library_, nullptr);
}

std::shared_ptr<const screens::ArtistPage> Browser::take_artist()
{
    std::scoped_lock guard(lock_);
    return std::exchange(artist_, nullptr);
}

long long Browser::library_fetched_ms()
{
    std::scoped_lock guard(lock_);
    return library_fetched_ms_;
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
                if (job.forget)
                {
                    sections_.clear();
                    entities_.clear();
                }
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
            case Job::Kind::library:
                fetch_library();
                break;
            case Job::Kind::artist:
                fetch_artist(job.uri);
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
            else if (job.kind == Job::Kind::artist)
            {
                auto page = std::make_shared<screens::ArtistPage>();
                page->card.uri = job.uri;
                page->loading = false;
                page->error = "Couldn't load this artist. Try again in a moment.";
                artist_ = std::move(page);
            }
        }
        std::scoped_lock guard(lock_);
        working_ = false;
    }
}

void Browser::set_timezone(std::string timezone)
{
    std::scoped_lock guard(lock_);
    timezone_ = std::move(timezone);
}

void Browser::fetch_home()
{
    std::string timezone;
    {
        std::scoped_lock guard(lock_);
        timezone = timezone_;
    }
    const Reply reply = call(*keys_,
                             "casita/v1/home/default?timezone=" + timezone +
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

// Your Library, a page of 50 at a time (spaced out), then the wide art for
// items not seen before (the same metadata call Home uses).
void Browser::fetch_library()
{
    constexpr int kMaxItems = 1000; // ponytail: a bigger library shows its first 1000
    screens::Library fresh;
    int total = 0;
    for (int offset = 0; offset < kMaxItems; offset += library::kPageSize)
    {
        {
            std::scoped_lock guard(lock_);
            if (stop_)
                return;
        }
        if (offset > 0)
            hui::sys::sleep_us(300000);
        const Reply reply = pathfinder(*keys_, library::query("libraryV3", library::kLibraryV3,
                                                              library::library_variables(offset)));
        const std::size_t before = fresh.items.size();
        if (reply.status != 200 || !library::parse_library_page(reply.body, fresh.items, total))
            return; // keep what we have; never cache a partial error
        if (offset + library::kPageSize >= total || fresh.items.size() == before)
            break;
    }
    for (std::size_t i = 0; i < fresh.items.size(); ++i)
        fresh.items[i].recent = static_cast<int>(i);

    // Wide art: kept from the last library where known, else asked for.
    std::map<std::string, std::string> wide;
    for (const screens::LibraryItem &item : library_kept_.items)
        if (!item.card.wide.empty())
            wide[item.card.uri] = item.card.wide;
    std::vector<std::string> wanted;
    for (screens::LibraryItem &item : fresh.items)
    {
        if (const auto found = wide.find(item.card.uri); found != wide.end())
            item.card.wide = found->second;
        else if (item.kind == screens::LibraryItem::Kind::playlist || item.kind == screens::LibraryItem::Kind::album)
            wanted.push_back(item.card.uri); // artists get their header with their page
    }
    home::Entities art;
    for (std::size_t first = 0; first < wanted.size(); first += kTraitsBatch)
    {
        hui::sys::sleep_us(300000);
        const std::vector<std::string> batch(
            wanted.begin() + static_cast<std::ptrdiff_t>(first),
            wanted.begin() + static_cast<std::ptrdiff_t>(std::min(wanted.size(), first + kTraitsBatch)));
        const std::string request = home::metadata_request(batch, ctx_->config.countryCode, {home::kVisualIdentityTrait});
        const Reply reply = call(*keys_, "extended-metadata/v0/extended-metadata", "application/protobuf", &request);
        if (reply.status != 200 || !home::parse_traits(reply.body, art, now_ms()))
            break;
    }
    for (screens::LibraryItem &item : fresh.items)
        if (const auto found = art.find(item.card.uri); found != art.end() && !found->second.wide.empty())
            item.card.wide = found->second.wide;

    const long long now = now_ms();
    if (!app::write_file(app::paths::kLibrary, library::to_json(fresh, now)))
        hui::sys::log("[HOME] note: could not save %s", app::paths::kLibrary.c_str());
    hui::sys::log("[HOME] library: %zu items (%d in total), wide art asked for %zu", fresh.items.size(), total,
                  wanted.size());
    library_kept_ = fresh;
    std::scoped_lock guard(lock_);
    library_ = std::make_shared<screens::Library>(std::move(fresh));
    library_fetched_ms_ = now;
}

void Browser::fetch_artist(const std::string &uri)
{
    auto page = std::make_shared<screens::ArtistPage>();
    page->card.uri = uri;
    const Reply reply = pathfinder(*keys_, library::query("queryArtistOverview", library::kArtistOverview,
                                                          library::artist_variables(uri)));
    if (reply.status != 200 || !library::parse_artist(reply.body, *page))
    {
        page->loading = false;
        page->error = "Couldn't load this artist. Try again in a moment.";
    }
    hui::sys::log("[HOME] artist %s: %zu top tracks, header %s", uri.c_str(), page->top.size(),
                  page->card.wide.empty() ? "none" : "yes");
    std::scoped_lock guard(lock_);
    artist_ = std::move(page);
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
    if (request.radio)
    {
        // Song radio, as the phone makes it: the track's inspired-by mix, a
        // generated playlist that is then played like any other.
        const Reply reply =
            call(*keys_, "inspiredby-mix/v2/seed_to_playlist/" + request.uri + "?response-format=json", "application/json");
        const auto json = nlohmann::json::parse(reply.body, nullptr, false);
        std::string mix;
        if (reply.status == 200 && json.is_object() && json.contains("mediaItems"))
            for (const auto &item : json["mediaItems"])
                if (item.is_object() && item.value("uri", "").rfind("spotify:playlist:", 0) == 0)
                {
                    mix = item.value("uri", "");
                    break;
                }
        hui::sys::log("[HOME] song radio %s -> HTTP %d, %s", request.uri.c_str(), reply.status,
                      mix.empty() ? reply.body.substr(0, 300).c_str() : mix.c_str());
        if (mix.empty())
        {
            resolved.error = "Couldn't start the song radio. Try again in a moment.";
            return resolved;
        }
        screens::PlayRequest playlist;
        playlist.uri = mix;
        return fetch_tracks(playlist);
    }
    resolved.context_uri =
        library::playable_uri(home::playable_uri(request.uri, ctx_->config.username), ctx_->config.username);
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

} // namespace service
