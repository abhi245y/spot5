// spot5 - Your Library: playlists, albums and artists as a card grid.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens/library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

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

// Spotify's dark surfaces, and its green for the focus.
const Color kTable = Color::rgb(0x0b0b0d);
const Color kSurface = Color::rgb(0x1c1c1f);
const Color kSurfaceLift = Color::rgb(0x2a2a2e);
const Color kWhite = Color::rgb(0xffffff);
const Color kBlack = Color::rgb(0x000000);
const Color kText = Color::rgb(0xf4f4f6);
const Color kMuted = Color::rgb(0xa7a7ad);

// The grid: Paper Library's numbers.
constexpr int kColumns = 6;
constexpr float kCardW = 268.0f;
constexpr float kCardH = 344.0f;
constexpr float kCardGap = 24.0f;
constexpr float kRowPitch = kCardH + kCardGap;
constexpr float kCardRadius = 14.0f;
constexpr float kCoverInset = 12.0f;
constexpr float kLift = 0.08f;
constexpr float kTilt = 0.042f;
constexpr float kRingGap = 12.0f;
constexpr float kGridTop = 300.0f;
constexpr float kGridBottom = 990.0f;
constexpr float kGridView = kGridBottom - kGridTop;
constexpr float kRowInset = 30.0f;
constexpr float kReveal = 38.0f;
constexpr float kFeather = 14.0f;
constexpr float kEdgeCover = 8.0f;

// Header (under the Home · Library tabs).
constexpr float kTitleBaseline = 196.0f;
constexpr float kChipY = 222.0f;
constexpr float kChipH = 46.0f;
constexpr float kChipGap = 10.0f;
constexpr float kShoulder = 34.0f;

constexpr float kTravelStep = 0.014f;
constexpr float kAppearStep = 0.028f;
constexpr float kAppearWait = 0.09f;

enum class Filter : int
{
    all,
    playlists,
    albums,
    artists,
    count,
};
constexpr int kFilters = static_cast<int>(Filter::count);
struct Chip
{
    const char *label;
    float width;
};
constexpr Chip kChips[kFilters] = {{"All", 112.0f}, {"Playlists", 168.0f}, {"Albums", 150.0f}, {"Artists", 150.0f}};

enum class Sort : int
{
    recents,
    added,
    title,
    creator,
    count,
};
constexpr int kSorts = static_cast<int>(Sort::count);
constexpr const char *kSortNames[kSorts] = {"Recents", "Recently added", "A \xE2\x80\x93 Z", "Creator"};

// One card's motion. It belongs to the item (by URI), so it survives every
// re-ordering and refresh: only its targets change.
struct CardMotion
{
    tween::Spring x, y;
    tween::Spring shown;
    tween::Spring lift;
    float target_x = 0.0f;
    float target_y = 0.0f;
    float delay = 0.0f;
    bool wanted = false;
    int slot = 0;
};

Rect slot_rect(int slot)
{
    const int column = slot % kColumns;
    const int row = slot / kColumns;
    return {kMargin + static_cast<float>(column) * (kCardW + kCardGap),
            kRowInset + static_cast<float>(row) * kRowPitch, kCardW, kCardH};
}

Rect scaled(const Rect &r, float scale)
{
    return {r.cx() - r.w * scale * 0.5f, r.cy() - r.h * scale * 0.5f, r.w * scale, r.h * scale};
}

Rect chip_rect(int index)
{
    float x = kMargin + ui::button_width(ui::Button::l2, kShoulder) + 18.0f;
    for (int i = 0; i < index; ++i)
        x += kChips[i].width + kChipGap;
    return {x, kChipY, kChips[index].width, kChipH};
}

std::string lower(std::string value)
{
    for (char &c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return value;
}

class LibraryScreen final : public Screen
{
  public:
    explicit LibraryScreen(Context &context) : context_(context)
    {
        chip_.snap(chip_rect(0));
        for (ui::SpringColor &colour : palette_)
            colour.snap(kTable);
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &commands) override
    {
        const bool reduced = context_.settings.reduced_motion;
        age_ += dt;
        clock_ += dt;
        if (model.library != library_)
        {
            library_ = model.library;
            rebuild(motion_.empty());
        }
        update_grid(model, input, feedback, commands);

        // ---- animation ----
        const std::string focused = focused_uri();
        for (auto &[uri, card] : motion_)
        {
            card.delay -= dt;
            if (card.delay <= 0.0f)
            {
                card.delay = 0.0f;
                card.x.target = card.target_x;
                card.y.target = card.target_y;
                card.shown.target = card.wanted ? 1.0f : 0.0f;
            }
            card.x.update(dt, reduced ? 60.0f : 9.5f);
            card.y.update(dt, reduced ? 60.0f : 9.5f);
            card.shown.update(dt, 16.0f);
            card.lift.target = uri == focused ? 1.0f : 0.0f;
            card.lift.update(dt, reduced ? 40.0f : 18.0f);
        }
        if (!focused.empty())
        {
            const Rect row = slot_rect(focus_);
            scroll_.reveal(row.y - kEdgeCover, row.y + row.h, kGridView, kReveal, content_height());
            ring_.target(ring_target());
        }
        else
        {
            scroll_.position.target = 0.0f;
        }
        scroll_.update(dt, reduced ? 40.0f : 12.0f);
        ring_.update(dt, reduced ? 60.0f : 20.0f);
        ring_alpha_.target = focused.empty() ? 0.0f : 1.0f;
        ring_alpha_.update(dt, 14.0f);
        nudge_.update(dt, 9.0f);
        chip_.target(chip_rect(static_cast<int>(filter_)));
        chip_.update(dt, reduced ? 60.0f : 18.0f);
        sort_label_.update(dt);

        // The table takes the colours of the focused cover, eased.
        if (const LibraryItem *item = focused_item())
        {
            const Art art = cover(item->card.image);
            if (art.texture)
            {
                const AlbumPalette &p = art.palette;
                const Color targets[4] = {gfx::mix(p.dark, kTable, 0.7f), gfx::mix(p.dark, kTable, 0.5f),
                                          gfx::mix(p.mid, kTable, 0.55f), gfx::mix(p.accent, kTable, 0.5f)};
                for (int i = 0; i < 4; ++i)
                    palette_[i].target(targets[i]);
            }
        }
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 3.0f);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        for (int i = 0; i < 4; ++i)
            frame.backdrop.colors[i] = palette_[i].value();
        frame.backdrop.time = context_.settings.reduced_motion ? 0.0f : clock_ * 0.6f;
        gfx::DrawList &list = frame.scene;
        draw_grid(list);
        draw_edges(list);
        draw_empty(list, model);
        draw_header(list, model);
        draw_hints(list, model);
    }

  private:
    const std::vector<LibraryItem> &items() const
    {
        static const std::vector<LibraryItem> kNone;
        return library_ ? library_->items : kNone;
    }

    const LibraryItem *focused_item() const
    {
        return visible_.empty() ? nullptr : &items()[static_cast<std::size_t>(visible_[static_cast<std::size_t>(focus_)])];
    }

    std::string focused_uri() const
    {
        const LibraryItem *item = focused_item();
        return item ? item->card.uri : std::string();
    }

    Art cover(const std::string &url) const
    {
        return context_.art && !url.empty() ? context_.art(url) : Art{};
    }

    bool matches(const LibraryItem &item, Filter filter) const
    {
        switch (filter)
        {
        case Filter::playlists:
            return item.kind == LibraryItem::Kind::playlist || item.kind == LibraryItem::Kind::collection;
        case Filter::albums:
            return item.kind == LibraryItem::Kind::album;
        case Filter::artists:
            return item.kind == LibraryItem::Kind::artist;
        case Filter::all:
        case Filter::count:
            break;
        }
        return true;
    }

    int count_of(Filter filter) const
    {
        int count = 0;
        for (const LibraryItem &item : items())
            count += matches(item, filter) ? 1 : 0;
        return count;
    }

    // Pinned first (Liked Songs), then the chosen order; titles break ties.
    bool before(const LibraryItem &a, const LibraryItem &b) const
    {
        if (a.pinned != b.pinned)
            return a.pinned;
        switch (sort_)
        {
        case Sort::recents:
            return a.recent < b.recent;
        case Sort::added:
            if (a.added_at != b.added_at)
                return a.added_at > b.added_at;
            break;
        case Sort::creator:
            if (lower(a.creator) != lower(b.creator))
                return lower(a.creator) < lower(b.creator);
            break;
        case Sort::title:
        case Sort::count:
            break;
        }
        return lower(a.card.title) < lower(b.card.title);
    }

    float content_height() const
    {
        const int rows = (static_cast<int>(visible_.size()) + kColumns - 1) / kColumns;
        return kRowInset + static_cast<float>(rows) * kRowPitch - kCardGap + kReveal;
    }

    Rect ring_target() const
    {
        const auto found = motion_.find(focused_uri());
        if (found == motion_.end())
            return ring_.value();
        return scaled({found->second.x.value, found->second.y.value, kCardW, kCardH}, 1.0f + kLift);
    }

    // Which cards are on the table and where each belongs, after a filter,
    // sort or library change. Nothing moves here: each card gets a target
    // and a delay, and the springs do the travel.
    void rebuild(bool snap)
    {
        const bool reduced = context_.settings.reduced_motion;
        const std::string keep = focused_uri();
        visible_.clear();
        for (int i = 0; i < static_cast<int>(items().size()); ++i)
            if (matches(items()[static_cast<std::size_t>(i)], filter_))
                visible_.push_back(i);
        std::stable_sort(visible_.begin(), visible_.end(), [this](int a, int b) {
            return before(items()[static_cast<std::size_t>(a)], items()[static_cast<std::size_t>(b)]);
        });
        focus_ = std::clamp(focus_, 0, std::max(0, static_cast<int>(visible_.size()) - 1));
        for (int slot = 0; slot < static_cast<int>(visible_.size()); ++slot)
            if (items()[static_cast<std::size_t>(visible_[static_cast<std::size_t>(slot)])].card.uri == keep)
                focus_ = slot;

        for (auto &[uri, card] : motion_)
            card.wanted = false;
        for (int slot = 0; slot < static_cast<int>(visible_.size()); ++slot)
        {
            const std::string &uri = items()[static_cast<std::size_t>(visible_[static_cast<std::size_t>(slot)])].card.uri;
            const bool fresh = motion_.find(uri) == motion_.end();
            CardMotion &card = motion_[uri];
            const Rect place = slot_rect(slot);
            card.wanted = true;
            card.slot = slot;
            card.target_x = place.x;
            card.target_y = place.y;
            if (snap || fresh || reduced || card.shown.value < 0.02f)
            {
                // New, unseen or reduced motion: it simply appears where it belongs.
                card.x.snap(place.x);
                card.y.snap(place.y);
                if (snap)
                    card.shown.snap(1.0f);
                card.delay = snap || reduced ? 0.0f : kAppearWait + kAppearStep * static_cast<float>(std::min(slot, 17));
            }
            else
            {
                card.delay = kTravelStep * static_cast<float>(std::min(slot, 40));
            }
        }
        for (auto &[uri, card] : motion_)
            if (!card.wanted)
                card.delay = 0.0f; // leaving cards go at once
        if (snap)
            ring_.snap(ring_target());
    }

    void refuse(ui::Feedback &feedback, float dx, float dy)
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_.trigger();
        nudge_x_ = dx;
        nudge_y_ = dy;
    }

    void update_grid(const Model &, const InputFrame &input, ui::Feedback &feedback, std::vector<Command> &commands)
    {
        const int count = static_cast<int>(visible_.size());
        const int turn = (input.is_pressed(Action::jump_next) ? 1 : 0) - (input.is_pressed(Action::jump_prev) ? 1 : 0);
        if (turn != 0)
        {
            filter_ = static_cast<Filter>((static_cast<int>(filter_) + turn + kFilters) % kFilters);
            feedback.play(audio::Cue::tab, 0.94f + 0.04f * static_cast<float>(filter_),
                          ui::pan_for_x(chip_rect(static_cast<int>(filter_)).cx()));
            rebuild(false);
            return;
        }
        if (input.is_pressed(Action::west))
        {
            previous_sort_ = sort_;
            sort_ = static_cast<Sort>((static_cast<int>(sort_) + 1) % kSorts);
            sort_label_.start(context_.settings.reduced_motion ? 0.12f : 0.36f);
            feedback.play(audio::Cue::flip, 0.96f + 0.05f * static_cast<float>(sort_));
            rebuild(false);
            return;
        }
        if (input.is_pressed(Action::r3))
        {
            feedback.play(audio::Cue::select);
            commands.push_back(Command::refresh_library);
        }
        if (input.nav != Direction::none)
        {
            if (count == 0)
            {
                if (!input.nav_repeat)
                    refuse(feedback, 0.0f, 0.0f);
            }
            else
                navigate(input, feedback, count);
        }
        if (input.is_pressed(Action::confirm) && focused_item())
        {
            Request request;
            request.kind = Request::Kind::open;
            request.card = focused_item()->card;
            context_.requests.push_back(request);
            feedback.play(audio::Cue::open);
        }
    }

    void navigate(const InputFrame &input, ui::Feedback &feedback, int count)
    {
        const int column = focus_ % kColumns;
        const int row = focus_ / kColumns;
        const int last_row = (count - 1) / kColumns;
        int next = focus_;
        float dx = 0.0f, dy = 0.0f;
        switch (input.nav)
        {
        case Direction::left:
            dx = -1.0f;
            if (column > 0)
                next = focus_ - 1;
            break;
        case Direction::right:
            dx = 1.0f;
            if (column < kColumns - 1 && focus_ + 1 < count)
                next = focus_ + 1;
            break;
        case Direction::up:
            dy = -1.0f;
            if (row > 0)
                next = focus_ - kColumns;
            break;
        case Direction::down:
            dy = 1.0f;
            if (row < last_row)
                next = std::min(focus_ + kColumns, count - 1);
            break;
        case Direction::none:
            break;
        }
        if (next != focus_)
        {
            focus_ = next;
            feedback.play(audio::Cue::focus, 1.05f - 0.035f * static_cast<float>(std::min(next / kColumns, 8)),
                          ui::pan_for_x(slot_rect(next).cx()));
        }
        else if (!input.nav_repeat)
            refuse(feedback, dx, dy);
    }

    // ---- drawing ----

    void draw_card(gfx::DrawList &list, const LibraryItem &item, const CardMotion &card, bool focused) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const bool reduced = context_.settings.reduced_motion;
        const int first = kColumns * static_cast<int>(scroll_.position.target / kRowPitch);
        const int order = std::clamp(card.slot - first, 0, 17);
        const float in = tween::stagger(age_, 3 + order, 0.035f, 0.45f);
        const float alpha = tween::clamp01(card.shown.value) * in;
        if (alpha <= 0.01f)
            return;
        const float lift = card.lift.value;
        const float speed = std::hypot(card.x.velocity, card.y.velocity);
        const float flight = reduced ? 0.0f : tween::clamp01(speed / 1600.0f);
        const float air = std::max(lift, flight);
        Rect r{card.x.value, card.y.value + kGridTop - scroll_.offset(), kCardW, kCardH};
        if (r.y > kGridBottom + 60.0f || r.y + r.h < kGridTop - 60.0f)
            return;
        if (!reduced)
            r.y += 34.0f * (1.0f - in);
        if (focused)
        {
            r.x += ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_x_;
            r.y += ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_y_;
        }
        const float scale = (0.9f + 0.1f * tween::clamp01(card.shown.value)) * (1.0f + kLift * lift + 0.03f * flight);
        list.push_opacity(alpha);
        list.push_transform(scale, r.cx(), r.cy(), 0, 0);
        list.shadow({r.x, r.y + 5 + 17 * air, r.w, r.h}, kCardRadius, 14 + 28 * air, kBlack.with_alpha(0.5f + 0.2f * air));
        // The lifted card turns a second sheet out from under it.
        const float side = card.slot % 2 == 0 ? 1.0f : -1.0f;
        const float lean = std::clamp(card.x.velocity * 0.00006f, -0.07f, 0.07f);
        const float tilt = reduced ? 0.0f : kTilt * lift * side - lean;
        if (std::fabs(tilt) > 0.002f)
            list.rotated_rect(r, kCardRadius, tilt, kSurfaceLift);
        list.gradient_rect(r, kCardRadius, gfx::mix(kSurface, kSurfaceLift, lift), kSurface);

        const Rect art{r.x + kCoverInset, r.y + kCoverInset, r.w - 2 * kCoverInset, r.w - 2 * kCoverInset};
        const float corner = item.card.round ? art.w * 0.5f : 8.0f;
        const Art image = cover(item.card.image);
        if (image.texture)
            list.image(image.texture, art, gfx::kFullUv, kWhite, corner);
        else
        {
            list.rounded_rect(art, corner, kWhite.with_alpha(0.06f));
            ui::text(list, fonts.semibold, ui::upper(item.card.title.substr(0, 1)), art.cx(), art.cy() + 22, 64,
                     kWhite.with_alpha(0.2f), gfx::Align::center);
        }
        const float text_x = r.x + kCoverInset + 2;
        const float room = r.w - 2 * kCoverInset - 4;
        const float base = art.y + art.h;
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(item.card.title, 23, room), text_x, base + 32, 23,
                 focused ? kWhite : kText);
        ui::text(list, fonts.regular, fonts.regular.font->fit(item.card.subtitle, 19, room), text_x, base + 58, 19, kMuted);
        if (item.pinned)
            list.rotated_rect({r.x + r.w - 30, r.y + r.w - 4, 12, 12}, 2, 0.785f, kGreen);
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_grid(gfx::DrawList &list) const
    {
        const std::string focused = focused_uri();
        list.push_clip({0, kGridTop, gfx::kVirtualWidth, kGridView});
        // Leaving cards lowest, then the shelf in order, then the focus.
        for (const LibraryItem &item : items())
        {
            const auto found = motion_.find(item.card.uri);
            if (found != motion_.end() && !found->second.wanted)
                draw_card(list, item, found->second, false);
        }
        for (int index : visible_)
        {
            const LibraryItem &item = items()[static_cast<std::size_t>(index)];
            if (item.card.uri != focused)
                draw_card(list, item, motion_.at(item.card.uri), false);
        }
        const float ring_alpha = ring_alpha_.value * tween::stagger(age_, 6, 0.07f, 0.4f);
        Rect ring = ring_.value();
        const Rect home = ring_target();
        const float away = std::fabs(ring.x - home.x) + std::fabs(ring.y - home.y);
        const float landed = tween::clamp01(1.0f - away / 90.0f);
        ring.y += kGridTop - scroll_.offset();
        ring.x += ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_x_;
        ring.y += ui::shake(nudge_.value, clock_, 12.0f, 8.0f) * nudge_y_;
        const float radius = kCardRadius * (1.0f + kLift);
        if (ring_alpha > 0.01f)
        {
            const float breath = context_.settings.reduced_motion ? 0.5f : ui::breathe(clock_);
            list.glow(ring.inset(-kRingGap), radius + kRingGap, 26,
                      kGreen.with_alpha((0.28f + 0.18f * breath) * ring_alpha * landed));
        }
        if (const LibraryItem *item = focused_item())
            draw_card(list, *item, motion_.at(item->card.uri), true);
        if (ring_alpha > 0.01f)
            list.bordered_rect(ring.inset(-kRingGap), radius + kRingGap, Color::rgb(0, 0.0f), 4.0f,
                               kGreen.with_alpha(ring_alpha));
        list.pop_clip();
    }

    // Scrolled edges dissolve into the table instead of cutting cards.
    void draw_edges(gfx::DrawList &list) const
    {
        const float offset = scroll_.offset();
        const float above = tween::clamp01(offset / 40.0f);
        const float below = tween::clamp01((content_height() - kGridView - offset) / 40.0f);
        const float width = gfx::kVirtualWidth;
        const Color table = gfx::mix(palette_[0].value(), palette_[1].value(), 0.3f);
        if (above > 0.01f)
        {
            const Color c = table.with_alpha(above);
            list.gradient_rect({0, kGridTop - kFeather, width, kFeather}, 0, c.with_alpha(0.0f), c);
            list.rounded_rect({0, kGridTop, width, kEdgeCover}, 0, c);
            list.gradient_rect({0, kGridTop + kEdgeCover, width, kFeather}, 0, c, c.with_alpha(0.0f));
        }
        if (below > 0.01f)
        {
            const Color c = table.with_alpha(below);
            list.gradient_rect({0, kGridBottom - kFeather, width, kFeather}, 0, c.with_alpha(0.0f), c);
            list.gradient_rect({0, kGridBottom, width, kFeather}, 0, c, c.with_alpha(0.0f));
        }
    }

    void draw_empty(gfx::DrawList &list, const Model &model) const
    {
        if (!visible_.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 3, 0.07f, 0.5f);
        list.push_opacity(in);
        const char *headline = model.library_loading || !library_ ? "Loading your library" : "Nothing here yet";
        const char *line = model.library_loading || !library_ ? "Playlists, albums and artists you saved on Spotify."
                           : filter_ == Filter::artists ? "Artists you follow on Spotify show up here."
                           : filter_ == Filter::albums  ? "Albums you save on Spotify show up here."
                                                        : "Playlists you save on Spotify show up here.";
        if (model.library_loading)
            list.arc(kCentreX, 520, 22, 4, clock_ * 5.5f, 2.2f, kGreen);
        ui::text(list, fonts.display, headline, kCentreX, 620, 44, kWhite, gfx::Align::center);
        ui::text(list, fonts.regular, line, kCentreX, 670, 26, kMuted, gfx::Align::center);
        list.pop_opacity();
    }

    void draw_header(gfx::DrawList &list, const Model &model) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const bool reduced = context_.settings.reduced_motion;
        char text[64];
        float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        // The tabs, as on Home: Library is the one underlined.
        list.push_opacity(in);
        const char *tabs[] = {"Home", "Library"};
        draw_logo(list, context_, {kMargin - 4, 56, 52, 52});
        float x = kMargin + 64;
        for (int i = 0; i < 2; ++i)
        {
            const bool active = i == 1;
            const float w = ui::text(list, active ? fonts.semibold : fonts.regular, tabs[i], x, 92 - 10 * (1.0f - in), 26,
                                     kWhite.with_alpha(active ? 1.0f : 0.4f));
            if (active)
                list.rounded_rect({x, 104, w, 4}, 2, kGreen);
            x += w + 44;
        }
        if (!model.username.empty())
        {
            list.circle(1800, 82, 22, kGreen);
            ui::text(list, fonts.semibold, ui::upper(model.username.substr(0, 1)), 1800, 91, 22, Color::rgb(0x0b0d16),
                     gfx::Align::center);
        }
        const float rise = reduced ? 0.0f : 14.0f * (1.0f - in);
        const float title_w = ui::text(list, fonts.display, "Your Library", kMargin - 3, kTitleBaseline - rise, 52, kWhite);
        std::snprintf(text, sizeof(text), "%d saved", static_cast<int>(items().size()));
        ui::text(list, fonts.regular, text, kMargin + title_w + 24, kTitleBaseline - rise, 26, kMuted);
        list.pop_opacity();

        // Filter chips between L2 and R2; one green tab slides under the labels.
        in = tween::stagger(age_, 1, 0.07f, 0.5f);
        list.push_opacity(in);
        const ui::GlyphStyle glyphs = style::hint_style();
        const float chip_cy = kChipY + kChipH * 0.5f;
        ui::draw_button(list, fonts, glyphs, ui::Button::l2, kMargin, chip_cy, kShoulder);
        const Rect last = chip_rect(kFilters - 1);
        ui::draw_button(list, fonts, glyphs, ui::Button::r2, last.x + last.w + 18, chip_cy, kShoulder);
        const Rect tab = chip_.value();
        list.shadow({tab.x, tab.y + 5, tab.w, tab.h}, kChipH * 0.5f, 12, kBlack.with_alpha(0.4f));
        list.rounded_rect(tab, kChipH * 0.5f, kGreen);
        for (int i = 0; i < kFilters; ++i)
        {
            const Rect chip = chip_rect(i);
            const float away = std::fabs(tab.cx() - chip.cx()) / (chip.w * 0.6f);
            const float cover = tween::clamp01(1.0f - away);
            if (cover < 0.99f)
                list.bordered_rect(chip, kChipH * 0.5f, kWhite.with_alpha(0.05f * (1.0f - cover)), 1.5f,
                                   kWhite.with_alpha(0.2f * (1.0f - cover)));
            std::snprintf(text, sizeof(text), "%d", count_of(static_cast<Filter>(i)));
            const float label_w = fonts.semibold.measure(kChips[i].label, 22);
            const float count_w = fonts.mono.measure(text, 18);
            const float cx = chip.cx() - (label_w + 10 + count_w) * 0.5f;
            ui::text(list, fonts.semibold, kChips[i].label, cx, chip_cy + 8, 22,
                     gfx::mix(kWhite.with_alpha(0.8f), kBlack, cover));
            ui::text(list, fonts.mono, text, cx + label_w + 10, chip_cy + 7, 18,
                     gfx::mix(kWhite.with_alpha(0.45f), kBlack.with_alpha(0.6f), cover));
        }
        // The sort order at the right, turning like a card when it changes.
        const float t = sort_label_.running ? sort_label_.progress() : 1.0f;
        const float right = kRight - ui::button_width(ui::Button::square, kShoulder) - 14;
        ui::draw_button(list, fonts, glyphs, ui::Button::square, right + 14, chip_cy, kShoulder);
        list.push_clip({right - 400, kChipY, 400, kChipH});
        if (t < 1.0f)
            ui::text(list, fonts.semibold, kSortNames[static_cast<int>(previous_sort_)], right, chip_cy + 8 - 26 * t, 22,
                     kWhite.with_alpha(0.85f * (1.0f - t)), gfx::Align::right);
        ui::text(list, fonts.semibold, kSortNames[static_cast<int>(sort_)], right, chip_cy + 8 + 26 * (1.0f - t), 22,
                 kWhite.with_alpha(0.85f * t), gfx::Align::right);
        list.pop_clip();
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list, const Model &model) const
    {
        style::hint_fade(list); // content scrolled down there fades out before the hints
        list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f));
        ui::Hint hints[5];
        int count = 0;
        if (focused_item())
            hints[count++] = {ui::Button::cross, "Open"};
        if (model.has_track)
            hints[count++] = {ui::Button::triangle, "Now Playing"};
        hints[count++] = {ui::Button::l1, "Home", ui::Button::r1};
        hints[count++] = {ui::Button::right_stick, "Refresh"};
        hints[count++] = {ui::Button::touchpad, "Settings \xC2\xB7 Hold to quit"};
        ui::draw_hints(list, context_.fonts, style::hint_style(), hints, count, kRight, true, style::hint_layout());
        list.pop_opacity();
    }

    Context &context_;
    std::shared_ptr<const screens::Library> library_;
    std::map<std::string, CardMotion> motion_; // by URI: survives sorting and refreshes
    std::vector<int> visible_;                 // indices into items(), in shown order
    int focus_ = 0;
    Filter filter_ = Filter::all;
    Sort sort_ = Sort::recents;
    Sort previous_sort_ = Sort::recents;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    ui::Scroller scroll_;
    ui::SpringRect ring_;
    tween::Spring ring_alpha_;
    ui::Pulse nudge_;
    float nudge_x_ = 0.0f, nudge_y_ = 0.0f;
    ui::SpringRect chip_;
    tween::Timer sort_label_;
    ui::SpringColor palette_[4];
};

} // namespace

std::unique_ptr<Screen> make_library(Context &context)
{
    return std::make_unique<LibraryScreen>(context);
}

} // namespace screens
