// ps5-spotify - Now Playing: what the phone is playing on this console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-homebrew-ui src/concepts/player.cpp (GPL-3.0-or-later,
// BlackBearReloaded). Kept: layout numbers, the pseudo-spectrum, artwork as
// play state, slide-through track change, spring scrubber, focus ring, the
// play/pause morph. Changed: data comes from the Model, the buttons send
// commands to the phone's session, and the queue panel is not shown yet.

#include "screens/now_playing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include "core/tween.hpp"
#include "screens/style.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

namespace screens
{

namespace
{

using gfx::Color;
using gfx::Rect;
using style::kClear;
using style::kGreen;
using style::kNight;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16); // icons on the white play button

// ---- layout (player.cpp, wide layout: the queue is hidden) ----
constexpr float kMargin = 218.0f; // player.cpp's kWideMargin
constexpr float kArt = 520.0f;
constexpr float kArtY = 172.0f;
constexpr float kArtRadius = 40.0f;
constexpr float kPausedScale = 0.94f;
constexpr float kColumnGap = 84.0f;

constexpr int kBars = 32;
constexpr float kBarsBase = 640.0f;
constexpr float kBarsHeight = 204.0f;

constexpr float kScrubY = 772.0f;
constexpr float kTransportY = 888.0f;
constexpr float kPlayRadius = 46.0f;
constexpr float kButtonRadius = 32.0f;
constexpr float kSoft = 10.0f;

enum Control
{
    kPrevious,
    kPlay,
    kNext,
    kControls,
};
constexpr float kControlOffset[kControls] = {-114.0f, 0.0f, 114.0f};

constexpr float kX = kMargin;
constexpr float kW = gfx::kVirtualWidth - 2.0f * kMargin;
constexpr float kCx = kX + kW * 0.5f;
constexpr float kColumnX = kX + kArt + kColumnGap;
constexpr float kColumnW = kW - kArt - kColumnGap;

// A triangle pointing left (dir -1) or right (dir 1): three thick lines, so
// the edges are smooth (player.cpp).
void side_triangle(gfx::DrawList &list, float cx, float cy, float size, float dir, Color color)
{
    const float stroke = size * 0.4f;
    const float half = size * 0.3f;
    const float back = cx - dir * size * 0.22f;
    const float tip = cx + dir * size * 0.3f;
    list.line(back, cy - half, back, cy + half, stroke, color);
    list.line(back, cy - half, tip, cy, stroke, color);
    list.line(back, cy + half, tip, cy, stroke, color);
}

// What one track looks like: kept for both sides of a track change.
struct Shown
{
    std::string title, artist, album;
    style::AlbumPalette palette;
};

class NowPlaying final : public Screen
{
  public:
    explicit NowPlaying(Context &context) : context_(context)
    {
        play_.snap(0.0f);
        morph_.snap(0.0f);
        ring_.snap(ring_target());
    }

    void enter() override { age_ = 0.0f; }

    void update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &commands) override
    {
        age_ += dt;
        clock_ += dt;
        const bool reduced = context_.reduced_motion;
        if (!reduced)
            drift_ += dt;

        // ---- a new track: the old one slides out, the new one in ----
        if (model.track.id != track_id_)
        {
            if (!track_id_.empty())
            {
                previous_ = current_;
                swap_.start(reduced ? 0.14f : 0.6f);
            }
            track_id_ = model.track.id;
            current_ = {model.track.title, model.track.artist, model.track.album,
                        style::album_palette(model.track.album)};
            apply_palette(previous_.title.empty() && !swap_.running);
            travel_ = next_travel_;
            next_travel_ = 1.0f;
        }
        duration_ = std::max(1.0f, static_cast<float>(model.track.duration_ms) / 1000.0f);
        position_ = static_cast<float>(model.position_ms) / 1000.0f;
        playing_ = !model.paused;

        update_input(input, feedback, commands);
        update_spectrum(dt);

        const float glide = reduced ? 40.0f : kSoft;
        shown_.target = tween::clamp01(position_ / duration_);
        shown_.update(dt, 14.0f);
        play_.target = playing_ ? 1.0f : 0.0f;
        play_.update(dt, glide);
        morph_.target = play_.target;
        morph_.update(dt, reduced ? 40.0f : 16.0f);
        glow_.target = energy_;
        glow_.update(dt, 9.0f);
        swap_.update(dt);
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 3.5f);
        accent_.update(dt, 4.0f);
        body_.update(dt, 4.0f);
        ring_.target(ring_target());
        ring_.update(dt, 18.0f);
        nudge_.update(dt, 9.0f);
        for (ui::Pulse &press : press_)
            press.update(dt, 10.0f);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::bokeh;
        for (int i = 0; i < 4; ++i)
            frame.backdrop.colors[i] = palette_[i].value();
        frame.backdrop.time = drift_;

        gfx::DrawList &list = frame.scene;
        draw_header(list, model);
        draw_artwork(list);
        draw_info(list);
        draw_visualizer(list);
        draw_focus_ring(list, true);
        draw_scrubber(list);
        draw_transport(list);
        draw_focus_ring(list, false);
        draw_hints(list);
    }

  private:
    float appear(int order) const { return tween::stagger(age_, order, 0.07f, 0.55f); }

    float slide(float in, float distance) const
    {
        return context_.reduced_motion ? 0.0f : distance * (1.0f - in);
    }

    Rect ring_target() const
    {
        const float radius = (button_ == kPlay ? kPlayRadius : kButtonRadius) + 9.0f;
        return {kControlOffset[button_], kTransportY, radius * 2.0f, radius * 2.0f};
    }

    // The backdrop and accents come from the album (player.cpp apply_palette).
    void apply_palette(bool snap)
    {
        const style::AlbumPalette &p = current_.palette;
        const Color targets[4] = {
            gfx::mix(p.dark, Color::rgb(0x05060c), 0.45f),
            gfx::mix(p.dark, p.mid, 0.3f),
            p.mid,
            gfx::mix(p.mid, p.accent, 0.6f),
        };
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
        if (snap)
        {
            accent_.snap(p.accent);
            body_.snap(p.mid);
        }
        else
        {
            accent_.target(p.accent);
            body_.target(p.mid);
        }
    }

    void refuse(ui::Feedback &feedback, float direction)
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_.trigger();
        nudge_direction_ = direction;
    }

    void activate(int control, ui::Feedback &feedback, std::vector<Command> &commands)
    {
        const float pan = ui::pan_for_x(kCx + kControlOffset[control]);
        press_[control].trigger();
        switch (control)
        {
        case kPlay:
            feedback.play(audio::Cue::select, playing_ ? 0.9f : 1.0f, pan);
            commands.push_back(Command::play_pause);
            break;
        case kPrevious:
            feedback.play(audio::Cue::tab, 0.94f, -0.4f);
            // A few seconds in, "previous" restarts the track: no slide.
            next_travel_ = position_ > 3.0f ? 1.0f : -1.0f;
            commands.push_back(Command::previous);
            break;
        case kNext:
            feedback.play(audio::Cue::tab, 1.0f, 0.4f);
            next_travel_ = 1.0f;
            commands.push_back(Command::next);
            break;
        default:
            break;
        }
    }

    void update_input(const InputFrame &input, ui::Feedback &feedback,
                      std::vector<Command> &commands)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int step = input.nav == Direction::right ? 1 : -1;
            const int next = button_ + step;
            if (next >= 0 && next < kControls)
            {
                button_ = next;
                feedback.play(audio::Cue::focus, 1.0f, ui::pan_for_x(kCx + kControlOffset[next]));
            }
            else if (!input.nav_repeat)
            {
                refuse(feedback, static_cast<float>(step));
            }
        }
        if (input.is_pressed(Action::confirm))
            activate(button_, feedback, commands);
        if (input.is_pressed(Action::page_prev))
            activate(kPrevious, feedback, commands);
        if (input.is_pressed(Action::page_next))
            activate(kNext, feedback, commands);
    }

    // A believable spectrum without analysing audio (player.cpp
    // update_spectrum): sines per bar, a beat on the low end, fast attack and
    // slow decay, peak caps. The tempo is fixed per album.
    void update_spectrum(float dt)
    {
        const float t = position_;
        const float seed = current_.palette.accent.r * 7.0f;
        const float bpm = 92.0f + 36.0f * current_.palette.accent.g;
        const float beat = t * bpm / 60.0f;
        const float kick = std::exp(-5.0f * (beat - std::floor(beat)));
        const float off = beat + 0.5f;
        const float snare = std::exp(-7.0f * (off - std::floor(off)));
        float bass = 0.0f;
        for (int i = 0; i < kBars; ++i)
        {
            const float fi = static_cast<float>(i);
            const float u = fi / static_cast<float>(kBars - 1);
            float wanted = 0.0f;
            if (playing_)
            {
                const float shape = 1.0f - 0.62f * std::pow(u, 0.7f);
                const float slow =
                    0.5f + 0.5f * std::sin(t * (1.9f + 0.31f * fi) + fi * 1.7f + seed);
                const float fast = 0.5f + 0.5f * std::sin(t * (5.3f + 0.17f * fi) + fi * 0.6f);
                const float air = 0.5f + 0.5f * std::sin(t * (10.0f + 0.9f * std::fmod(fi, 5.0f)) +
                                                         fi * 2.3f + seed);
                const float low = tween::clamp01(1.0f - u * 3.2f);
                const float mid = std::exp(-(u - 0.45f) * (u - 0.45f) / 0.03f);
                wanted = tween::clamp01(shape * (0.12f + 0.46f * slow + 0.34f * fast * air) +
                                        0.42f * kick * low + 0.22f * snare * mid);
            }
            float &level = levels_[static_cast<std::size_t>(i)];
            const float rate = wanted > level ? 30.0f : 7.0f;
            level += (wanted - level) * (1.0f - std::exp(-rate * dt));
            float &peak = peaks_[static_cast<std::size_t>(i)];
            peak = std::max(level, peak - 0.7f * dt);
            if (i < 6)
                bass += level;
        }
        energy_ = bass / 6.0f;
    }

    // ---- drawing ----

    void draw_header(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(0);
        list.push_opacity(in);
        const float y = 116.0f - slide(in, 12.0f);
        const float w = ui::text(list, fonts.display, "Spotify Connect", kX, y, 32, kWhite);
        const std::string where = "on " + model.device_name + "  \xC2\xB7  320 kbps";
        ui::text(list, fonts.regular, where, kX + w + 24, y, 22, kWhite.with_alpha(0.6f));
        list.pop_opacity();
    }

    // A cover made from the album's palette and name, until real artwork is
    // decoded: a soft gradient, two orbits and the name at the foot.
    void draw_generated_cover(gfx::DrawList &list, const Rect &r, const Shown &track,
                              Color tint) const
    {
        const style::AlbumPalette &p = track.palette;
        list.gradient_rect(r, kArtRadius, gfx::mix(p.mid, tint, 0.0f).with_alpha(tint.a),
                           gfx::mix(p.dark, kNight, 0.3f).with_alpha(tint.a));
        list.push_clip(r);
        const float cx = r.x + r.w * 0.62f;
        const float cy = r.y + r.h * 0.38f;
        list.glow({cx - 70, cy - 70, 140, 140}, 70, 90, p.accent.with_alpha(0.45f * tint.a));
        list.circle(cx, cy, r.w * 0.16f, p.accent.with_alpha(0.9f * tint.a));
        list.ring(cx, cy, r.w * 0.27f, 3.0f, kWhite.with_alpha(0.25f * tint.a));
        list.ring(cx, cy, r.w * 0.38f, 2.0f, kWhite.with_alpha(0.14f * tint.a));
        list.pop_clip();
        const ui::Fonts &fonts = context_.fonts;
        const float size = r.w * 0.085f;
        ui::paragraph(list, fonts.display, track.album, r.x + r.w * 0.06f, r.y + r.h * 0.8f, size,
                      r.w * 0.86f, size * 1.15f, kWhite.with_alpha(tint.a), 2);
    }

    void draw_cover(gfx::DrawList &list, const Rect &art, const Shown &track, float alpha,
                    float dx, float live) const
    {
        if (alpha <= 0.01f || track.title.empty())
            return;
        const Rect r{art.x + dx, art.y, art.w, art.h};
        list.push_opacity(alpha);
        list.shadow({r.x, r.y + 28, r.w, r.h}, kArtRadius, 50, Color::rgb(0x000000, 0.5f));
        const Color tint = gfx::mix(Color::rgb(0xb9bcc8), kWhite, live);
        draw_generated_cover(list, r, track, tint);
        list.bordered_rect(r, kArtRadius, kClear, 2, kWhite.with_alpha(0.16f));
        list.pop_opacity();
    }

    void draw_artwork(gfx::DrawList &list) const
    {
        const bool reduced = context_.reduced_motion;
        const float in = appear(1);
        const float live = play_.value;
        const float pulse = glow_.value;
        Rect art{kX, kArtY, kArt, kArt};
        art.x -= slide(in, 36.0f);
        const float scale =
            tween::lerp(kPausedScale, 1.0f, live) + (reduced ? 0.0f : 0.008f * pulse);
        list.push_opacity(in);
        list.push_transform(scale, art.cx(), art.cy(), 0, 0);
        const float lit = (0.14f + 0.36f * pulse) * (0.3f + 0.7f * live);
        list.glow(art.inset(-4), kArtRadius + 4, 90.0f + 60.0f * pulse,
                  gfx::mix(accent_.value(), body_.value(), 0.25f).with_alpha(lit));
        if (swap_.running)
        {
            const float t = swap_.progress();
            const float leave = tween::clamp01(t * 2.3f);
            const float arrive = tween::clamp01((t - 0.2f) / 0.8f);
            const float distance = reduced ? 0.0f : 150.0f;
            draw_cover(list, art, previous_, 1.0f - tween::smoothstep(leave),
                       -travel_ * distance * tween::cubic_in(leave), live);
            draw_cover(list, art, current_, tween::smoothstep(arrive * 1.6f),
                       travel_ * distance * (1.0f - tween::quint_out(arrive)), live);
        }
        else
        {
            draw_cover(list, art, current_, 1.0f, 0.0f, live);
        }
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_track_text(gfx::DrawList &list, const Shown &track, float alpha, float dx) const
    {
        if (alpha <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float x = kColumnX + dx;
        list.push_opacity(alpha);
        // A long title first drops a size, then takes an ellipsis.
        const float size = fonts.display.measure(track.title, 76) <= kColumnW ? 76.0f : 60.0f;
        ui::text(list, fonts.display, fonts.display.font->fit(track.title, size, kColumnW), x - 3,
                 306, size, kWhite);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(track.artist, 30, kColumnW), x, 356,
                 30, kWhite.with_alpha(0.92f));
        ui::text(list, fonts.regular, fonts.regular.font->fit(track.album, 24, kColumnW), x, 396,
                 24, kWhite.with_alpha(0.62f));
        list.pop_opacity();
    }

    void draw_info(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(2);
        const float live = play_.value;
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.3f);
        list.push_opacity(in);
        const float enter = slide(in, 30.0f);
        const float label_y = 206.0f;
        const float x = kColumnX + enter;
        if (live > 0.01f)
            ui::text(list, fonts.semibold, "NOW PLAYING", x, label_y + 8.0f * (1.0f - live), 18,
                     accent.with_alpha(live), gfx::Align::left, 4.0f);
        if (live < 0.99f)
            ui::text(list, fonts.semibold, "PAUSED", x, label_y - 8.0f * live, 18,
                     kWhite.with_alpha(0.7f * (1.0f - live)), gfx::Align::left, 4.0f);

        if (swap_.running)
        {
            const float t = swap_.progress();
            const float distance = context_.reduced_motion ? 0.0f : 1.0f;
            const float leave = tween::clamp01(t * 3.2f);
            const float arrive = tween::clamp01((t - 0.26f) / 0.74f);
            draw_track_text(list, previous_, 1.0f - tween::smoothstep(leave),
                            -travel_ * 40.0f * distance * tween::cubic_in(leave));
            draw_track_text(list, current_, tween::smoothstep(arrive),
                            travel_ * 56.0f * distance * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_track_text(list, current_, 1.0f, enter);
        }
        list.pop_opacity();
    }

    void draw_visualizer(gfx::DrawList &list) const
    {
        const float in = appear(3);
        const Color top = gfx::mix(accent_.value(), kWhite, 0.4f);
        const Color bottom = gfx::mix(body_.value(), accent_.value(), 0.45f);
        const float width = kColumnW / static_cast<float>(kBars) * 0.56f;
        const float pitch = (kColumnW - width) / static_cast<float>(kBars - 1);
        list.push_opacity(in);
        for (int i = 0; i < kBars; ++i)
        {
            const float grow = tween::stagger(age_, i, 0.012f, 0.5f);
            const float level = levels_[static_cast<std::size_t>(i)] * grow;
            const float h = width + level * (kBarsHeight - width);
            const float x = kColumnX + static_cast<float>(i) * pitch;
            const Color tip = gfx::mix(bottom, top, 0.35f + 0.65f * level);
            list.gradient_rect({x, kBarsBase - h, width, h}, width * 0.5f, tip, bottom);
            const float mirror = width + (h - width) * 0.42f;
            list.gradient_rect({x, kBarsBase + 8.0f, width, mirror}, width * 0.5f,
                               bottom.with_alpha(0.3f), bottom.with_alpha(0.0f));
            const float peak = peaks_[static_cast<std::size_t>(i)] * grow;
            const float lifted = peak - level;
            if (lifted > 0.02f)
            {
                const float y = kBarsBase - width - peak * (kBarsHeight - width) - 8.0f;
                list.rounded_rect(
                    {x, y, width, 3.0f}, 1.5f,
                    top.with_alpha(tween::clamp01(lifted * 8.0f) * 0.55f * play_.value));
            }
        }
        list.pop_opacity();
    }

    void draw_scrubber(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(4);
        const float thumb = kX + tween::clamp01(shown_.value) * kW;
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.25f);
        char text[16];
        list.push_opacity(in);
        const float y = kScrubY + slide(in, 24.0f);
        const float thick = 8.0f;
        const Rect bar{kX, y - thick * 0.5f, kW, thick};
        list.rounded_rect(bar, thick * 0.5f, kWhite.with_alpha(0.16f));
        const float filled = std::max(thick, thumb - kX);
        list.glow({bar.x, bar.y, filled, bar.h}, thick * 0.5f, 10.0f, accent.with_alpha(0.22f));
        list.gradient_rect_h({bar.x, bar.y, filled, bar.h}, thick * 0.5f,
                             gfx::mix(body_.value(), accent, 0.5f), accent);
        const float radius = 9.0f;
        list.shadow({thumb - radius, y - radius + 4, radius * 2, radius * 2}, radius, 10,
                    Color::rgb(0x000000, 0.45f));
        list.circle(thumb, y, radius, kWhite);
        style::format_time(text, sizeof(text), position_);
        ui::text(list, fonts.mono, text, kX, y + 46, 22, kWhite.with_alpha(0.86f));
        style::format_time(text, sizeof(text), duration_);
        ui::text(list, fonts.mono, text, kX + kW, y + 46, 22, kWhite.with_alpha(0.6f),
                 gfx::Align::right);
        list.pop_opacity();
    }

    void draw_skip_icon(gfx::DrawList &list, float cx, float cy, float dir, Color c) const
    {
        side_triangle(list, cx - dir * 6.0f, cy, 24, dir, c);
        list.line(cx + dir * 12.0f, cy - 9, cx + dir * 12.0f, cy + 9, 4.5f, c);
    }

    void draw_transport(gfx::DrawList &list) const
    {
        const float in = appear(5);
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.3f);
        list.push_opacity(in);
        const float y = kTransportY + slide(in, 28.0f);
        for (int i = 0; i < kControls; ++i)
        {
            const float cx = kCx + kControlOffset[i];
            const bool focused = i == button_;
            list.push_transform(1.0f - 0.08f * press_[i].value, cx, y, 0, 0);
            if (i == kPlay)
            {
                const float live = play_.value;
                list.glow({cx - kPlayRadius, y - kPlayRadius, kPlayRadius * 2, kPlayRadius * 2},
                          kPlayRadius, 22.0f + 14.0f * glow_.value,
                          accent.with_alpha(0.2f + 0.3f * glow_.value * live));
                list.shadow(
                    {cx - kPlayRadius, y - kPlayRadius + 8, kPlayRadius * 2, kPlayRadius * 2},
                    kPlayRadius, 18, Color::rgb(0x000000, 0.4f));
                list.circle(cx, y, kPlayRadius, kWhite);
                // Play and pause trade places; faded in colour, so the
                // overlapping strokes never show through.
                const float m = morph_.value;
                if (m < 0.99f)
                {
                    const float s = 0.6f + 0.4f * (1.0f - m);
                    side_triangle(list, cx + 2.0f, y, 38.0f * s, 1.0f, gfx::mix(kInk, kWhite, m));
                }
                if (m > 0.01f)
                {
                    const float s = 0.6f + 0.4f * m;
                    const Color c = gfx::mix(kWhite, kInk, m);
                    list.rounded_rect({cx - 15 * s, y - 17 * s, 10 * s, 34 * s}, 4 * s, c);
                    list.rounded_rect({cx + 5 * s, y - 17 * s, 10 * s, 34 * s}, 4 * s, c);
                }
            }
            else
            {
                list.circle(cx, y, kButtonRadius, kWhite.with_alpha(focused ? 0.18f : 0.08f));
                list.ring(cx, y, kButtonRadius, 1.5f, kWhite.with_alpha(0.16f));
                const Color c = focused ? kWhite : Color::rgb(0xd4d7e2);
                draw_skip_icon(list, cx, y, i == kNext ? 1.0f : -1.0f, c);
            }
            list.pop_transform();
        }
        list.pop_opacity();
    }

    void draw_focus_ring(gfx::DrawList &list, bool light) const
    {
        const float alpha = appear(5);
        if (alpha <= 0.01f)
            return;
        const Rect ring = ring_.value();
        const bool sideways = nudge_direction_ != 0.0f;
        const float cx =
            kCx + ring.x +
            (sideways ? ui::shake(nudge_.value, clock_, 12.0f) * nudge_direction_ : 0.0f);
        const float cy = ring.y + slide(appear(5), 28.0f);
        const float radius = ring.w * 0.5f;
        list.push_opacity(alpha);
        if (light)
        {
            const Color accent = gfx::mix(accent_.value(), kWhite, 0.3f);
            list.glow({cx - radius, cy - radius, radius * 2, radius * 2}, radius, 18,
                      accent.with_alpha(0.34f + 0.2f * ui::breathe(clock_)));
        }
        else
        {
            list.ring(cx, cy, radius, 3.5f, kWhite);
        }
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list) const
    {
        constexpr const char *kLabels[kControls] = {"Previous", "Play", "Next"};
        const char *label = button_ == kPlay ? (playing_ ? "Pause" : "Play") : kLabels[button_];
        list.push_opacity(appear(8));
        const ui::Hint hints[] = {{ui::Button::cross, label},
                                  {ui::Button::l1, "Previous / Next", ui::Button::r1},
                                  {ui::Button::options, "Quit"}};
        ui::draw_hints(list, context_.fonts, ui::GlyphStyle::dark(), hints, 3, 1824, true);
        list.pop_opacity();
    }

    Context &context_;

    // ---- what is playing ----
    std::string track_id_;
    Shown current_, previous_;
    float position_ = 0.0f;
    float duration_ = 1.0f;
    bool playing_ = false;
    float next_travel_ = 1.0f; // direction of the next track change
    float travel_ = 1.0f;

    // ---- focus ----
    int button_ = kPlay;

    // ---- animation ----
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float drift_ = 0.0f;
    std::array<float, kBars> levels_{};
    std::array<float, kBars> peaks_{};
    float energy_ = 0.0f;
    tween::Spring glow_;
    tween::Spring shown_;
    tween::Spring play_;
    tween::Spring morph_;
    tween::Timer swap_;
    ui::SpringColor palette_[4];
    ui::SpringColor accent_;
    ui::SpringColor body_;
    ui::SpringRect ring_;
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    ui::Pulse press_[kControls];
};

} // namespace

std::unique_ptr<Screen> make_now_playing(Context &context)
{
    return std::make_unique<NowPlaying>(context);
}

} // namespace screens
