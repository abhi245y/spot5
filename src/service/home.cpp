// spot5 - The Home page: Spotify's casita feed plus entity metadata.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "service/home.hpp"

#include "nlohmann/json.hpp"
#include "service/protobuf.hpp"
#include "service/text.hpp"

namespace service::home
{

namespace
{

// home.json's format; bumped when entities gain fields (2: wide art).
constexpr int kCacheVersion = 2;

// Header title: 1 = plain string (shortcuts: ""), 2 = {1: text},
// 3 = {1: template "More like {0}", 2..: {1: name, 2: uri, 3: image}}.
std::string header_title(std::string_view header)
{
    if (std::string_view plain = pb::at(header, {2, 1}); !plain.empty())
        return std::string(plain);
    const std::string_view templated = pb::first(header, 3);
    if (templated.empty())
        return {};
    std::string title(pb::first(templated, 1));
    int index = 0;
    pb::Reader reader(templated);
    pb::Field field;
    while (reader.next(field))
    {
        if (field.number != 2 || field.wire != 2)
            continue;
        const std::string key = "{" + std::to_string(index++) + "}";
        if (const std::size_t at = title.find(key); at != std::string::npos)
            title.replace(at, key.size(), std::string(pb::first(field.bytes, 1)));
    }
    return title;
}

std::string subtitle(const Entity &entity)
{
    if (entity.type == "Artist")
        return "Artist";
    if (!entity.owner.empty())
        return entity.type + " \xC2\xB7 " + entity.owner;
    return entity.type;
}

} // namespace

std::vector<Section> parse_home(std::string_view reply)
{
    std::vector<Section> sections;
    pb::Reader structure(pb::first(reply, 1)); // ResolvedHome.structure
    pb::Field section_field;
    while (structure.next(section_field))
    {
        if (section_field.number != 1 || section_field.wire != 2)
            continue;
        // Section: 1 = info {1: "spotify:section:<id>|<shelf key>"}; the one
        // other field is the shelf kind (2 = shortcuts, 3 = a plain shelf...),
        // each {1: header, 2: {1: list {1: item {1: uri}}}}.
        Section section;
        const std::string_view id = pb::at(section_field.bytes, {1, 1});
        section.shortcuts = id.find("|shortcuts") != std::string_view::npos;
        pb::Reader parts(section_field.bytes);
        pb::Field feature;
        while (parts.next(feature))
        {
            if (feature.number == 1 || feature.wire != 2)
                continue;
            section.title = drawable(header_title(pb::first(feature.bytes, 1)));
            pb::Reader items(pb::at(feature.bytes, {2, 1}));
            pb::Field item;
            while (items.next(item))
            {
                const std::string_view uri = pb::first(item.bytes, 1);
                if (item.number == 1 && uri.substr(0, 8) == "spotify:")
                    section.uris.emplace_back(uri);
            }
        }
        if (!section.uris.empty())
            sections.push_back(std::move(section));
    }
    return sections;
}

std::string metadata_request(const std::vector<std::string> &uris, const std::string &country,
                             std::initializer_list<int> kinds)
{
    std::string header;
    pb::put_bytes_field(header, 1, country);
    pb::put_bytes_field(header, 2, "premium");
    std::string request;
    pb::put_bytes_field(request, 1, header);
    for (const std::string &uri : uris)
    {
        std::string entity;
        pb::put_bytes_field(entity, 1, uri);
        for (int kind : kinds)
        {
            std::string query;
            pb::put_varint_field(query, 1, static_cast<std::uint64_t>(kind));
            pb::put_bytes_field(entity, 2, query);
        }
        pb::put_bytes_field(request, 2, entity);
    }
    return request;
}

bool parse_traits(std::string_view reply, Entities &entities, std::int64_t now_ms)
{
    // BatchedExtensionResponse: 2 = per kind {2: kind, 3: per entity
    // {2: uri, 3: Any {1: type url, 2: value}}}.
    pb::Reader kinds(reply);
    pb::Field kind;
    while (kinds.next(kind))
    {
        if (kind.number != 2 || kind.wire != 2)
            continue;
        pb::Reader list(kind.bytes);
        pb::Field data;
        while (list.next(data))
        {
            if (data.number != 3 || data.wire != 2)
                continue;
            const std::string uri(pb::first(data.bytes, 2));
            const std::string_view any = pb::first(data.bytes, 3);
            const std::string_view type_url = pb::first(any, 1);
            const std::string_view value = pb::first(any, 2);
            if (uri.empty() || value.empty())
                continue;
            Entity &entity = entities[uri];
            entity.fetched_ms = now_ms;
            if (type_url.find("VisualIdentityTrait") != std::string_view::npos)
            {
                // Landscape art (SPOTIFY_MOBILE_RESEARCH.md §13.8): 2 = 16:9
                // cover, 6 = wide full-bleed (artist-style banner); each a
                // group of {1: {1: url}, 2: size}; the last listed is largest.
                for (std::uint32_t group : {2u, 6u})
                {
                    pb::Reader wide(pb::first(value, group));
                    pb::Field image;
                    while (wide.next(image))
                        if (image.number == 1 && image.wire == 2)
                            if (std::string_view url = pb::at(image.bytes, {1, 1}); !url.empty())
                                entity.wide = std::string(url);
                    if (!entity.wide.empty())
                        break;
                }
                // 1 = square cover {1: image {1: {1: url}, 2: size 1|2|3}}
                pb::Reader images(pb::first(value, 1));
                pb::Field image;
                while (images.next(image))
                {
                    if (image.number != 1 || image.wire != 2)
                        continue;
                    std::uint64_t size = 0;
                    pb::Reader fields(image.bytes);
                    pb::Field f;
                    while (fields.next(f))
                        if (f.number == 2 && f.wire == 0)
                            size = f.varint;
                    if (size >= 1 && size <= 3)
                        entity.images[size - 1] = std::string(pb::at(image.bytes, {1, 1}));
                }
            }
            else if (type_url.find("IdentityTrait") != std::string_view::npos)
            {
                entity.type = std::string(pb::first(value, 1));
                entity.title = drawable(std::string(pb::first(value, 2)));
                entity.description = drawable(std::string(pb::first(value, 3)));
                entity.owner = drawable(std::string(pb::at(value, {5, 1})));
            }
        }
        if (!list.ok())
            return false;
    }
    return kinds.ok();
}

namespace
{

// A 16-byte Spotify id as the 22 base62 characters of its URI.
std::string base62(std::string_view gid)
{
    static const char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::vector<unsigned char> number(gid.begin(), gid.end());
    std::string out(22, '0');
    for (int at = 21; at >= 0; --at)
    {
        unsigned remainder = 0; // long division of the big-endian number by 62
        for (unsigned char &byte : number)
        {
            const unsigned value = (remainder << 8) | byte;
            byte = static_cast<unsigned char>(value / 62);
            remainder = value % 62;
        }
        out[static_cast<std::size_t>(at)] = kDigits[remainder];
    }
    return out;
}

} // namespace

bool parse_tracks(std::string_view reply, std::map<std::string, screens::TrackRow> &tracks)
{
    // Same envelope as the traits; the value is spotify.metadata.Track:
    // 2 name, 3 album {1 gid, 2 name, 17 cover group {1 image {1 file id, 2 size}}},
    // 4 artist {1 gid, 2 name} (repeated), 7 duration ms.
    pb::Reader kinds(reply);
    pb::Field kind;
    while (kinds.next(kind))
    {
        if (kind.number != 2 || kind.wire != 2)
            continue;
        pb::Reader list(kind.bytes);
        pb::Field data;
        while (list.next(data))
        {
            if (data.number != 3 || data.wire != 2)
                continue;
            const std::string uri(pb::first(data.bytes, 2));
            const std::string_view track = pb::at(data.bytes, {3, 2});
            if (uri.empty() || track.empty())
                continue;
            screens::TrackRow row;
            row.uri = uri;
            row.title = drawable(std::string(pb::first(track, 2)));
            const std::string_view album = pb::first(track, 3);
            row.album = drawable(std::string(pb::first(album, 2)));
            if (const std::string_view gid = pb::first(album, 1); gid.size() == 16)
                row.album_uri = "spotify:album:" + base62(gid);
            pb::Reader fields(track);
            pb::Field f;
            std::vector<std::string> artists;
            while (fields.next(f))
            {
                if (f.number == 4 && f.wire == 2)
                {
                    artists.push_back(drawable(std::string(pb::first(f.bytes, 2))));
                    if (const std::string_view gid = pb::first(f.bytes, 1); gid.size() == 16)
                    {
                        screens::Card artist;
                        artist.uri = "spotify:artist:" + base62(gid);
                        artist.title = artists.back();
                        artist.subtitle = "Artist";
                        artist.round = true;
                        row.artist_cards.push_back(std::move(artist));
                    }
                }
                else if (f.number == 7 && f.wire == 0)
                    row.duration_ms = static_cast<std::uint32_t>(f.varint);
            }
            for (std::size_t i = 0; i < artists.size(); ++i)
                row.artists += (i == 0 ? "" : i + 1 == artists.size() ? " & " : ", ") + artists[i];
            // The 300 px cover (size 0): its file id, as hex, is the image id.
            pb::Reader images(pb::first(album, 17));
            pb::Field image;
            while (images.next(image))
            {
                std::uint64_t size = 99;
                pb::Reader parts(image.bytes);
                pb::Field part;
                while (parts.next(part))
                    if (part.number == 2 && part.wire == 0)
                        size = part.varint;
                const std::string_view id = pb::first(image.bytes, 1);
                if (size == 0 && !id.empty())
                {
                    static const char kHex[] = "0123456789abcdef";
                    row.image = "https://i.scdn.co/image/";
                    for (unsigned char c : id)
                        row.image += {kHex[c >> 4], kHex[c & 15]};
                }
            }
            if (!row.title.empty())
                tracks[uri] = std::move(row);
        }
        if (!list.ok())
            return false;
    }
    return kinds.ok();
}

screens::Card card_of(const std::string &uri, const Entity &entity)
{
    screens::Card card;
    card.uri = uri;
    card.title = entity.title;
    card.subtitle = subtitle(entity);
    card.description = entity.description;
    card.image = !entity.images[1].empty() ? entity.images[1] : entity.images[2];
    card.image_large = larger_cover(!entity.images[2].empty() ? entity.images[2] : card.image);
    card.wide = entity.wide;
    card.round = entity.type == "Artist";
    return card;
}

screens::HomePage build_page(const std::vector<Section> &sections, const Entities &entities)
{
    screens::HomePage page;
    for (const Section &section : sections)
    {
        screens::Shelf shelf;
        shelf.title = section.title;
        shelf.shortcuts = section.shortcuts;
        for (const std::string &uri : section.uris)
        {
            const auto found = entities.find(uri);
            if (found == entities.end() || found->second.title.empty())
                continue;
            shelf.cards.push_back(card_of(uri, found->second));
        }
        if (!shelf.cards.empty() && (shelf.shortcuts || !shelf.title.empty()))
            page.shelves.push_back(std::move(shelf));
    }
    return page;
}

std::string playable_uri(const std::string &uri, const std::string &username)
{
    if (uri == "spotify:user:anonymized:collection" && !username.empty())
        return "spotify:user:" + username + ":collection";
    return uri;
}

std::string to_json(const std::vector<Section> &sections, const Entities &entities,
                    std::int64_t fetched_ms)
{
    nlohmann::json json;
    json["version"] = kCacheVersion;
    json["fetched_ms"] = fetched_ms;
    json["sections"] = nlohmann::json::array();
    for (const Section &section : sections)
        json["sections"].push_back(
            {{"title", section.title}, {"shortcuts", section.shortcuts}, {"uris", section.uris}});
    json["entities"] = nlohmann::json::object();
    for (const auto &[uri, e] : entities)
        json["entities"][uri] = {{"type", e.type},
                                 {"title", e.title},
                                 {"description", e.description},
                                 {"owner", e.owner},
                                 {"images", {e.images[0], e.images[1], e.images[2]}},
                                 {"wide", e.wide},
                                 {"fetched_ms", e.fetched_ms}};
    return json.dump();
}

bool from_json(const std::string &text, std::vector<Section> &sections, Entities &entities,
               std::int64_t &fetched_ms)
{
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("version", 0) != kCacheVersion)
        return false; // an older format: fetch everything again
    try
    {
        fetched_ms = json.value("fetched_ms", std::int64_t{0});
        sections.clear();
        for (const auto &s : json.at("sections"))
            sections.push_back({s.at("title").get<std::string>(), s.at("shortcuts").get<bool>(),
                                s.at("uris").get<std::vector<std::string>>()});
        entities.clear();
        for (const auto &[uri, e] : json.at("entities").items())
        {
            Entity &entity = entities[uri];
            entity.type = e.at("type").get<std::string>();
            entity.title = e.at("title").get<std::string>();
            entity.description = e.at("description").get<std::string>();
            entity.owner = e.at("owner").get<std::string>();
            for (int i = 0; i < 3; ++i)
                entity.images[i] = e.at("images").at(static_cast<std::size_t>(i)).get<std::string>();
            entity.fetched_ms = e.at("fetched_ms").get<std::int64_t>();
            entity.wide = e.value("wide", std::string());
        }
    }
    catch (const nlohmann::json::exception &)
    {
        return false;
    }
    return true;
}

} // namespace service::home
