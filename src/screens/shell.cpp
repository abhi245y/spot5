// ps5-spotify - Chooses the screen, moves between screens, owns the overlays.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The moves are ps5-homebrew-ui's: the iris and through-black moves from
// src/concepts/boot.cpp, the slide from src/app/shell_render.cpp.

#include "screens/shell.hpp"

#include <cmath>

#include "gfx/renderer.hpp"
#include "platform/ps5/system.hpp"
#include "screens/home.hpp"
#include "screens/launch.hpp"
#include "screens/now_playing.hpp"
#include "screens/style.hpp"
#include "ui/theme.hpp"

namespace screens
{

namespace
{

using namespace style;
constexpr float kIrisRadius = 1160.0f; // past the corners of the screen
constexpr float kSlide = 90.0f;        // how far a screen slides on a switch

// The frosted Acrylic theme, in the app's green.
ui::Theme dialog_theme()
{
    ui::Theme theme = ui::default_theme();
    for (const ui::Theme &t : ui::themes())
        if (std::string_view(t.id) == "acrylic")
            theme = t;
    theme.primary = kGreen;
    theme.accent = kGreen;
    theme.on_primary = Color::rgb(0x03140a);
    theme.surface = Color::rgb(0x0c1f15, 0.55f);
    return theme;
}

} // namespace

Shell::Shell(Context &context)
    : context_(context), launch_(make_launch(context)), home_(make_home(context)),
      playing_(make_now_playing(context))
{
    quit_.style.theme = dialog_theme();
    quit_.style.reduced_motion = context.reduced_motion;
}

Shell::~Shell() = default;

Screen &Shell::screen(Id id)
{
    switch (id)
    {
    case Id::launch:
        return *launch_;
    case Id::home:
        return *home_;
    case Id::playing:
        return *playing_;
    }
    return *launch_;
}

Shell::Id Shell::wanted(const Model &model) const
{
    if (model.connection == Connection::error)
        return Id::launch; // the loader shows the error and the retry
    if (current_ == Id::launch && !launch_finished(*launch_))
        return Id::launch; // let the loader finish what it shows
    if (model.connection == Connection::connecting)
        return Id::launch;
    return model.has_track ? Id::playing : Id::home;
}

void Shell::go(Id next, Move move, float seconds)
{
    static const char *kNames[] = {"launch", "home", "playing"};
    static const char *kMoves[] = {"none", "iris", "black", "slide"};
    sys::log("[UI] screen: %s -> %s (%s)", kNames[static_cast<int>(current_)],
             kNames[static_cast<int>(next)], kMoves[static_cast<int>(move)]);
    if (context_.reduced_motion)
        seconds = std::min(seconds, 0.4f);
    previous_ = current_;
    current_ = next;
    move_ = move;
    transition_.start(seconds);
    screen(next).enter();
}

void Shell::update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                   std::vector<Command> &commands)
{
    clock_ += dt;
    transition_.update(dt);

    const Id next = wanted(model);
    if (next != current_)
    {
        if (current_ == Id::launch)
        {
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.5f, 0.12f);
            go(next, Move::iris, 1.1f);
        }
        else if (next == Id::launch)
            go(next, Move::black, 0.9f);
        else
            go(next, Move::slide, 0.6f);
    }

    // The dialog takes every input while it is open.
    if (quit_.is_open())
    {
        const ui::Event event = quit_.handle(input, feedback);
        if (event == ui::Event::activated && quit_.choice() == 1)
            commands.push_back(Command::quit);
    }
    else if (input.is_pressed(Action::menu))
    {
        quit_.open({ui::StatusKind::none,
                    "Quit Spotify?",
                    "The music stops and the app closes. You can also close it any time with "
                    "the PS button.",
                    {{"Keep listening"}, {"Quit", ui::ButtonKind::primary, true}},
                    0},
                   feedback);
    }
    else
    {
        screen(current_).update(model, input, dt, feedback, commands);
    }
    // The screen that is leaving keeps animating while it is still visible.
    if (transition_.running && previous_ != current_)
    {
        InputFrame none;
        std::vector<Command> ignored;
        ui::Feedback quiet;
        screen(previous_).update(model, none, dt, quiet, ignored);
    }
    quit_.update(dt);

    reconnecting_.target = model.connection == Connection::reconnecting ? 1.0f : 0.0f;
    reconnecting_.update(dt, 10.0f);
}

void Shell::record(Frame &frame, Id id, const Model &model, float opacity, float dx, float scale)
{
    frame.reset();
    frame.glass_texture = glass_texture_;
    if (opacity < 1.0f || dx != 0.0f || scale != 1.0f)
    {
        for (gfx::DrawList *list : {&frame.scene, &frame.overlay})
        {
            list->push_opacity(opacity);
            list->push_transform(scale, kCentreX, kCentreY, dx, 0.0f);
        }
    }
    screen(id).draw(model, frame);
}

void Shell::compose(gfx::Renderer &renderer, const Model &model)
{
    renderer.begin();
    glass_texture_ = renderer.glass_texture();
    Frame &now = frames_[0];
    Frame &old = frames_[1];
    const bool moving = transition_.running && previous_ != current_;
    const float t = moving ? transition_.progress() : 1.0f;
    const Move move = moving ? (context_.reduced_motion && move_ == Move::iris ? Move::black : move_)
                             : Move::none;

    switch (move)
    {
    case Move::none:
        record(now, current_, model, 1.0f, 0.0f, 1.0f);
        renderer.backdrop(now.backdrop);
        renderer.draw(now.scene);
        if (now.glass)
            renderer.glass();
        renderer.draw(now.overlay);
        renderer.backdrop(now.post);
        break;
    case Move::black:
    case Move::iris:
    {
        // Covered at the midpoint: that is where the screens change hands.
        const Id shown = t < 0.5f ? previous_ : current_;
        record(now, shown, model, 1.0f, 0.0f, 1.0f);
        renderer.backdrop(now.backdrop);
        renderer.draw(now.scene);
        renderer.draw(now.overlay);
        renderer.backdrop(now.post);
        cover_.clear();
        const gfx::Rect full{0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight};
        if (move == Move::black)
        {
            const float cover = t < 0.5f ? tween::smoothstep(t * 2.0f)
                                         : 1.0f - tween::smoothstep((t - 0.5f) * 2.0f);
            cover_.rounded_rect(full, 0, kNight.with_alpha(cover));
        }
        else
        {
            // One very thick ring is a screen of black with a round hole.
            const float open = t < 0.5f ? 1.0f - tween::cubic_in_out(t / 0.42f)
                                        : tween::cubic_in_out((t - 0.5f) * 2.0f);
            const float hole = kIrisRadius * open;
            cover_.ring(kCentreX, kCentreY, kIrisRadius, kIrisRadius - hole, kNight);
            if (hole > 6.0f)
                cover_.ring(kCentreX, kCentreY, hole + 4.0f, 4.0f,
                             kGreen.with_alpha(0.55f * (1.0f - open)));
        }
        renderer.draw(cover_);
        break;
    }
    case Move::slide:
    {
        // The old screen fades and slides away while the new one arrives from
        // the other side; their backdrops cross-fade underneath.
        const float out = 1.0f - tween::smoothstep(t * 1.7f);
        const float in = tween::smoothstep((t - 0.2f) / 0.8f);
        const float slide = context_.reduced_motion ? 0.0f : kSlide;
        const float travel = tween::quint_out(t);
        const float direction = current_ == Id::playing ? 1.0f : -1.0f;
        record(old, previous_, model, out, -direction * slide * travel, 1.0f - 0.03f * travel);
        record(now, current_, model, in, direction * slide * (1.0f - travel),
               0.97f + 0.03f * travel);
        renderer.backdrop(old.backdrop);
        renderer.backdrop(now.backdrop, tween::smoothstep(t));
        renderer.draw(old.scene);
        renderer.draw(now.scene);
        renderer.draw(now.overlay);
        renderer.backdrop(old.post, 1.0f - t);
        renderer.backdrop(now.post, t);
        break;
    }
    }

    // Overlays: frosted over whatever the screen drew.
    chrome_.clear();
    if (quit_.visible())
    {
        renderer.glass();
        draw_overlays(chrome_, model, renderer.glass_texture());
    }
    else
    {
        draw_overlays(chrome_, model, 0);
    }
    renderer.draw(chrome_);
}

void Shell::draw_overlays(gfx::DrawList &list, const Model &model, std::uint32_t glass)
{
    const ui::Fonts &fonts = context_.fonts;

    // "Reconnecting": a pill in the top-right corner while cspot retries.
    const float shown = tween::clamp01(reconnecting_.value);
    if (shown > 0.01f && current_ != Id::launch)
    {
        const char *label = "Reconnecting\xE2\x80\xA6";
        const float w = 74.0f + fonts.semibold.measure(label, 22);
        const gfx::Rect pill{kRight - w + 30.0f * (1.0f - shown), 72, w, 52};
        list.push_opacity(shown);
        list.shadow({pill.x, pill.y + 8, pill.w, pill.h}, 26, 24, Color::rgb(0x000000, 0.35f));
        list.rounded_rect(pill, 26, kNight.with_alpha(0.7f));
        list.bordered_rect(pill, 26, kClear, 1.5f, kInk.with_alpha(0.16f));
        list.arc(pill.x + 32, pill.cy(), 10, 3, clock_ * 5.5f, 2.2f, kGreen);
        ui::text(list, fonts.semibold, label, pill.x + 54, pill.cy() + 8, 22, kInk);
        list.pop_opacity();
        (void)model;
    }

    if (quit_.visible())
    {
        ui::Canvas canvas{list, fonts, glass, clock_};
        quit_.draw(canvas);
    }
}

} // namespace screens
