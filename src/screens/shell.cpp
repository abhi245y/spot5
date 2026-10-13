// ps5-spotify - Chooses the screen, moves between screens, owns the overlays.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The moves are ps5-homebrew-ui's: the iris and through-black moves from
// src/concepts/boot.cpp, the slide from src/app/shell_render.cpp.

#include "screens/shell.hpp"

#include <cmath>

#include "gfx/renderer.hpp"
#include "platform/ps5/system.hpp"
#include "screens/control_room.hpp"
#include "screens/home.hpp"
#include "screens/launch.hpp"
#include "screens/now_playing.hpp"
#include "screens/artist.hpp"
#include "screens/library.hpp"
#include "screens/playlist.hpp"
#include "screens/style.hpp"

namespace screens
{

namespace
{

using namespace style;
constexpr float kIrisRadius = 1160.0f; // past the corners of the screen
constexpr float kSlide = 90.0f;        // how far a screen slides on a switch

} // namespace

Shell::Shell(Context &context)
    : context_(context), launch_(make_launch(context)), home_(make_home(context)),
      playing_(make_now_playing(context)), menu_(make_control_room(context)),
      page_(make_playlist(context)), library_(make_library(context)), artist_(make_artist(context))
{
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
    case Id::menu:
        return *menu_;
    case Id::page:
        return *page_;
    case Id::library:
        return *library_;
    case Id::artist:
        return *artist_;
    }
    return *launch_;
}

Shell::Id Shell::wanted(const Model &model) const
{
    if (menu_open_)
        return Id::menu;
    if (model.connection == Connection::error || model.connection == Connection::pairing)
        return Id::launch; // the loader shows the error and the retry, or how to pair
    if (current_ == Id::launch && !launch_finished(*launch_))
        return Id::launch; // let the loader finish what it shows
    if (model.connection == Connection::connecting)
        return Id::launch;
    if (model.has_track && playing_open_)
        return Id::playing;
    if (!stack_.empty())
        return stack_.back();
    return library_tab_ ? Id::library : Id::home;
}

void Shell::go(Id next, Move move, float seconds)
{
    static const char *kNames[] = {"launch", "home", "playing", "control room", "page", "library", "artist"};
    static const char *kMoves[] = {"none", "iris", "black", "slide"};
    sys::log("[UI] screen: %s -> %s (%s)", kNames[static_cast<int>(current_)],
             kNames[static_cast<int>(next)], kMoves[static_cast<int>(move)]);
    if (context_.settings.reduced_motion)
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

    // Home is the hub: Triangle opens Now Playing, Circle steps back (Now
    // Playing keeps Circle while its queue has the focus).
    const bool browsing = current_ == Id::home || current_ == Id::library || current_ == Id::page ||
                          current_ == Id::artist;
    if (!menu_open_ && browsing && model.has_track && input.is_pressed(Action::north))
    {
        playing_open_ = true;
        feedback.play(audio::Cue::open);
    }
    else if (!menu_open_ && current_ == Id::playing && input.is_pressed(Action::back) &&
             !now_playing_uses_back(*playing_))
    {
        playing_open_ = false;
        feedback.play(audio::Cue::back);
    }
    else if (!menu_open_ && (current_ == Id::page || current_ == Id::artist) && !stack_.empty() &&
             input.is_pressed(Action::back))
    {
        stack_.pop_back();
        feedback.play(audio::Cue::back);
    }
    else if (!menu_open_ && (current_ == Id::home || current_ == Id::library) && stack_.empty() &&
             (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next)))
    {
        library_tab_ = !library_tab_;
        feedback.play(audio::Cue::tab, library_tab_ ? 1.06f : 0.96f);
    }
    if (!model.has_track)
        playing_open_ = false;

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

    // The touchpad opens Control Room from any screen; it closes itself.
    if (!menu_open_ && input.is_pressed(Action::touch))
    {
        menu_open_ = true;
        feedback.play(audio::Cue::modal_open);
        go(Id::menu, Move::slide, 0.5f);
    }
    else
    {
        screen(current_).update(model, input, dt, feedback, commands);
        // Cross on a card asked for its page: an artist's or a list's.
        for (const Request &request : context_.requests)
            if (request.kind == Request::Kind::open)
            {
                const Id next = request.card.uri.rfind("spotify:artist:", 0) == 0 ? Id::artist : Id::page;
                if (stack_.empty() || stack_.back() != next)
                    stack_.push_back(next);
            }
        if (current_ == Id::menu && control_room_wants_close(*menu_))
        {
            menu_open_ = false;
        }
    }
    // The screen that is leaving keeps animating while it is still visible.
    if (transition_.running && previous_ != current_)
    {
        InputFrame none;
        std::vector<Command> ignored;
        ui::Feedback quiet;
        screen(previous_).update(model, none, dt, quiet, ignored);
    }
    if (model.notice_id != notice_id_)
    {
        notice_id_ = model.notice_id;
        notice_ = model.notice;
        notice_age_ = 0.0f;
        if (!notice_.empty())
            feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
    }
    notice_age_ += dt;
    // ---- top bar: now-playing bar and the status pill ----
    const bool hub = current_ == Id::home || current_ == Id::library || current_ == Id::page ||
                     current_ == Id::artist;
    mini_.target = hub && model.has_track && !menu_open_ ? 1.0f : 0.0f;
    mini_.update(dt, 9.0f);
    std::string status;
    if (model.connection == Connection::reconnecting)
        status = "Reconnecting";
    else if (!model.busy.empty())
        status = model.busy;
    else if (model.home_loading && hub)
        status = "Updating Home";
    else if (model.library_loading && current_ == Id::library)
        status = "Updating Library";
    if (current_ == Id::launch)
        status.clear(); // the loader says it all
    if (!status.empty() && status != status_text_)
    {
        status_previous_ = status_text_;
        status_text_ = status;
        status_swap_.start(context_.settings.reduced_motion ? 0.1f : 0.32f);
    }
    status_shown_.target = status.empty() ? 0.0f : 1.0f;
    status_shown_.update(dt, 10.0f);
    status_width_.target = 92.0f + context_.fonts.semibold.measure(status_text_, 22);
    if (status_shown_.value < 0.02f)
        status_width_.snap(status_width_.target);
    status_width_.update(dt, 14.0f);
    status_swap_.update(dt);
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
    const Move move = moving ? (context_.settings.reduced_motion && move_ == Move::iris ? Move::black : move_)
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
        const float slide = context_.settings.reduced_motion ? 0.0f : kSlide;
        const float travel = tween::quint_out(t);
        // Deeper screens arrive from the right: playing after home, the menu
        // after anything; going back slides the other way.
        // Deeper screens (and the Library tab) arrive from the right; going
        // back slides the other way.
        const bool deeper =
            current_ == Id::menu || (current_ == Id::playing && previous_ != Id::menu) ||
            ((current_ == Id::page || current_ == Id::artist) && previous_ != Id::playing && previous_ != Id::menu &&
             !(current_ == Id::artist && previous_ == Id::page)) ||
            (current_ == Id::library && previous_ == Id::home);
        const float direction = deeper ? 1.0f : -1.0f;
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

    chrome_.clear();
    draw_overlays(chrome_, model);
    renderer.draw(chrome_);
}

void Shell::draw_overlays(gfx::DrawList &list, const Model &model)
{
    const ui::Fonts &fonts = context_.fonts;

    // ---- the now-playing bar, top right (Home and pages) ----
    const gfx::Rect bar{1740 - 26 - 560 + 30.0f * (1.0f - mini_.value), 50, 560, 64};
    if (mini_.value > 0.01f && model.has_track)
    {
        list.push_opacity(mini_.value);
        list.shadow({bar.x, bar.y + 10, bar.w, bar.h}, 18, 30, Color::rgb(0x000000, 0.5f));
        list.rounded_rect(bar, 18, kNight.with_alpha(0.82f));
        list.bordered_rect(bar, 18, kClear, 1.5f, kInk.with_alpha(0.14f));
        const gfx::Rect cover{bar.x + 10, bar.y + 10, bar.h - 20, bar.h - 20};
        if (model.track.art)
            list.image(model.track.art, cover, gfx::kFullUv, kInk, 8);
        else
            list.rounded_rect(cover, 8, kInk.with_alpha(0.1f));
        const float x = cover.x + cover.w + 18;
        const float width = bar.w - (x - bar.x) - 70;
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(model.track.title, 22, width), x, bar.y + 29, 22, kInk);
        ui::text(list, fonts.regular, fonts.regular.font->fit(model.track.artist, 19, width), x, bar.y + 53, 19,
                 kInk.with_alpha(0.65f));
        // Play state at the right; progress as a hairline along the bottom.
        const float px = bar.x + bar.w - 40;
        if (model.paused)
            list.triangle({px - 9, bar.cy() - 10, 18, 20}, kInk, 0.0f, 1.5708f);
        else
        {
            list.rounded_rect({px - 8, bar.cy() - 10, 6, 20}, 2, kInk);
            list.rounded_rect({px + 3, bar.cy() - 10, 6, 20}, 2, kInk);
        }
        if (model.track.duration_ms > 0)
        {
            const float done =
                std::min(1.0f, static_cast<float>(model.position_ms) / static_cast<float>(model.track.duration_ms));
            list.rounded_rect({bar.x + 18, bar.y + bar.h - 5, (bar.w - 36) * done, 3}, 1.5f, kGreen);
        }
        list.pop_opacity();
    }

    // ---- the status pill: what is under way ----
    const float shown = tween::clamp01(status_shown_.value);
    if (shown > 0.01f)
    {
        // Left of the now-playing bar, else left of Home's account initial,
        // else in the corner.
        const float right = mini_.value > 0.5f                                  ? bar.x - 16
                            : current_ == Id::home || current_ == Id::library ? 1758.0f
                                                                              : kRight;
        const float w = status_width_.value;
        const gfx::Rect pill{right - w + 24.0f * (1.0f - shown), 56, w, 52};
        list.push_opacity(shown);
        list.shadow({pill.x, pill.y + 8, pill.w, pill.h}, 26, 24, Color::rgb(0x000000, 0.35f));
        list.rounded_rect(pill, 26, kNight.with_alpha(0.78f));
        list.bordered_rect(pill, 26, kClear, 1.5f, kInk.with_alpha(0.16f));
        list.arc(pill.x + 32, pill.cy(), 11, 3, 0.0f, kTau, kInk.with_alpha(0.12f));
        list.arc(pill.x + 32, pill.cy(), 11, 3, clock_ * 5.5f, 2.2f, kGreen);
        // A new verb slides up into place as the old one leaves.
        list.push_clip({pill.x + 50, pill.y, pill.w - 60, pill.h});
        const float t = status_swap_.running ? tween::cubic_out(status_swap_.progress()) : 1.0f;
        const std::string ellipsis = "\xE2\x80\xA6";
        if (t < 1.0f && !status_previous_.empty())
            ui::text(list, fonts.semibold, status_previous_ + ellipsis, pill.x + 56, pill.cy() + 8 - 22.0f * t, 22,
                     kInk.with_alpha(1.0f - t));
        ui::text(list, fonts.semibold, status_text_ + ellipsis, pill.x + 56, pill.cy() + 8 + 22.0f * (1.0f - t), 22,
                 kInk.with_alpha(t));
        list.pop_clip();
        list.pop_opacity();
    }

    // A short message from the app ("This can't be played here yet").
    constexpr float kNoticeSeconds = 3.2f;
    if (!notice_.empty() && notice_age_ < kNoticeSeconds)
    {
        const float in = tween::cubic_out(notice_age_ / 0.3f);
        const float out = 1.0f - tween::smoothstep((notice_age_ - (kNoticeSeconds - 0.4f)) / 0.4f);
        const float w = fonts.semibold.measure(notice_, 24) + 64.0f;
        const gfx::Rect pill{kCentreX - w * 0.5f, 140 - 20.0f * (1.0f - in), w, 60};
        list.push_opacity(std::min(in, out));
        list.shadow({pill.x, pill.y + 8, pill.w, pill.h}, 30, 24, Color::rgb(0x000000, 0.4f));
        list.rounded_rect(pill, 30, kNight.with_alpha(0.92f));
        list.bordered_rect(pill, 30, kClear, 1.5f, kInk.with_alpha(0.18f));
        ui::text(list, fonts.semibold, notice_, kCentreX, pill.cy() + 9, 24, kInk, gfx::Align::center);
        list.pop_opacity();
    }

}

} // namespace screens
