// ps5-spotify - The app: display, input and drawing on the main thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include <GL/glcorearb.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "X509Bundle.h"
#include "BellLogger.h"
#include "app/config.hpp"
#include "app/controller.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/renderer.hpp"
#include "platform/pad.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

namespace app
{

namespace
{

using namespace hui;

constexpr const char *kFonts = "/app0/assets/fonts/";

bool load_font(gfx::Renderer &renderer, const char *name, gfx::Font &font, ui::FontRef &ref)
{
    std::string data;
    if (!save::read_file(std::string(kFonts) + name, &data) || !font.load(data))
    {
        sys::log("[APP] font %s failed: %s", name, font.error().c_str());
        return false;
    }
    ref.font = &font;
    ref.texture = renderer.batch().create_font_texture(font);
    return true;
}

// Without the root bundle bell would accept any TLS certificate; it ships
// with the app, so a failure here is a broken install, not a retryable error.
bool init_certificates()
{
    std::string bundle = read_file(paths::kCertBundle);
    try
    {
        bell::X509Bundle::init(reinterpret_cast<const uint8_t *>(bundle.data()), bundle.size());
        return true;
    }
    catch (const std::exception &e)
    {
        sys::log("[APP] certificate bundle unusable: %s", e.what());
        return false;
    }
}

// Ends the app the way the system expects: stop drawing, then ask it to close
// the title (exit() would end in the crash reporter).
[[noreturn]] void quit(ps5::Display &display)
{
    sys::log("[APP] quit requested");
    display.close();
    sys::quit();
}

void draw(gfx::DrawList &list, const ui::Fonts &fonts, const Status &status)
{
    const gfx::Color white = gfx::Color::rgb(0xffffff);
    const gfx::Color dim = gfx::Color::rgb(0xffffff, 0.6f);
    ui::text(list, fonts.semibold, "Spotify", 960, 420, 72, white, gfx::Align::center);
    ui::text(list, fonts.regular, state_name(status.state), 960, 520, 40, white,
             gfx::Align::center);
    if (!status.message.empty())
        ui::text(list, fonts.regular, status.message, 960, 580, 30, dim, gfx::Align::center);
    if (!status.now_playing.title.empty())
        ui::text(list, fonts.regular,
                 status.now_playing.artist + " - " + status.now_playing.title, 960, 640, 34,
                 white, gfx::Align::center);
    const char *hint = status.state == State::Error ? "X Retry      OPTIONS Quit" : "OPTIONS Quit";
    ui::text(list, fonts.regular, hint, 960, 1000, 26, dim, gfx::Align::center);
}

} // namespace

void App::run()
{
    sys::log("[APP] start");
    bell::setDefaultLogger();

    ps5::Display display;
    if (!display.open(1920, 1080))
    {
        sys::log("[APP] fatal: display open failed");
        sys::park();
    }
    gfx::Renderer renderer;
    gfx::Font regular_font, semibold_font;
    ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, "inter-regular.huifont", regular_font, fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", semibold_font, fonts.semibold))
    {
        sys::log("[APP] fatal: renderer or fonts failed");
        sys::park();
    }

    Controller controller;
    if (!init_certificates() || !controller.start())
    {
        sys::log("[APP] fatal: could not start");
        sys::park();
    }

    platform::Pad pad;
    pad.open();
    gfx::DrawList list;
    gfx::BackdropSpec sky;
    sky.mode = gfx::BackdropMode::aurora;
    sky.colors[0] = gfx::Color::rgb(0x07120c);
    sky.colors[1] = gfx::Color::rgb(0x0d2418);
    sky.colors[2] = gfx::Color::rgb(0x1db954);
    sky.colors[3] = gfx::Color::rgb(0x1a6b47);
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t frames = 0;

    for (;;)
    {
        if (!pad.is_open())
            pad.open();
        std::uint32_t pressed = pad.poll_pressed();
        Status status = controller.status();
        if (pressed & pad_bits::kOptions)
            quit(display);
        if ((pressed & pad_bits::kCross) && status.state == State::Error)
            controller.send(Command::Retry);

        list.clear();
        draw(list, fonts, status);
        sky.time = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        renderer.begin();
        renderer.backdrop(sky);
        renderer.draw(list);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[APP] fatal: swap failed frame=%llu",
                     static_cast<unsigned long long>(frames));
            sys::park();
        }
        if (++frames == 1)
            sys::log("[APP] first frame; splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
    }
}

} // namespace app
