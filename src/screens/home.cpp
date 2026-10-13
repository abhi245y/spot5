// ps5-spotify - Home: connected, nothing playing yet.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Layout and motion from ps5-homebrew-ui src/concepts/aurora.cpp
// (GPL-3.0-or-later, BlackBearReloaded): top bar, hero block, floating card.

#include "screens/home.hpp"

#include <cmath>
#include <cstdio>

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

const Color kWhite = Color::rgb(0xffffff);
const Color kTeal = Color::rgb(0x1aa39a);
const Rect kCard{1316, 132, 440, 440};

class Home final : public Screen
{
  public:
    explicit Home(Context &context) : context_(context) {}

    void enter() override { age_ = 0.0f; }

    void update(const Model &, const InputFrame &, float dt, ui::Feedback &,
                std::vector<Command> &) override
    {
        age_ += dt;
        clock_ += dt;
        if (!context_.reduced_motion)
            drift_ += dt;
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        frame.backdrop.colors[0] = Color::rgb(0x03100a);
        frame.backdrop.colors[1] = Color::rgb(0x08241a);
        frame.backdrop.colors[2] = gfx::mix(kGreen, kDeep, 0.35f);
        frame.backdrop.colors[3] = kTeal;
        frame.backdrop.time = drift_;

        gfx::DrawList &list = frame.scene;
        draw_top_bar(list, model);
        draw_card(list, model);
        draw_hero(list, model);
        draw_hints(list);
    }

  private:
    void draw_top_bar(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        const float y = 92 - 10 * (1.0f - in);
        const float w = ui::text(list, fonts.semibold, "Home", kMargin, y, 26, kWhite);
        list.rounded_rect({kMargin, 104, w, 4}, 2, kGreen);

        // This device, online: a chip like the profile badge in the design.
        const std::string name = model.device_name;
        const float tw = fonts.semibold.measure(name, 22);
        const Rect chip{kRight - tw - 70, 60, tw + 70, 46};
        list.rounded_rect(chip, 23, kWhite.with_alpha(0.08f));
        list.bordered_rect(chip, 23, kClear, 1.5f, kWhite.with_alpha(0.18f));
        list.glow({chip.x + 20, chip.cy() - 6, 12, 12}, 6, 10,
                  kGreen.with_alpha(0.4f + 0.3f * ui::breathe(clock_)));
        list.circle(chip.x + 26, chip.cy(), 6, kGreen);
        ui::text(list, fonts.semibold, name, chip.x + 46, chip.cy() + 8, 22, kWhite);
        list.pop_opacity();
    }

    // The card: this console as a speaker, rings going out on the beat of
    // its own breathing, where the design shows a game's cover.
    void draw_card(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.7f);
        const float bob = context_.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 6.0f;
        const Rect art{kCard.x + 40.0f * (1.0f - in), kCard.y + bob, kCard.w, kCard.h};
        list.push_opacity(in);
        list.glow(art.inset(30), 60, 90, kGreen.with_alpha(0.28f));
        list.shadow({art.x, art.y + 26, art.w, art.h}, 36, 46, Color::rgb(0x000000, 0.55f));
        list.gradient_rect(art, 36, Color::rgb(0x0f3a26), Color::rgb(0x051a10));
        list.push_clip(art);
        const float cx = art.cx();
        const float cy = art.y + 196;
        for (int i = 0; i < 4; ++i)
        {
            // Each ring grows outward and fades: sound leaving the speaker.
            const float phase =
                context_.reduced_motion ? 0.25f * i : std::fmod(clock_ * 0.35f + 0.25f * i, 1.0f);
            list.ring(cx, cy, 60.0f + 170.0f * phase, 3.0f,
                      kGreen.with_alpha(0.55f * (1.0f - phase)));
        }
        list.glow({cx - 40, cy - 40, 80, 80}, 40, 50, kGreen.with_alpha(0.35f));
        list.circle(cx, cy, 46, Color::rgb(0x0b2a1b));
        list.ring(cx, cy, 46, 4, kMint);
        list.circle(cx, cy, 16, kMint);
        list.pop_clip();
        ui::text(list, fonts.semibold, "Spotify Connect", art.x + 28, art.y + 48, 20,
                 kInk.with_alpha(0.7f));
        ui::text(list, fonts.display, model.device_name, art.x + 28, art.y + art.h - 34, 54,
                 kWhite);
        list.bordered_rect(art, 36, kClear, 2, kWhite.with_alpha(0.16f));
        list.pop_opacity();
    }

    void draw_step(gfx::DrawList &list, int number, const char *line, float x, float y,
                   float in) const
    {
        const ui::Fonts &fonts = context_.fonts;
        char digit[4];
        std::snprintf(digit, sizeof(digit), "%d", number);
        list.push_opacity(in);
        x += 24.0f * (1.0f - in);
        list.circle(x + 22, y - 9, 22, kWhite.with_alpha(0.1f));
        list.ring(x + 22, y - 9, 22, 1.5f, kGreen.with_alpha(0.7f));
        ui::text(list, fonts.semibold, digit, x + 22, y - 1, 22, kGreen, gfx::Align::center);
        ui::text(list, fonts.regular, line, x + 64, y, 28, kWhite.with_alpha(0.9f));
        list.pop_opacity();
    }

    void draw_hero(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        const float x = kMargin + 30.0f * (1.0f - in);
        list.push_opacity(in);
        ui::text(list, fonts.semibold, "READY TO PLAY", x, 212, 20, kGreen, gfx::Align::left,
                 4.0f);
        ui::text(list, fonts.display, "Play from your phone", x - 4, 304, 88, kWhite);
        ui::text(list, fonts.regular,
                 "Spotify Connect  \xC2\xB7  Very high quality  \xC2\xB7  320 kbps", x, 358, 26,
                 kWhite.with_alpha(0.78f));
        list.pop_opacity();

        char pick[64];
        std::snprintf(pick, sizeof(pick), "Pick \"%s\"", model.device_name.c_str());
        draw_step(list, 1, "Open Spotify on your phone or computer", kMargin, 470,
                  tween::stagger(age_, 2, 0.08f, 0.6f));
        draw_step(list, 2, "Tap the devices icon", kMargin, 540,
                  tween::stagger(age_, 3, 0.08f, 0.6f));
        draw_step(list, 3, pick, kMargin, 610, tween::stagger(age_, 4, 0.08f, 0.6f));
    }

    void draw_hints(gfx::DrawList &list) const
    {
        list.push_opacity(tween::stagger(age_, 6, 0.08f, 0.6f));
        const ui::Hint hints[] = {{ui::Button::options, "Quit"}};
        ui::draw_hints(list, context_.fonts, ui::GlyphStyle::dark(), hints, 1, kRight, true);
        list.pop_opacity();
    }

    Context &context_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float drift_ = 0.0f;
};

} // namespace

std::unique_ptr<Screen> make_home(Context &context)
{
    return std::make_unique<Home>(context);
}

} // namespace screens
