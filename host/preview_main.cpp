// ps5-spotify - PC preview: renders the app's screens to PNG files.
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

#include "core/save_file.hpp"
#include "gfx/canvas.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "screens/shell.hpp"
#include "third_party/stb/stb_image.h"
#include "ui/fonts.hpp"

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
// by `change` at the start and `press` held for the first frame; then save a
// picture named `capture` if there is one.
struct Beat
{
    float seconds;
    std::function<void(Model &)> change;
    std::uint32_t press = 0;
    const char *capture = nullptr;
    Direction nav = Direction::none;
};

void playing(Model &m, const char *id, const char *title, const char *artist, const char *album,
             std::uint32_t at_ms, std::uint32_t length_ms)
{
    m.connection = Connection::ready;
    m.step = Step::done;
    m.has_track = true;
    m.track = {id, title, artist, album, length_ms};
    m.position_ms = at_ms;
    m.paused = false;
}

// Test covers (tests/covers, made with ffmpeg): textures and palettes.
struct TestCover
{
    std::uint32_t texture = 0;
    screens::style::AlbumPalette palette;
};
TestCover g_covers[2];

void with_cover(Model &m, int which)
{
    m.track.art = g_covers[which].texture;
    m.track.palette = g_covers[which].palette;
    m.track.has_palette = true;
}

const std::vector<Beat> &script()
{
    static const std::vector<Beat> beats = {
        {1.4f, [](Model &m) { m.step = Step::certificates; }, 0, "01-launch-title"},
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
        {0.3f, [](Model &m) { playing(m, "a", "Aashiqana", "Chaar Diwaari", "Aashiqana", 47000, 214000); },
         0, "07-home-to-playing"},
        {0.4f, nullptr, 0, "08a-cover-pending"},
        {0.35f, [](Model &m) { with_cover(m, 0); }, 0, "08b-cover-fading-in"},
        {2.5f, nullptr, 0, "08-playing"},
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
    screens::Shell shell(context);
    Model model;
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
            if (i == 0)
            {
                input.pressed = beat.press;
                input.nav = beat.nav;
            }
            ui::Feedback feedback;
            std::vector<Command> commands;
            shell.update(model, input, kDt, feedback, commands);
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
