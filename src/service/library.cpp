// spot5 - Your Library and artist pages from pathfinder's JSON.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/library.hpp"

#include <cstdlib>

#include "nlohmann/json.hpp"
#include "service/text.hpp"

namespace service::library
{

namespace
{

using json = nlohmann::json;

// Replies may hold null where a value is optional (a biography with no
// text): json::value() throws on those, so reads go through these.
std::string str(const json &object, const char *key, const std::string &fallback = {})
{
    if (!object.is_object())
        return fallback;
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : fallback;
}

json field(const json &object, const char *key)
{
    if (!object.is_object())
        return json();
    const auto found = object.find(key);
    return found != object.end() ? *found : json();
}

bool flag(const json &object, const char *key)
{
    const json value = field(object, key);
    return value.is_boolean() && value.get<bool>();
}

long long num(const json &object, const char *key)
{
    if (!object.is_object())
        return 0;
    const auto found = object.find(key);
    return found != object.end() && found->is_number() ? found->get<long long>() : 0;
}

// pathfinder images: {"sources": [{"url", "width", "height"}...]}; widths
// may be missing. `want` 0 means the widest.
std::string pick(const json &image, int want)
{
    if (!image.is_object() || !image.contains("sources") || !image["sources"].is_array())
        return {};
    std::string best;
    int best_width = -1;
    for (const json &source : image["sources"])
    {
        const std::string url = str(source, "url");
        const int width = source.contains("width") && source["width"].is_number() ? source["width"].get<int>()
                          : source.contains("maxWidth") && source["maxWidth"].is_number()
                              ? source["maxWidth"].get<int>()
                              : 0;
        if (url.empty())
            continue;
        // The closest to `want` (or the widest), up to what the loader keeps.
        const bool better = best.empty() ||
                            (want == 0 ? width > best_width && width <= 2880
                                       : std::abs(width - want) < std::abs(best_width - want));
        if (better)
        {
            best = url;
            best_width = width;
        }
    }
    return best;
}

std::string names(const json &artists)
{
    std::string out;
    if (!artists.is_object() || !artists.contains("items"))
        return out;
    for (const json &artist : artists["items"])
    {
        const std::string name = artist.contains("profile") ? drawable(str(artist["profile"], "name")) : "";
        if (!name.empty())
            out += (out.empty() ? "" : ", ") + name;
    }
    return out;
}

std::string with_commas(long long n)
{
    std::string digits = std::to_string(n), out;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

screens::Card release_card(const json &release, const char *type)
{
    screens::Card card;
    card.uri = str(release, "uri");
    card.title = drawable(str(release, "name"));
    const int year = release.contains("date") ? num(release["date"], "year") : 0;
    card.subtitle = std::string(type) + (year ? " \xC2\xB7 " + std::to_string(year) : "");
    card.image = pick(field(release, "coverArt"), 300);
    card.image_large = pick(field(release, "coverArt"), 640);
    return card;
}

} // namespace

std::string query(const char *operation, const char *hash, const std::string &variables_json)
{
    json body = {{"operationName", operation},
                 {"variables", json::parse(variables_json)},
                 {"extensions", {{"persistedQuery", {{"version", 1}, {"sha256Hash", hash}}}}}};
    return body.dump();
}

std::string library_variables(int offset)
{
    json variables = {{"filters", json::array()},
                      {"order", nullptr},
                      {"textFilter", ""},
                      {"features", {"LIKED_SONGS", "YOUR_EPISODES"}},
                      {"limit", kPageSize},
                      {"offset", offset},
                      {"flatten", true},
                      {"expandedFolders", json::array()},
                      {"folderUri", nullptr},
                      {"includeFoldersWhenFlattening", false}};
    return variables.dump();
}

std::string artist_variables(const std::string &uri)
{
    return json({{"uri", uri}, {"locale", ""}, {"includePrerelease", true}}).dump();
}

bool parse_library_page(const std::string &reply, std::vector<screens::LibraryItem> &items, int &total)
{
    const json doc = json::parse(reply, nullptr, false);
    if (doc.is_discarded())
        return false;
    try
    {
        const json &library = doc.at("data").at("me").at("libraryV3");
        total = num(library, "totalCount");
        for (const json &entry : library.at("items"))
        {
            const json &item = entry.at("item");
            const std::string type = str(item, "__typename");
            const json &data = item.at("data");
            screens::LibraryItem out;
            out.card.uri = str(item, "_uri");
            out.added_at = entry.contains("addedAt") && entry["addedAt"].is_object()
                               ? str(entry["addedAt"], "isoString")
                               : "";
            out.pinned = flag(entry, "pinned");
            out.recent = static_cast<int>(items.size());
            if (type == "PlaylistResponseWrapper")
            {
                out.kind = screens::LibraryItem::Kind::playlist;
                out.card.title = drawable(str(data, "name"));
                out.card.description = drawable(str(data, "description"));
                if (data.contains("ownerV2") && data["ownerV2"].contains("data"))
                    out.creator = drawable(str(data["ownerV2"]["data"], "name"));
                if (data.contains("images") && data["images"].contains("items") && !data["images"]["items"].empty())
                    out.card.image = out.card.image_large = pick(data["images"]["items"][0], 0);
                out.card.subtitle = "Playlist" + (out.creator.empty() ? "" : " \xC2\xB7 " + out.creator);
            }
            else if (type == "AlbumResponseWrapper")
            {
                out.kind = screens::LibraryItem::Kind::album;
                out.card.title = drawable(str(data, "name"));
                out.creator = names(field(data, "artists"));
                out.card.image = pick(field(data, "coverArt"), 300);
                out.card.image_large = pick(field(data, "coverArt"), 640);
                out.card.subtitle = "Album" + (out.creator.empty() ? "" : " \xC2\xB7 " + out.creator);
            }
            else if (type == "ArtistResponseWrapper")
            {
                out.kind = screens::LibraryItem::Kind::artist;
                out.card.title = data.contains("profile") ? drawable(str(data["profile"], "name")) : "";
                if (data.contains("visuals"))
                {
                    out.card.image = pick(field(data["visuals"], "avatarImage"), 300);
                    out.card.image_large = pick(field(data["visuals"], "avatarImage"), 640);
                }
                out.card.subtitle = "Artist";
                out.card.round = true;
                out.creator = out.card.title;
            }
            else if (type == "LibraryPseudoPlaylistResponseWrapper" && out.card.uri == "spotify:collection:tracks")
            {
                out.kind = screens::LibraryItem::Kind::collection;
                out.card.title = drawable(str(data, "name", "Liked Songs"));
                out.card.image = pick(field(data, "image"), 300);
                out.card.image_large = pick(field(data, "image"), 640);
                out.card.subtitle = "Playlist \xC2\xB7 " + std::to_string(num(data, "count")) + " songs";
            }
            else
            {
                continue; // folders, podcasts, episodes
            }
            if (!out.card.uri.empty() && !out.card.title.empty())
                items.push_back(std::move(out));
        }
    }
    catch (const json::exception &)
    {
        return false;
    }
    return true;
}

bool parse_artist(const std::string &reply, screens::ArtistPage &page)
{
    const json doc = json::parse(reply, nullptr, false);
    if (doc.is_discarded())
        return false;
    try
    {
        const json &artist = doc.at("data").at("artistUnion");
        page.card.uri = str(artist, "uri", page.card.uri);
        page.card.title = drawable(str(artist.at("profile"), "name", page.card.title));
        page.card.subtitle = "Artist";
        page.card.round = true;
        if (artist.contains("visuals"))
        {
            page.card.image = pick(field(artist["visuals"], "avatarImage"), 300);
            page.card.image_large = pick(field(artist["visuals"], "avatarImage"), 640);
        }
        // The landscape header: the 1920 px version (or the widest kept).
        if (artist.contains("headerImage") && artist["headerImage"].is_object() &&
            artist["headerImage"].contains("data"))
            page.card.wide = pick(artist["headerImage"]["data"], 1920);
        if (artist.contains("stats") && artist["stats"].contains("monthlyListeners") &&
            artist["stats"]["monthlyListeners"].is_number())
            page.listeners = with_commas(artist["stats"]["monthlyListeners"].get<long long>()) + " monthly listeners";
        if (artist["profile"].contains("biography") && artist["profile"]["biography"].is_object())
            page.biography = drawable(str(artist["profile"]["biography"], "text"));

        // Smaller artists may lack any of these: a page without them is
        // still a page.
        const json empty_object = json::object();
        const json &discography = artist.contains("discography") ? artist["discography"] : empty_object;
        const json top = discography.contains("topTracks") && discography["topTracks"].contains("items")
                             ? discography["topTracks"]["items"]
                             : json::array();
        for (const json &entry : top)
        {
            const json &track = entry.at("track");
            screens::TrackRow row;
            row.uri = str(track, "uri");
            row.title = drawable(str(track, "name"));
            row.artists = names(field(track, "artists"));
            const json &artists = field(track, "artists");
            if (artists.is_object() && artists.contains("items"))
                for (const json &artist : artists["items"])
                {
                    screens::Card card;
                    card.uri = str(artist, "uri");
                    card.title = artist.contains("profile") ? drawable(str(artist["profile"], "name")) : "";
                    card.subtitle = "Artist";
                    card.round = true;
                    if (!card.uri.empty() && !card.title.empty())
                        row.artist_cards.push_back(std::move(card));
                }
            if (track.contains("duration"))
                row.duration_ms = num(track["duration"], "totalMilliseconds");
            if (track.contains("albumOfTrack"))
            {
                row.album_uri = str(track["albumOfTrack"], "uri");
                // Sources without widths: the 300 px one by its id prefix.
                for (const json &source : track["albumOfTrack"]["coverArt"]["sources"])
                {
                    const std::string url = str(source, "url");
                    if (url.find("1e02") != std::string::npos || row.image.empty())
                        row.image = url;
                }
            }
            if (!row.uri.empty())
                page.top.push_back(std::move(row));
        }
        const auto releases = [&](const char *group, const char *type, std::vector<screens::Card> &out) {
            if (!discography.contains(group) || !discography[group].contains("items"))
                return;
            for (const json &entry : discography[group]["items"])
                if (entry.contains("releases") && !entry["releases"]["items"].empty())
                    out.push_back(release_card(entry["releases"]["items"][0], type));
        };
        releases("albums", "Album", page.albums);
        releases("singles", "Single", page.singles);
    }
    catch (const json::exception &)
    {
        return false;
    }
    page.loading = false;
    return true;
}

std::string playable_uri(const std::string &uri, const std::string &username)
{
    if (uri == "spotify:collection:tracks" && !username.empty())
        return "spotify:user:" + username + ":collection";
    return uri;
}

std::string to_json(const screens::Library &library, long long fetched_ms)
{
    json items = json::array();
    for (const screens::LibraryItem &item : library.items)
        items.push_back({{"kind", static_cast<int>(item.kind)},
                         {"uri", item.card.uri},
                         {"title", item.card.title},
                         {"subtitle", item.card.subtitle},
                         {"description", item.card.description},
                         {"image", item.card.image},
                         {"image_large", item.card.image_large},
                         {"wide", item.card.wide},
                         {"added_at", item.added_at},
                         {"creator", item.creator},
                         {"pinned", item.pinned}});
    return json({{"version", 1}, {"fetched_ms", fetched_ms}, {"items", items}}).dump();
}

bool from_json(const std::string &text, screens::Library &library, long long &fetched_ms)
{
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || num(doc, "version") != 1)
        return false;
    try
    {
        fetched_ms = num(doc, "fetched_ms");
        library.items.clear();
        for (const json &j : doc.at("items"))
        {
            screens::LibraryItem item;
            item.kind = static_cast<screens::LibraryItem::Kind>(j.at("kind").get<int>());
            item.card.uri = j.at("uri").get<std::string>();
            item.card.title = j.at("title").get<std::string>();
            item.card.subtitle = str(j, "subtitle");
            item.card.description = drawable(str(j, "description"));
            item.card.image = str(j, "image");
            item.card.image_large = str(j, "image_large");
            item.card.wide = str(j, "wide");
            item.card.round = item.kind == screens::LibraryItem::Kind::artist;
            item.added_at = str(j, "added_at");
            item.creator = str(j, "creator");
            item.pinned = flag(j, "pinned");
            item.recent = static_cast<int>(library.items.size());
            library.items.push_back(std::move(item));
        }
    }
    catch (const json::exception &)
    {
        return false;
    }
    return true;
}

} // namespace service::library
