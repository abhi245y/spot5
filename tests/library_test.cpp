// ps5-spotify - Host check for spotify/library (libraryV3 + artist overview parsing).
// SPDX-License-Identifier: GPL-3.0-or-later
// Build/run: clang++ -std=c++20 -Isrc -I../cspot/cspot/bell/external/nlohmann_json/include \
//   tests/library_test.cpp src/spotify/library.cpp -o /tmp/lt && /tmp/lt [tests/home-fixture]
// With the fixture directory (real replies, kept out of git) it also parses those.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "spotify/library.hpp"

using namespace spotify;

static std::string slurp(const std::string &path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main(int argc, char **argv)
{
    const std::string page = R"({"data":{"me":{"libraryV3":{"totalCount":4,"items":[
      {"addedAt":{"isoString":"2026-01-01T00:00:00Z"},"pinned":true,"item":{"__typename":"LibraryPseudoPlaylistResponseWrapper","_uri":"spotify:collection:tracks",
        "data":{"name":"Liked Songs","count":964,"image":{"sources":[{"url":"liked-64","width":64},{"url":"liked-300","width":300},{"url":"liked-640","width":640}]}}}},
      {"addedAt":{"isoString":"2025-05-05T00:00:00Z"},"item":{"__typename":"PlaylistResponseWrapper","_uri":"spotify:playlist:p",
        "data":{"name":"Focus","ownerV2":{"data":{"name":"Spotify"}},"images":{"items":[{"sources":[{"url":"p-img","width":null}]}]}}}},
      {"item":{"__typename":"ArtistResponseWrapper","_uri":"spotify:artist:a","data":{"profile":{"name":"Tame Impala"},
        "visuals":{"avatarImage":{"sources":[{"url":"a-640","width":640},{"url":"a-160","width":160}]}}}}},
      {"item":{"__typename":"LibraryFolderResponseWrapper","_uri":"spotify:user:x:folder:1","data":{"name":"Folder"}}}
    ]}}}})";
    std::vector<screens::LibraryItem> items;
    int total = 0;
    assert(library::parse_library_page(page, items, total));
    assert(total == 4 && items.size() == 3); // the folder is left out
    assert(items[0].kind == screens::LibraryItem::Kind::collection && items[0].card.image == "liked-300");
    assert(items[0].pinned && items[0].card.subtitle == "Playlist \xC2\xB7 964 songs");
    assert(items[1].creator == "Spotify" && items[1].card.image == "p-img");
    assert(items[2].card.round && items[2].card.image == "a-160" && items[2].card.image_large == "a-640");
    assert(!library::parse_library_page("<html>", items, total));
    // Optional fields may be null: still a page.
    screens::ArtistPage nulls;
    assert(library::parse_artist(R"({"data":{"artistUnion":{"uri":"spotify:artist:n","profile":{"name":"N","biography":{"text":null}},
        "visuals":null,"headerImage":null,"stats":{"monthlyListeners":null},"discography":{"topTracks":null,"albums":{"items":[]}}}}})",
                                 nulls));
    assert(nulls.card.title == "N" && nulls.biography.empty() && nulls.top.empty() && !nulls.loading);

    screens::Library library;
    library.items = items;
    screens::Library back;
    long long fetched = 0;
    assert(library::from_json(library::to_json(library, 7), back, fetched));
    assert(fetched == 7 && back.items.size() == 3 && back.items[2].card.round);

    assert(library::playable_uri("spotify:collection:tracks", "u") == "spotify:user:u:collection");
    assert(library::query("libraryV3", library::kLibraryV3, library::library_variables(50)).find("\"offset\":50") !=
           std::string::npos);

    if (argc == 2)
    {
        const std::string dir = argv[1];
        for (const char *name : {"lib_playlists.json", "lib_albums.json", "lib_artists.json"})
        {
            std::vector<screens::LibraryItem> real;
            assert(library::parse_library_page(slurp(dir + "/" + name), real, total));
            std::printf("%s: %zu items of %d; first \"%s\" (%s) image %s\n", name, real.size(), total,
                        real[0].card.title.c_str(), real[0].card.subtitle.c_str(), real[0].card.image.c_str());
        }
        screens::ArtistPage artist;
        assert(library::parse_artist(slurp(dir + "/artist.json"), artist));
        std::printf("artist %s: %s; wide %s; %zu top, %zu albums, %zu singles\n", artist.card.title.c_str(),
                    artist.listeners.c_str(), artist.card.wide.c_str(), artist.top.size(), artist.albums.size(),
                    artist.singles.size());
        // A small artist: no header, no albums, a biography whose text is null.
        screens::ArtistPage small;
        assert(library::parse_artist(slurp(dir + "/artist_small.json"), small));
        std::printf("small artist %s: %s; header %s; %zu top, %zu albums, %zu singles\n", small.card.title.c_str(),
                    small.listeners.c_str(), small.card.wide.empty() ? "none" : "yes", small.top.size(),
                    small.albums.size(), small.singles.size());
        std::printf("  top 1: %s by %s, %u ms, %s\n", artist.top[0].title.c_str(), artist.top[0].artists.c_str(),
                    artist.top[0].duration_ms, artist.top[0].image.c_str());
    }
    std::puts("library_test: ok");
    return 0;
}
