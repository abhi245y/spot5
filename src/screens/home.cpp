// ps5-spotify - Home: your personalised Spotify Home, as shelves of covers.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps5-homebrew-ui's "Aurora Shelf" (src/concepts/aurora.cpp, GPL-3.0-or-later,
// BlackBearReloaded) with the shelves the phone shows (Model::home):
//   - a hero for the focused card, cross-fading on every move, over a backdrop
//     that takes the cover's colours;
//   - the phone's shortcuts as a 2x4 grid of tiles, then shelves of covers
//     with a spring-scrolled row, a grown focused card and a gliding ring;
//   - Cross opens the card's page (screens/playlist); R3 refreshes Home
//     behind a frosted "Refreshing" dialog;
//   - a slim now-playing bar at the bottom once something plays.

#include "screens/home.hpp"

#include <algorithm>
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

// Shelves (Aurora's numbers).
constexpr float kCard = 196.0f;
constexpr float kCardGap = 26.0f;
constexpr float kCardGrow = 1.2f;
constexpr float kShelfY = 730.0f;     // top of the focused shelf
constexpr float kShelfPitch = 304.0f; // distance between shelves
// Shortcut tiles: four across the screen, two lines.
constexpr int kTileColumns = 4;
constexpr float kTileGap = 22.0f;
constexpr float kTileW = (gfx::kVirtualWidth - 2.0f * kMargin - 3.0f * kTileGap) / kTileColumns;
constexpr float kTileH = 84.0f;
// Hero.
const Rect kHeroArt{1316, 132, 440, 440};
constexpr float kHeroTextWidth = 1120.0f;
// Now-playing bar.
// Now-playing pill in the top bar, ending left of the account initial.
const Rect kMiniBar{1740 - 26 - 560, 50, 560, 64};

// Spotify descriptions may carry links (<a href=...>name</a>); keep the text.
std::string plain_text(const std::string &value)
{
    std::string out;
    bool in_tag = false;
    for (char c : value)
    {
        if (c == '<')
            in_tag = true;
        else if (c == '>')
            in_tag = false;
        else if (!in_tag)
            out += c;
    }
    return out;
}

struct Hero
{
    Card card;
    std::string shelf;
};

class Home final : public Screen
{
  public:
    explicit Home(Context &context) : context_(context)
    {
        for (int i = 0; i < 4; ++i)
            palette_[i].snap(default_palette(i));
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &commands) override
    {
        age_ += dt;
        clock_ += dt;
        sync(model);

        if (refreshing_)
        {
            // The dialog holds the screen until the new Home is in.
            seen_loading_ = seen_loading_ || model.home_loading;
            refresh_age_ += dt;
            if ((seen_loading_ && !model.home_loading) || (!seen_loading_ && refresh_age_ > 1.5f))
            {
                refreshing_ = false;
                feedback.play(audio::Cue::modal_close);
            }
        }
        else if (!shelves().empty())
            update_shelves(input, feedback);
        if (!refreshing_ && input.is_pressed(Action::r3))
        {
            feedback.play(audio::Cue::modal_open);
            commands.push_back(Command::refresh_home);
            refreshing_ = true;
            seen_loading_ = false;
            refresh_age_ = 0.0f;
        }

        // ---- animation ----
        if (const Card *focused = focused_card(); focused && focused->uri != shown_.card.uri)
        {
            previous_ = shown_;
            shown_ = {*focused, shelves()[static_cast<std::size_t>(row_)].title};
            hero_.start(context_.settings.reduced_motion ? 0.12f : 0.42f);
        }
        hero_.update(dt);
        apply_palette();
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.0f);
        row_position_.target = static_cast<float>(row_);
        row_position_.update(dt, 11.0f);
        for (int r = 0; r < static_cast<int>(rows_.size()); ++r)
        {
            Row &row = rows_[static_cast<std::size_t>(r)];
            const float start = static_cast<float>(row.index) * (kCard + kCardGap);
            row.scroll.reveal(start, start + kCard * kCardGrow, gfx::kVirtualWidth - kMargin,
                              kMargin * 2.2f);
            row.scroll.update(dt, 12.0f);
        }
        if (!shelves().empty())
            ring_.target(focus_rect(row_, rows_[static_cast<std::size_t>(row_)].index));
        ring_.update(dt, 20.0f);
        nudge_.update(dt, 9.0f);
        dialog_.target = refreshing_ ? 1.0f : 0.0f;
        dialog_.update(dt, context_.settings.reduced_motion ? 40.0f : 13.0f);
        mini_.target = model.has_track ? 1.0f : 0.0f;
        mini_.update(dt, 9.0f);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        for (int i = 0; i < 4; ++i)
            frame.backdrop.colors[i] = palette_[i].value();
        frame.backdrop.time = context_.settings.reduced_motion ? 0.0f : clock_;

        gfx::DrawList &list = frame.scene;
        const float back = dialog_.value;
        list.push_transform(1.0f - 0.035f * back, 960, 540, 0, 0);
        draw_top_bar(list, model);
        if (shelves().empty())
            draw_empty(list, model);
        else
        {
            draw_hero(list);
            draw_shelves(list);
        }
        // A soft scrim under the bottom row: the shelf peeking in below stays
        // a hint and never fights the now-playing bar or the button hints.
        list.gradient_rect({0, 900, gfx::kVirtualWidth, gfx::kVirtualHeight - 900}, 0,
                           palette_[0].value(0.0f), palette_[0].value(0.92f));
        draw_mini_player(list, model);
        list.pop_transform();
        if (back > 0.01f)
        {
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              Color::rgb(0x05070f, 0.45f * back));
            frame.glass = true;
            draw_refreshing(frame.overlay, frame.glass_texture);
        }
        if (back <= 0.5f)
            draw_hints(frame.scene, model);
    }

  private:
    struct Row
    {
        int index = 0; // focused card in this shelf (remembered per shelf)
        ui::Scroller scroll;
    };

    const std::vector<Shelf> &shelves() const
    {
        static const std::vector<Shelf> kNone;
        return home_ ? home_->shelves : kNone;
    }

    // A new page keeps the focus on the same card when it still exists.
    void sync(const Model &model)
    {
        if (model.home == home_)
            return;
        const std::string keep = focused_card() ? focused_card()->uri : std::string();
        home_ = model.home;
        rows_.assign(shelves().size(), Row{});
        row_ = 0;
        for (int r = 0; r < static_cast<int>(shelves().size()); ++r)
        {
            const auto &cards = shelves()[static_cast<std::size_t>(r)].cards;
            for (int c = 0; c < static_cast<int>(cards.size()); ++c)
                if (!keep.empty() && cards[static_cast<std::size_t>(c)].uri == keep)
                {
                    row_ = r;
                    rows_[static_cast<std::size_t>(r)].index = c;
                }
        }
        row_position_.snap(static_cast<float>(row_));
        if (!shelves().empty())
            ring_.snap(focus_rect(row_, rows_[static_cast<std::size_t>(row_)].index));
    }

    const Card *focused_card() const
    {
        if (shelves().empty() || row_ >= static_cast<int>(shelves().size()))
            return nullptr;
        const auto &cards = shelves()[static_cast<std::size_t>(row_)].cards;
        const int index = rows_[static_cast<std::size_t>(row_)].index;
        return index < static_cast<int>(cards.size()) ? &cards[static_cast<std::size_t>(index)] : nullptr;
    }

    Art art(const std::string &url) const
    {
        return context_.art && !url.empty() ? context_.art(url) : Art{};
    }

    static Color default_palette(int i)
    {
        const Color colours[4] = {Color::rgb(0x03100a), Color::rgb(0x08241a),
                                  gfx::mix(kGreen, kDeep, 0.35f), kTeal};
        return colours[i];
    }

    // The backdrop follows the hero's cover once it has loaded.
    void apply_palette()
    {
        const Art cover = art(shown_.card.image_large);
        const Art small = cover.texture ? cover : art(shown_.card.image);
        for (int i = 0; i < 4; ++i)
        {
            Color target = default_palette(i);
            if (small.texture)
            {
                const AlbumPalette &p = small.palette;
                const Color targets[4] = {gfx::mix(p.dark, Color::rgb(0x05060c), 0.35f),
                                          gfx::mix(p.dark, p.mid, 0.35f), p.mid,
                                          gfx::mix(p.mid, p.accent, 0.55f)};
                target = targets[i];
            }
            palette_[i].target(target);
        }
    }

    Color accent() const
    {
        return palette_[3].value();
    }

    bool is_grid(int row) const
    {
        return shelves()[static_cast<std::size_t>(row)].shortcuts;
    }

    float row_top(int row) const
    {
        return kShelfY + (static_cast<float>(row) - row_position_.value) * kShelfPitch;
    }

    // A card or tile at rest (no growth, no room-making).
    Rect item_rect(int row, int index) const
    {
        const float top = row_top(row);
        if (is_grid(row))
        {
            const int column = index % kTileColumns;
            const int line = index / kTileColumns;
            return {kMargin + static_cast<float>(column) * (kTileW + kTileGap),
                    top - 40.0f + static_cast<float>(line) * (kTileH + kTileGap), kTileW, kTileH};
        }
        const float x = kMargin + static_cast<float>(index) * (kCard + kCardGap) -
                        rows_[static_cast<std::size_t>(row)].scroll.offset();
        return {x, top, kCard, kCard};
    }

    // Where the ring goes: the grown card, or a little around a tile.
    Rect focus_rect(int row, int index) const
    {
        const Rect rest = item_rect(row, index);
        if (is_grid(row))
            return rest.inset(-4);
        const float grown = kCard * kCardGrow;
        return {rest.x, rest.y - (grown - kCard), grown, grown};
    }

    void refuse(ui::Feedback &feedback, const InputFrame &input, float direction)
    {
        if (input.nav_repeat)
            return;
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_direction_ = direction;
        nudge_.trigger();
    }

    void update_shelves(const InputFrame &input, ui::Feedback &feedback)
    {
        Row &row = rows_[static_cast<std::size_t>(row_)];
        const int count = static_cast<int>(shelves()[static_cast<std::size_t>(row_)].cards.size());
        const bool grid = is_grid(row_);
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int step = input.nav == Direction::right ? 1 : -1;
            const int next = row.index + step;
            const bool same_line = !grid || next / kTileColumns == row.index / kTileColumns;
            if (next >= 0 && next < count && (same_line || !grid))
            {
                row.index = next;
                feedback.play(audio::Cue::focus, 1.0f, ui::pan_for_x(item_rect(row_, next).cx()));
            }
            else
                refuse(feedback, input, static_cast<float>(step));
            break;
        }
        case Direction::up:
        case Direction::down:
        {
            const int step = input.nav == Direction::down ? 1 : -1;
            // Inside the grid, up/down moves between its lines first.
            if (grid)
            {
                const int next = row.index + step * kTileColumns;
                if (next >= 0 && next < count)
                {
                    row.index = next;
                    feedback.play(audio::Cue::focus, 1.05f);
                    break;
                }
            }
            const int next_row = row_ + step;
            if (next_row >= 0 && next_row < static_cast<int>(shelves().size()))
            {
                row_ = next_row;
                feedback.play(audio::Cue::tab, 1.12f - 0.04f * static_cast<float>(std::min(row_, 6)));
            }
            else
                refuse(feedback, input, 0.0f);
            break;
        }
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm) && focused_card())
        {
            Request request;
            request.kind = Request::Kind::open;
            request.card = *focused_card();
            context_.requests.push_back(request);
            feedback.play(audio::Cue::open);
        }
    }

    // ---- drawing ----

    void draw_cover(gfx::DrawList &list, const Card &card, const std::string &url, const Rect &r,
                    float radius, float alpha) const
    {
        const float corner = card.round ? r.w * 0.5f : radius;
        const Art cover = art(url);
        if (cover.texture)
        {
            list.image(cover.texture, r, gfx::kFullUv, kWhite.with_alpha(alpha), corner);
            return;
        }
        // Loading: a soft tile with the title's first letter.
        list.rounded_rect(r, corner, kWhite.with_alpha(0.07f * alpha));
        const std::string letter = card.title.substr(0, 1);
        ui::text(list, context_.fonts.semibold, ui::upper(letter), r.cx(), r.cy() + r.h * 0.14f,
                 r.h * 0.4f, kWhite.with_alpha(0.25f * alpha), gfx::Align::center);
    }

    void draw_top_bar(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        const char *tabs[] = {"Home", "Library"};
        float x = kMargin;
        for (int i = 0; i < 2; ++i)
        {
            const bool active = i == 0;
            const float w = ui::text(list, active ? fonts.semibold : fonts.regular, tabs[i], x,
                                     92 - 10 * (1.0f - in), 26, kWhite.with_alpha(active ? 1.0f : 0.4f));
            if (active)
                list.rounded_rect({x, 104, w, 4}, 2, accent());
            x += w + 44;
        }
        // Account initial, and a spinner while Home is being fetched.
        if (!model.username.empty())
        {
            list.circle(1800, 82, 22, accent());
            ui::text(list, fonts.semibold, ui::upper(model.username.substr(0, 1)), 1800, 91, 22,
                     Color::rgb(0x0b0d16), gfx::Align::center);
        }
        if (model.home_loading)
            list.arc(model.has_track ? kMiniBar.x - 34 : 1740, 82, 12, 3, clock_ * 5.5f, 2.2f,
                     kWhite.with_alpha(0.7f));
        list.pop_opacity();
    }

    void draw_empty(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const float x = kMargin + 30.0f * (1.0f - in);
        ui::text(list, fonts.semibold, "HOME", x, 212, 20, accent(), gfx::Align::left, 4.0f);
        if (model.home_loading)
        {
            ui::text(list, fonts.display, "Loading your Home", x - 4, 304, 88, kWhite);
            ui::text(list, fonts.regular, "Your shelves, mixes and recent plays, as on your phone.", x,
                     358, 26, kWhite.with_alpha(0.78f));
        }
        else
        {
            ui::text(list, fonts.display, "Home isn't here yet", x - 4, 304, 88, kWhite);
            ui::text(list, fonts.regular,
                     "Press R3 to try again. You can still play from your phone.", x, 358, 26,
                     kWhite.with_alpha(0.78f));
        }
        list.pop_opacity();
    }

    // One card's hero block, drawn at an opacity and an offset so two of them
    // can cross-fade.
    void draw_hero_item(gfx::DrawList &list, const Hero &hero, float alpha, float slide) const
    {
        if (alpha <= 0.01f || hero.card.uri.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const Card &card = hero.card;
        list.push_opacity(alpha);

        const float bob = context_.settings.reduced_motion ? 0.0f : std::sin(clock_ * 0.8f) * 6.0f;
        const Rect cover{kHeroArt.x + slide * 1.6f, kHeroArt.y + bob, kHeroArt.w, kHeroArt.h};
        list.glow(cover.inset(30), 60, 90, accent().with_alpha(0.3f));
        list.shadow({cover.x, cover.y + 26, cover.w, cover.h}, 36, 46, Color::rgb(0x000000, 0.55f));
        const Art large = art(card.image_large);
        draw_cover(list, card, large.texture ? card.image_large : card.image, cover, 36, 1.0f);
        if (!card.round)
            list.bordered_rect(cover, 36, Color::rgb(0x000000, 0.0f), 2, kWhite.with_alpha(0.16f));

        const float x = kMargin + slide;
        ui::text(list, fonts.semibold, ui::upper(hero.shelf.empty() ? "Shortcuts" : hero.shelf), x, 212,
                 20, accent(), gfx::Align::left, 4.0f);
        ui::text(list, fonts.display, fonts.display.font->fit(card.title, 80, kHeroTextWidth), x - 4,
                 300, 80, kWhite);
        ui::text(list, fonts.regular, card.subtitle, x, 354, 26, kWhite.with_alpha(0.78f));
        ui::paragraph(list, fonts.regular, plain_text(card.description), x, 414, 28, kHeroTextWidth - 200,
                      40, kWhite.with_alpha(0.86f), 2);
        list.pop_opacity();
    }

    void draw_hero(gfx::DrawList &list) const
    {
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        if (hero_.running)
        {
            // The old card leaves quickly; the new one arrives a beat later.
            const float t = hero_.progress();
            draw_hero_item(list, previous_, 1.0f - tween::smoothstep(t * 2.2f),
                           -36.0f * tween::cubic_in(tween::clamp01(t * 2.2f)));
            const float arrive = tween::clamp01((t - 0.25f) / 0.75f);
            draw_hero_item(list, shown_, tween::smoothstep(arrive), 44.0f * (1.0f - tween::quint_out(arrive)));
        }
        else
        {
            draw_hero_item(list, shown_, 1.0f, 30.0f * (1.0f - in));
        }
        list.pop_opacity();
    }

    void draw_tile(gfx::DrawList &list, const Card &card, const Rect &r, bool focused, float alpha) const
    {
        list.rounded_rect(r, 10, kWhite.with_alpha((focused ? 0.18f : 0.08f) * alpha));
        draw_cover(list, card, card.image, {r.x, r.y, r.h, r.h}, 10, alpha);
        const ui::Fonts &fonts = context_.fonts;
        const float width = r.w - r.h - 36.0f;
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(card.title, 23, width), r.x + r.h + 20,
                 r.cy() + 8, 23, kWhite.with_alpha(alpha));
    }

    void draw_shelves(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float nudge = ui::shake(nudge_.value, clock_, 16.0f, 8.0f) * nudge_direction_;
        for (int r = 0; r < static_cast<int>(shelves().size()); ++r)
        {
            const Shelf &shelf = shelves()[static_cast<std::size_t>(r)];
            // Shelves above the focused one are gone; the one below peeks in.
            const float distance = static_cast<float>(r) - row_position_.value;
            const float visible = tween::clamp01(1.0f + distance * 2.5f) *
                                  (distance > 0.0f ? 1.0f - 0.45f * tween::clamp01(distance) : 1.0f);
            if (visible <= 0.01f || distance > 1.6f)
                continue;
            const float in = tween::stagger(age_, 3 + std::min(r, 4), 0.09f, 0.6f);
            const float lift = 40.0f * (1.0f - in);
            list.push_opacity(visible * in);
            list.push_transform(1.0f, 0, 0, 0, lift);
            const Row &row = rows_[static_cast<std::size_t>(r)];
            if (!shelf.shortcuts)
                ui::text(list, fonts.semibold, shelf.title, kMargin, row_top(r) - 62, 24,
                         kWhite.with_alpha(r == row_ ? 0.95f : 0.6f));
            for (int c = 0; c < static_cast<int>(shelf.cards.size()); ++c)
            {
                const Card &card = shelf.cards[static_cast<std::size_t>(c)];
                Rect rect = item_rect(r, c);
                if (rect.x > gfx::kVirtualWidth || rect.x + rect.w < -80.0f)
                    continue;
                const bool focused = r == row_ && c == row.index;
                if (shelf.shortcuts)
                {
                    draw_tile(list, card, rect, focused, 1.0f);
                    continue;
                }
                if (focused)
                    continue; // drawn last, on top
                // Cards to the right of a grown card make room for it.
                if (r == row_ && c > row.index)
                    rect.x += kCard * (kCardGrow - 1.0f);
                draw_cover(list, card, card.image, rect, 22, 0.82f);
            }
            list.pop_transform();
            list.pop_opacity();
        }

        // The focused item: ringed (and for cards, grown and lifted).
        const Card *card = focused_card();
        if (!card)
            return;
        Rect ring = ring_.value();
        ring.x += nudge;
        const float in = tween::stagger(age_, 3 + std::min(row_, 4), 0.09f, 0.6f);
        list.push_opacity(in);
        if (is_grid(row_))
        {
            list.bordered_rect(ring, 14, Color::rgb(0x000000, 0.0f), 3, kWhite);
        }
        else
        {
            const float corner = card->round ? ring.w * 0.5f : 26.0f;
            list.shadow({ring.x, ring.y + 18, ring.w, ring.h}, corner, 34, Color::rgb(0x000000, 0.6f));
            list.glow(ring, corner, 26, accent().with_alpha(0.4f + 0.15f * ui::breathe(clock_)));
            draw_cover(list, *card, card->image, ring, 26, 1.0f);
            list.bordered_rect(ring.inset(-5), corner + 4, Color::rgb(0x000000, 0.0f), 4, kWhite);
        }
        list.pop_opacity();
    }

    void draw_mini_player(gfx::DrawList &list, const Model &model) const
    {
        const float shown = mini_.value;
        if (shown <= 0.01f || !model.has_track)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const Rect bar{kMiniBar.x + 30.0f * (1.0f - shown), kMiniBar.y, kMiniBar.w, kMiniBar.h};
        list.push_opacity(shown);
        list.shadow({bar.x, bar.y + 10, bar.w, bar.h}, 18, 30, Color::rgb(0x000000, 0.5f));
        list.rounded_rect(bar, 18, Color::rgb(0x0b0d16, 0.82f));
        list.bordered_rect(bar, 18, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.14f));
        const Rect cover{bar.x + 10, bar.y + 10, bar.h - 20, bar.h - 20};
        if (model.track.art)
            list.image(model.track.art, cover, gfx::kFullUv, kWhite, 8);
        else
            list.rounded_rect(cover, 8, kWhite.with_alpha(0.1f));
        const float x = cover.x + cover.w + 18;
        const float width = bar.w - (x - bar.x) - 70;
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(model.track.title, 22, width), x,
                 bar.y + 29, 22, kWhite);
        ui::text(list, fonts.regular, fonts.regular.font->fit(model.track.artist, 19, width), x,
                 bar.y + 53, 19, kWhite.with_alpha(0.65f));
        // Play state at the right; progress as a hairline along the bottom.
        const float px = bar.x + bar.w - 40;
        if (model.paused)
            list.triangle({px - 9, bar.cy() - 10, 18, 20}, kWhite, 0.0f, 1.5708f);
        else
        {
            list.rounded_rect({px - 8, bar.cy() - 10, 6, 20}, 2, kWhite);
            list.rounded_rect({px + 3, bar.cy() - 10, 6, 20}, 2, kWhite);
        }
        if (model.track.duration_ms > 0)
        {
            const float done = std::min(1.0f, static_cast<float>(model.position_ms) /
                                                  static_cast<float>(model.track.duration_ms));
            list.rounded_rect({bar.x + 18, bar.y + bar.h - 5, (bar.w - 36) * done, 3}, 1.5f,
                              accent());
        }
        list.pop_opacity();
    }

    // R3: a frosted dialog over Home while the new one is fetched.
    void draw_refreshing(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = dialog_.value;
        const Rect panel{kCentreX - 330, kCentreY - 110 + 30.0f * (1.0f - t), 660, 220};
        list.push_opacity(tween::clamp01(t * 1.4f));
        list.shadow({panel.x, panel.y + 18, panel.w, panel.h}, 36, 50, Color::rgb(0x000000, 0.5f));
        list.glass(glass, panel, 36, kWhite);
        list.rounded_rect(panel, 36, gfx::mix(palette_[0].value(), Color::rgb(0x0b0d16), 0.5f).with_alpha(0.62f));
        list.bordered_rect(panel, 36, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.22f));
        list.arc(panel.x + 70, panel.cy(), 22, 4, 0.0f, kTau, kWhite.with_alpha(0.12f));
        list.arc(panel.x + 70, panel.cy(), 22, 4, clock_ * 5.5f, 2.2f, accent());
        ui::text(list, fonts.semibold, "Refreshing your Home", panel.x + 120, panel.cy() - 6, 32, kWhite);
        ui::text(list, fonts.regular, "Shelves, mixes and artwork", panel.x + 120, panel.cy() + 34, 24,
                 kWhite.with_alpha(0.7f));
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - dialog_.value));
        ui::Hint hints[4];
        int count = 0;
        if (focused_card())
            hints[count++] = {ui::Button::cross, "Open"};
        if (model.has_track)
            hints[count++] = {ui::Button::triangle, "Now Playing"};
        hints[count++] = {ui::Button::right_stick, "Refresh"};
        hints[count++] = {ui::Button::touchpad, "Settings"};
        ui::draw_hints(list, fonts, style, hints, count, kRight, true);
        list.pop_opacity();
    }

    Context &context_;
    std::shared_ptr<const HomePage> home_;
    std::vector<Row> rows_;
    int row_ = 0;
    float age_ = 0.0f;   // seconds since enter(): drives the entrance
    float clock_ = 0.0f; // free-running time for idle motion
    Hero shown_;         // the card the hero shows
    Hero previous_;      // ... and the one it is fading out
    tween::Timer hero_;
    ui::SpringColor palette_[4];
    tween::Spring row_position_;
    ui::SpringRect ring_;
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    bool refreshing_ = false;   // R3 dialog up
    bool seen_loading_ = false; // ... and the fetch has started
    float refresh_age_ = 0.0f;
    tween::Spring dialog_;
    tween::Spring mini_;
};

} // namespace

std::unique_ptr<Screen> make_home(Context &context)
{
    return std::make_unique<Home>(context);
}

} // namespace screens
