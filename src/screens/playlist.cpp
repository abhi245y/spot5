// spot5 - A playlist/album page: its cover, its story and every track.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens/playlist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

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

const Rect kCover{kMargin, 150, 620, 620};
constexpr float kColumnX = 800.0f;
constexpr float kColumnW = kRight - kColumnX;
constexpr float kActionsY = 452.0f; // top of the action row
constexpr float kListTop = 560.0f;  // first row's top (scrolled)
constexpr float kListBottom = 972.0f;
constexpr float kRowH = 80.0f;
constexpr float kThumb = 58.0f;
constexpr int kPageJump = 5;      // L1 / R1 move this many rows
constexpr int kDescribeAhead = 12; // rows below the view whose details are asked for early

enum class Zone
{
    actions,
    list,
};

std::string clock_text(std::uint32_t ms)
{
    const std::uint32_t seconds = ms / 1000;
    char text[16];
    std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
    return text;
}

std::string length_text(std::uint64_t ms)
{
    const std::uint64_t minutes = (ms + 30000) / 60000;
    char text[32];
    if (minutes >= 60)
        std::snprintf(text, sizeof(text), "%llu h %llu min", static_cast<unsigned long long>(minutes / 60),
                      static_cast<unsigned long long>(minutes % 60));
    else
        std::snprintf(text, sizeof(text), "%llu min", static_cast<unsigned long long>(minutes));
    return text;
}

// Descriptions may carry links (<a href=...>name</a>); keep the text.
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

class Playlist final : public Screen
{
  public:
    explicit Playlist(Context &context) : context_(context)
    {
        for (ui::SpringColor &colour : palette_)
            colour.snap(kDeep);
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
        const PlaylistPage *page = model.page.get();
        if (!page)
            return;
        if (page->card.uri != uri_)
        {
            // A new page: start at Play, list at the top.
            uri_ = page->card.uri;
            zone_ = Zone::actions;
            action_ = 0;
            row_ = 0;
            scroll_.snap(0.0f);
            asked_.clear();
            pill_.snap(focus_rect(*page));
        }
        const int rows = static_cast<int>(page->tracks.size());
        navigate(input, feedback, rows);

        if (input.is_pressed(Action::confirm))
        {
            Request request;
            request.play.uri = page->card.uri;
            if (zone_ == Zone::actions)
                request.play.shuffle = action_ == 1;
            else
                request.play.index = row_;
            if (zone_ == Zone::actions || row_ < rows)
            {
                context_.requests.push_back(request);
                // When the song starts, take the list to it (a shuffle starts anywhere).
                follow_ = true;
                follow_from_ = playing_;
                follow_age_ = 0.0f;
                feedback.play(audio::Cue::launch);
                feedback.rumble(0.6f, 0.15f);
            }
        }

        if (zone_ == Zone::list && row_ < rows && input.is_pressed(Action::menu))
            context_.song_pick = Context::SongPick{page->tracks[static_cast<std::size_t>(row_)], focus_rect(*page)};

        // Ask for the details of rows on screen (and a few below) once.
        const int first = std::max(0, static_cast<int>(scroll_.target) - 2);
        const int last = std::min(rows, first + visible_rows() + kDescribeAhead);
        Request describe;
        describe.kind = Request::Kind::describe;
        for (int i = first; i < last; ++i)
        {
            const TrackRow &row = page->tracks[static_cast<std::size_t>(i)];
            if (row.title.empty() && asked_.insert(row.uri).second)
                describe.uris.push_back(row.uri);
        }
        if (!describe.uris.empty())
            context_.requests.push_back(describe);

        // ---- animation ----
        scroll_.target = zone_ == Zone::list ? std::clamp(static_cast<float>(row_) - 1.0f, 0.0f,
                                                          std::max(0.0f, static_cast<float>(rows - visible_rows())))
                                             : 0.0f;
        scroll_.update(dt, 13.0f);
        pill_.target(focus_rect(*page));
        pill_.update(dt, 22.0f);
        apply_palette(*page);
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.0f);
        playing_ = playing_row(model, *page);
        if (follow_)
        {
            follow_age_ += dt;
            if (playing_ >= 0 && playing_ != follow_from_)
            {
                zone_ = Zone::list;
                row_ = playing_;
                follow_ = false;
            }
            else if (follow_age_ > 25.0f)
                follow_ = false;
        }
        breath_.target = playing_ >= 0 && !model.paused ? 1.0f : 0.0f;
        breath_.update(dt, 4.0f);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::aurora;
        for (int i = 0; i < 4; ++i)
            frame.backdrop.colors[i] = palette_[i].value();
        frame.backdrop.time = context_.settings.reduced_motion ? 0.0f : clock_;
        const PlaylistPage *page = model.page.get();
        if (!page)
            return;
        gfx::DrawList &list = frame.scene;
        draw_cover(list, *page);
        draw_header(list, *page);
        draw_actions(list);
        draw_tracks(list, model, *page);
        draw_hints(list, model, *page);
    }

  private:
    int visible_rows() const
    {
        return static_cast<int>((kListBottom - kListTop) / kRowH);
    }

    void navigate(const InputFrame &input, ui::Feedback &feedback, int rows)
    {
        const auto refuse = [&] {
            if (!input.nav_repeat)
            {
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
                feedback.rumble(0.2f, 0.05f);
            }
        };
        if (zone_ == Zone::actions)
        {
            if (input.nav == Direction::left || input.nav == Direction::right)
            {
                const int next = action_ + (input.nav == Direction::right ? 1 : -1);
                if (next >= 0 && next <= 1)
                {
                    action_ = next;
                    feedback.play(audio::Cue::focus);
                }
                else
                    refuse();
            }
            else if (input.nav == Direction::down)
            {
                if (rows > 0)
                {
                    zone_ = Zone::list;
                    row_ = std::min(row_, rows - 1);
                    feedback.play(audio::Cue::tab);
                }
                else
                    refuse();
            }
            else if (input.nav == Direction::up)
                refuse();
            return;
        }
        int step = 0;
        if (input.nav == Direction::down)
            step = 1;
        else if (input.nav == Direction::up)
            step = -1;
        else if (input.is_pressed(Action::page_next))
            step = kPageJump;
        else if (input.is_pressed(Action::page_prev))
            step = -kPageJump;
        if (step == 0)
            return;
        if (row_ + step < 0)
        {
            if (row_ == 0)
            {
                zone_ = Zone::actions; // back up to Play / Shuffle
                feedback.play(audio::Cue::tab);
                return;
            }
            row_ = 0;
        }
        else if (row_ + step >= rows)
        {
            if (row_ == rows - 1)
            {
                refuse();
                return;
            }
            row_ = rows - 1;
        }
        else
            row_ += step;
        feedback.play(audio::Cue::focus, 1.0f, 0.3f);
    }

    Rect action_rect(int action) const
    {
        return action == 0 ? Rect{kColumnX, kActionsY, 200, 68} : Rect{kColumnX + 224, kActionsY, 68, 68};
    }

    float row_y(int row) const
    {
        return kListTop + (static_cast<float>(row) - scroll_.value) * kRowH;
    }

    Rect focus_rect(const PlaylistPage &) const
    {
        if (zone_ == Zone::actions)
            return action_rect(action_).inset(-6);
        return {kColumnX - 20, row_y(row_), kColumnW + 40, kRowH - 6};
    }

    // Which row plays now: the queue's playing track, if it is on this page.
    static int playing_row(const Model &model, const PlaylistPage &page)
    {
        if (!model.queue || model.queue->playing < 0 ||
            model.queue->playing >= static_cast<int>(model.queue->tracks.size()))
            return -1;
        const std::string &uri = model.queue->tracks[static_cast<std::size_t>(model.queue->playing)].uri;
        for (int i = 0; i < static_cast<int>(page.tracks.size()); ++i)
            if (page.tracks[static_cast<std::size_t>(i)].uri == uri)
                return i;
        return -1;
    }

    Art cover_art(const PlaylistPage &page) const
    {
        if (!context_.art)
            return {};
        const Art large = context_.art(page.card.image_large);
        return large.texture ? large : context_.art(page.card.image);
    }

    void apply_palette(const PlaylistPage &page)
    {
        const Art art = cover_art(page);
        if (!art.texture)
            return;
        const AlbumPalette &p = art.palette;
        const Color targets[4] = {gfx::mix(p.dark, Color::rgb(0x05060c), 0.45f), gfx::mix(p.dark, p.mid, 0.3f),
                                  p.mid, gfx::mix(p.mid, p.accent, 0.5f)};
        for (int i = 0; i < 4; ++i)
            palette_[i].target(targets[i]);
    }

    Color accent() const
    {
        return palette_[3].value();
    }

    void draw_cover(gfx::DrawList &list, const PlaylistPage &page) const
    {
        const float in = tween::stagger(age_, 0, 0.08f, 0.6f);
        const float breathe = context_.settings.reduced_motion ? 0.0f : breath_.value * 0.012f * ui::breathe(clock_, 3.2f);
        const float radius = page.card.round ? kCover.w * 0.5f : 28.0f;
        list.push_opacity(in);
        list.push_transform(1.0f + breathe - 0.03f * (1.0f - in), kCover.cx(), kCover.cy(), 0, 0);
        list.glow(kCover.inset(40), radius, 110, accent().with_alpha(0.28f + 0.12f * breath_.value));
        list.shadow({kCover.x, kCover.y + 30, kCover.w, kCover.h}, radius, 50, Color::rgb(0x000000, 0.55f));
        const Art art = cover_art(page);
        if (art.texture)
            list.image(art.texture, kCover, gfx::kFullUv, kWhite, radius);
        else
            list.rounded_rect(kCover, radius, kWhite.with_alpha(0.08f));
        if (!page.card.round)
            list.bordered_rect(kCover, radius, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.14f));
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_header(gfx::DrawList &list, const PlaylistPage &page) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        const float x = kColumnX + 30.0f * (1.0f - in);
        list.push_opacity(in);
        // "Playlist · Spotify": the type as the label, the owner in the line below.
        const std::string &subtitle = page.card.subtitle;
        const std::size_t dot = subtitle.find(" \xC2\xB7 ");
        const std::string type = subtitle.substr(0, dot);
        const std::string owner = dot == std::string::npos ? std::string() : subtitle.substr(dot + 4);
        ui::text(list, fonts.semibold, ui::upper(type), x, 196, 20, accent(), gfx::Align::left, 4.0f);
        ui::text(list, fonts.display, fonts.display.font->fit(page.card.title, 64, kColumnW), x - 3, 270, 64,
                 kWhite);

        std::string line = owner;
        if (!page.loading && !page.tracks.empty())
        {
            std::uint64_t total = 0;
            bool all = true;
            for (const TrackRow &row : page.tracks)
            {
                total += row.duration_ms;
                all = all && row.duration_ms > 0;
            }
            line += (line.empty() ? "" : "  \xC2\xB7  ") + std::to_string(page.tracks.size()) + " songs";
            if (all)
                line += "  \xC2\xB7  " + length_text(total);
        }
        ui::text(list, fonts.semibold, line, x, 320, 26, kWhite.with_alpha(0.85f));
        ui::paragraph(list, fonts.regular, plain_text(page.card.description), x, 372, 24, kColumnW, 34,
                      kWhite.with_alpha(0.72f), 2);
        list.pop_opacity();
    }

    void draw_shuffle_glyph(gfx::DrawList &list, float cx, float cy, Color colour) const
    {
        // Two crossing paths with arrowheads, as on every player.
        list.line(cx - 14, cy - 9, cx + 8, cy + 9, 3, colour);
        list.line(cx - 14, cy + 9, cx + 8, cy - 9, 3, colour);
        list.triangle({cx + 6, cy - 15, 12, 12}, colour, 0.0f, 1.5708f);
        list.triangle({cx + 6, cy + 3, 12, 12}, colour, 0.0f, 1.5708f);
    }

    void draw_actions(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 2, 0.08f, 0.6f);
        list.push_opacity(in);
        // Spotify's green Play, then Shuffle.
        const Rect play = action_rect(0);
        list.glow(play, play.h * 0.5f, 22, kGreen.with_alpha(0.35f));
        list.rounded_rect(play, play.h * 0.5f, kGreen);
        list.triangle({play.x + 38, play.cy() - 13, 24, 26}, kBlack, 0.0f, 1.5708f);
        ui::text(list, fonts.semibold, "Play", play.x + 80, play.cy() + 10, 28, kBlack);
        const Rect shuffle = action_rect(1);
        list.bordered_rect(shuffle, shuffle.h * 0.5f, kWhite.with_alpha(0.1f), 2, kWhite.with_alpha(0.35f));
        draw_shuffle_glyph(list, shuffle.cx() - 2, shuffle.cy(), kWhite);
        // The focus ring lives on the actions while they are focused.
        if (zone_ == Zone::actions)
        {
            const Rect ring = pill_.value();
            list.bordered_rect(ring, ring.h * 0.5f, Color::rgb(0x000000, 0.0f), 3.5f, kWhite);
        }
        list.pop_opacity();
    }

    // Spotify's playing marker: three bars that move with the music.
    void draw_equaliser(gfx::DrawList &list, float x, float cy, bool moving) const
    {
        for (int i = 0; i < 3; ++i)
        {
            const float level = moving ? 0.35f + 0.65f * std::fabs(std::sin(clock_ * (3.1f + i * 1.7f) + i)) : 0.4f;
            const float h = 24.0f * level;
            list.rounded_rect({x + static_cast<float>(i) * 8.0f, cy + 12 - h, 5, h}, 1.5f, kGreen);
        }
    }

    void draw_tracks(gfx::DrawList &list, const Model &model, const PlaylistPage &page) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 3, 0.08f, 0.6f);
        list.push_opacity(in);
        list.rounded_rect({kColumnX, kListTop - 26, kColumnW, 1.5f}, 0, kWhite.with_alpha(0.14f));
        if (page.loading || page.tracks.empty())
        {
            const char *text = page.loading ? "Loading songs\xE2\x80\xA6"
                               : page.error.empty() ? "No songs here" : page.error.c_str();
            if (page.loading)
                list.arc(kColumnX + 14, kListTop + 40, 12, 3, clock_ * 5.5f, 2.2f, accent());
            ui::text(list, fonts.regular, text, kColumnX + (page.loading ? 44 : 0), kListTop + 49, 26,
                     kWhite.with_alpha(0.75f));
            list.pop_opacity();
            return;
        }

        list.push_clip({kColumnX - 40, kListTop - 10, kColumnW + 80, kListBottom - kListTop + 20});
        if (zone_ == Zone::list)
        {
            const Rect pill = pill_.value();
            list.rounded_rect(pill, 18, kWhite.with_alpha(0.16f));
            list.bordered_rect(pill, 18, Color::rgb(0x000000, 0.0f), 1.5f, kWhite.with_alpha(0.3f));
        }
        const int first = std::max(0, static_cast<int>(scroll_.value) - 1);
        const int last = std::min(static_cast<int>(page.tracks.size()), first + visible_rows() + 3);
        for (int i = first; i < last; ++i)
        {
            const TrackRow &row = page.tracks[static_cast<std::size_t>(i)];
            const float y = row_y(i);
            const float cy = y + (kRowH - 6) * 0.5f;
            const bool playing = i == playing_;
            const Color title_colour = playing ? kGreen : kWhite;
            if (playing)
                draw_equaliser(list, kColumnX + 4, cy, !model.paused);
            else
                ui::text(list, fonts.mono, std::to_string(i + 1), kColumnX + 30, cy + 8, 22,
                         kWhite.with_alpha(0.5f), gfx::Align::right);
            const Rect thumb{kColumnX + 50, cy - kThumb * 0.5f, kThumb, kThumb};
            const Art art = context_.art && !row.image.empty() ? context_.art(row.image) : Art{};
            if (art.texture)
                list.image(art.texture, thumb, gfx::kFullUv, kWhite, 8);
            else
                list.rounded_rect(thumb, 8, kWhite.with_alpha(0.08f));
            const float tx = thumb.x + thumb.w + 22;
            const float width = kColumnW - (tx - kColumnX) - 110;
            if (row.title.empty())
            {
                // Details on their way: two soft bars where the text will be.
                list.rounded_rect({tx, cy - 22, width * 0.45f, 16}, 8, kWhite.with_alpha(0.08f));
                list.rounded_rect({tx, cy + 6, width * 0.3f, 14}, 7, kWhite.with_alpha(0.06f));
                continue;
            }
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.title, 25, width), tx, cy - 4, 25,
                     title_colour);
            ui::text(list, fonts.regular, fonts.regular.font->fit(row.artists, 21, width), tx, cy + 25, 21,
                     kWhite.with_alpha(0.62f));
            if (row.duration_ms)
                ui::text(list, fonts.mono, clock_text(row.duration_ms), kRight - 6, cy + 8, 22,
                         kWhite.with_alpha(0.62f), gfx::Align::right);
        }
        list.pop_clip();
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list, const Model &model, const PlaylistPage &page) const
    {
        style::hint_fade(list); // content scrolled down there fades out before the hints
        if (context_.song_menu_open)
            return;
        list.push_opacity(tween::stagger(age_, 5, 0.08f, 0.5f));
        const char *play = zone_ == Zone::list ? "Play from here" : action_ == 1 ? "Shuffle" : "Play";
        ui::Hint hints[6];
        int count = 0;
        if (!page.tracks.empty() || zone_ == Zone::actions)
            hints[count++] = {ui::Button::cross, play};
        if (zone_ == Zone::list)
        {
            hints[count++] = {ui::Button::l1, "Page", ui::Button::r1};
            hints[count++] = {ui::Button::options, "More"};
        }
        hints[count++] = {ui::Button::circle, "Back"};
        if (model.has_track)
            hints[count++] = {ui::Button::triangle, "Now Playing"};
        ui::draw_hints(list, context_.fonts, style::hint_style(), hints, count, kRight, true, style::hint_layout());
        list.pop_opacity();
    }

    Context &context_;
    std::string uri_; // the page shown (a new one resets the focus)
    Zone zone_ = Zone::actions;
    int action_ = 0;
    int row_ = 0;
    int playing_ = -1;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    tween::Spring scroll_;
    ui::SpringRect pill_;
    ui::SpringColor palette_[4];
    tween::Spring breath_;
    std::set<std::string> asked_;
    bool follow_ = false; // move to the playing row once the asked-for song starts
    int follow_from_ = -1;
    float follow_age_ = 0.0f;
};

} // namespace

std::unique_ptr<Screen> make_playlist(Context &context)
{
    return std::make_unique<Playlist>(context);
}

} // namespace screens
