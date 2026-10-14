// spot5 - The Home page: Spotify's casita feed plus entity metadata.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the phone builds Home (SPOTIFY_MOBILE_RESEARCH.md §13):
//   1. GET  casita/v1/home/default        sections, each a title + entity URIs
//   2. POST extended-metadata (batched)   title/owner (trait 178) and cover
//                                         images (trait 179) per URI
// This file only parses and assembles (no network), so it is host-tested;
// service/browser does the fetching.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "screens/model.hpp"

namespace service::home
{

constexpr int kTrackV4 = 10; // full spotify.metadata.Track
constexpr int kIdentityTrait = 178;
constexpr int kVisualIdentityTrait = 179;

// One section of casita's reply, before metadata.
struct Section
{
    std::string title; // template arguments already filled in
    bool shortcuts = false;
    std::vector<std::string> uris;
};

// What extended-metadata says about one URI.
struct Entity
{
    std::string type; // "Playlist", "Artist", "Album", "Show"...
    std::string title;
    std::string description;
    std::string owner;
    std::string images[3]; // small (64), medium (300), large (640); any may be empty
    std::string wide;      // the largest landscape image (16:9 cover or wide full-bleed)
    std::int64_t fetched_ms = 0;
};
using Entities = std::map<std::string, Entity>;

// casita/v1/home reply -> sections. Empty if it is not a Home page.
std::vector<Section> parse_home(std::string_view reply);

// The extended-metadata request body for these URIs: `kinds` per URI
// (Home cards: identity + images; tracks: kTrackV4).
std::string metadata_request(const std::vector<std::string> &uris, const std::string &country,
                             std::initializer_list<int> kinds = {kIdentityTrait, kVisualIdentityTrait});

// Adds the tracks a TRACK_V4 reply describes to `tracks` (by URI).
bool parse_tracks(std::string_view reply, std::map<std::string, screens::TrackRow> &tracks);

// Adds what an extended-metadata reply says to `entities` (stamped `now_ms`).
// Returns false if the reply is malformed.
bool parse_traits(std::string_view reply, Entities &entities, std::int64_t now_ms);

// One URI's metadata as a card.
screens::Card card_of(const std::string &uri, const Entity &entity);

// Sections + metadata -> the page the screens draw. Items without a title
// are left out, and so are empty shelves.
screens::HomePage build_page(const std::vector<Section> &sections, const Entities &entities);

// The sharpest version of an album cover Spotify serves (the original
// upload, ~2000 px; the loader halves it) for big canvases. Album art only:
// ab67616d0000b273<id> is 640 px, ab67616d000082c1<id> the original. Playlist,
// artist and mosaic images have no such size and come back unchanged.
inline std::string larger_cover(const std::string &url)
{
    static const std::string kPrefix = "https://i.scdn.co/image/ab67616d0000b273";
    if (url.rfind(kPrefix, 0) != 0)
        return url;
    return "https://i.scdn.co/image/ab67616d000082c1" + url.substr(kPrefix.size());
}

// The same album cover at another size: album cover links differ only in the
// four hex digits after "ab67616d0000" (4851 64 px, 1e02 300 px, b273 640 px,
// 82c1 the original). Anything else (playlist, artist art) comes back empty.
inline std::string album_cover_at(const std::string &url, const char *size_code)
{
    static const std::string kPrefix = "https://i.scdn.co/image/ab67616d0000";
    if (url.rfind(kPrefix, 0) != 0 || url.size() < kPrefix.size() + 4)
        return {};
    return kPrefix + size_code + url.substr(kPrefix.size() + 4);
}

// Liked Songs appears on Home as an anonymised URI; playback needs the real one.
std::string playable_uri(const std::string &uri, const std::string &username);

// Cache file (home.json): sections + entities, so the next launch shows Home at once.
std::string to_json(const std::vector<Section> &sections, const Entities &entities,
                    std::int64_t fetched_ms);
bool from_json(const std::string &text, std::vector<Section> &sections, Entities &entities,
               std::int64_t &fetched_ms);

} // namespace service::home
