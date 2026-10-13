// ps5-spotify - Host check for spotify/home (casita + traits parsing, cache).
// SPDX-License-Identifier: GPL-3.0-or-later
// Build/run: clang++ -std=c++20 -Isrc -I../cspot/cspot/bell/external/nlohmann_json/include \
//   tests/home_test.cpp src/spotify/home.cpp -o /tmp/ht && /tmp/ht [home.pb traits.pb]
// With the two optional files (real replies, kept out of git) it also prints
// the page they make.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "spotify/home.hpp"
#include "spotify/protobuf.hpp"

using namespace spotify;

namespace
{

std::string msg(std::uint32_t field, const std::string &inner)
{
    std::string out;
    pb::put_bytes_field(out, field, inner);
    return out;
}

// casita Section: info + one shelf (feature field 3) with a header and URIs.
std::string section(const std::string &id, const std::string &header,
                    const std::vector<std::string> &uris, std::uint32_t feature = 3)
{
    std::string list;
    for (const std::string &uri : uris)
        list += msg(1, msg(1, uri));
    return msg(1, msg(1, msg(1, id)) + msg(feature, msg(1, header) + msg(2, msg(1, list))));
}

std::string identity(const std::string &uri, const std::string &type, const std::string &title,
                     const std::string &owner)
{
    std::string value = msg(1, type) + msg(2, title) + msg(5, msg(1, owner));
    std::string any = msg(1, "type.googleapis.com/spotify.contentagnostic.v2.IdentityTrait") + msg(2, value);
    return msg(3, msg(2, uri) + msg(3, any));
}

std::string visual(const std::string &uri, const std::string &url300)
{
    std::string image;
    pb::put_bytes_field(image, 1, msg(1, url300));
    pb::put_varint_field(image, 2, 2);
    std::string value = msg(1, msg(1, image));
    std::string any = msg(1, "type.googleapis.com/spotify.contentagnostic.v2.VisualIdentityTrait") + msg(2, value);
    return msg(3, msg(2, uri) + msg(3, any));
}

std::string slurp(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

int main(int argc, char **argv)
{
    // Home: shortcuts (feature 2, empty title), a plain shelf, a templated one.
    std::string templated = msg(3, msg(1, "More like {0}") + msg(2, msg(1, "Jakes Bejoy")));
    std::string structure = section("spotify:section:a|shortcuts", msg(1, ""),
                                    {"spotify:user:anonymized:collection", "spotify:playlist:p1"}, 2) +
                            section("spotify:section:b|jump-back-in", msg(2, msg(1, "Jump back in")),
                                    {"spotify:playlist:p1", "spotify:artist:a1", "spotify:album:missing"}) +
                            section("spotify:section:c|more-like", templated, {"spotify:album:x"});
    auto sections = home::parse_home(msg(1, structure));
    assert(sections.size() == 3);
    assert(sections[0].shortcuts && sections[0].uris.size() == 2);
    assert(sections[1].title == "Jump back in" && sections[1].uris.size() == 3);
    assert(sections[2].title == "More like Jakes Bejoy");

    // Garbage in: no crash, nothing out.
    assert(home::parse_home(std::string("\xff\xff\xff", 3)).empty());

    // Traits: titles and a 300 px image.
    std::string reply = msg(2, identity("spotify:playlist:p1", "Playlist", "Focus EDM Mix", "abhi245y") +
                                   visual("spotify:playlist:p1", "https://i.scdn.co/image/p1-300")) +
                        msg(2, identity("spotify:artist:a1", "Artist", "Abhiram Lolla", "") +
                                   identity("spotify:user:anonymized:collection", "Playlist", "Liked Songs", "abhi245y"));
    home::Entities entities;
    assert(home::parse_traits(reply, entities, 1000));
    assert(entities["spotify:playlist:p1"].title == "Focus EDM Mix");
    assert(entities["spotify:playlist:p1"].images[1] == "https://i.scdn.co/image/p1-300");

    // Page: entries without metadata dropped, artists round, subtitles.
    screens::HomePage page = home::build_page(sections, entities);
    assert(page.shelves.size() == 2); // "More like" had nothing resolved
    assert(page.shelves[1].cards.size() == 2);
    assert(page.shelves[1].cards[0].subtitle == "Playlist \xC2\xB7 abhi245y");
    assert(page.shelves[1].cards[0].image_large == "https://i.scdn.co/image/p1-300");
    assert(page.shelves[1].cards[1].round);

    // Request: decodes back to the URIs with both trait kinds.
    std::string request = home::traits_request({"spotify:playlist:p1"}, "IN");
    assert(pb::at(request, {1, 1}) == "IN");
    assert(pb::at(request, {2, 1}) == "spotify:playlist:p1");

    // Cache round trip.
    std::vector<home::Section> sections2;
    home::Entities entities2;
    std::int64_t fetched = 0;
    assert(home::from_json(home::to_json(sections, entities, 42), sections2, entities2, fetched));
    assert(fetched == 42 && sections2.size() == 3 && entities2.size() == entities.size());
    assert(!home::from_json("not json", sections2, entities2, fetched));

    assert(home::playable_uri("spotify:user:anonymized:collection", "abhi") == "spotify:user:abhi:collection");

    if (argc == 3)
    {
        auto real = home::parse_home(slurp(argv[1]));
        home::Entities real_entities;
        assert(home::parse_traits(slurp(argv[2]), real_entities, 1));
        std::printf("real reply: %zu sections, %zu entities\n", real.size(), real_entities.size());
        for (const auto &s : real)
            std::printf("  %-45s %zu items%s\n", s.title.c_str(), s.uris.size(), s.shortcuts ? " (shortcuts)" : "");
    }
    std::puts("home_test: ok");
    return 0;
}
