// ps5-spotify - The Home page: Spotify's casita feed plus entity metadata.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// How the phone builds Home (SPOTIFY_MOBILE_RESEARCH.md §13):
//   1. GET  casita/v1/home/default        sections, each a title + entity URIs
//   2. POST extended-metadata (batched)   title/owner (trait 178) and cover
//                                         images (trait 179) per URI
// This file only parses and assembles (no network), so it is host-tested;
// spotify/browser does the fetching.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "screens/model.hpp"

namespace spotify::home
{

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
    std::int64_t fetched_ms = 0;
};
using Entities = std::map<std::string, Entity>;

// casita/v1/home reply -> sections. Empty if it is not a Home page.
std::vector<Section> parse_home(std::string_view reply);

// The extended-metadata request body for these URIs (both traits).
std::string traits_request(const std::vector<std::string> &uris, const std::string &country);

// Adds what an extended-metadata reply says to `entities` (stamped `now_ms`).
// Returns false if the reply is malformed.
bool parse_traits(std::string_view reply, Entities &entities, std::int64_t now_ms);

// Sections + metadata -> the page the screens draw. Items without a title
// are left out, and so are empty shelves.
screens::HomePage build_page(const std::vector<Section> &sections, const Entities &entities);

// Liked Songs appears on Home as an anonymised URI; playback needs the real one.
std::string playable_uri(const std::string &uri, const std::string &username);

// Cache file (home.json): sections + entities, so the next launch shows Home at once.
std::string to_json(const std::vector<Section> &sections, const Entities &entities,
                    std::int64_t fetched_ms);
bool from_json(const std::string &text, std::vector<Section> &sections, Entities &entities,
               std::int64_t &fetched_ms);

} // namespace spotify::home
