// spot5 - An artist: the landscape header, Popular, Albums, Singles.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens/artist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

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
const Color kBlack = Color::rgb(0x0b0d16);

// Content coordinates (before scrolling).
constexpr float kBannerH = 860.0f;
constexpr float kNameBaseline = 500.0f;
constexpr float kActionsY = 600.0f;
constexpr float kPopularTop = 790.0f; // first row
constexpr float kRowH = 80.0f;
constexpr int kPopular = 5;
constexpr float kListW = 1080.0f;
constexpr float kShelfCard = 240.0f;
constexpr float kShelfGap = 28.0f;
constexpr float kShelfPitch = 420.0f;
constexpr float kAlbumsTop = kPopularTop + kPopular * kRowH + 130.0f;

enum class Zone
{
    actions,
    popular,
    albums,
    singles,
};

std::string clock_text(std::uint32_t ms)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%u:%02u", ms / 60000, ms / 1000 % 60);
    return text;
}

class ArtistScreen final : public Screen
{
  public:
    explicit ArtistScreen(Context &context) : context_(context)
    {
        for (ui::SpringColor &colour : palette_)
            colour.snap(Color::rgb(0x101014));
    }

    void enter() override
    {
        age_ = 0.0f;
    }

    void update(const Model &model, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &) override
    {
        age_ += dt;
        clock_ += dt;
        const ArtistPage *page = model.artist.get();
        if (!page)
            return;
        if (page->card.uri != uri_)
        {
            // A new artist starts at Play; one returned to (Back) where it was left.
            if (!uri_.empty())
            {
                if (left_.size() > 32)
                    left_.clear();
                left_[uri_] = {zone_, {index_[0], index_[1], index_[2], index_[3]}};
            }
            uri_ = page->card.uri;
            const auto was = left_.find(uri_);
            const Spot spot = was != left_.end() ? was->second : Spot{};
            zone_ = spot.zone;
            std::copy(std::begin(spot.index), std::end(spot.index), index_);
            scroll_.snap(std::max(0.0f, zone_top(zone_) - 380.0f));
            for (ui::Scroller &shelf : shelves_)
                shelf.position.snap(0.0f);
        }
        navigate(*page, input, feedback);
        if (input.is_pressed(Action::confirm))
            activate(*page, feedback);
        if (zone_ == Zone::popular && index_[1] < static_cast<int>(page->top.size()) &&
            input.is_pressed(Action::menu))
            context_.song_pick =
                Context::SongPick{page->top[static_cast<std::size_t>(index_[1])], focus_rect(*page)};

        // ---- animation ----
        scroll_.target = std::max(0.0f, zone_top(zone_) - 380.0f);
        if (zone_ == Zone::actions)
            scroll_.target = 0.0f;
        scroll_.update(dt, context_.settings.reduced_motion ? 40.0f : 11.0f);
        for (int s = 0; s < 2; ++s)
        {
            const float start = static_cast<float>(index_[2 + s]) * (kShelfCard + kShelfGap);
            shelves_[s].reveal(start, start + kShelfCard, gfx::kVirtualWidth - kMargin, kMargin * 2.0f);
            shelves_[s].update(dt, 12.0f);
        }
        ring_.target(focus_rect(*page));
        ring_.update(dt, 20.0f);
        const Art art = cover(page->card.wide).texture ? cover(page->card.wide) : cover(page->card.image_large);
        if (art.texture)
        {
            const AlbumPalette &p = art.palette;
            const Color targets[4] = {gfx::mix(p.dark, Color::rgb(0x05060c), 0.5f), gfx::mix(p.dark, p.mid, 0.25f),
                                      gfx::mix(p.mid, Color::rgb(0x05060c), 0.3f), gfx::mix(p.mid, p.accent, 0.5f)};
            for (int i = 0; i < 4; ++i)
                palette_[i].target(targets[i]);
        }
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 3.5f);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        for (int i = 0; i < 4; ++i)
            frame.backdrop.colors[i] = palette_[i].value();
        frame.backdrop.time = context_.settings.reduced_motion ? 0.0f : clock_ * 0.5f;
        const ArtistPage *page = model.artist.get();
        if (!page)
            return;
        gfx::DrawList &list = frame.scene;
        list.push_transform(1.0f, 0, 0, 0, -scroll_.value);
        draw_banner(list, *page);
        draw_title(list, *page);
        draw_popular(list, model, *page);
        draw_shelf(list, "Albums", page->albums, 0, kAlbumsTop);
        draw_shelf(list, "Singles and EPs", page->singles, 1, kAlbumsTop + kShelfPitch);
        draw_ring(list);
        list.pop_transform();
        // The hint row sits on a scrim, over whatever has scrolled under it.
        list.gradient_rect({0, 880, gfx::kVirtualWidth, 90}, 0, palette_[0].value(0.0f), palette_[0].value(0.96f));
        list.rounded_rect({0, 970, gfx::kVirtualWidth, 110}, 0, palette_[0].value(0.96f));
        draw_hints(list, model, *page);
    }

  private:
    Art cover(const std::string &url) const
    {
        return context_.art && !url.empty() ? context_.art(url) : Art{};
    }

    const std::vector<Card> &shelf(const ArtistPage &page, int s) const
    {
        return s == 0 ? page.albums : page.singles;
    }

    int zone_size(const ArtistPage &page, Zone zone) const
    {
        switch (zone)
        {
        case Zone::actions:
            return 2;
        case Zone::popular:
            return std::min<int>(kPopular, static_cast<int>(page.top.size()));
        case Zone::albums:
            return static_cast<int>(page.albums.size());
        case Zone::singles:
            return static_cast<int>(page.singles.size());
        }
        return 0;
    }

    float zone_top(Zone zone) const
    {
        switch (zone)
        {
        case Zone::actions:
            return kActionsY;
        case Zone::popular:
            return kPopularTop + static_cast<float>(index_[1]) * kRowH;
        case Zone::albums:
            return kAlbumsTop;
        case Zone::singles:
            return kAlbumsTop + kShelfPitch;
        }
        return 0.0f;
    }

    Rect action_rect(int action) const
    {
        return action == 0 ? Rect{kMargin, kActionsY, 200, 68} : Rect{kMargin + 224, kActionsY, 68, 68};
    }

    Rect shelf_rect(int s, int index) const
    {
        const float top = kAlbumsTop + static_cast<float>(s) * kShelfPitch;
        return {kMargin + static_cast<float>(index) * (kShelfCard + kShelfGap) - shelves_[s].offset(), top, kShelfCard,
                kShelfCard};
    }

    Rect focus_rect(const ArtistPage &) const
    {
        switch (zone_)
        {
        case Zone::actions:
            return action_rect(index_[0]).inset(-6);
        case Zone::popular:
            return {kMargin - 20, kPopularTop + static_cast<float>(index_[1]) * kRowH, kListW + 40, kRowH - 6};
        case Zone::albums:
        case Zone::singles:
        {
            const int s = zone_ == Zone::albums ? 0 : 1;
            return shelf_rect(s, index_[2 + s]).inset(-8);
        }
        }
        return {};
    }

    void navigate(const ArtistPage &page, const InputFrame &input, ui::Feedback &feedback)
    {
        const int zone = static_cast<int>(zone_);
        int &index = index_[zone];
        const auto refuse = [&] {
            if (!input.nav_repeat)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                feedback.rumble(0.2f, 0.05f);
            }
        };
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            if (zone_ == Zone::popular)
                return refuse();
            const int next = index + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < zone_size(page, zone_))
            {
                index = next;
                feedback.play(audio::Cue::focus);
            }
            else
                refuse();
        }
        else if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int step = input.nav == Direction::down ? 1 : -1;
            if (zone_ == Zone::popular)
            {
                const int next = index + step;
                if (next >= 0 && next < zone_size(page, Zone::popular))
                {
                    index = next;
                    feedback.play(audio::Cue::focus, 1.0f, 0.2f);
                    return;
                }
            }
            // The next zone that has something in it.
            for (int z = zone + step; z >= 0 && z <= 3; z += step)
                if (zone_size(page, static_cast<Zone>(z)) > 0)
                {
                    zone_ = static_cast<Zone>(z);
                    index_[z] = std::min(index_[z], zone_size(page, zone_) - 1);
                    if (zone_ == Zone::popular)
                        index_[z] = step > 0 ? 0 : zone_size(page, zone_) - 1;
                    feedback.play(audio::Cue::tab);
                    return;
                }
            refuse();
        }
    }

    void activate(const ArtistPage &page, ui::Feedback &feedback)
    {
        Request request;
        switch (zone_)
        {
        case Zone::actions:
            request.play.uri = page.card.uri;
            request.play.shuffle = index_[0] == 1;
            break;
        case Zone::popular:
            if (page.top.empty())
                return;
            request.play.uri = page.card.uri;
            request.play.index = index_[1];
            for (const TrackRow &row : page.top)
                request.play.tracks.push_back(row.uri);
            break;
        case Zone::albums:
        case Zone::singles:
        {
            const auto &cards = shelf(page, zone_ == Zone::albums ? 0 : 1);
            const int index = index_[zone_ == Zone::albums ? 2 : 3];
            if (index >= static_cast<int>(cards.size()))
                return;
            request.kind = Request::Kind::open;
            request.card = cards[static_cast<std::size_t>(index)];
            context_.requests.push_back(request);
            feedback.play(audio::Cue::open);
            return;
        }
        }
        context_.requests.push_back(request);
        feedback.play(audio::Cue::launch);
        feedback.rumble(0.6f, 0.15f);
    }

    // ---- drawing (content coordinates; the page transform scrolls them) ----

    void draw_banner(gfx::DrawList &list, const ArtistPage &page) const
    {
        const Art wide = cover(page.card.wide);
        const float in = tween::stagger(age_, 0, 0.08f, 0.8f);
        if (wide.texture)
        {
            const float target = gfx::kVirtualWidth / kBannerH;
            const float aspect = wide.height > 0 ? static_cast<float>(wide.width) / static_cast<float>(wide.height) : target;
            Rect uv{0, 0, 1, 1};
            if (aspect > target)
            {
                uv.w = target / aspect;
                uv.x = (1.0f - uv.w) * 0.5f;
            }
            else
            {
                uv.h = aspect / target;
                uv.y = (1.0f - uv.h) * 0.3f;
            }
            const float zoom = context_.settings.reduced_motion ? 1.0f : 1.04f - 0.04f * tween::cubic_out(in);
            list.push_opacity(in);
            list.push_transform(zoom, kCentreX, kBannerH * 0.4f, 0, 0);
            list.image_gradient(wide.texture, {0, 0, gfx::kVirtualWidth, kBannerH}, uv, kWhite, kWhite.with_alpha(0.0f));
            list.pop_transform();
            const Color base = palette_[0].value();
            list.gradient_rect({0, 0, gfx::kVirtualWidth, 200}, 0, Color::rgb(0x000000, 0.4f), Color::rgb(0x000000, 0.0f));
            list.gradient_rect_h({0, 0, 1400, kBannerH}, 0, base.with_alpha(0.8f), base.with_alpha(0.0f));
            list.pop_opacity();
            return;
        }
        // No landscape art: the round photo at the right, glowing in its colours.
        const Art photo = cover(page.card.image_large).texture ? cover(page.card.image_large) : cover(page.card.image);
        const Rect r{1300, 150, 460, 460};
        list.push_opacity(in);
        list.glow(r.inset(40), r.w * 0.5f, 120, palette_[3].value().with_alpha(0.35f));
        if (photo.texture)
            list.image(photo.texture, r, gfx::kFullUv, kWhite, r.w * 0.5f);
        else
            list.circle(r.cx(), r.cy(), r.w * 0.5f, kWhite.with_alpha(0.08f));
        list.pop_opacity();
    }

    void draw_title(gfx::DrawList &list, const ArtistPage &page) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        const float rise = context_.settings.reduced_motion ? 0.0f : 20.0f * (1.0f - in);
        list.push_opacity(in);
        ui::text(list, fonts.semibold, "ARTIST", kMargin, kNameBaseline - 130 + rise, 20, palette_[3].value(),
                 gfx::Align::left, 4.0f);
        const float size = fonts.display.measure(page.card.title, 110) <= 1500 ? 110.0f : 76.0f;
        ui::text(list, fonts.display, fonts.display.font->fit(page.card.title, size, 1600), kMargin - 6,
                 kNameBaseline + rise, size, kWhite);
        ui::text(list, fonts.semibold, page.loading ? std::string("Loading\xE2\x80\xA6") : page.listeners, kMargin,
                 kNameBaseline + 52 + rise, 26, kWhite.with_alpha(0.82f));
        // Spotify's green Play, then Shuffle.
        const Rect play = action_rect(0);
        list.glow(play, play.h * 0.5f, 22, kGreen.with_alpha(0.35f));
        list.rounded_rect(play, play.h * 0.5f, kGreen);
        list.triangle({play.x + 38, play.cy() - 13, 24, 26}, kBlack, 0.0f, 1.5708f);
        ui::text(list, fonts.semibold, "Play", play.x + 80, play.cy() + 10, 28, kBlack);
        const Rect shuffle = action_rect(1);
        list.bordered_rect(shuffle, shuffle.h * 0.5f, kWhite.with_alpha(0.1f), 2, kWhite.with_alpha(0.35f));
        const float cx = shuffle.cx() - 2, cy = shuffle.cy();
        list.line(cx - 14, cy - 9, cx + 8, cy + 9, 3, kWhite);
        list.line(cx - 14, cy + 9, cx + 8, cy - 9, 3, kWhite);
        list.triangle({cx + 6, cy - 15, 12, 12}, kWhite, 0.0f, 1.5708f);
        list.triangle({cx + 6, cy + 3, 12, 12}, kWhite, 0.0f, 1.5708f);
        list.pop_opacity();
    }

    void draw_popular(gfx::DrawList &list, const Model &model, const ArtistPage &page) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 2, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.semibold, "Popular", kMargin, kPopularTop - 30, 30, kWhite);
        if (page.loading)
            list.arc(kMargin + 20, kPopularTop + 40, 12, 3, clock_ * 5.5f, 2.2f, kGreen);
        const std::string playing = model.queue && model.queue->playing >= 0 &&
                                            model.queue->playing < static_cast<int>(model.queue->tracks.size())
                                        ? model.queue->tracks[static_cast<std::size_t>(model.queue->playing)].uri
                                        : std::string();
        for (int i = 0; i < std::min<int>(kPopular, static_cast<int>(page.top.size())); ++i)
        {
            const TrackRow &row = page.top[static_cast<std::size_t>(i)];
            const float cy = kPopularTop + static_cast<float>(i) * kRowH + (kRowH - 6) * 0.5f;
            const bool now = row.uri == playing;
            if (now)
                for (int b = 0; b < 3; ++b)
                {
                    const float level = model.paused ? 0.4f : 0.35f + 0.65f * std::fabs(std::sin(clock_ * (3.1f + b * 1.7f) + b));
                    list.rounded_rect({kMargin + 4 + static_cast<float>(b) * 8, cy + 12 - 24 * level, 5, 24 * level}, 1.5f,
                                      kGreen);
                }
            else
                ui::text(list, fonts.mono, std::to_string(i + 1), kMargin + 28, cy + 8, 22, kWhite.with_alpha(0.5f),
                         gfx::Align::right);
            const Rect thumb{kMargin + 50, cy - 29, 58, 58};
            const Art art = cover(row.image);
            if (art.texture)
                list.image(art.texture, thumb, gfx::kFullUv, kWhite, 8);
            else
                list.rounded_rect(thumb, 8, kWhite.with_alpha(0.08f));
            const float width = kListW - 260;
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.title, 25, width), thumb.x + 80, cy - 3, 25,
                     now ? kGreen : kWhite);
            ui::text(list, fonts.regular, fonts.regular.font->fit(row.artists, 20, width), thumb.x + 80, cy + 24, 20,
                     kWhite.with_alpha(0.6f));
            if (row.duration_ms)
                ui::text(list, fonts.mono, clock_text(row.duration_ms), kMargin + kListW, cy + 8, 22,
                         kWhite.with_alpha(0.6f), gfx::Align::right);
        }
        // About, beside Popular.
        if (!page.biography.empty())
        {
            const float x = kMargin + kListW + 90;
            ui::text(list, fonts.semibold, "About", x, kPopularTop - 30, 30, kWhite);
            std::string plain;
            bool tag = false;
            for (char c : page.biography)
            {
                if (c == '<')
                    tag = true;
                else if (c == '>')
                    tag = false;
                else if (!tag)
                    plain += c;
            }
            ui::paragraph(list, fonts.regular, plain, x, kPopularTop + 20, 23, kRight - x, 34, kWhite.with_alpha(0.72f), 10);
        }
        list.pop_opacity();
    }

    void draw_shelf(gfx::DrawList &list, const char *title, const std::vector<Card> &cards, int s, float top) const
    {
        if (cards.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 3 + s, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.semibold, title, kMargin, top - 30, 30, kWhite);
        for (int i = 0; i < static_cast<int>(cards.size()); ++i)
        {
            const Rect r = shelf_rect(s, i);
            if (r.x > gfx::kVirtualWidth || r.x + r.w < -40)
                continue;
            const Card &card = cards[static_cast<std::size_t>(i)];
            const Art art = cover(card.image);
            if (art.texture)
                list.image(art.texture, r, gfx::kFullUv, kWhite, 14);
            else
                list.rounded_rect(r, 14, kWhite.with_alpha(0.07f));
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(card.title, 22, r.w), r.x, r.y + r.h + 34, 22, kWhite);
            ui::text(list, fonts.regular, card.subtitle, r.x, r.y + r.h + 62, 19, kWhite.with_alpha(0.6f));
        }
        list.pop_opacity();
    }

    void draw_ring(gfx::DrawList &list) const
    {
        const Rect ring = ring_.value();
        const float in = tween::stagger(age_, 2, 0.08f, 0.6f);
        list.push_opacity(in);
        if (zone_ == Zone::popular)
        {
            list.rounded_rect(ring, 18, kWhite.with_alpha(0.14f));
            list.bordered_rect(ring, 18, Color::rgb(0, 0.0f), 1.5f, kWhite.with_alpha(0.3f));
        }
        else
            list.bordered_rect(ring, zone_ == Zone::actions ? ring.h * 0.5f : 20.0f, Color::rgb(0, 0.0f), 3.5f, kWhite);
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list, const Model &model, const ArtistPage &) const
    {
        style::hint_fade(list); // content scrolled down there fades out before the hints
        if (context_.song_menu_open)
            return;
        list.push_opacity(tween::stagger(age_, 5, 0.08f, 0.5f));
        const char *cross = zone_ == Zone::actions   ? (index_[0] == 1 ? "Shuffle" : "Play")
                            : zone_ == Zone::popular ? "Play from here"
                                                     : "Open";
        ui::Hint hints[4];
        int count = 0;
        hints[count++] = {ui::Button::cross, cross};
        if (zone_ == Zone::popular)
            hints[count++] = {ui::Button::options, "More"};
        hints[count++] = {ui::Button::circle, "Back \xC2\xB7 Hold: Home"};
        if (model.has_track)
            hints[count++] = {ui::Button::triangle, "Now Playing"};
        ui::draw_hints(list, context_.fonts, style::hint_style(), hints, count, kRight, true, style::hint_layout());
        list.pop_opacity();
    }

    struct Spot
    {
        Zone zone = Zone::actions;
        int index[4] = {0, 0, 0, 0};
    };

    Context &context_;
    std::string uri_;
    std::map<std::string, Spot> left_; // where the focus was on artists left behind
    Zone zone_ = Zone::actions;
    int index_[4] = {0, 0, 0, 0}; // per zone: action, popular row, album, single
    float age_ = 0.0f;
    float clock_ = 0.0f;
    tween::Spring scroll_;
    ui::Scroller shelves_[2];
    ui::SpringRect ring_;
    ui::SpringColor palette_[4];
};

} // namespace

std::unique_ptr<Screen> make_artist(Context &context)
{
    return std::make_unique<ArtistScreen>(context);
}

} // namespace screens
