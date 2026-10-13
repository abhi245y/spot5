// ps5-spotify - The app: display, input and drawing on the main thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include <GL/glcorearb.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "BellLogger.h"
#include "X509Bundle.h"
#include "app/config.hpp"
#include "app/controller.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "screens/shell.hpp"
#include "spotify/cover.hpp"
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

screens::Connection connection(State state)
{
    switch (state)
    {
    case State::Connecting:
        return screens::Connection::connecting;
    case State::Ready:
        return screens::Connection::ready;
    case State::Reconnecting:
        return screens::Connection::reconnecting;
    case State::Error:
        return screens::Connection::error;
    }
    return screens::Connection::connecting;
}

// What the screens show, from the controller's status.
screens::Model model_from(const Status &status)
{
    screens::Model model;
    model.connection = connection(status.state);
    model.step = status.step;
    model.message = status.message;
    const NowPlaying &now = status.now_playing;
    model.has_track = !now.title.empty();
    model.track.id = now.track_id;
    model.track.title = now.title;
    model.track.artist = now.artist;
    model.track.album = now.album;
    model.track.duration_ms = now.duration_ms;
    model.track.image_url = now.image_url;
    model.paused = now.paused;
    model.position_ms = std::min(now.position_ms, now.duration_ms);
    return model;
}

// Covers as textures. The last few stay alive: a cover that is sliding out
// during a track change is still being drawn.
class Covers
{
  public:
    explicit Covers(gfx::Renderer &renderer) : renderer_(renderer) {}
    ~Covers()
    {
        for (const Entry &e : textures_)
            glDeleteTextures(1, &e.texture);
    }

    // Fills in the model's cover once it is loaded; asks for it otherwise.
    void apply(screens::Track &track)
    {
        loader_.request(track.image_url);
        if (std::optional<spotify::Cover> cover = loader_.take())
        {
            Entry e{cover->url,
                    renderer_.batch().create_texture(cover->width, cover->height, cover->rgba.data()),
                    cover->palette};
            textures_.push_back(e);
            if (textures_.size() > 3)
            {
                glDeleteTextures(1, &textures_.front().texture);
                textures_.pop_front();
            }
        }
        for (const Entry &e : textures_)
        {
            if (!track.image_url.empty() && e.url == track.image_url)
            {
                track.art = e.texture;
                track.palette = e.palette;
                track.has_palette = true;
            }
        }
    }

  private:
    struct Entry
    {
        std::string url;
        std::uint32_t texture;
        screens::style::AlbumPalette palette;
    };
    gfx::Renderer &renderer_;
    spotify::CoverLoader loader_;
    std::deque<Entry> textures_;
};

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
    gfx::Font regular, semibold, display_face, mono;
    ui::Fonts fonts;
    if (!renderer.init() || !load_font(renderer, "inter-regular.huifont", regular, fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", semibold, fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", display_face, fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", mono, fonts.mono))
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

    Covers covers(renderer);
    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    screens::Context context{fonts, false};
    screens::Shell shell(context);
    PadSample samples[64];
    std::int64_t last = sys::monotonic_us();
    std::uint64_t frames = 0;

    for (;;)
    {
        // Animation time is start-to-start, clamped so a hitch cannot
        // teleport anything (as in ps5-homebrew-ui's main loop).
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last) / 1e6f;
        last = now;
        dt = std::min(dt, 0.05f);

        if (!pad.is_open())
            pad.open();
        const std::size_t count = pad.read(samples);
        const InputFrame input =
            tracker.update(std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));

        screens::Model model = model_from(controller.status());
        if (model.has_track)
            covers.apply(model.track);
        ui::Feedback feedback;
        std::vector<screens::Command> commands;
        shell.update(model, input, dt, feedback, commands);
        for (screens::Command command : commands)
        {
            if (command == screens::Command::quit)
                quit(display);
            controller.send(command);
        }
        // UI sounds are not played yet (the Player owns the audio port);
        // rumble is.
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);

        shell.compose(renderer, model);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!display.swap())
        {
            sys::log("[APP] fatal: swap failed frame=%llu", static_cast<unsigned long long>(frames));
            sys::park();
        }
        if (++frames == 1)
            sys::log("[APP] first frame; splash hidden=%d", sys::hide_splash_screen() ? 1 : 0);
    }
}

} // namespace app
