// ps5-spotify - The app: display, input and drawing on the main thread.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/app.hpp"

#include <GL/glcorearb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

#include "BellLogger.h"
#include "nlohmann/json.hpp"
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
#include "spotify/home.hpp"
#include "ui/fonts.hpp"

extern "C" std::size_t hui_heap_capacity(void); // runtime/app_heap.c
extern "C" void hui_heap_stats(std::size_t *live, std::size_t *peak, std::size_t *blocks,
                               std::size_t *failures);

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
    case State::Pairing:
        return screens::Connection::pairing;
    case State::Ready:
        return screens::Connection::ready;
    case State::Reconnecting:
        return screens::Connection::reconnecting;
    case State::Error:
        return screens::Connection::error;
    }
    return screens::Connection::connecting;
}

// The console's address on the local network: "connect" a UDP socket toward
// a public address (nothing is sent) and ask which local address it chose.
// Only libkernel socket calls, which a native title can use.
std::string local_address()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return {};
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    sockaddr_in me{};
    socklen_t length = sizeof(me);
    std::string address;
    char text[INET_ADDRSTRLEN] = {};
    if (connect(fd, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0 &&
        getsockname(fd, reinterpret_cast<sockaddr *>(&me), &length) == 0 &&
        inet_ntop(AF_INET, &me.sin_addr, text, sizeof(text)))
        address = text;
    close(fd);
    return address;
}

#define SPT_STRING2(x) #x
#define SPT_STRING(x) SPT_STRING2(x)

screens::About about_facts()
{
    screens::About about;
    auto param = nlohmann::json::parse(read_file("/app0/sce_sys/param.json"), nullptr, false);
    if (!param.is_discarded())
        about.version = param.value("contentVersion", std::string());
#ifdef CSPOT_REV
    // Built in as branch_commit (the build's defines allow no other characters).
    about.engine = SPT_STRING(CSPOT_REV);
    if (const auto at = about.engine.rfind('_'); at != std::string::npos)
        about.engine[at] = '@';
#endif
    about.address = local_address();
    return about;
}

// What the screens show, from the controller's status.
screens::Model model_from(const Status &status)
{
    screens::Model model;
    model.connection = connection(status.state);
    model.step = status.step;
    model.message = status.message;
    model.username = status.username;
    model.home = status.home;
    model.home_loading = status.home_loading;
    model.page = status.page;
    model.queue = status.queue;
    model.artwork_done = status.artwork_done;
    model.artwork_total = status.artwork_total;
    model.notice = status.notice;
    model.notice_id = status.notice_id;
    model.busy = status.busy;
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
        const std::string url = spotify::home::larger_cover(track.image_url);
        loader_.request(url);
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
            if (!url.empty() && e.url == url)
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
    spotify::CoverLoader loader_{1, 1, paths::kCovers};
    std::deque<Entry> textures_;
};

// Home's covers as textures, by URL. Least recently drawn ones go first once
// there are too many; a texture drawn in the last frames is never deleted
// (the renderer still holds this frame's draw lists).
class CardArt
{
  public:
    explicit CardArt(gfx::Renderer &renderer) : renderer_(renderer) {}
    ~CardArt()
    {
        for (const auto &[url, entry] : entries_)
            glDeleteTextures(1, &entry.texture);
    }

    // Once per frame, before the screens draw.
    void pump()
    {
        ++frame_;
        // A few uploads per frame keep scrolling smooth.
        for (int i = 0; i < 4; ++i)
        {
            std::optional<spotify::Cover> cover = loader_.take();
            if (!cover)
                break;
            Entry &entry = entries_[cover->url];
            if (entry.texture)
                glDeleteTextures(1, &entry.texture);
            entry.texture = renderer_.batch().create_texture(cover->width, cover->height, cover->rgba.data());
            entry.palette = cover->palette;
            entry.used = frame_;
            entry.bytes = cover->rgba.size();
            bytes_ += entry.bytes;
        }
        while (bytes_ > kMaxBytes)
        {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it)
                if (it->second.used + 2 < frame_ && (oldest == entries_.end() || it->second.used < oldest->second.used))
                    oldest = it;
            if (oldest == entries_.end())
                break; // everything is on screen
            glDeleteTextures(1, &oldest->second.texture);
            bytes_ -= oldest->second.bytes;
            entries_.erase(oldest);
        }
    }

    screens::Art get(const std::string &url)
    {
        const auto found = entries_.find(url);
        if (found == entries_.end())
        {
            loader_.request(url);
            return {};
        }
        found->second.used = frame_;
        return {found->second.texture, found->second.palette};
    }

    // Asks for every cover of a page (the launch screen waits for them).
    // Returns how many of `urls` are loaded.
    std::size_t preload(const std::vector<std::string> &urls)
    {
        std::size_t ready = 0;
        for (const std::string &url : urls)
        {
            if (entries_.count(url) || loader_.given_up(url))
                ++ready; // a cover that will not come does not hold the launch
            else
                loader_.request(url);
        }
        return ready;
    }

  private:
    // Every Home card at 300 px is ~70 MB; big covers (1000 px, 4 MB each)
    // are evicted first by age once the budget is reached.
    static constexpr std::size_t kMaxBytes = 160u << 20;
    struct Entry
    {
        std::uint32_t texture = 0;
        screens::style::AlbumPalette palette;
        std::uint64_t used = 0;
        std::size_t bytes = 0;
    };
    gfx::Renderer &renderer_;
    // Long queue (the whole page at launch), six downloads at a time (each
    // is a new TLS connection, so more in flight is what makes it quick),
    // kept on disk for the next launch.
    spotify::CoverLoader loader_{512, 6, paths::kCovers};
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t frame_ = 0;
    std::size_t bytes_ = 0;
};

} // namespace

void App::run()
{
    sys::log("[APP] start");
    // The first allocation made the heap: say how big the console let it be.
    sys::log("[APP] heap %zu MB", hui_heap_capacity() >> 20);
    bell::setDefaultLogger();

    // The display opens at the saved resolution, falling back to 1080p
    // (everything is drawn at the physical size, so 4K is natively sharp).
    screens::Settings settings = load_settings();
    static constexpr int kModes[3][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
    const int *mode = kModes[std::clamp(settings.resolution, 0, 2)];
    ps5::Display display;
    if (!display.open(mode[0], mode[1]) && (settings.resolution == 0 || !display.open(1920, 1080)))
    {
        sys::log("[APP] fatal: display open failed");
        sys::park();
    }
    sys::log("[APP] display %dx%d (asked %dx%d)", display.width(), display.height(), mode[0], mode[1]);
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
    controller.apply(settings); // before start: the first login uses them
    if (!init_certificates() || !controller.start())
    {
        sys::log("[APP] fatal: could not start");
        sys::park();
    }

    Covers covers(renderer);
    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    CardArt card_art(renderer);
    screens::Context context{fonts, settings};
    context.art = [&card_art](const std::string &url) { return card_art.get(url); };
    const screens::About about = about_facts();
    std::int64_t save_at = 0; // when to write settings.json; 0: nothing pending
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
        model.device_name = settings.device_name;
        model.about = about;
        if (model.has_track)
            covers.apply(model.track);
        card_art.pump();
        // Launch: every Home cover, before the loader hands over.
        if (model.step == screens::Step::artwork && model.home)
        {
            std::vector<std::string> urls;
            for (const screens::Shelf &shelf : model.home->shelves)
                for (const screens::Card &card : shelf.cards)
                    if (!card.image.empty())
                        urls.push_back(card.image);
            controller.report_artwork(static_cast<int>(card_art.preload(urls)), static_cast<int>(urls.size()));
        }
        ui::Feedback feedback;
        std::vector<screens::Command> commands;
        shell.update(model, input, dt, feedback, commands);
        for (screens::Command command : commands)
        {
            if (command == screens::Command::quit)
                quit(display);
            controller.send(command);
        }
        for (const screens::Request &request : context.requests)
            controller.request(request);
        context.requests.clear();
        // Settings changed in Control Room: apply now, save once the user
        // stops (a held slider writes the file once, not per step).
        if (context.settings_changed)
        {
            context.settings_changed = false;
            controller.apply(settings);
            save_at = now + 750000;
        }
        if (save_at != 0 && now >= save_at)
        {
            save_at = 0;
            if (!save_settings(settings))
                sys::log("[APP] could not save %s", paths::kSettings.c_str());
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
        if (frames % 3600 == 0) // about once a minute: how much memory the app really uses
        {
            std::size_t live = 0, peak = 0, failures = 0;
            hui_heap_stats(&live, &peak, nullptr, &failures);
            sys::log("[APP] heap live %zu MB, peak %zu MB of %zu MB, failures %zu", live >> 20, peak >> 20,
                     hui_heap_capacity() >> 20, failures);
        }
    }
}

} // namespace app
