// spot5 - PC preview: renders the app's screens to PNG files.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: spt_preview <assets dir> <output dir>
//
// Runs src/screens (the same code as the console) through Mesa's surfaceless
// EGL with a fixed 60 Hz clock, drives it with a scripted Model and scripted
// buttons, and saves the moments listed in kScript. EGL setup and the PNG
// path follow ps5-homebrew-ui host/snapshot_main.cpp.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#include "../third_party/stb/stb_image_write.h"
#pragma clang diagnostic pop

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include <map>

#include "core/save_file.hpp"
#include "nlohmann/json.hpp"
#include "service/home.hpp"
#include "service/library.hpp"
#include "gfx/canvas.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "gfx/shaper.hpp"
#include "screens/shell.hpp"
#include "third_party/stb/stb_image.h"
#include "ui/fonts.hpp"

namespace hui::sys
{
extern std::int64_t g_preview_us; // platform_host.cpp: the simulated clock
}

using namespace hui;
using screens::Command;
using screens::Connection;
using screens::Model;
using screens::Step;

namespace
{

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION,       4,
                                 EGL_CONTEXT_MINOR_VERSION,       5,
                                 EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                 EGL_NONE};
    EGLContext context = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(gfx::Renderer &renderer, const std::string &path, gfx::Font &font,
               ui::FontRef &ref)
{
    std::string data;
    if (!save::read_file(path, &data) || !font.load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref.font = &font;
    ref.texture = renderer.batch().create_font_texture(font);
    return true;
}

// One step of the script: wait `seconds` (simulated), with the model changed
// by `change` at the start, `press` for the first frame and `hold` down the
// whole time; then save a picture named `capture` if there is one.
struct Beat
{
    float seconds;
    std::function<void(Model &)> change;
    std::uint32_t press = 0;
    const char *capture = nullptr;
    Direction nav = Direction::none;
    std::uint32_t hold = 0;
};

std::shared_ptr<const screens::Queue> fixture_queue();

void playing(Model &m, const char *id, const char *title, const char *artist, const char *album,
             std::uint32_t at_ms, std::uint32_t length_ms)
{
    m.connection = Connection::ready;
    m.step = Step::done;
    m.has_track = true;
    m.track = {id, title, artist, album, length_ms};
    m.position_ms = at_ms;
    m.paused = false;
    m.queue = fixture_queue();
}

// Test covers (tests/covers, made with ffmpeg): textures and palettes.
struct TestCover
{
    std::uint32_t texture = 0;
    int width = 0, height = 0;
    screens::style::AlbumPalette palette;
};
TestCover g_covers[2];

void with_cover(Model &m, int which)
{
    m.track.art = g_covers[which].texture;
    m.track.palette = g_covers[which].palette;
    m.track.has_palette = true;
}

// Home from real replies in tests/home-fixture (git-ignored: personal data):
// home.pb + traits_*.pb as the console fetches them, covers/<hash>.jpg.
std::shared_ptr<const screens::HomePage> g_home;
std::string g_fixture;
gfx::Renderer *g_renderer = nullptr;

std::shared_ptr<const screens::HomePage> load_home_fixture(const std::string &dir)
{
    std::string home;
    if (!save::read_file(dir + "/home.pb", &home))
        return nullptr;
    service::home::Entities entities;
    for (int i = 0;; ++i)
    {
        std::string traits;
        if (!save::read_file(dir + "/traits_" + std::to_string(i) + ".pb", &traits))
            break;
        service::home::parse_traits(traits, entities, 1);
    }
    return std::make_shared<screens::HomePage>(
        service::home::build_page(service::home::parse_home(home), entities));
}

std::string cover_file(const std::string &url)
{
    return g_fixture + "/covers/" + std::to_string(std::hash<std::string>{}(url)) + ".jpg";
}

// A page's tracks from the fixture: context.json (URIs) + tracks.pb (details
// for the first ones; the rest show as rows still loading).
std::vector<screens::TrackRow> g_rows;

void load_rows_fixture(const std::string &dir)
{
    std::string text, details;
    if (!save::read_file(dir + "/context.json", &text))
        return;
    std::map<std::string, screens::TrackRow> known;
    if (save::read_file(dir + "/tracks.pb", &details))
        service::home::parse_tracks(details, known);
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded())
        return;
    for (const auto &track : json["pages"][0]["tracks"])
    {
        const std::string uri = track.value("uri", "");
        g_rows.push_back(known.count(uri) ? known[uri] : screens::TrackRow{uri});
    }
}

// Library: the PoC's full dump (names, URIs, covers) for volume, after the
// real libraryV3 replies (exact shapes); artist page: Tame Impala.
std::shared_ptr<const screens::Library> g_library;
std::shared_ptr<const screens::ArtistPage> g_artist;

void load_library_fixture(const std::string &dir)
{
    auto library = std::make_shared<screens::Library>();
    int total = 0;
    for (const char *name : {"lib_playlists.json", "lib_albums.json", "lib_artists.json"})
    {
        std::string text;
        if (save::read_file(dir + "/" + name, &text))
            service::library::parse_library_page(text, library->items, total);
    }
    for (const char *kind : {"playlists", "albums"})
    {
        std::string text;
        if (!save::read_file(dir + "/library_" + std::string(kind) + ".json", &text))
            continue;
        const auto list = nlohmann::json::parse(text, nullptr, false);
        if (!list.is_array())
            continue;
        for (const auto &entry : list)
        {
            screens::LibraryItem item;
            item.card.uri = entry.value("uri", "");
            item.card.title = entry["name"].is_string() ? entry["name"].get<std::string>() : "";
            item.card.image = entry["image"].is_string() ? entry["image"].get<std::string>() : "";
            item.added_at = entry.value("added_at", "");
            item.kind = std::string(kind) == "albums" ? screens::LibraryItem::Kind::album
                                                      : screens::LibraryItem::Kind::playlist;
            item.card.subtitle = item.kind == screens::LibraryItem::Kind::album ? "Album" : "Playlist";
            bool seen = item.card.title.empty();
            for (const auto &have : library->items)
                seen = seen || have.card.uri == item.card.uri;
            if (!seen)
                library->items.push_back(item);
        }
    }
    for (std::size_t i = 0; i < library->items.size(); ++i)
        library->items[i].recent = static_cast<int>(i);
    if (!library->items.empty())
        g_library = library;
    std::string text;
    auto artist = std::make_shared<screens::ArtistPage>();
    if (save::read_file(dir + "/artist.json", &text) && service::library::parse_artist(text, *artist))
        g_artist = artist;
}

std::shared_ptr<const screens::Queue> fixture_queue()
{
    if (g_rows.empty())
        return nullptr;
    auto queue = std::make_shared<screens::Queue>();
    queue->tracks.assign(g_rows.begin(), g_rows.begin() + std::min<std::size_t>(g_rows.size(), 40));
    queue->playing = 2;
    queue->first = 0;
    return queue;
}

// Covers by URL: the fixture's file if downloaded, else a test cover. URLs
// with no file are listed in missing-covers.txt (tools/preview.sh fetches them).
screens::Art preview_art(const std::string &url)
{
    static std::map<std::string, screens::Art> loaded;
    if (auto found = loaded.find(url); found != loaded.end())
        return found->second;
    const TestCover &fallback = g_covers[std::hash<std::string>{}(url) % 2];
    screens::Art art{fallback.texture, fallback.palette, fallback.width, fallback.height};
    std::string data;
    if (save::read_file(cover_file(url), &data))
    {
        int w = 0, h = 0, n = 0;
        if (std::uint8_t *px = stbi_load_from_memory(reinterpret_cast<const std::uint8_t *>(data.data()),
                                                     static_cast<int>(data.size()), &w, &h, &n, 4))
        {
            art = {g_renderer->batch().create_texture(w, h, px), screens::style::palette_from_pixels(px, w, h), w, h};
            stbi_image_free(px);
        }
    }
    else if (FILE *list = std::fopen((g_fixture + "/missing-covers.txt").c_str(), "a"))
    {
        std::fprintf(list, "%s %s\n", url.c_str(), cover_file(url).c_str());
        std::fclose(list);
    }
    loaded[url] = art;
    return art;
}

const std::vector<Beat> &script()
{
    static const std::vector<Beat> beats = {
        {0.3f, [](Model &m) { m.step = Step::certificates; }, 0, "01a-logo-pop"},
        {0.2f, nullptr, 0, "01b-logo-pop"},
        {0.2f, nullptr, 0, "01c-logo-pop"},
        {0.38f, nullptr, 0, "01d-logo-sweep"},
        {0.32f, nullptr, 0, "01-launch-title"},
        {0.5f, [](Model &m) { m.step = Step::login; }},
        {2.2f, [](Model &m) { m.step = Step::connect; }, 0, "02-launch-connecting"},
        {1.2f,
         [](Model &m) {
             m.connection = Connection::error;
             m.message = "Could not reach Spotify (connect failed). Check the console's network "
                         "connection, then try again.";
         },
         0, "03-launch-error"},
        // Nothing saved: the loader makes way for how to pair.
        {0.25f,
         [](Model &m) {
             m.connection = Connection::pairing;
             m.message.clear();
             m.device_name = "Living Room PS5";
         },
         0, "03a-pairing-fading-in"},
        {2.0f, nullptr, 0, "03b-pairing"},
        {0.3f,
         [](Model &m) {
             m.connection = Connection::connecting;
             m.step = Step::connect;
             m.message.clear();
         }},
        {0.8f, [](Model &m) { m.step = Step::start; }},
        {0.6f, [](Model &m) { m.step = Step::home; }},
        {0.9f,
         [](Model &m) {
             m.step = Step::artwork;
             m.artwork_done = 84;
             m.artwork_total = 162;
         },
         0, "03c-launch-artwork"},
        {0.8f,
         [](Model &m) {
             m.connection = Connection::ready;
             m.step = Step::done;
         },
         0, "04-launch-ready"},
        {0.35f, nullptr, 0, "05a-handover"},
        {0.25f, nullptr, 0, "05b-handover"},
        {0.25f, nullptr, 0, "05c-handover"},
        {0.25f, nullptr, 0, "05d-handover"},
        {0.25f, nullptr, 0, "05e-handover"},
        {2.0f, nullptr, 0, "06-home"},
        {0.6f, nullptr, 0, "06a-home-tile", Direction::right},
        {0.4f, nullptr, 0, nullptr, Direction::down},           // grid line 2
        {0.6f, nullptr, 0, nullptr, Direction::down},           // first shelf
        {0.3f, nullptr, 0, nullptr, Direction::right},
        {0.9f, nullptr, 0, "06b-home-shelf", Direction::right},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::right},
        {0.3f, nullptr, 0, nullptr, Direction::right},
        {0.3f, nullptr, 0, nullptr, Direction::right},
        {0.3f, nullptr, 0, nullptr, Direction::right},
        {0.9f, nullptr, 0, "06c-home-scrolled", Direction::right},
        {1.2f, nullptr, action_bit(Action::confirm), "06d-page"},
        {0.3f, nullptr, 0, nullptr, Direction::down},           // into the list
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {1.0f, nullptr, 0, "06e-page-list", Direction::down},
        {0.8f, nullptr, action_bit(Action::back), "06e2-back-home"},
        {0.6f, [](Model &m) { m.notice = "This can't be played here yet"; ++m.notice_id; }, 0, "06f-notice"},
        // Library: R1 from Home, filter with R2, sort with Square, open an artist.
        {1.4f, nullptr, action_bit(Action::page_next), "06l1-library"},
        {0.3f, nullptr, 0, nullptr, Direction::right},
        {0.6f, nullptr, 0, "06l2-library-focus", Direction::down},
        {0.12f, nullptr, action_bit(Action::jump_next)},
        {0.5f, nullptr, 0, "06l3-filter-travel"},
        {0.8f, nullptr, action_bit(Action::jump_next), "06l4-albums"},
        {0.2f, nullptr, action_bit(Action::jump_next)},
        {0.9f, nullptr, action_bit(Action::west), "06l5-artists-sorted"},
        {1.3f, nullptr, action_bit(Action::confirm), "06a1-artist"},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.9f, nullptr, 0, "06a2-artist-popular", Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {1.0f, nullptr, 0, "06a3-artist-albums", Direction::right},
        {0.6f, nullptr, action_bit(Action::back)},
        {0.6f, nullptr, action_bit(Action::page_prev), "06l6-back-home"},
        {3.5f, [](Model &m) { playing(m, "a", "Amsham - അംശം", "Aksomaniac", "Amsham", 47000, 214000); },
         0, nullptr},
        {0.8f, nullptr, 0, "06g-home-mini-player"},
        {0.7f, [](Model &m) { m.busy = "Shuffling My playlist #6"; }, 0, "06h-busy-pill"},
        {0.2f, [](Model &m) { m.busy.clear(); }},
        // The bar docks under a list's cover and flies back on the way out.
        {0.18f, nullptr, action_bit(Action::confirm), "06m1-dock-flying"},
        {1.0f, nullptr, 0, "06m2-docked"},
        {0.18f, nullptr, action_bit(Action::back), "06m3-undock-flying"},
        {0.8f, nullptr, 0, "06m4-corner"},
        {0.3f, nullptr, action_bit(Action::north), "07-home-to-playing"},
        {0.4f, nullptr, 0, nullptr},
        {0.4f, nullptr, 0, "08a-cover-pending"},
        {0.35f, [](Model &m) { with_cover(m, 0); }, 0, "08b-cover-fading-in"},
        {2.5f, nullptr, 0, "08-playing"},
        {0.4f, nullptr, 0, nullptr, Direction::up},             // scrubber
        {0.25f, nullptr, 0, nullptr, Direction::right},
        {0.5f, nullptr, 0, "08b-scrubbing", Direction::right},
        {0.3f, nullptr, 0, nullptr, Direction::down},           // transport
        {0.3f, nullptr, 0, nullptr, Direction::down},           // queue
        {0.3f, nullptr, 0, nullptr, Direction::down},
        {0.6f, nullptr, 0, "08c-queue", Direction::down},
        {0.4f, nullptr, action_bit(Action::back)},              // back to the transport
        {0.6f, nullptr, action_bit(Action::west), "08d-queue-hidden"},
        {0.6f, nullptr, action_bit(Action::west)},
        {1.2f, [](Model &m) { m.paused = true; }, 0, "09-paused"},
        {0.6f, [](Model &m) { m.paused = false; }},
        {0.3f,
         [](Model &m) {
             playing(m, "b", "Never Gonna Give You Up", "Rick Astley", "Whenever You Need Somebody",
                     1000, 213573);
         },
         0, "10-track-change"},
        {0.1f, [](Model &m) { with_cover(m, 1); }},
        {2.0f, nullptr, 0, "11-playing-long-title"},
        {1.5f, [](Model &m) { m.connection = Connection::reconnecting; }, 0, "12-reconnecting"},
        {0.5f, [](Model &m) { m.connection = Connection::ready; }},
        // Control Room: Options opens it over Now Playing.
        {1.0f, nullptr, action_bit(Action::touch), "13-control-room"},
        {0.4f, nullptr, action_bit(Action::confirm)},          // into Playback's rows
        {0.3f, nullptr, 0, nullptr, Direction::down},           // Volume
        {0.25f, nullptr, 0, nullptr, Direction::left},          // 9
        {0.25f, nullptr, 0, nullptr, Direction::left},          // 8
        {1.3f, nullptr, 0, "14-volume-saved"},                  // the toast says Saved
        {0.3f, nullptr, action_bit(Action::back)},              // back to the rail
        {0.6f, nullptr, 0, "15-device", Direction::down},       // Device
        {0.3f, nullptr, action_bit(Action::confirm)},           // into its row
        {0.8f, nullptr, action_bit(Action::confirm), "16-rename-keyboard"},
        {0.5f, nullptr, action_bit(Action::back)},              // cancel the sheet
        {0.6f, nullptr, 0, "16b-timezone-row", Direction::down}, // Time zone
        {0.8f, nullptr, action_bit(Action::confirm), "16c-timezone-list"},
        {0.12f, nullptr, action_bit(Action::page_next)},
        {0.7f, nullptr, action_bit(Action::page_next), "16d-timezone-scrolled"},
        {0.5f, nullptr, action_bit(Action::back)},              // cancel the list
        {0.3f, nullptr, 0, nullptr, Direction::up},             // back to Name
        {0.3f, nullptr, action_bit(Action::back)},              // back to the rail
        {0.3f, nullptr, 0, nullptr, Direction::down},           // Account
        {0.6f, nullptr, 0, "17-account", Direction::none},
        {0.3f, nullptr, 0, nullptr, Direction::down},           // Display
        {0.3f, nullptr, 0, nullptr, Direction::down},           // About
        {0.8f, nullptr, 0, "18-about"},
        {0.3f, nullptr, 0, nullptr, Direction::down},           // Quit
        {0.3f, nullptr, action_bit(Action::confirm)},           // into its row
        {0.8f, nullptr, action_bit(Action::confirm), "19-quit-dialog"},
        {0.5f, nullptr, action_bit(Action::back)},              // Cancel
        {0.3f, nullptr, action_bit(Action::back)},              // rail
        {0.8f, nullptr, action_bit(Action::back), "20-back-to-playing"},
        // Holding the touchpad: the quit ring fills; let go before 6 s and it fades.
        {1.5f, nullptr, 0, "21-hold-to-quit", Direction::none, action_bit(Action::touch)},
        {0.15f, nullptr, 0, "21b-hold-draining"},
        {0.8f, nullptr, 0, "21c-hold-gone"},
    };
    return beats;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir>\n", argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string out = argv[2];
    if (!open_context())
    {
        std::fprintf(stderr, "no OpenGL 4.5 context (Mesa surfaceless EGL)\n");
        return 1;
    }
    gfx::set_glsl_prefix("#version 450 core\n");

    gfx::Renderer renderer;
    gfx::Font regular, semibold, display, mono;
    ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, assets + "/fonts/inter-regular.huifont", regular, fonts.regular) ||
        !load_font(renderer, assets + "/fonts/inter-semibold.huifont", semibold, fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/montserrat-medium.huifont", display, fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", mono, fonts.mono))
        return 1;
    display.add_fallback(&semibold, fonts.semibold.texture); // Montserrat has no Greek
    gfx::Shaper shaper;
    {
        std::string data;
        if (!save::read_file(assets + "/fonts/NotoSansMalayalam-Regular.ttf", &data) ||
            !shaper.add_face(std::move(data)))
            return 1;
    }
    shaper.per_frame = 1 << 20; // only captured frames are drawn
    shaper.attach(renderer.batch());
    for (gfx::Font *font : {&regular, &semibold, &display, &mono})
        font->set_shaper(&shaper);

    for (int i = 0; i < 2; ++i)
    {
        std::string data;
        const std::string path = assets + "/../tests/covers/" + (i ? "b.jpg" : "a.jpg");
        int w = 0, h = 0, n = 0;
        if (save::read_file(path, &data))
            if (std::uint8_t *px = stbi_load_from_memory(
                    reinterpret_cast<const std::uint8_t *>(data.data()), static_cast<int>(data.size()),
                    &w, &h, &n, 4))
            {
                g_covers[i].texture = renderer.batch().create_texture(w, h, px);
                g_covers[i].width = w;
                g_covers[i].height = h;
                g_covers[i].palette = screens::style::palette_from_pixels(px, w, h);
                stbi_image_free(px);
            }
        if (!g_covers[i].texture)
            std::fprintf(stderr, "cannot load %s\n", path.c_str());
    }

    gfx::Canvas target;
    if (!target.create(kWidth, kHeight, 1))
        return 1;

    screens::Settings settings;
    screens::Context context{fonts, settings};
    context.art = preview_art;
    context.console_utc_minutes = 330; // a console set to India
    for (int i = 0; i < 4; ++i)
    {
        std::string data;
        int w = 0, h = 0, n = 0;
        if (!save::read_file(assets + "/logo/" + screens::kLogoFiles[i], &data))
            continue;
        if (std::uint8_t *px = stbi_load_from_memory(reinterpret_cast<const std::uint8_t *>(data.data()),
                                                     static_cast<int>(data.size()), &w, &h, &n, 4))
        {
            context.logo[i] = renderer.batch().create_texture(w, h, px);
            stbi_image_free(px);
        }
    }
    g_renderer = &renderer;
    g_fixture = assets + "/../tests/home-fixture";
    std::remove((g_fixture + "/missing-covers.txt").c_str());
    g_home = load_home_fixture(g_fixture);
    load_rows_fixture(g_fixture);
    // Letters beyond ASCII: baked accents, Greek and Cyrillic; shaped Malayalam.
    const char *const samples[][2] = {
        {"Amsham - അംശം", "Aksomaniac"},
        {"സ്ത്രീ കേരളം മലയാളം പാട്ട് ഒരു വളരെ നീണ്ട പേര്", "Sushin Shyam"},
        {"Dancing In The Flames - Live from São Paulo", "Ludwig Göransson, Μάριος Φραγκούλης, Жанна"},
    };
    for (std::size_t i = 0; i < std::size(samples) && i < g_rows.size(); ++i)
    {
        g_rows[i].title = samples[i][0];
        g_rows[i].artists = samples[i][1];
    }
    load_library_fixture(g_fixture);
    std::printf("library fixture: %zu items, artist %s\n", g_library ? g_library->items.size() : 0,
                g_artist ? g_artist->card.title.c_str() : "none");
    std::printf("home fixture: %s\n", g_home ? "loaded" : "none (tests/home-fixture/home.pb)");
    screens::Shell shell(context);
    Model model;
    model.home = g_home;
    model.library = g_library;
    model.username = "abhi245y";
    model.about = {"01.000.000", "ps5@555ea3c", "192.168.1.2"};
    std::vector<unsigned char> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    constexpr float kDt = 1.0f / 60.0f;

    for (const Beat &beat : script())
    {
        if (beat.change)
            beat.change(model);
        const int frames = static_cast<int>(beat.seconds / kDt + 0.5f);
        for (int i = 0; i < frames; ++i)
        {
            InputFrame input;
            input.connected = true;
            input.held = beat.hold;
            if (i == 0)
            {
                input.pressed = beat.press;
                input.nav = beat.nav;
            }
            ui::Feedback feedback;
            std::vector<Command> commands;
            ::hui::sys::g_preview_us += static_cast<std::int64_t>(kDt * 1e6f);
            shell.update(model, input, kDt, feedback, commands);
            for (const screens::Request &request : context.requests)
                if (request.kind == screens::Request::Kind::open && request.card.uri.rfind("spotify:artist:", 0) == 0)
                    model.artist = g_artist;
                else if (request.kind == screens::Request::Kind::open)
                {
                    auto page = std::make_shared<screens::PlaylistPage>();
                    page->card = request.card;
                    page->tracks = g_rows;
                    page->loading = false;
                    model.page = page;
                }
            context.requests.clear();
            // Pretend the session follows: playback advances while playing.
            if (model.has_track && !model.paused && model.connection == Connection::ready)
                model.position_ms += static_cast<std::uint32_t>(kDt * 1000.0f);
            for (Command c : commands)
                if (c == Command::play_pause)
                    model.paused = !model.paused;
        }
        if (!beat.capture)
            continue;
        target.bind();
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        shell.compose(renderer, model);
        shaper.upload(renderer.batch());
        renderer.present(target.framebuffer(), kWidth, kHeight);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer());
        glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const std::string path = out + "/" + beat.capture + ".png";
        stbi_flip_vertically_on_write(1);
        const bool ok = stbi_write_png(path.c_str(), kWidth, kHeight, 4, pixels.data(), kWidth * 4) != 0;
        std::printf("%s %s (draws=%zu shapes=%zu gl=0x%x)\n", ok ? "wrote" : "FAILED", path.c_str(),
                    renderer.last_draw_calls(), renderer.last_instances(), glGetError());
    }
    return 0;
}
