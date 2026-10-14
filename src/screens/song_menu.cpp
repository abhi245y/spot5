// spot5 - The song menu: a panel from the right edge or a pop-up at the row.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "screens/song_menu.hpp"

#include <algorithm>
#include <cstdio>

#include "screens/style.hpp"
#include "ui/glyphs.hpp"

namespace screens
{

namespace
{

using gfx::Color;
using gfx::Rect;
using namespace style;

constexpr float kPanelW = 536.0f;
constexpr float kPanelX = gfx::kVirtualWidth - 24.0f - kPanelW; // open
constexpr float kPopupW = 440.0f;
constexpr float kPopupRow = 66.0f;
constexpr float kPopupPad = 14.0f;

const char *label(int act)
{
    static const char *kLabels[] = {"Song radio", "Go to album", "Go to artist", "Details"};
    return kLabels[act];
}

// Small line icons, centred on (cx, cy), about 30 px across.
void icon(gfx::DrawList &list, int act, float cx, float cy, Color ink)
{
    switch (act)
    {
    case 0: // radio: a dot with waves on both sides
        list.circle(cx, cy, 3.5f, ink);
        list.arc(cx, cy, 9.0f, 2.5f, -0.75f, 1.5f, ink);
        list.arc(cx, cy, 9.0f, 2.5f, 3.1416f - 0.75f, 1.5f, ink);
        list.arc(cx, cy, 15.0f, 2.5f, -0.75f, 1.5f, ink);
        list.arc(cx, cy, 15.0f, 2.5f, 3.1416f - 0.75f, 1.5f, ink);
        break;
    case 1: // album: a record
        list.ring(cx, cy, 13.0f, 2.5f, ink);
        list.circle(cx, cy, 3.5f, ink);
        break;
    case 2: // artist: head and shoulders
        list.ring(cx, cy - 6.0f, 6.0f, 2.5f, ink);
        list.bordered_rect({cx - 11.0f, cy + 4.0f, 22.0f, 12.0f}, 6.0f, kClear, 2.5f, ink);
        break;
    default: // details: i in a circle
        list.ring(cx, cy, 13.0f, 2.5f, ink);
        list.circle(cx, cy - 6.0f, 1.8f, ink);
        list.line(cx, cy - 1.0f, cx, cy + 7.0f, 2.5f, ink);
        break;
    }
}

std::string length_text(std::uint32_t ms)
{
    char text[16];
    format_time(text, sizeof(text), static_cast<float>(ms) / 1000.0f);
    return text;
}

} // namespace

void SongMenu::open(const TrackRow &song, const gfx::Rect &anchor, bool panel, ui::Feedback &feedback)
{
    if (song.title.empty())
        return; // details still on their way: nothing to offer yet
    song_ = song;
    anchor_ = anchor;
    acts_.clear();
    if (song.uri.rfind("spotify:track:", 0) == 0)
        acts_.push_back(Act::radio);
    if (!song.album_uri.empty())
        acts_.push_back(Act::album);
    if (!song.artist_cards.empty())
        acts_.push_back(Act::artist);
    acts_.push_back(Act::details);
    popup_ = !panel && context_.settings.song_menu == 1;
    open_ = true;
    show(View::actions);
    if (amount_.value < 0.01f)
        pill_.snap(0.0f);
    feedback.play(audio::Cue::modal_open);
}

void SongMenu::show(View view)
{
    view_ = view;
    focus_ = 0;
    pill_.target = 0.0f;
    view_in_.snap(0.0f);
    view_in_.target = 1.0f;
}

int SongMenu::count() const
{
    switch (view_)
    {
    case View::actions:
        return static_cast<int>(acts_.size());
    case View::artists:
        return static_cast<int>(song_.artist_cards.size());
    case View::details:
        return 0;
    }
    return 0;
}

void SongMenu::activate(ui::Feedback &feedback)
{
    const auto open_card = [&](const Card &card) {
        Request request;
        request.kind = Request::Kind::open;
        request.card = card;
        context_.requests.push_back(request);
        open_ = false;
        feedback.play(audio::Cue::open);
    };
    if (view_ == View::artists)
    {
        open_card(song_.artist_cards[static_cast<std::size_t>(focus_)]);
        return;
    }
    switch (acts_[static_cast<std::size_t>(focus_)])
    {
    case Act::radio:
    {
        // A page of its own, as on the phone: the song's cover and name
        // until Spotify's radio playlist arrives, then it plays.
        Request request;
        request.kind = Request::Kind::radio;
        request.card.uri = song_.uri;
        request.card.title = song_.title + " Radio";
        request.card.subtitle = "Playlist \xC2\xB7 Spotify";
        request.card.image = song_.image;
        context_.requests.push_back(request);
        open_ = false;
        feedback.play(audio::Cue::launch);
        feedback.rumble(0.6f, 0.15f);
        break;
    }
    case Act::album:
    {
        Card album;
        album.uri = song_.album_uri;
        album.title = song_.album;
        album.subtitle = "Album";
        album.image = song_.image;
        open_card(album);
        break;
    }
    case Act::artist:
        if (song_.artist_cards.size() == 1)
            open_card(song_.artist_cards.front());
        else
        {
            show(View::artists);
            feedback.play(audio::Cue::tab);
        }
        break;
    case Act::details:
        show(View::details);
        feedback.play(audio::Cue::tab);
        break;
    }
}

void SongMenu::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    if (open_)
    {
        const int rows = count();
        if (input.is_pressed(Action::back) || input.is_pressed(Action::menu))
        {
            if (view_ != View::actions && input.is_pressed(Action::back))
            {
                show(View::actions);
                feedback.play(audio::Cue::back);
            }
            else
            {
                open_ = false;
                feedback.play(audio::Cue::modal_close);
            }
        }
        else if (input.is_pressed(Action::confirm) && rows > 0)
            activate(feedback);
        else if ((input.nav == Direction::down || input.nav == Direction::up) && rows > 0)
        {
            const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < rows)
            {
                focus_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.3f);
            }
            else if (!input.nav_repeat)
                feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        }
    }
    const bool calm = context_.settings.reduced_motion;
    amount_.target = open_ ? 1.0f : 0.0f;
    amount_.update(dt, calm ? 40.0f : 16.0f);
    pill_.target = static_cast<float>(focus_);
    pill_.update(dt, 24.0f);
    view_in_.update(dt, calm ? 40.0f : 14.0f);
}

void SongMenu::draw_rows(gfx::DrawList &list, float x, float top, float w, float row_h, float size) const
{
    const ui::Fonts &fonts = context_.fonts;
    const float in = view_in_.value;
    if (view_ == View::details)
    {
        const std::pair<const char *, std::string> facts[] = {
            {"SONG", song_.title},
            {"ARTISTS", song_.artists},
            {"ALBUM", song_.album.empty() ? "Unknown" : song_.album},
            {"LENGTH", song_.duration_ms ? length_text(song_.duration_ms) : "Unknown"},
        };
        for (int i = 0; i < 4; ++i)
        {
            const float y = top + static_cast<float>(i) * row_h + 10.0f * (1.0f - in);
            list.push_opacity(in);
            ui::text(list, fonts.semibold, facts[i].first, x + 24, y + row_h * 0.38f, 17,
                     kInk.with_alpha(0.55f), gfx::Align::left, 2.5f);
            ui::text(list, fonts.regular, fonts.regular.font->fit(facts[i].second, size * 0.85f, w - 48), x + 24,
                     y + row_h * 0.78f, size * 0.85f, kInk);
            list.pop_opacity();
        }
        return;
    }
    if (count() > 0)
    {
        const Rect pill{x + 10, top + pill_.value * row_h + 4, w - 20, row_h - 8};
        list.rounded_rect(pill, 14, kInk.with_alpha(0.14f));
        list.bordered_rect(pill, 14, kClear, 2.5f, kInk.with_alpha(0.9f));
    }
    for (int i = 0; i < count(); ++i)
    {
        const float y = top + static_cast<float>(i) * row_h;
        const float cy = y + row_h * 0.5f;
        const bool focused = i == focus_;
        const Color ink = kInk.with_alpha(focused ? 1.0f : 0.82f);
        list.push_opacity(std::clamp(in * 1.6f - static_cast<float>(i) * 0.12f, 0.0f, 1.0f));
        std::string text;
        if (view_ == View::artists)
        {
            icon(list, 2, x + 52, cy, ink);
            text = song_.artist_cards[static_cast<std::size_t>(i)].title;
        }
        else
        {
            const int act = static_cast<int>(acts_[static_cast<std::size_t>(i)]);
            icon(list, act, x + 52, cy, ink);
            text = label(act);
        }
        const ui::FontRef &face = focused ? fonts.semibold : fonts.regular;
        ui::text(list, face, face.font->fit(text, size, w - 120), x + 96, cy + size * 0.36f, size, ink);
        list.pop_opacity();
    }
}

void SongMenu::draw_panel(gfx::DrawList &list, float amount) const
{
    const ui::Fonts &fonts = context_.fonts;
    const float slide = context_.settings.reduced_motion ? 0.0f : (1.0f - amount) * (kPanelW + 40.0f);
    const Rect panel{kPanelX + slide, 24, kPanelW, gfx::kVirtualHeight - 48};
    list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000, 0.32f * amount));
    list.push_opacity(context_.settings.reduced_motion ? amount : 1.0f);
    list.shadow({panel.x - 10, panel.y + 14, panel.w, panel.h}, 32, 50, Color::rgb(0x000000, 0.55f * amount));
    list.rounded_rect(panel, 32, Color::rgb(0x15121a));
    list.bordered_rect(panel, 32, kClear, 1.5f, kInk.with_alpha(0.12f));

    // The song: its cover, title and artists.
    const float x = panel.x + 48;
    const float w = panel.w - 96;
    const Rect cover{x, panel.y + 56, 180, 180};
    const Art art = context_.art && !song_.image.empty() ? context_.art(song_.image) : Art{};
    if (art.texture)
        list.image(art.texture, cover, gfx::kFullUv, kInk, 18);
    else
        list.rounded_rect(cover, 18, kInk.with_alpha(0.08f));
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(song_.title, 36, w), x, panel.y + 296, 36, kInk);
    ui::paragraph(list, fonts.regular, song_.artists, x, panel.y + 344, 25, w, 34, kInk.with_alpha(0.7f), 2);
    list.rounded_rect({x, panel.y + 404, w, 1.5f}, 0, kInk.with_alpha(0.16f));
    if (view_ != View::actions)
        ui::text(list, fonts.semibold, view_ == View::artists ? "ARTISTS" : "DETAILS", x, panel.y + 452, 18,
                 kGreen, gfx::Align::left, 4.0f);
    const float top = panel.y + (view_ == View::actions ? 430 : 474);
    draw_rows(list, panel.x + 20, top, panel.w - 40, view_ == View::details ? 92.0f : 76.0f, 29);

    ui::Hint hints[2];
    int n = 0;
    if (count() > 0)
        hints[n++] = {ui::Button::cross, "Select"};
    hints[n++] = {ui::Button::circle, view_ == View::actions ? "Close" : "Back"};
    ui::draw_hints(list, fonts, style::hint_style(), hints, n, panel.x + panel.w - 40, true, style::hint_layout());
    list.pop_opacity();
}

void SongMenu::draw_popup(gfx::DrawList &list, float amount) const
{
    const ui::Fonts &fonts = context_.fonts;
    const int rows = view_ == View::details ? 4 : count();
    const float row_h = view_ == View::details ? 80.0f : kPopupRow;
    const float h = static_cast<float>(rows) * row_h + 2.0f * kPopupPad;
    const float x = std::clamp(anchor_.x + anchor_.w - kPopupW - 40.0f, kMargin, kRight - kPopupW);
    // Above a row in the lower half of the screen, below one in the upper half.
    const bool above = anchor_.cy() > gfx::kVirtualHeight * 0.52f;
    float y = above ? anchor_.y - 16.0f - h : anchor_.y + anchor_.h + 16.0f;
    y = std::clamp(y, 40.0f, gfx::kVirtualHeight - 150.0f - h);
    const Rect box{x, y, kPopupW, h};

    list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000, 0.22f * amount));
    list.push_opacity(amount);
    // It springs from the pointer's end of the box.
    const float scale = context_.settings.reduced_motion ? 1.0f : 0.94f + 0.06f * amount;
    const float pointer_x = std::clamp(anchor_.x + anchor_.w - 140.0f, box.x + 40, box.x + box.w - 40);
    list.push_transform(scale, pointer_x, above ? box.y + box.h : box.y, 0, 0);
    list.shadow({box.x, box.y + 14, box.w, box.h}, 22, 40, Color::rgb(0x000000, 0.6f));
    list.rounded_rect(box, 22, Color::rgb(0x15121a));
    list.bordered_rect(box, 22, kClear, 1.5f, kInk.with_alpha(0.12f));
    // A small pointer at the song.
    const Rect tip{pointer_x - 14, above ? box.y + box.h - 2 : box.y - 12, 28, 14};
    list.triangle(tip, Color::rgb(0x15121a), 0.0f, above ? 3.1416f : 0.0f);
    draw_rows(list, box.x, box.y + kPopupPad, box.w, row_h, 27);
    list.pop_transform();

    ui::Hint hints[2];
    int n = 0;
    if (count() > 0)
        hints[n++] = {ui::Button::cross, "Select"};
    hints[n++] = {ui::Button::circle, view_ == View::actions ? "Close" : "Back"};
    ui::draw_hints(list, fonts, style::hint_style(), hints, n, kRight, true, style::hint_layout());
    list.pop_opacity();
}

void SongMenu::draw(gfx::DrawList &list) const
{
    const float amount = std::clamp(amount_.value, 0.0f, 1.0f);
    if (amount < 0.01f)
        return;
    if (popup_)
        draw_popup(list, amount);
    else
        draw_panel(list, amount);
}

} // namespace screens
