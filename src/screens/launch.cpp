// ps5-spotify - Launch screen: title card and an honest loader; with nothing
// saved, the loader makes way for how to pair the phone.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Techniques and numbers from ps5-homebrew-ui src/concepts/boot.cpp
// (GPL-3.0-or-later, BlackBearReloaded): draw_title() and draw_loading().

#include "screens/launch.hpp"

#include <algorithm>
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
using namespace style;

// ---- title ----
constexpr const char *kWord = "SPOTIFY";
constexpr float kLogoSize = 168.0f;
constexpr float kLogoTracking = 12.0f;
constexpr float kLogoBaseline = 520.0f;
constexpr int kLogoBands = 12;      // slices of the gradient fill
constexpr float kSweepFirst = 1.6f; // seconds after the title arrives
constexpr float kSweepPeriod = 5.0f;
constexpr float kSweepSeconds = 1.2f;
constexpr int kSweepStrips = 9;
constexpr int kSweepRows = 5;
constexpr float kSweepStrip = 15.0f;
constexpr float kSweepLean = 9.0f;
constexpr float kSweepProfile[kSweepStrips] = {0.08f, 0.22f, 0.42f, 0.64f, 0.80f,
                                               0.64f, 0.42f, 0.22f, 0.08f};

// ---- loader ----
constexpr float kBarY = 928.0f;
constexpr float kBarH = 8.0f;
constexpr float kHandOver = 0.9f; // seconds "ready" stays before the hand-over

// ---- pairing ----
constexpr float kCardW = 460.0f;
constexpr float kCardH = 84.0f;
constexpr float kCardGap = 30.0f;
constexpr float kCardY = 800.0f;

const char *step_name(Step step)
{
    switch (step)
    {
    case Step::certificates:
        return "Checking certificates";
    case Step::login:
        return "Reading your saved login";
    case Step::connect:
        return "Connecting to Spotify";
    case Step::start:
        return "Getting ready to play";
    case Step::home:
        return "Loading your Home";
    case Step::artwork:
        return "Fetching artwork";
    case Step::done:
        return "Ready";
    }
    return "";
}

void check_mark(gfx::DrawList &list, float cx, float cy, float s, float thickness, Color colour)
{
    list.line(cx - s * 0.42f, cy + s * 0.02f, cx - s * 0.12f, cy + s * 0.32f, thickness, colour);
    list.line(cx - s * 0.12f, cy + s * 0.32f, cx + s * 0.44f, cy - s * 0.30f, thickness, colour);
}

class Launch final : public Screen
{
  public:
    explicit Launch(Context &context) : context_(context) { enter(); }

    void enter() override
    {
        age_ = 0.0f;
        bar_.snap(0.0f);
        ready_ = false;
        ready_age_ = 0.0f;
    }

    void update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &commands) override
    {
        const bool reduced = context_.settings.reduced_motion;
        age_ += dt;
        clock_ += dt;
        if (!reduced)
            drift_ += dt;

        const bool failed = model.connection == Connection::error;
        if (failed != failed_)
        {
            failed_ = failed;
            if (failed)
            {
                deny_.trigger();
                feedback.play(audio::Cue::error);
                feedback.rumble(0.3f, 0.08f);
            }
        }
        if (failed && input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            commands.push_back(Command::retry);
        }

        // The bar shows real steps. Inside a step it creeps toward (never to)
        // the step's end, so a slow network still looks alive but the bar
        // cannot claim work that has not happened.
        const int step = static_cast<int>(model.step);
        if (step != step_)
        {
            previous_step_ = step_;
            step_ = step;
            step_age_ = 0.0f;
            stage_fade_.start(0.3f);
        }
        step_age_ += dt;
        const float share = 1.0f / static_cast<float>(kStepCount);
        // The artwork step shows real progress: covers loaded of all.
        const float creep =
            failed ? 0.0f
            : model.step == Step::artwork && model.artwork_total > 0
                ? 0.95f * static_cast<float>(model.artwork_done) / static_cast<float>(model.artwork_total)
                : 0.6f * (1.0f - std::exp(-step_age_ / 1.5f));
        bar_.target = std::min(1.0f, (static_cast<float>(step) + creep) * share);
        bar_.update(dt, 12.0f);

        // "Ready" only when the bar the user sees is full, as in the design.
        if (!ready_ && model.step == Step::done && bar_.value > 0.99f)
        {
            ready_ = true;
            ready_age_ = 0.0f;
            flash_.trigger();
            feedback.play(audio::Cue::resume);
        }
        if (ready_)
            ready_age_ += dt;

        pair_.target = model.connection == Connection::pairing ? 1.0f : 0.0f;
        pair_.update(dt, 7.0f);
        if (pair_.target > 0.5f)
            pair_age_ += dt;
        else
            pair_age_ = 0.0f;

        spinner_.target = ready_ || failed ? 0.0f : 0.45f + 0.55f * (step_age_ > 2.0f);
        spinner_.update(dt, 8.0f);
        flash_.update(dt, 2.6f);
        deny_.update(dt, 9.0f);
        stage_fade_.update(dt);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::stars;
        frame.backdrop.colors[0] = gfx::mix(kDeep, kNight, 0.82f);
        frame.backdrop.colors[1] = gfx::mix(kDeep, kNight, 0.5f);
        frame.backdrop.colors[2] = kDeep;
        frame.backdrop.colors[3] = gfx::mix(kDeep, kGreen, 0.35f);
        frame.backdrop.params[0] = drift_ * 0.05f; // a slow pan keeps the stars alive
        frame.backdrop.time = drift_;
        frame.post.mode = gfx::BackdropMode::vignette;
        frame.post.colors[0] = kNight;
        frame.post.params[0] = 0.45f;

        draw_title(frame.scene);
        frame.scene.push_opacity(1.0f - pair_.value);
        draw_loader(frame.scene, model);
        frame.scene.pop_opacity();
        draw_pairing(frame.scene, model);
        draw_hints(frame.scene, model);
    }

    bool finished() const { return ready_ && ready_age_ >= kHandOver; }

  private:
    // The app icon's mark above the wordmark: three tilted arcs, widest on
    // top (sce_sys/icon0.png).
    void draw_emblem(gfx::DrawList &list, float cy, float in) const
    {
        const float lit = 0.32f + 0.10f * ui::breathe(clock_, 3.0f);
        list.push_opacity(in);
        list.glow({kCentreX - 12, cy + 2, 24, 24}, 12, 70, kGreen.with_alpha(lit));
        constexpr float kHalf[] = {46.0f, 38.0f, 30.0f};    // half the chord
        constexpr float kSag[] = {13.0f, 10.0f, 7.5f};      // how far it bows up
        constexpr float kThick[] = {12.0f, 10.0f, 8.0f};
        for (int i = 0; i < 3; ++i)
        {
            // Each arc lights up in turn, top to bottom.
            const float wave = context_.settings.reduced_motion
                                   ? 1.0f
                                   : 0.55f + 0.45f * ui::breathe(clock_ - 0.25f * i, 2.2f);
            const float radius = (kHalf[i] * kHalf[i] + kSag[i] * kSag[i]) / (2.0f * kSag[i]);
            const float half_angle = std::asin(kHalf[i] / radius);
            const float top = cy - 14.0f + 25.0f * static_cast<float>(i);
            list.arc(kCentreX, top + radius, radius + kThick[i] * 0.5f, kThick[i], -half_angle - 0.12f,
                     2.0f * half_angle, gfx::mix(kMint, kGreen, 0.3f * i).with_alpha(wave));
        }
        const float rule = 260.0f * in;
        list.gradient_rect_h({kCentreX - 74 - rule, cy + 6, rule, 2}, 1, kClear,
                             kGreen.with_alpha(0.7f));
        list.gradient_rect_h({kCentreX + 74, cy + 6, rule, 2}, 1, kGreen.with_alpha(0.7f), kClear);
        list.pop_opacity();
    }

    void draw_title(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const bool reduced = context_.settings.reduced_motion;
        const float t = age_;
        const Color pale = gfx::mix(kGreen, Color::rgb(0xffffff), 0.65f);
        const Color deep = gfx::mix(kGreen, kDeep, 0.35f);

        const std::string logo = kWord;
        const float size = kLogoSize;
        const float tracking = kLogoTracking;
        const float width = fonts.display.measure(logo, size, tracking);
        const float x = kCentreX - width * 0.5f;
        const float baseline = kLogoBaseline;
        const float top = baseline - size * 0.76f; // cap height of the display face
        const float height = size * 0.80f;
        const auto letters = [&](float dx, float dy, Color colour) {
            ui::text(list, fonts.display, logo, x + dx, baseline + dy, size, colour,
                     gfx::Align::left, tracking);
        };

        draw_emblem(list, top - 110.0f + 20.0f * (1.0f - tween::stagger(t, 0, 0.1f, 0.7f)),
                    tween::stagger(t, 0, 0.1f, 0.7f));

        // ---- the wordmark, back to front ----
        const float arrive = tween::quint_out((t - 0.15f) / 0.9f);
        list.push_opacity(tween::clamp01(arrive * 1.2f));
        list.push_transform(1.0f + (reduced ? 0.0f : 0.06f * (1.0f - arrive)), kCentreX,
                            baseline - size * 0.38f, 0, 0);
        // 1. bloom
        list.glow({x + 60, top + height * 0.25f, width - 120, height * 0.5f}, height * 0.25f, 150,
                  kGreen.with_alpha(0.10f + 0.04f * ui::breathe(clock_, 4.0f)));
        // 2. a dark copy below gives the letters weight
        letters(0.0f, 9.0f, kNight.with_alpha(0.55f));
        // 3. halo: faint copies on a circle
        for (int i = 0; i < 8; ++i)
        {
            const float angle = kTau * static_cast<float>(i) / 8.0f;
            letters(std::sin(angle) * 5.0f, std::cos(angle) * 5.0f, kGreen.with_alpha(0.09f));
        }
        // 4. fill drawn in clipped bands, pale to deep green
        for (int band = 0; band < kLogoBands; ++band)
        {
            const float slice = height / static_cast<float>(kLogoBands);
            Rect clip{x - 40, top + slice * static_cast<float>(band), width + 80, slice + 1.0f};
            if (band == 0)
            {
                clip.y -= size * 0.3f;
                clip.h += size * 0.3f;
            }
            if (band == kLogoBands - 1)
                clip.h += size * 0.3f;
            list.push_clip(clip);
            letters(0.0f, 0.0f,
                    gfx::mix(pale, deep,
                             static_cast<float>(band) / static_cast<float>(kLogoBands - 1)));
            list.pop_clip();
        }
        // 5. the light sweep: a slanted band of white only where letters are
        const float cycle = std::fmod(t + kSweepPeriod - kSweepFirst, kSweepPeriod) / kSweepSeconds;
        if (!reduced && t > kSweepFirst && cycle < 1.0f)
        {
            const float band_width = kSweepStrip * static_cast<float>(kSweepStrips);
            const float centre =
                tween::lerp(x - band_width, x + width + band_width, tween::smoothstep(cycle));
            const float row_height = (height + size * 0.1f) / static_cast<float>(kSweepRows);
            for (int row = 0; row < kSweepRows; ++row)
            {
                const float lean = kSweepLean * static_cast<float>(kSweepRows - 1 - row);
                for (int strip = 0; strip < kSweepStrips; ++strip)
                {
                    list.push_clip({centre + lean - band_width * 0.5f +
                                        kSweepStrip * static_cast<float>(strip),
                                    top - size * 0.05f + row_height * static_cast<float>(row),
                                    kSweepStrip, row_height});
                    letters(0.0f, 0.0f, Color::rgb(0xffffff, kSweepProfile[strip]));
                    list.pop_clip();
                }
            }
        }
        list.pop_transform();
        list.pop_opacity();

        // ---- the plate under the wordmark opens from the middle ----
        const float plate = tween::cubic_out((t - 0.5f) / 0.5f);
        const float opened = tween::quint_out((t - 0.5f) / 0.8f);
        const float half = (width * 0.5f + 20.0f) * opened;
        const Rect left{kCentreX - half, baseline + 40, half, 46};
        const Rect right{kCentreX, baseline + 40, half, 46};
        list.push_opacity(plate);
        list.gradient_rect_h(left, 0, kClear, kGreen.with_alpha(0.18f));
        list.gradient_rect_h(right, 0, kGreen.with_alpha(0.18f), kClear);
        for (float edge : {left.y, left.y + left.h - 1.5f})
        {
            list.gradient_rect_h({left.x, edge, half, 1.5f}, 0, kClear, kGreen.with_alpha(0.6f));
            list.gradient_rect_h({right.x, edge, half, 1.5f}, 0, kGreen.with_alpha(0.6f), kClear);
        }
        ui::text(list, fonts.semibold, "FOR PS5", kCentreX + 5, left.y + 31, 22,
                 kInk.with_alpha(tween::cubic_out((t - 0.8f) / 0.5f)), gfx::Align::center, 10.0f);
        list.pop_opacity();
    }

    void draw_stage(gfx::DrawList &list, const Model &model, int step, float alpha,
                    float dy) const
    {
        if (alpha <= 0.01f)
            return;
        const bool failed = model.connection == Connection::error;
        std::string name = failed ? "Could not connect" : step_name(static_cast<Step>(step));
        if (!failed && static_cast<Step>(step) == Step::artwork && model.artwork_total > 0)
            name += "  " + std::to_string(model.artwork_done) + " / " + std::to_string(model.artwork_total);
        ui::text(list, context_.fonts.semibold, ui::upper(name), kMargin + 36, 906 + dy, 17,
                 (failed ? kDanger : kInk).with_alpha(0.85f * alpha), gfx::Align::left, 3.5f);
    }

    void draw_loader(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = age_;
        const bool failed = model.connection == Connection::error;
        const float shown = tween::clamp01(bar_.value);
        const float ready = ready_ ? tween::cubic_out(ready_age_ / 0.4f) : 0.0f;
        const Color tone = failed ? kDanger : kGreen;
        char text[32];

        list.push_opacity(tween::stagger(t, 3, 0.08f, 0.6f));

        // The error, in words: what happened and what to do.
        if (failed)
        {
            ui::paragraph(list, fonts.regular, model.message, kMargin + ui::shake(deny_.value, clock_, 8.0f),
                          826, 28, 1100, 40, kInk.with_alpha(0.92f), 2);
        }

        // Spinner: brighter and wider in a slow step; a check mark when done.
        const float spin_x = kMargin + 11;
        const float spin_y = 900;
        const float busy = spinner_.value;
        if (failed)
        {
            list.circle(spin_x, spin_y, 11, kDanger.with_alpha(0.25f));
            list.line(spin_x - 5, spin_y - 5, spin_x + 5, spin_y + 5, 3, kDanger);
            list.line(spin_x - 5, spin_y + 5, spin_x + 5, spin_y - 5, 3, kDanger);
        }
        else if (ready < 1.0f)
        {
            list.arc(spin_x, spin_y, 11, 3, 0.0f, kTau, kInk.with_alpha(0.14f * (1.0f - ready)));
            list.arc(spin_x, spin_y, 11, 3, clock_ * 5.5f,
                     1.3f + 1.5f * busy + 0.5f * std::sin(clock_ * 2.3f),
                     gfx::mix(kInk, kGreen, busy).with_alpha((0.5f + 0.5f * busy) * (1.0f - ready)));
        }
        if (ready > 0.0f)
            check_mark(list, spin_x, spin_y, 22.0f * tween::back_out(ready), 3.5f,
                       kGreen.with_alpha(ready));

        if (stage_fade_.running)
        {
            const float f = stage_fade_.progress();
            draw_stage(list, model, previous_step_, 1.0f - tween::smoothstep(f * 2.0f), -8.0f * f);
            draw_stage(list, model, step_, tween::smoothstep(f), 8.0f * (1.0f - f));
        }
        else
        {
            draw_stage(list, model, step_, 1.0f, 0.0f);
        }

        const int percent = ready_ ? 100 : std::min(99, static_cast<int>(shown * 100.0f));
        std::snprintf(text, sizeof(text), "%d%%", percent);
        ui::text(list, fonts.mono, text, kRight + ui::shake(deny_.value, clock_, 8.0f, 10.0f), 908, 24,
                 gfx::mix(kInk, tone, std::max(ready, deny_.value)), gfx::Align::right);

        const Rect track{kMargin, kBarY, kRight - kMargin, kBarH};
        const Rect fill{track.x, track.y, std::max(kBarH, track.w * shown), track.h};
        list.rounded_rect(track, kBarH * 0.5f, kInk.with_alpha(0.16f));
        list.glow(fill, kBarH * 0.5f, 14 + 22 * flash_.value,
                  tone.with_alpha(0.22f + 0.5f * flash_.value));
        list.gradient_rect_h(fill, kBarH * 0.5f, gfx::mix(tone, kDeep, 0.45f), tone);
        if (!failed)
        {
            // A glint runs along the filled part, so the bar lives while a
            // step takes its time.
            const float glint = std::fmod(clock_, 1.8f) / 1.8f;
            const float glint_x = fill.x - 200.0f + (fill.w + 200.0f) * glint;
            list.push_clip(fill);
            list.gradient_rect_h({glint_x, fill.y, 100, fill.h}, 0, Color::rgb(0xffffff, 0.0f),
                                 Color::rgb(0xffffff, 0.65f));
            list.gradient_rect_h({glint_x + 100, fill.y, 100, fill.h}, 0,
                                 Color::rgb(0xffffff, 0.65f), Color::rgb(0xffffff, 0.0f));
            list.pop_clip();
        }
        if (!ready_)
            list.circle(fill.x + fill.w - 2, fill.y + fill.h * 0.5f, 6, gfx::mix(tone, kInk, 0.6f));
        list.rounded_rect(track, kBarH * 0.5f, Color::rgb(0xffffff, 0.85f * flash_.value));
        list.pop_opacity();
    }

    // How to pair: three steps on cards, then a radar while the console
    // waits for the phone. Fades in over the loader's place.
    void draw_pairing(gfx::DrawList &list, const Model &model) const
    {
        const float in = pair_.value;
        if (in <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float rise = 16.0f * (1.0f - in);
        list.push_opacity(in);

        ui::text(list, fonts.semibold, "Pair with your phone", kCentreX, 712 + rise, 40, kInk,
                 gfx::Align::center);
        ui::text(list, fonts.regular, "Your phone and this PS5 need to be on the same network.",
                 kCentreX, 754 + rise, 24, kInk.with_alpha(0.6f), gfx::Align::center);

        const std::string steps[3][2] = {{"Open Spotify on", "your phone"},
                                         {"Tap the", "devices icon"},
                                         {"Choose", model.device_name}};
        const float left = kCentreX - (3.0f * kCardW + 2.0f * kCardGap) * 0.5f;
        for (int i = 0; i < 3; ++i)
        {
            const float arrive = tween::stagger(pair_age_, i, 0.09f, 0.5f);
            const Rect card{left + static_cast<float>(i) * (kCardW + kCardGap),
                            kCardY + rise + 14.0f * (1.0f - arrive), kCardW, kCardH};
            list.push_opacity(arrive);
            list.bordered_rect(card, 18, kInk.with_alpha(0.05f), 1.5f, kGreen.with_alpha(0.28f));
            const float cx = card.x + 44;
            const float cy = card.y + card.h * 0.5f;
            list.ring(cx, cy, 21, 2, kGreen);
            char digit[2] = {static_cast<char>('1' + i), 0};
            ui::text(list, fonts.mono, digit, cx, cy + 8, 22, kGreen, gfx::Align::center);
            list.push_clip({card.x, card.y, card.w - 20, card.h}); // long device names
            const float lead = ui::text(list, fonts.regular, steps[i][0], card.x + 84, cy + 9, 26,
                                        kInk.with_alpha(0.75f));
            ui::text(list, fonts.semibold, steps[i][1], card.x + 84 + lead + 8, cy + 9, 26,
                     i == 2 ? kMint : kInk);
            list.pop_clip();
            list.pop_opacity();
        }

        // Radar: rings go out from a dot, like the console calling out.
        const std::string waiting = ui::upper("Waiting for your phone");
        const float waiting_w = fonts.semibold.measure(waiting, 17, 3.5f);
        const float spin_x = kCentreX - (waiting_w + 36.0f) * 0.5f + 11.0f;
        const float spin_y = 948;
        const bool reduced = context_.settings.reduced_motion;
        for (int k = 0; k < 3; ++k)
        {
            const float phase =
                reduced ? 0.33f * static_cast<float>(k + 1)
                        : std::fmod(clock_ * 0.55f + static_cast<float>(k) / 3.0f, 1.0f);
            list.ring(spin_x, spin_y, 6.0f + 20.0f * phase, 2,
                      kGreen.with_alpha(0.65f * (1.0f - phase)));
        }
        list.circle(spin_x, spin_y, 5, kMint);
        ui::text(list, fonts.semibold, waiting, spin_x + 25, spin_y + 6, 17, kInk.with_alpha(0.85f),
                 gfx::Align::left, 3.5f);
        list.pop_opacity();
    }

    // Hints: what the buttons do right now.
    void draw_hints(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const bool failed = model.connection == Connection::error;
        list.push_opacity(tween::stagger(age_, 4, 0.08f, 0.6f));
        const ui::GlyphStyle glyphs = ui::GlyphStyle::dark();
        if (failed)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Try again"},
                                      {ui::Button::touchpad, "Settings"}};
            ui::draw_hints(list, fonts, glyphs, hints, 2, kRight, true);
        }
        else
        {
            const ui::Hint hints[] = {{ui::Button::touchpad, "Settings"}};
            ui::draw_hints(list, fonts, glyphs, hints, 1, kRight, true);
        }
        list.pop_opacity();
    }

    Context &context_;
    float age_ = 0.0f;   // since enter(): the entrance
    float clock_ = 0.0f; // free-running: breathing, spinner, glints
    float drift_ = 0.0f; // stands still under reduced motion
    bool failed_ = false;

    int step_ = 0;
    int previous_step_ = 0;
    float step_age_ = 0.0f;
    tween::Timer stage_fade_;
    tween::Spring bar_;
    tween::Spring spinner_;
    tween::Spring pair_; // 1 while pairing: the steps replace the loader
    float pair_age_ = 0.0f;
    ui::Pulse flash_;
    ui::Pulse deny_;
    bool ready_ = false;
    float ready_age_ = 0.0f;
};

} // namespace

std::unique_ptr<Screen> make_launch(Context &context)
{
    return std::make_unique<Launch>(context);
}

bool launch_finished(const Screen &launch)
{
    return static_cast<const Launch &>(launch).finished();
}

} // namespace screens
