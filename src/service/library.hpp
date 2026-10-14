// spot5 - Your Library and artist pages from pathfinder's JSON.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// pathfinder (api-partner.spotify.com/pathfinder/v1/query) is the GraphQL
// service Spotify's web player uses; it answers this app's login5 token with
// a client-token and the web player's app-platform header (checked live,
// SPOTIFY_MOBILE_RESEARCH.md §9). Persisted operations are named by hash.
// This file only builds requests and parses replies (no network), so it is
// host-tested; service/browser does the fetching.

#pragma once

#include <string>
#include <vector>

#include "screens/model.hpp"

namespace service::library
{

constexpr const char *kLibraryV3 = "390c78e5b951029bad359785e69b07b536a509c581cbcd0aded5e5067f187455";
constexpr const char *kArtistOverview = "9f8134ef565e78621f1e1793555bd6633c5ac144ae0f89604ed3ae3f80b3c8e6";
constexpr int kPageSize = 50;

// The body of a pathfinder request.
std::string query(const char *operation, const char *hash, const std::string &variables_json);
// libraryV3 variables: everything (no filter), Spotify's Recents order.
std::string library_variables(int offset);
std::string artist_variables(const std::string &uri);

// Adds one libraryV3 page's items to `items` (folders, podcasts and episodes
// are left out); `total` gets the library's size. False if not a library page.
bool parse_library_page(const std::string &reply, std::vector<screens::LibraryItem> &items, int &total);

// queryArtistOverview -> the artist's page. False if not an artist reply.
bool parse_artist(const std::string &reply, screens::ArtistPage &page);

// Liked Songs appears as spotify:collection:tracks; playback needs the user's URI.
std::string playable_uri(const std::string &uri, const std::string &username);

// Cache file (library.json).
std::string to_json(const screens::Library &library, long long fetched_ms);
bool from_json(const std::string &text, screens::Library &library, long long &fetched_ms);

} // namespace service::library
