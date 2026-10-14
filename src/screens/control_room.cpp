// spot5 - Control Room: the app's settings, opened with the touchpad.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-homebrew-ui src/concepts/settings.cpp (GPL-3.0-or-later,
// BlackBearReloaded): one spring focus between rail and rows, slider /
// stepper / toggle drawn from shapes and springs, footer and hint
// cross-fades, the frosted confirm dialog, the "Saved" toast. Added: an info
// row, a name row that opens the kit keyboard in a sheet, three dialogs.

#include "screens/control_room.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/tween.hpp"
#include "screens/style.hpp"
#include "screens/timezones.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"
#include "ui/theme.hpp"

namespace screens
{

namespace
{

using gfx::Color;
using gfx::Rect;

// ---- the design language (settings.cpp, accent turned toward the app's green) ----

const Color kInk = Color::rgb(0xe9f3ef);
const Color kAccent = Color::rgb(0x45e0a0);
const Color kDanger = Color::rgb(0xff8f8f);
const Color kDeep = Color::rgb(0x08170f);
const Color kSurface = Color::rgb(0x11241b);
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kPi = 3.14159265f;
constexpr float kMargin = 96.0f;
constexpr float kRadius = 18.0f;
constexpr float kPanelRadius = 28.0f;

constexpr float kRailW = 372.0f;
constexpr float kRailY = 236.0f;
constexpr float kRailH = 72.0f;
constexpr float kRailPitch = 84.0f;

constexpr Rect kPanel{504.0f, 236.0f, 1320.0f, 692.0f};
constexpr float kPanelPad = 24.0f;
constexpr float kRowPad = 28.0f;
constexpr float kRowsY = kPanel.y + 104.0f;
constexpr float kRowH = 72.0f;
constexpr float kRowPitch = 80.0f;
constexpr float kFooterY = kPanel.y + kPanel.h - 84.0f;
constexpr float kTileH = 150.0f; // About: two lines of tiles above two action rows
constexpr float kTileGap = 16.0f;

constexpr float kSliderW = 440.0f;
constexpr float kToggleW = 84.0f;
constexpr float kToggleH = 44.0f;
constexpr float kStepperW = 300.0f;

constexpr Rect kDialog{560.0f, 348.0f, 800.0f, 384.0f};
constexpr Rect kSheet{360.0f, 300.0f, 1200.0f, 600.0f};
constexpr Rect kPicker{510.0f, 170.0f, 900.0f, 740.0f};
constexpr float kPickerRowH = 62.0f;
constexpr float kPickerTop = 150.0f; // the list starts this far into the picker
constexpr int kPickerPage = 8;       // L1 / R1 move this many zones
constexpr int kNameLimit = 24;

constexpr float kSaveDelay = 0.8f; // the app writes the file 0.75 s after the last change
constexpr float kToastSeconds = 2.0f;

// ---- what the screen edits ----

enum class Field : std::uint8_t
{
    quality,
    volume,
    phone_volume,
    device_name,
    account,
    forget,
    reduce_motion,
    resolution,
    timezone,
    song_menu,
    clear_cache,
    reset,
    quit,
    count,
};
constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::count);

enum class Kind : std::uint8_t
{
    slider,  // 0..10, left and right
    stepper, // a few named choices, left and right
    toggle,  // on or off, confirm
    action,  // confirm asks first (dialog)
    name,    // shows a name; confirm opens the keyboard
    choose,  // shows a value; confirm opens a list to pick from
    info,    // shows a value; nothing to do
};

struct Row
{
    const char *label;
    const char *description;
    Kind kind;
    Field field;
};

struct Category
{
    const char *name;
    const char *summary;
    const Row *rows;
    int count;
};

constexpr Row kPlaybackRows[] = {
    {"Audio quality", "Higher sounds better and uses more data. Applies from the next track.",
     Kind::stepper, Field::quality},
    {"Volume", "This console's own level, on top of the volume set on your phone.", Kind::slider,
     Field::volume},
    {"Phone controls volume", "The volume buttons on your phone also turn this console up and down.",
     Kind::toggle, Field::phone_volume},
};
constexpr Row kDeviceRows[] = {
    {"Name", "How this console appears in Spotify's device list. Reconnects to apply.", Kind::name,
     Field::device_name},
    {"Time zone", "Shapes Home's morning and evening shelves. Applies when Home next refreshes.",
     Kind::choose, Field::timezone},
};
constexpr Row kAccountRows[] = {
    {"Signed in as", "The Spotify account this console plays for.", Kind::info, Field::account},
    {"Forget this login", "Deletes the saved login from this console. You will need to pair again.",
     Kind::action, Field::forget},
};
constexpr Row kDisplayRows[] = {
    {"Resolution", "How sharp everything is drawn. 4K matches a 4K TV. Applies the next time the app starts.",
     Kind::stepper, Field::resolution},
    {"Reduce motion", "Replaces slides, zooms and overshoot with short fades.", Kind::toggle,
     Field::reduce_motion},
    {"Song menu", "What Options on a song opens: a panel from the right edge, or a pop-up next to the song.",
     Kind::stepper, Field::song_menu},
};
constexpr Row kAboutRows[] = {
    {"Clear cache", "Deletes saved covers, Home and Library; they download again as you browse.",
     Kind::action, Field::clear_cache},
    {"Reset all settings", "Returns every option on these pages to its default value.",
     Kind::action, Field::reset},
};
constexpr Row kQuitRows[] = {
    {"Quit Spot5", "The music stops and the app closes. The PS button can also close it.",
     Kind::action, Field::quit},
};

constexpr int kAbout = 4;
constexpr int kQuit = 5;
constexpr Category kCategories[] = {
    {"Playback", "Sound quality and how loud this console plays.", kPlaybackRows, 3},
    {"Device", "How this console shows up in Spotify, and its local time.", kDeviceRows, 2},
    {"Account", "Who is signed in, and how to forget it.", kAccountRows, 2},
    {"Display", "How sharp and how calm the interface is, and how menus open.", kDisplayRows, 3},
    {"About", "What this app is built from, its saved data, and the way back to its defaults.", kAboutRows, 2},
    {"Quit", "Close the app.", kQuitRows, 1},
};
constexpr int kCategoryCount = 6;

constexpr const char *kQualities[] = {"96 kbps", "160 kbps", "320 kbps"};
constexpr const char *kResolutions[] = {"1080p", "1440p", "4K"};
constexpr const char *kSongMenus[] = {"Side panel", "Pop-up"};
constexpr const char *kQualityNames[] = {"Normal", "High", "Very high"};
constexpr const char *kMotionNotes[] = {
    "Screens and panels fade instead of sliding",
    "The backdrops and the idle glows hold still",
    "Switches and dialogs settle without overshoot",
    "Sounds and the focus highlight work exactly as before",
};

enum class HintMode : std::uint8_t
{
    rail,
    slider,
    stepper,
    toggle_off,
    toggle_on,
    action,
    name,
    choose,
    info,
    dialog,
    keyboard,
    picker,
};

enum class Ask : std::uint8_t
{
    none,
    forget,
    clear,
    reset,
    quit,
};

struct Control
{
    int seen = 0;
    int previous = 0;
    float direction = 1.0f;
    tween::Spring level;
    tween::Bounce thumb;
    tween::Timer change;
    ui::Pulse press;
};

float as_float(int value)
{
    return static_cast<float>(value);
}

Rect rail_rect(int category)
{
    return {kMargin, kRailY + as_float(category) * kRailPitch, kRailW, kRailH};
}

Rect row_rect(int category, int row)
{
    float y = kRowsY + as_float(row) * kRowPitch;
    if (category == kAbout)
        y += 2.0f * (kTileH + kTileGap) + 8.0f; // the action sits under the fact tiles
    return {kPanel.x + kPanelPad, y, kPanel.w - 2.0f * kPanelPad, kRowH};
}

float centred(float cy, float size)
{
    return cy + 0.35f * size;
}

ui::Theme keyboard_theme()
{
    ui::Theme theme = ui::default_theme();
    for (const ui::Theme &t : ui::themes())
        if (std::string_view(t.id) == "acrylic")
            theme = t;
    theme.primary = kAccent;
    theme.accent = kAccent;
    theme.focus = kAccent;
    theme.on_primary = kDeep;
    theme.surface = kSurface.with_alpha(0.8f);
    return theme;
}

class ControlRoom final : public Screen
{
  public:
    explicit ControlRoom(Context &context) : context_(context)
    {
        for (std::size_t i = 0; i < kFieldCount; ++i)
        {
            Control &control = controls_[i];
            control.seen = control.previous = value(static_cast<Field>(i));
            control.level.snap(as_float(control.seen));
            control.thumb.snap(as_float(control.seen));
        }
        focus_.snap(rail_rect(0));
        marker_.snap(rail_rect(0).y);
        footer_shown_ = footer_previous_ = footer_text();

        keyboard_.style.theme = keyboard_theme();
        keyboard_.style.bindings = ui::KeyboardBindings::standard();
        keyboard_.style.max_length = kNameLimit;
        keyboard_.set_bounds({kSheet.x + 60, kSheet.y + 210, kSheet.w - 120, kSheet.h - 250});
        keyboard_.on_text = [this](std::string_view utf8) {
            if (static_cast<int>(name_edit_.size() + utf8.size()) <= kNameLimit)
                name_edit_ += utf8;
        };
        keyboard_.on_backspace = [this] {
            // UTF-8 aware: remove the whole last character.
            while (!name_edit_.empty())
            {
                const unsigned char c = static_cast<unsigned char>(name_edit_.back());
                name_edit_.pop_back();
                if ((c & 0xC0) != 0x80)
                    break;
            }
        };
    }

    void enter() override
    {
        age_ = 0.0f;
        content_age_ = -0.2f;
        leave_.running = false;
        ask_ = Ask::none;
        sheet_open_ = false;
        picker_open_ = false;
        close_ = false;
        column_ = Column::rail;
    }

    void update(const Model &, const InputFrame &input, float dt, ui::Feedback &feedback,
                std::vector<Command> &commands) override
    {
        const bool calm = context_.settings.reduced_motion;
        age_ += dt;
        clock_ += dt;
        content_age_ += dt;
        if (!calm)
            drift_ += dt;

        if (ask_ != Ask::none)
            update_dialog(input, feedback, commands);
        else if (sheet_open_)
            update_sheet(input, feedback);
        else if (picker_open_)
            update_picker(input, feedback);
        else
            update_screen(input, feedback);
        update_toast(dt, feedback);

        animate_controls(dt, calm);
        focus_.target(column_ == Column::rail ? rail_rect(category_)
                                              : row_rect(category_, row_[category_]));
        focus_.update(dt, calm ? 36.0f : 20.0f);
        marker_.target = rail_rect(category_).y;
        marker_.update(dt, calm ? 36.0f : 16.0f);
        refuse_.update(dt, 9.0f);
        leave_.update(dt);
        dialog_.target = ask_ != Ask::none ? 1.0f : 0.0f;
        dialog_.update(dt, calm ? 40.0f : 14.0f);
        sheet_.target = sheet_open_ ? 1.0f : 0.0f;
        sheet_.update(dt, calm ? 40.0f : 14.0f);
        picker_.target = picker_open_ ? 1.0f : 0.0f;
        picker_.update(dt, calm ? 40.0f : 14.0f);
        picker_focus_.target = as_float(picker_index_) * kPickerRowH;
        picker_focus_.update(dt, calm ? 40.0f : 22.0f);
        const float view = kPicker.h - kPickerTop - 40.0f;
        picker_scroll_.target = std::clamp(picker_focus_.target - view * 0.5f + kPickerRowH * 0.5f, 0.0f,
                                           std::max(0.0f, as_float(kTimeZoneCount + 1) * kPickerRowH - view));
        picker_scroll_.update(dt, calm ? 40.0f : 16.0f);
        keyboard_.style.reduced_motion = calm;
        keyboard_.set_length(static_cast<int>(name_edit_.size()));
        keyboard_.update(dt);
        choice_position_.target = as_float(choice_);
        choice_position_.update(dt, calm ? 36.0f : 22.0f);
        toast_.target = toast_left_ > 0.0f ? 1.0f : 0.0f;
        toast_.update(dt, calm ? 40.0f : 13.0f);

        if (const char *wanted = footer_text(); wanted != footer_shown_)
        {
            footer_previous_ = footer_shown_;
            footer_shown_ = wanted;
            footer_.start(calm ? 0.12f : 0.24f);
        }
        footer_.update(dt);
        if (const HintMode wanted = hint_mode(); wanted != hints_shown_)
        {
            hints_previous_ = hints_shown_;
            hints_shown_ = wanted;
            hints_.start(calm ? 0.12f : 0.22f);
        }
        hints_.update(dt);
    }

    void draw(const Model &model, Frame &frame) const override
    {
        frame.backdrop.mode = gfx::BackdropMode::dots;
        frame.backdrop.colors[0] = Color::rgb(0x061109);
        frame.backdrop.colors[1] = Color::rgb(0x0c2a1c);
        frame.backdrop.colors[2] = Color::rgb(0x2f8f62);
        frame.backdrop.time = drift_;

        gfx::DrawList &list = frame.scene;
        const float back = std::max({dialog_.value, sheet_.value, picker_.value});
        list.push_transform(1.0f - 0.03f * back * travel(), 960, 540, 0, 0);
        draw_header(list);
        draw_rail(list);
        draw_panel(list);
        draw_focus(list);
        draw_rail_labels(list);
        if (leave_.running)
        {
            const float t = leave_.progress();
            draw_category(list, model, previous_, 1.0f - tween::smoothstep(t * 1.25f),
                          -change_direction_ * 18.0f * tween::cubic_in(t) * travel(), 10.0f);
        }
        draw_category(list, model, category_, 1.0f, 0.0f, content_age_);
        draw_footer(list);
        list.pop_transform();
        if (back > 0.01f)
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              kDeep.with_alpha(0.6f * back));

        if (dialog_.value > 0.01f)
        {
            frame.glass = true;
            draw_dialog(frame.overlay, frame.glass_texture);
        }
        if (sheet_.value > 0.01f)
        {
            frame.glass = true;
            draw_sheet(frame.overlay, frame.glass_texture);
        }
        if (picker_.value > 0.01f)
        {
            frame.glass = true;
            draw_picker(frame.overlay, frame.glass_texture);
        }
        draw_hints(frame);
        draw_toast(frame.overlay);
    }

    bool wants_close() const { return close_; }

  private:
    enum class Column : std::uint8_t
    {
        rail,
        rows,
    };

    // ---- the settings, by field ----

    int value(Field field) const
    {
        const Settings &s = context_.settings;
        switch (field)
        {
        case Field::quality:
            return s.quality;
        case Field::volume:
            return s.volume;
        case Field::phone_volume:
            return s.phone_volume ? 1 : 0;
        case Field::reduce_motion:
            return s.reduced_motion ? 1 : 0;
        case Field::resolution:
            return s.resolution;
        case Field::song_menu:
            return s.song_menu;
        default:
            return 0;
        }
    }

    static int limit(Field field)
    {
        switch (field)
        {
        case Field::volume:
            return 10;
        case Field::quality:
        case Field::resolution:
            return 2;
        default:
            return 1;
        }
    }

    void store(Field field, int next)
    {
        Settings &s = context_.settings;
        switch (field)
        {
        case Field::quality:
            s.quality = next;
            break;
        case Field::volume:
            s.volume = next;
            break;
        case Field::phone_volume:
            s.phone_volume = next != 0;
            break;
        case Field::reduce_motion:
            s.reduced_motion = next != 0;
            break;
        case Field::resolution:
            s.resolution = next;
            mark_changed("Saved \xC2\xB7 next launch");
            return;
        case Field::song_menu:
            s.song_menu = next;
            break;
        default:
            return;
        }
        mark_changed("Saved");
    }

    void mark_changed(const char *message)
    {
        context_.settings_changed = true;
        save_wait_ = kSaveDelay;
        pending_message_ = message;
    }

    const Control &control(Field field) const
    {
        return controls_[static_cast<std::size_t>(field)];
    }
    Control &control(Field field)
    {
        return controls_[static_cast<std::size_t>(field)];
    }

    const Row &focused_row() const
    {
        return kCategories[category_].rows[row_[category_]];
    }

    float travel() const
    {
        return context_.settings.reduced_motion ? 0.0f : 1.0f;
    }

    const char *footer_text() const
    {
        return column_ == Column::rail ? kCategories[category_].summary : focused_row().description;
    }

    HintMode hint_mode() const
    {
        if (ask_ != Ask::none)
            return HintMode::dialog;
        if (sheet_open_)
            return HintMode::keyboard;
        if (picker_open_)
            return HintMode::picker;
        if (column_ == Column::rail)
            return HintMode::rail;
        const Row &row = focused_row();
        switch (row.kind)
        {
        case Kind::slider:
            return HintMode::slider;
        case Kind::stepper:
            return HintMode::stepper;
        case Kind::toggle:
            return value(row.field) != 0 ? HintMode::toggle_on : HintMode::toggle_off;
        case Kind::name:
            return HintMode::name;
        case Kind::choose:
            return HintMode::choose;
        case Kind::info:
            return HintMode::info;
        default:
            return HintMode::action;
        }
    }

    // ---- input ----

    void refuse(const InputFrame &input, ui::Feedback &feedback)
    {
        if (input.nav_repeat)
            return;
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        refuse_.trigger();
    }

    void show_category(int next, ui::Feedback &feedback)
    {
        previous_ = category_;
        change_direction_ = next > category_ ? 1.0f : -1.0f;
        category_ = next;
        leave_.start(context_.settings.reduced_motion ? 0.1f : 0.14f);
        content_age_ = -0.06f;
        feedback.play(audio::Cue::tab, 1.1f - 0.045f * as_float(next),
                      ui::pan_for_x(rail_rect(next).cx()));
    }

    void step_category(int delta, const InputFrame &input, ui::Feedback &feedback)
    {
        const int next = category_ + delta;
        if (next < 0 || next >= kCategoryCount)
            refuse(input, feedback);
        else
            show_category(next, feedback);
    }

    void update_screen(const InputFrame &input, ui::Feedback &feedback)
    {
        if (input.is_pressed(Action::jump_prev))
            step_category(-1, input, feedback);
        else if (input.is_pressed(Action::jump_next))
            step_category(1, input, feedback);
        else if (column_ == Column::rail)
            update_rail(input, feedback);
        else
            update_rows(input, feedback);
    }

    void update_rail(const InputFrame &input, ui::Feedback &feedback)
    {
        switch (input.nav)
        {
        case Direction::up:
            step_category(-1, input, feedback);
            break;
        case Direction::down:
            step_category(1, input, feedback);
            break;
        case Direction::right:
            enter_rows(feedback, audio::Cue::focus);
            break;
        case Direction::left:
            refuse(input, feedback);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm))
            enter_rows(feedback, audio::Cue::select);
        else if (input.is_pressed(Action::back))
        {
            // Back from the rail leaves Control Room.
            close_ = true;
            feedback.play(audio::Cue::back);
        }
    }

    void enter_rows(ui::Feedback &feedback, audio::Cue cue)
    {
        if (column_ == Column::rows)
            return;
        column_ = Column::rows;
        feedback.play(cue, 1.0f, ui::pan_for_x(kPanel.cx()));
    }

    void leave_rows(ui::Feedback &feedback, audio::Cue cue)
    {
        column_ = Column::rail;
        feedback.play(cue, 1.0f, ui::pan_for_x(rail_rect(category_).cx()));
    }

    void update_rows(const InputFrame &input, ui::Feedback &feedback)
    {
        const Category &category = kCategories[category_];
        int &row = row_[category_];
        const Row &current = category.rows[row];
        const bool adjustable = current.kind == Kind::slider || current.kind == Kind::stepper;
        switch (input.nav)
        {
        case Direction::up:
        case Direction::down:
        {
            const int next = row + (input.nav == Direction::down ? 1 : -1);
            if (next < 0 || next >= category.count)
            {
                refuse(input, feedback);
                break;
            }
            row = next;
            feedback.play(audio::Cue::focus, 1.04f - 0.03f * as_float(next),
                          ui::pan_for_x(kPanel.cx()));
            break;
        }
        case Direction::left:
            if (adjustable)
                adjust(current, -1, input, feedback);
            else
                leave_rows(feedback, audio::Cue::focus);
            break;
        case Direction::right:
            if (adjustable)
                adjust(current, 1, input, feedback);
            else
                refuse(input, feedback);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm))
            activate(category.rows[row], feedback);
        if (input.is_pressed(Action::back))
            leave_rows(feedback, audio::Cue::back);
    }

    void adjust(const Row &row, int delta, const InputFrame &input, ui::Feedback &feedback)
    {
        const int next = value(row.field) + delta;
        Control &state = control(row.field);
        state.direction = as_float(delta);
        if (next < 0 || next > limit(row.field))
        {
            refuse(input, feedback);
            return;
        }
        store(row.field, next);
        state.press.trigger();
        const float pan = ui::pan_for_x(control_x(row, next));
        if (row.kind == Kind::slider)
            feedback.play(audio::Cue::slider, 0.85f + 0.04f * as_float(next), pan);
        else
            feedback.play(audio::Cue::slider, 0.94f + 0.06f * as_float(next), pan);
    }

    void activate(const Row &row, ui::Feedback &feedback)
    {
        switch (row.kind)
        {
        case Kind::toggle:
        {
            const bool on = value(row.field) == 0;
            store(row.field, on ? 1 : 0);
            control(row.field).press.trigger();
            feedback.play(audio::Cue::toggle, on ? 1.06f : 0.94f,
                          ui::pan_for_x(control_x(row, on ? 1 : 0)));
            break;
        }
        case Kind::stepper:
        {
            const int next = (value(row.field) + 1) % (limit(row.field) + 1);
            store(row.field, next);
            control(row.field).press.trigger();
            control(row.field).direction = 1.0f;
            feedback.play(audio::Cue::slider, 0.94f + 0.06f * as_float(next),
                          ui::pan_for_x(control_x(row, next)));
            break;
        }
        case Kind::action:
            ask_ = row.field == Field::forget        ? Ask::forget
                   : row.field == Field::clear_cache ? Ask::clear
                   : row.field == Field::reset       ? Ask::reset
                                                     : Ask::quit;
            last_ask_ = ask_;
            choice_ = 0; // the safe choice
            choice_position_.snap(0.0f);
            feedback.play(audio::Cue::modal_open);
            break;
        case Kind::name:
            sheet_open_ = true;
            name_edit_ = context_.settings.device_name;
            keyboard_.set_layout(0);
            feedback.play(audio::Cue::modal_open);
            break;
        case Kind::choose:
            // The list opens on what is chosen now (0 is Automatic).
            picker_open_ = true;
            picker_index_ = zone_index(context_.settings.timezone) + 1;
            picker_focus_.snap(as_float(picker_index_) * kPickerRowH);
            picker_scroll_.snap(0.0f);
            feedback.play(audio::Cue::modal_open);
            break;
        case Kind::slider:
        case Kind::info:
            break;
        }
    }

    void update_sheet(const InputFrame &input, ui::Feedback &feedback)
    {
        const ui::Event event = keyboard_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            // Done: keep a non-empty name; an unchanged one is not a change.
            if (!name_edit_.empty() && name_edit_ != context_.settings.device_name)
            {
                context_.settings.device_name = name_edit_;
                mark_changed("Renamed");
            }
            sheet_open_ = false;
        }
        else if (event == ui::Event::cancelled || input.is_pressed(Action::back))
        {
            sheet_open_ = false;
            feedback.play(audio::Cue::back);
        }
    }

    // The time zone list: up / down (L1 / R1 by a page), Cross picks, Circle cancels.
    void update_picker(const InputFrame &input, ui::Feedback &feedback)
    {
        int step = 0;
        if (input.nav == Direction::up || input.nav == Direction::down)
            step = input.nav == Direction::down ? 1 : -1;
        else if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
            step = input.is_pressed(Action::page_next) ? kPickerPage : -kPickerPage;
        if (step != 0)
        {
            const int next = std::clamp(picker_index_ + step, 0, kTimeZoneCount);
            if (next == picker_index_)
                refuse(input, feedback);
            else
            {
                picker_index_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.0f);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            const std::string chosen = picker_index_ == 0 ? std::string() : kTimeZones[picker_index_ - 1].iana;
            picker_open_ = false;
            feedback.play(audio::Cue::select);
            if (chosen != context_.settings.timezone)
            {
                context_.settings.timezone = chosen;
                mark_changed("Saved");
            }
        }
        else if (input.is_pressed(Action::back))
        {
            picker_open_ = false;
            feedback.play(audio::Cue::back);
        }
    }

    // What the Time zone row shows: Automatic with the console's offset, or the city.
    std::string timezone_label() const
    {
        const std::string &chosen = context_.settings.timezone;
        if (chosen.empty())
            return "Automatic \xC2\xB7 " + utc_label(context_.console_utc_minutes);
        const int index = zone_index(chosen);
        if (index < 0)
            return chosen;
        return std::string(kTimeZones[index].city) + " \xC2\xB7 " + utc_label(kTimeZones[index].minutes);
    }

    void update_dialog(const InputFrame &input, ui::Feedback &feedback,
                       std::vector<Command> &commands)
    {
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = input.nav == Direction::right ? 1 : 0;
            if (next != choice_)
            {
                choice_ = next;
                feedback.play(audio::Cue::focus, 1.0f, next == 1 ? 0.2f : -0.2f);
            }
            else
            {
                refuse(input, feedback);
            }
        }
        if (input.is_pressed(Action::confirm))
        {
            const Ask asked = ask_;
            ask_ = Ask::none;
            if (choice_ != 1)
            {
                feedback.play(audio::Cue::modal_close);
                return;
            }
            feedback.play(audio::Cue::select);
            feedback.rumble(0.4f, 0.1f);
            switch (asked)
            {
            case Ask::forget:
                commands.push_back(Command::forget_login);
                close_ = true; // back to the screens: the launch screen explains
                break;
            case Ask::clear:
                commands.push_back(Command::clear_cache);
                mark_changed("Cache cleared");
                break;
            case Ask::reset:
                context_.settings = Settings{};
                mark_changed("Defaults restored");
                break;
            case Ask::quit:
                commands.push_back(Command::quit);
                break;
            case Ask::none:
                break;
            }
        }
        else if (input.is_pressed(Action::back))
        {
            ask_ = Ask::none;
            feedback.play(audio::Cue::back);
        }
    }

    void update_toast(float dt, ui::Feedback &feedback)
    {
        toast_left_ = std::max(0.0f, toast_left_ - dt);
        toast_age_ += dt;
        if (save_wait_ <= 0.0f)
            return;
        save_wait_ -= dt;
        if (save_wait_ > 0.0f)
            return;
        if (toast_left_ <= 0.0f || toast_message_ != pending_message_)
        {
            feedback.play(audio::Cue::saved, 1.0f, 0.4f);
            toast_age_ = 0.0f;
        }
        toast_message_ = pending_message_;
        toast_left_ = kToastSeconds;
    }

    void animate_controls(float dt, bool calm)
    {
        for (std::size_t i = 0; i < kFieldCount; ++i)
        {
            Control &state = controls_[i];
            const int now = value(static_cast<Field>(i));
            if (now != state.seen)
            {
                if (state.press.value <= 0.0f)
                    state.direction = now > state.seen ? 1.0f : -1.0f;
                state.previous = state.seen;
                state.seen = now;
                state.change.start(calm ? 0.12f : 0.26f);
            }
            state.level.target = as_float(now);
            state.level.update(dt, calm ? 40.0f : 18.0f);
            state.thumb.target = as_float(now);
            state.thumb.update(dt, 22.0f, calm ? 1.0f : 0.6f);
            state.change.update(dt);
            state.press.update(dt, 10.0f);
        }
    }

    // ---- geometry ----

    static float control_right(const Rect &row)
    {
        return row.x + row.w - kRowPad;
    }

    float control_x(const Row &row, int at) const
    {
        const float right = control_right(row_rect(category_, row_[category_]));
        if (row.kind == Kind::slider)
            return right - 76.0f - kSliderW + kSliderW * as_float(at) / 10.0f;
        if (row.kind == Kind::stepper)
            return right - kStepperW * 0.5f;
        return right - kToggleW * 0.5f;
    }

    // ---- drawing ----

    void draw_header(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 0, 0.06f, 0.5f);
        const float lift = 14.0f * (1.0f - in) * travel();
        list.push_opacity(in);
        ui::text(list, fonts.semibold, "CONTROL ROOM", kMargin + 2, 118 - lift, 18, kAccent,
                 gfx::Align::left, 4.0f);
        ui::text(list, fonts.display, "Settings", kMargin - 3, 186 - lift, 64, kInk);
        list.pop_opacity();
    }

    void draw_rail(gfx::DrawList &list) const
    {
        const float in = tween::stagger(age_, 1, 0.05f, 0.45f);
        const Rect plate{kMargin, marker_.value, kRailW, kRailH};
        list.rounded_rect(plate, kRadius, kInk.with_alpha(0.06f * in));
        const float note_in = tween::stagger(age_, 8, 0.07f, 0.5f);
        list.push_opacity(note_in);
        list.circle(kMargin + 6, 866, 4, kAccent);
        ui::paragraph(list, context_.fonts.regular, "Changes apply at once and are saved for you.",
                      kMargin + 24, 874, 22, kRailW - 40, 32, kInk.with_alpha(0.55f), 2);
        list.pop_opacity();
    }

    void draw_rail_labels(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        for (int i = 0; i < kCategoryCount; ++i)
        {
            const float in = tween::stagger(age_, 1 + i, 0.05f, 0.45f);
            const Rect r = rail_rect(i);
            const bool active = i == category_;
            const float x = r.x - 28.0f * (1.0f - in) * travel();
            const Color ink = i == kQuit && !active ? kDanger.with_alpha(0.75f)
                              : active             ? kAccent
                                                   : kInk.with_alpha(0.6f);
            list.push_opacity(in);
            draw_icon(list, i, x + 46, r.cy(), ink);
            ui::text(list, active ? fonts.semibold : fonts.regular, kCategories[i].name, x + 88,
                     centred(r.cy(), 28), 28, kInk.with_alpha(active ? 1.0f : 0.68f));
            list.pop_opacity();
        }
        list.rounded_rect({kMargin + 8, marker_.value + 20, 4, kRailH - 40}, 2,
                          kAccent.with_alpha(tween::stagger(age_, 1, 0.05f, 0.45f)));
    }

    static void draw_icon(gfx::DrawList &list, int category, float cx, float cy, Color ink)
    {
        switch (category)
        {
        case 0: // speaker: body, cone and two waves
        {
            list.rounded_rect({cx - 15, cy - 5, 8, 10}, 2, ink);
            const float cone[] = {cx - 9, cy - 5, cx - 1, cy - 12, cx - 1, cy + 12, cx - 9, cy + 5};
            list.polygon(cone, 4, ink);
            list.arc(cx - 1, cy, 9, 2.5f, kPi * 0.22f, kPi * 0.56f, ink);
            list.arc(cx - 1, cy, 16, 2.5f, kPi * 0.22f, kPi * 0.56f, ink);
            break;
        }
        case 1: // a console standing up, with its light strip
            list.bordered_rect({cx - 8, cy - 16, 16, 32}, 5, kClear, 2.5f, ink);
            list.line(cx - 2, cy - 10, cx - 2, cy + 10, 2.0f, ink);
            break;
        case 2: // a person in a circle
            list.ring(cx, cy, 16, 2.5f, ink);
            list.circle(cx, cy - 4, 5, ink);
            list.arc(cx, cy + 14, 10, 2.5f, -kPi * 0.35f, kPi * 0.7f, ink);
            break;
        case 3: // screen on a stand
            list.bordered_rect({cx - 16, cy - 13, 32, 21}, 4, kClear, 2.5f, ink);
            list.line(cx - 7, cy + 13, cx + 7, cy + 13, 2.5f, ink);
            break;
        case 4: // "i" in a circle
            list.ring(cx, cy, 16, 2.5f, ink);
            list.circle(cx, cy - 7, 2.2f, ink);
            list.line(cx, cy - 1, cx, cy + 8, 3.0f, ink);
            break;
        default: // power: an open ring and a stroke
            list.arc(cx, cy + 1, 14, 2.8f, kPi * 0.2f, kPi * 1.6f, ink);
            list.line(cx, cy - 16, cx, cy - 2, 3.0f, ink);
            break;
        }
    }

    void draw_panel(gfx::DrawList &list) const
    {
        const float in = tween::stagger(age_, 3, 0.06f, 0.55f);
        list.push_opacity(in);
        list.shadow({kPanel.x, kPanel.y + 18, kPanel.w, kPanel.h}, kPanelRadius, 44,
                    Color::rgb(0x000000, 0.4f));
        list.gradient_rect(kPanel, kPanelRadius, kSurface.with_alpha(0.94f),
                           gfx::mix(kSurface, kDeep, 0.35f).with_alpha(0.94f));
        list.bordered_rect(kPanel, kPanelRadius, kClear, 1.5f, kInk.with_alpha(0.09f));
        list.rounded_rect({kPanel.x + kPanelPad, kFooterY, kPanel.w - 2 * kPanelPad, 1.5f}, 0,
                          kInk.with_alpha(0.1f));
        list.pop_opacity();
    }

    void draw_focus(gfx::DrawList &list) const
    {
        Rect r = focus_.value();
        r.x += ui::shake(refuse_.value, clock_, 9.0f, 9.0f);
        const float breath = context_.settings.reduced_motion ? 0.5f : ui::breathe(drift_);
        const bool danger = column_ == Column::rows && focused_row().kind == Kind::action;
        const Color tone = danger ? kDanger : kAccent;
        list.push_opacity(tween::stagger(age_, 4, 0.06f, 0.4f));
        list.shadow({r.x, r.y + 10, r.w, r.h}, kRadius, 24, Color::rgb(0x000000, 0.4f));
        list.glow(r, kRadius, 18, tone.with_alpha(0.14f + 0.1f * breath));
        list.rounded_rect(r, kRadius, gfx::mix(kSurface, tone, 0.17f));
        list.bordered_rect(r, kRadius, kClear, 2.0f, tone);
        list.pop_opacity();
    }

    void draw_category(gfx::DrawList &list, const Model &model, int index, float alpha,
                       float slide, float clock) const
    {
        if (alpha <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const Category &category = kCategories[index];
        const bool current = index == category_ && alpha >= 1.0f;
        const float rise = 18.0f * travel();
        list.push_opacity(alpha);

        const float title_in = tween::stagger(clock, 0, 0.05f, 0.3f);
        list.push_opacity(title_in);
        ui::text(list, fonts.semibold, category.name, kPanel.x + kPanelPad + kRowPad,
                 kPanel.y + 66 + slide + rise * (1.0f - title_in), 36, kInk);
        list.pop_opacity();

        const int first = index == kAbout ? 3 : 1;
        for (int i = 0; i < category.count; ++i)
        {
            const float in = tween::stagger(clock, first + i, 0.05f, 0.32f);
            Rect r = row_rect(index, i);
            r.y += slide + rise * (1.0f - in);
            list.push_opacity(in);
            draw_row(list, model, category.rows[i], r,
                     current && column_ == Column::rows && i == row_[index]);
            list.pop_opacity();
        }

        const float extra_in =
            tween::stagger(clock, index == kAbout ? 1 : category.count + 1, 0.05f, 0.36f);
        const float y = slide + rise * (1.0f - extra_in);
        list.push_opacity(extra_in);
        switch (index)
        {
        case 0:
            draw_level(list, y);
            break;
        case 1:
            draw_device_card(list, y);
            break;
        case 2:
            draw_account_card(list, y);
            break;
        case 3:
            draw_motion_card(list, y);
            break;
        case 4:
            draw_facts(list, model, y);
            break;
        default:
            draw_quit_card(list, y);
            break;
        }
        list.pop_opacity();
        list.pop_opacity();
    }

    void draw_row(gfx::DrawList &list, const Model &model, const Row &row, const Rect &r,
                  bool focused) const
    {
        const ui::Fonts &fonts = context_.fonts;
        if (!focused)
            list.rounded_rect(r, kRadius, kInk.with_alpha(0.035f));
        const bool danger = row.kind == Kind::action && (row.field == Field::forget ||
                                                         row.field == Field::quit);
        ui::text(list, focused ? fonts.semibold : fonts.regular, row.label, r.x + kRowPad,
                 centred(r.cy(), 28), 28,
                 (danger ? kDanger : kInk).with_alpha(focused ? 1.0f : 0.84f));
        switch (row.kind)
        {
        case Kind::slider:
            draw_slider(list, row.field, r, focused);
            break;
        case Kind::stepper:
            draw_stepper(list, row.field, r, focused);
            break;
        case Kind::toggle:
            draw_toggle(list, row.field, r, focused);
            break;
        case Kind::action:
            draw_action(list, r, focused);
            break;
        case Kind::name:
            draw_value(list, r, context_.settings.device_name, focused, true);
            break;
        case Kind::choose:
            draw_value(list, r, timezone_label(), focused, true);
            break;
        case Kind::info:
            draw_value(list, r, model.username.empty() ? "Not signed in" : model.username, focused,
                       false);
            break;
        }
    }

    void draw_slider(gfx::DrawList &list, Field field, const Rect &r, bool focused) const
    {
        const Control &state = control(field);
        const float right = control_right(r);
        const float cy = r.cy();
        const Rect track{right - 76.0f - kSliderW, cy - 4, kSliderW, 8};
        const float level = tween::clamp01(state.level.value / 10.0f);
        const float x = track.x + track.w * level;
        list.rounded_rect(track, 4, kInk.with_alpha(0.16f));
        if (level > 0.004f)
            list.gradient_rect_h({track.x, track.y, std::max(8.0f, track.w * level), track.h}, 4,
                                 gfx::mix(kAccent, kSurface, 0.45f), kAccent);
        for (int i = 0; i <= 10; ++i)
        {
            const float tx = track.x + track.w * as_float(i) / 10.0f;
            const bool lit = as_float(i) <= state.level.value + 0.05f;
            list.circle(tx, cy + 17, 1.6f, lit ? kAccent.with_alpha(0.7f) : kInk.with_alpha(0.22f));
        }
        if (focused)
        {
            const float size = 14.0f + 3.0f * state.press.value;
            list.glow({x - size, cy - size, size * 2, size * 2}, size, 16,
                      kAccent.with_alpha(0.55f));
            list.circle(x, cy, size, kInk);
            list.circle(x, cy, 5, kAccent);
        }
        else
        {
            list.circle(x, cy, 10, kInk.with_alpha(0.9f));
        }
        char text[8];
        std::snprintf(text, sizeof(text), "%d", state.seen);
        ui::text(list, context_.fonts.mono, text, right, centred(cy, 30), 30,
                 focused ? kInk : kInk.with_alpha(0.8f), gfx::Align::right);
    }

    void draw_toggle(gfx::DrawList &list, Field field, const Rect &r, bool focused) const
    {
        const Control &state = control(field);
        const float right = control_right(r);
        const Rect pill{right - kToggleW, r.cy() - kToggleH * 0.5f, kToggleW, kToggleH};
        const float on = tween::clamp01(state.level.value);
        const float stretch = 7.0f * state.press.value;
        const float x = pill.x + 22.0f + (kToggleW - 44.0f) * state.thumb.value;
        if (focused)
            list.glow(pill, kToggleH * 0.5f, 12, kAccent.with_alpha(0.18f + 0.3f * on));
        list.bordered_rect(pill, kToggleH * 0.5f, gfx::mix(Color::rgb(0x2f4a3c), kAccent, on), 1.5f,
                           gfx::mix(kInk.with_alpha(0.3f), kAccent, on));
        const Rect thumb{x - 17 - stretch * 0.5f, pill.cy() - 17, 34 + stretch, 34};
        list.shadow({thumb.x, thumb.y + 2, thumb.w, thumb.h}, 17, 8, Color::rgb(0x000000, 0.45f));
        list.rounded_rect(thumb, 17, gfx::mix(kInk, kDeep, on));
        const ui::Fonts &fonts = context_.fonts;
        const float alpha = focused ? 0.95f : 0.66f;
        const float baseline = centred(r.cy(), 24);
        ui::text(list, fonts.regular, "On", pill.x - 20, baseline, 24,
                 kInk.with_alpha(alpha * tween::clamp01(on * 2.0f - 1.0f)), gfx::Align::right);
        ui::text(list, fonts.regular, "Off", pill.x - 20, baseline, 24,
                 kInk.with_alpha(alpha * tween::clamp01(1.0f - on * 2.0f)), gfx::Align::right);
    }

    void draw_changing(gfx::DrawList &list, const ui::FontRef &font, const Control &state,
                       const char *before, const char *now, float x, float baseline, float size,
                       Color color, gfx::Align align) const
    {
        if (!state.change.running)
        {
            ui::text(list, font, now, x, baseline, size, color, align);
            return;
        }
        const float t = state.change.progress();
        const float reach = 30.0f * state.direction * travel();
        ui::text(list, font, before, x - reach * tween::cubic_in(tween::clamp01(t * 2.2f)),
                 baseline, size, color.with_alpha(1.0f - tween::smoothstep(t * 2.2f)), align);
        const float arrive = tween::clamp01((t - 0.2f) / 0.8f);
        ui::text(list, font, now, x + reach * (1.0f - tween::quint_out(arrive)), baseline, size,
                 color.with_alpha(tween::smoothstep(arrive)), align);
    }

    void draw_stepper(gfx::DrawList &list, Field field, const Rect &r, bool focused) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const Control &state = control(field);
        const float right = control_right(r);
        const float cx = right - kStepperW * 0.5f;
        const float cy = r.cy();
        if (focused)
            list.rounded_rect({cx - kStepperW * 0.5f + 34, cy - 24, kStepperW - 68, 48}, 14,
                              kDeep.with_alpha(0.35f));
        const char *const *names = field == Field::resolution  ? kResolutions
                                   : field == Field::song_menu ? kSongMenus
                                                               : kQualities;
        draw_changing(list, fonts.semibold, state, names[std::clamp(state.previous, 0, limit(field))],
                      names[std::clamp(state.seen, 0, limit(field))], cx, centred(cy, 28), 28,
                      focused ? kInk : kInk.with_alpha(0.84f), gfx::Align::center);
        const float nudge = 7.0f * state.press.value * travel();
        const float rest = focused ? 1.0f : 0.5f;
        const float left_alpha = state.seen > 0 ? rest : 0.16f;
        const float right_alpha = state.seen < limit(field) ? rest : 0.16f;
        const Color tint = focused ? kAccent : kInk;
        ui::text(list, fonts.mono, "\xE2\x97\x80",
                 cx - kStepperW * 0.5f + 12 - (state.direction < 0.0f ? nudge : 0.0f),
                 centred(cy, 32) - 1, 32, tint.with_alpha(left_alpha), gfx::Align::center);
        ui::text(list, fonts.mono, "\xE2\x96\xB6",
                 cx + kStepperW * 0.5f - 12 + (state.direction > 0.0f ? nudge : 0.0f),
                 centred(cy, 32) - 1, 32, tint.with_alpha(right_alpha), gfx::Align::center);
    }

    void draw_action(gfx::DrawList &list, const Rect &r, bool focused) const
    {
        const float right = control_right(r);
        const float cy = r.cy();
        const Color ink = focused ? kDanger : kInk.with_alpha(0.6f);
        ui::text(list, context_.fonts.regular, "Asks before it does anything", right - 34,
                 centred(cy, 24), 24, kInk.with_alpha(focused ? 0.8f : 0.55f), gfx::Align::right);
        list.line(right - 12, cy - 9, right - 3, cy, 3.0f, ink);
        list.line(right - 12, cy + 9, right - 3, cy, 3.0f, ink);
    }

    // A value on the right of a row; with a chevron when confirm edits it.
    void draw_value(gfx::DrawList &list, const Rect &r, const std::string &text, bool focused,
                    bool editable) const
    {
        const float right = control_right(r);
        const float cy = r.cy();
        const float x = editable ? right - 34 : right;
        ui::text(list, context_.fonts.semibold, context_.fonts.semibold.font->fit(text, 28, 600), x,
                 centred(cy, 28), 28, focused ? kInk : kInk.with_alpha(0.84f), gfx::Align::right);
        if (editable)
        {
            const Color ink = focused ? kAccent : kInk.with_alpha(0.6f);
            list.line(right - 12, cy - 9, right - 3, cy, 3.0f, ink);
            list.line(right - 12, cy + 9, right - 3, cy, 3.0f, ink);
        }
    }

    Rect draw_card(gfx::DrawList &list, int index, float dy, const char *title) const
    {
        const Rect last = row_rect(index, kCategories[index].count - 1);
        const float top = last.y + last.h + 24.0f;
        const Rect card{last.x, top + dy, last.w, kFooterY - 24.0f - top};
        list.bordered_rect(card, kRadius, kDeep.with_alpha(0.32f), 1.5f, kInk.with_alpha(0.06f));
        ui::text(list, context_.fonts.semibold, title, card.x + kRowPad, card.y + 42, 16,
                 kInk.with_alpha(0.5f), gfx::Align::left, 3.5f);
        return card;
    }

    // Playback: the stream being asked for, and the level this console adds.
    void draw_level(gfx::DrawList &list, float dy) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const Rect card = draw_card(list, 0, dy, "WHAT YOU HEAR");
        const Control &quality = control(Field::quality);
        draw_changing(list, fonts.mono, quality, kQualities[std::clamp(quality.previous, 0, 2)],
                      kQualities[std::clamp(quality.seen, 0, 2)], card.x + kRowPad, card.y + 100,
                      40, kInk, gfx::Align::left);
        ui::text(list, fonts.regular, kQualityNames[std::clamp(quality.seen, 0, 2)],
                 card.x + kRowPad, card.y + 142, 22, kInk.with_alpha(0.6f));

        // The console's level as a meter in decibels (perceptual curve).
        constexpr int kSegments = 32;
        const float x0 = card.x + 420;
        const float x1 = card.x + card.w - 180;
        const float pitch = (x1 - x0) / as_float(kSegments);
        const float cy = card.y + 110;
        const Control &volume = control(Field::volume);
        const float step = std::max(volume.level.value, 0.0f) / 10.0f;
        const float db = step > 0.001f ? 40.0f * std::log10(step) : -96.0f;
        const float lit = tween::clamp01(1.0f + db / 48.0f) * as_float(kSegments);
        for (int s = 0; s < kSegments; ++s)
        {
            const float amount = tween::clamp01(lit - as_float(s));
            const Color on = gfx::mix(gfx::mix(kAccent, kSurface, 0.4f), kAccent,
                                      as_float(s) / as_float(kSegments));
            list.rounded_rect({x0 + as_float(s) * pitch, cy - 10, pitch - 4, 20}, 3,
                              gfx::mix(kInk.with_alpha(0.1f), on, amount));
        }
        const float right = card.x + card.w - kRowPad;
        if (volume.seen == 0)
        {
            ui::text(list, fonts.regular, "Muted", right, centred(cy, 22), 22, kInk.with_alpha(0.6f),
                     gfx::Align::right);
        }
        else
        {
            char text[16];
            std::snprintf(text, sizeof(text), "%.0f",
                          static_cast<double>(40.0f * std::log10(as_float(volume.seen) / 10.0f)));
            ui::text(list, fonts.regular, "dB", right, centred(cy, 22), 22, kInk.with_alpha(0.6f),
                     gfx::Align::right);
            ui::text(list, fonts.mono, text, right - 36, centred(cy, 22), 22,
                     kInk.with_alpha(0.9f), gfx::Align::right);
        }
        ui::text(list, fonts.regular,
                 context_.settings.phone_volume ? "Your phone's volume applies on top"
                                                : "Your phone's volume buttons are ignored",
                 x0, card.y + 160, 22, kInk.with_alpha(0.6f));
    }

    // Device: how the name looks in the phone's list.
    void draw_device_card(gfx::DrawList &list, float dy) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const Rect card = draw_card(list, 1, dy, "IN YOUR PHONE'S DEVICE LIST");
        const Rect item{card.x + kRowPad, card.y + 80, 620, 96};
        list.rounded_rect(item, 16, kInk.with_alpha(0.06f));
        // A speaker glyph, then the name and "Spotify Connect".
        list.circle(item.x + 48, item.cy(), 26, kAccent.with_alpha(0.18f));
        list.ring(item.x + 48, item.cy(), 14, 3, kAccent);
        list.circle(item.x + 48, item.cy(), 5, kAccent);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(context_.settings.device_name, 30, 480),
                 item.x + 96, item.cy() - 2, 30, kInk);
        ui::text(list, fonts.regular, "Spotify Connect", item.x + 96, item.cy() + 30, 22,
                 kInk.with_alpha(0.6f));
    }

    void draw_account_card(gfx::DrawList &list, float dy) const
    {
        const Rect card = draw_card(list, 2, dy, "WHERE THE LOGIN LIVES");
        ui::paragraph(list, context_.fonts.regular,
                      "Your saved login is stored on this console, next to the app. Forgetting it "
                      "deletes it; Spotify on your phone keeps working as before.",
                      card.x + kRowPad, card.y + 96, 25, card.w - 2 * kRowPad, 38,
                      kInk.with_alpha(0.75f), 3);
    }

    void draw_motion_card(gfx::DrawList &list, float dy) const
    {
        const Rect card = draw_card(list, 3, dy, "WITH REDUCE MOTION ON");
        const float on = tween::clamp01(control(Field::reduce_motion).level.value);
        for (int i = 0; i < 4; ++i)
        {
            const float y = card.y + 106 + as_float(i) * 50;
            list.circle(card.x + kRowPad + 6, y - 9, 5,
                        gfx::mix(kInk.with_alpha(0.3f), kAccent, on));
            ui::text(list, context_.fonts.regular, kMotionNotes[i], card.x + kRowPad + 30, y, 25,
                     kInk.with_alpha(0.68f + 0.26f * on));
        }
    }

    void draw_facts(gfx::DrawList &list, const Model &model, float dy) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const std::string version = "Version " + model.about.version;
        const char *labels[] = {"APPLICATION", "ENGINE", "THIS CONSOLE", "LICENCES"};
        const std::string values[] = {"Spot5", "cspot " + model.about.engine,
                                      model.about.address.empty() ? "Unknown" : model.about.address,
                                      "GPL-3.0"};
        const char *notes[] = {version.c_str(), "Spotify Connect, login5, verified TLS",
                               "On your local network",
                               "mbedTLS: Apache-2.0  \xC2\xB7  libwebp: BSD  \xC2\xB7  fonts: OFL"};
        const Rect area = row_rect(0, 0);
        const float w = (area.w - kTileGap) * 0.5f;
        for (int i = 0; i < 4; ++i)
        {
            const Rect tile{area.x + as_float(i % 2) * (w + kTileGap),
                            kRowsY + as_float(i / 2) * (kTileH + kTileGap) + dy, w, kTileH};
            list.bordered_rect(tile, kRadius, kDeep.with_alpha(0.32f), 1.5f,
                               kInk.with_alpha(0.06f));
            ui::text(list, fonts.semibold, labels[i], tile.x + kRowPad, tile.y + 40, 16,
                     kInk.with_alpha(0.5f), gfx::Align::left, 3.5f);
            // The application's name follows its mark.
            const float name_x = tile.x + kRowPad + (i == 0 ? 56.0f : 0.0f);
            if (i == 0)
                draw_logo(list, context_, {tile.x + kRowPad - 4, tile.y + 46, 50, 50});
            ui::text(list, i == 2 ? fonts.mono : fonts.semibold,
                     fonts.semibold.font->fit(values[i], 34, w - 2 * kRowPad), name_x,
                     tile.y + 87, 34, kInk);
            ui::text(list, fonts.regular, fonts.regular.font->fit(notes[i], 22, w - 2 * kRowPad),
                     tile.x + kRowPad, tile.y + 124, 22, kInk.with_alpha(0.6f));
        }
    }

    void draw_quit_card(gfx::DrawList &list, float dy) const
    {
        const Rect card = draw_card(list, kQuit, dy, "CLOSING");
        ui::paragraph(list, context_.fonts.regular,
                      "Quitting stops the music on this console. Your phone keeps the queue, so "
                      "you can pick another device and carry on.",
                      card.x + kRowPad, card.y + 96, 25, card.w - 2 * kRowPad, 38,
                      kInk.with_alpha(0.75f), 3);
    }

    void draw_footer(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 7, 0.07f, 0.5f);
        const float x = kPanel.x + kPanelPad + kRowPad;
        const float cy = kFooterY + 42;
        const float width = kPanel.w - 2 * (kPanelPad + kRowPad) - 44;
        list.push_opacity(in);
        list.ring(x + 13, cy, 13, 2, kAccent.with_alpha(0.8f));
        list.circle(x + 13, cy - 5.5f, 1.8f, kAccent);
        list.line(x + 13, cy - 0.5f, x + 13, cy + 6, 2.4f, kAccent);
        const float t = footer_.running ? footer_.progress() : 1.0f;
        if (t < 1.0f)
            ui::text(list, fonts.regular, fonts.regular.font->fit(footer_previous_, 24, width),
                     x + 44, centred(cy, 24), 24,
                     kInk.with_alpha(0.74f * (1.0f - tween::smoothstep(t * 2.0f))));
        const float arrive = tween::smoothstep((t - 0.45f) / 0.55f);
        ui::text(list, fonts.regular, fonts.regular.font->fit(footer_shown_, 24, width),
                 x + 44 + 12.0f * (1.0f - arrive) * travel(), centred(cy, 24), 24,
                 kInk.with_alpha(0.74f * arrive));
        list.pop_opacity();
    }

    static gfx::DrawList &hint_list(Frame &frame, HintMode mode)
    {
        return mode == HintMode::dialog || mode == HintMode::keyboard || mode == HintMode::picker ? frame.overlay
                                                                                                 : frame.scene;
    }

    int build_hints(HintMode mode, ui::Hint *hints) const
    {
        const ui::Hint category{ui::Button::l2, "Category", ui::Button::r2};
        const ui::Hint back{ui::Button::circle, "Back"};
        int count = 0;
        switch (mode)
        {
        case HintMode::rail:
            hints[count++] = {ui::Button::dpad, "Choose"};
            hints[count++] = {ui::Button::cross, "Open"};
            hints[count++] = {ui::Button::circle, "Close"};
            break;
        case HintMode::slider:
            hints[count++] = {ui::Button::dpad, "Adjust"};
            hints[count++] = back;
            break;
        case HintMode::stepper:
            hints[count++] = {ui::Button::dpad, "Change"};
            hints[count++] = back;
            break;
        case HintMode::toggle_off:
            hints[count++] = {ui::Button::cross, "Turn on"};
            hints[count++] = back;
            break;
        case HintMode::toggle_on:
            hints[count++] = {ui::Button::cross, "Turn off"};
            hints[count++] = back;
            break;
        case HintMode::action:
            hints[count++] = {ui::Button::cross, "Choose"};
            hints[count++] = back;
            break;
        case HintMode::name:
            hints[count++] = {ui::Button::cross, "Rename"};
            hints[count++] = back;
            break;
        case HintMode::choose:
            hints[count++] = {ui::Button::cross, "Change"};
            hints[count++] = back;
            break;
        case HintMode::info:
            hints[count++] = back;
            break;
        case HintMode::picker:
            hints[count++] = {ui::Button::cross, "Choose"};
            hints[count++] = {ui::Button::l1, "Page", ui::Button::r1};
            hints[count++] = {ui::Button::circle, "Cancel"};
            break;
        case HintMode::dialog:
            hints[count++] = {ui::Button::cross, "Choose"};
            hints[count++] = {ui::Button::circle, "Cancel"};
            break;
        case HintMode::keyboard:
            hints[count++] = {ui::Button::square, "Delete"};
            hints[count++] = {ui::Button::triangle, "Space"};
            hints[count++] = {ui::Button::circle, "Cancel"};
            break;
        }
        if (mode != HintMode::dialog && mode != HintMode::keyboard && mode != HintMode::picker)
            hints[count++] = category;
        return count;
    }

    // Shared hints stay put; only the ones that differ cross-fade (settings.cpp).
    void draw_hints(Frame &frame) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle glyphs = style::hint_style();
        const ui::HintLayout layout = style::hint_layout();
        const float in = tween::stagger(age_, 9, 0.07f, 0.5f);
        gfx::DrawList &list = hint_list(frame, hints_shown_);
        gfx::DrawList &old_list = hint_list(frame, hints_previous_);
        ui::Hint now[5];
        ui::Hint before[5];
        const int now_count = build_hints(hints_shown_, now);
        const int before_count = hints_.running ? build_hints(hints_previous_, before) : 0;

        int shared = 0;
        if (hints_.running && &list == &old_list)
        {
            while (shared < std::min(now_count, before_count))
            {
                const ui::Hint &a = now[now_count - 1 - shared];
                const ui::Hint &b = before[before_count - 1 - shared];
                if (a.button != b.button || a.second != b.second || std::strcmp(a.label, b.label))
                    break;
                ++shared;
            }
        }
        const int kept = hints_.running ? shared : now_count;
        float x = gfx::kVirtualWidth - kMargin;
        list.push_opacity(in);
        const float kept_width =
            ui::draw_hints(list, fonts, glyphs, now + (now_count - kept), kept, x, true, layout);
        list.pop_opacity();
        if (!hints_.running)
            return;
        if (kept > 0)
            x -= kept_width + layout.item_gap;
        const float t = hints_.progress();
        old_list.push_opacity(in * (1.0f - tween::smoothstep(t * 2.0f)));
        ui::draw_hints(old_list, fonts, glyphs, before, before_count - shared, x, true, layout);
        old_list.pop_opacity();
        list.push_opacity(in * tween::smoothstep(t * 2.0f - 1.0f));
        ui::draw_hints(list, fonts, glyphs, now, now_count - shared, x, true, layout);
        list.pop_opacity();
    }

    static Rect dialog_button(float position)
    {
        constexpr float kPad = 48.0f;
        constexpr float kGap = 24.0f;
        const float w = (kDialog.w - 2 * kPad - kGap) * 0.5f;
        return {kDialog.x + kPad + position * (w + kGap), kDialog.y + kDialog.h - kPad - 68, w, 68};
    }

    void draw_dialog(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = dialog_.value;
        // What is being asked stays readable while the dialog leaves.
        const Ask shown = ask_ != Ask::none ? ask_ : last_ask_;
        const char *title = shown == Ask::forget ? "Forget this login?"
                            : shown == Ask::clear ? "Clear the cache?"
                            : shown == Ask::reset ? "Reset all settings?"
                                                  : "Quit Spot5?";
        const char *body =
            shown == Ask::forget
                ? "The saved login is deleted from this console. To play here again you will need "
                  "to pair it with your phone."
            : shown == Ask::clear
                ? "Saved covers, Home and Library are deleted and download again. Your login and "
                  "settings stay."
            : shown == Ask::reset
                ? "Every option on these pages goes back to how it was when the app first started, "
                  "time zone included."
                : "The music stops and the app closes. You can also close it any time with the PS "
                  "button.";
        const char *yes = shown == Ask::forget  ? "Forget"
                          : shown == Ask::clear ? "Clear"
                          : shown == Ask::reset ? "Reset"
                                                : "Quit";
        list.push_opacity(tween::clamp01(t * 1.5f));
        list.push_transform(1.0f - 0.06f * (1.0f - t) * travel(), 960, 540, 0,
                            26.0f * (1.0f - t) * travel());
        list.shadow({kDialog.x, kDialog.y + 24, kDialog.w, kDialog.h}, kPanelRadius, 60,
                    Color::rgb(0x000000, 0.55f));
        list.glass(glass, kDialog, kPanelRadius, Color::rgb(0xffffff));
        list.gradient_rect(kDialog, kPanelRadius,
                           gfx::mix(kSurface, kAccent, 0.08f).with_alpha(0.5f),
                           kDeep.with_alpha(0.62f));
        list.bordered_rect(kDialog, kPanelRadius, kClear, 1.5f, kInk.with_alpha(0.24f));

        const float x = kDialog.x + 48;
        list.ring(x + 24, kDialog.y + 78, 24, 3, kDanger);
        list.line(x + 24, kDialog.y + 67, x + 24, kDialog.y + 81, 3.5f, kDanger);
        list.circle(x + 24, kDialog.y + 90, 2.4f, kDanger);
        ui::text(list, fonts.semibold, title, x + 68, kDialog.y + 92, 40, kInk);
        ui::paragraph(list, fonts.regular, body, x, kDialog.y + 164, 26, kDialog.w - 96, 38,
                      kInk.with_alpha(0.86f), 2);

        const float position = choice_position_.value;
        Rect lit = dialog_button(position);
        lit.x += ui::shake(refuse_.value, clock_, 8.0f, 9.0f);
        const Color fill = gfx::mix(kInk, kDanger, tween::clamp01(position));
        list.glow(lit, 34, 16, fill.with_alpha(0.3f));
        list.rounded_rect(lit, 34, fill);
        const char *names[] = {"Cancel", yes};
        for (int i = 0; i < 2; ++i)
        {
            const Rect r = dialog_button(as_float(i));
            const bool focused = i == choice_;
            if (!focused)
                list.bordered_rect(r, 34, kInk.with_alpha(0.06f), 1.5f, kInk.with_alpha(0.2f));
            ui::text(list, fonts.semibold, names[i], r.cx(), centred(r.cy(), 28), 28,
                     focused ? kDeep : (i == 1 ? kDanger : kInk.with_alpha(0.9f)),
                     gfx::Align::center);
        }
        list.pop_transform();
        list.pop_opacity();
    }

    // The rename sheet: the name as it is being typed, and the kit keyboard.
    void draw_sheet(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = sheet_.value;
        list.push_opacity(tween::clamp01(t * 1.5f));
        list.push_transform(1.0f - 0.04f * (1.0f - t) * travel(), 960, 540, 0,
                            30.0f * (1.0f - t) * travel());
        list.shadow({kSheet.x, kSheet.y + 24, kSheet.w, kSheet.h}, kPanelRadius, 60,
                    Color::rgb(0x000000, 0.55f));
        list.glass(glass, kSheet, kPanelRadius, Color::rgb(0xffffff));
        list.gradient_rect(kSheet, kPanelRadius, gfx::mix(kSurface, kAccent, 0.06f).with_alpha(0.55f),
                           kDeep.with_alpha(0.7f));
        list.bordered_rect(kSheet, kPanelRadius, kClear, 1.5f, kInk.with_alpha(0.24f));
        ui::text(list, fonts.semibold, "DEVICE NAME", kSheet.x + 60, kSheet.y + 70, 18, kAccent,
                 gfx::Align::left, 4.0f);

        // The field: text, a blinking caret, and how much room is left.
        const Rect field{kSheet.x + 60, kSheet.y + 96, kSheet.w - 120, 84};
        list.rounded_rect(field, 18, kDeep.with_alpha(0.5f));
        list.bordered_rect(field, 18, kClear, 2.0f, kAccent.with_alpha(0.6f));
        const float w = ui::text(list, fonts.display, name_edit_, field.x + 28, field.cy() + 16, 44,
                                 kInk);
        const bool caret = context_.settings.reduced_motion || std::fmod(clock_, 1.0f) < 0.55f;
        if (caret)
            list.rounded_rect({field.x + 32 + w, field.cy() - 22, 3, 44}, 1.5f, kAccent);
        char count[16];
        std::snprintf(count, sizeof(count), "%zu / %d", name_edit_.size(), kNameLimit);
        ui::text(list, fonts.mono, count, field.x + field.w - 24, field.cy() + 8, 22,
                 kInk.with_alpha(0.55f), gfx::Align::right);

        ui::Canvas canvas{list, fonts, glass, clock_};
        keyboard_.draw(canvas);
        list.pop_transform();
        list.pop_opacity();
    }

    // The time zone list: Automatic first, then the cities by offset. The
    // focus glides; the list scrolls to keep it in the middle.
    void draw_picker(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float t = picker_.value;
        list.push_opacity(tween::clamp01(t * 1.5f));
        list.push_transform(1.0f - 0.04f * (1.0f - t) * travel(), 960, 540, 0, 30.0f * (1.0f - t) * travel());
        list.shadow({kPicker.x, kPicker.y + 24, kPicker.w, kPicker.h}, kPanelRadius, 60, Color::rgb(0x000000, 0.55f));
        list.glass(glass, kPicker, kPanelRadius, Color::rgb(0xffffff));
        list.gradient_rect(kPicker, kPanelRadius, gfx::mix(kSurface, kAccent, 0.06f).with_alpha(0.55f),
                           kDeep.with_alpha(0.72f));
        list.bordered_rect(kPicker, kPanelRadius, kClear, 1.5f, kInk.with_alpha(0.24f));
        ui::text(list, fonts.semibold, "TIME ZONE", kPicker.x + 56, kPicker.y + 70, 18, kAccent, gfx::Align::left,
                 4.0f);
        ui::text(list, fonts.regular, "Home's morning and evening shelves follow it.", kPicker.x + 56,
                 kPicker.y + 112, 24, kInk.with_alpha(0.7f));

        const Rect area{kPicker.x + 32, kPicker.y + kPickerTop, kPicker.w - 64, kPicker.h - kPickerTop - 40};
        list.push_clip(area);
        const float top = area.y - picker_scroll_.value;
        const Rect lit{area.x, top + picker_focus_.value + 4, area.w, kPickerRowH - 8};
        list.rounded_rect(lit, 16, gfx::mix(kSurface, kAccent, 0.2f));
        list.bordered_rect(lit, 16, kClear, 2.0f, kAccent);
        const int chosen = zone_index(context_.settings.timezone) + 1;
        for (int i = 0; i <= kTimeZoneCount; ++i)
        {
            const float y = top + as_float(i) * kPickerRowH;
            if (y + kPickerRowH < area.y || y > area.y + area.h)
                continue;
            const float cy = y + kPickerRowH * 0.5f;
            const bool focused = i == picker_index_;
            const std::string name = i == 0 ? "Automatic" : kTimeZones[i - 1].city;
            const std::string offset = i == 0 ? utc_label(context_.console_utc_minutes) + "  \xC2\xB7  this console"
                                              : utc_label(kTimeZones[i - 1].minutes);
            if (i == chosen)
                list.circle(area.x + 28, cy, 6, kAccent);
            ui::text(list, focused ? fonts.semibold : fonts.regular, name, area.x + 52, centred(cy, 28), 28,
                     kInk.with_alpha(focused ? 1.0f : 0.8f));
            ui::text(list, fonts.mono, offset, area.x + area.w - 24, centred(cy, 22), 22,
                     kInk.with_alpha(focused ? 0.9f : 0.55f), gfx::Align::right);
        }
        list.pop_clip();
        list.pop_transform();
        list.pop_opacity();
    }

    void draw_toast(gfx::DrawList &list) const
    {
        const float t = toast_.value;
        if (t <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float w = 84.0f + fonts.semibold.measure(toast_message_, 24);
        const Rect pill{gfx::kVirtualWidth - kMargin - w, 92.0f - 70.0f * (1.0f - t) * travel(), w,
                        60};
        list.push_opacity(tween::clamp01(t * 1.3f));
        list.shadow({pill.x, pill.y + 12, pill.w, pill.h}, 30, 30, Color::rgb(0x000000, 0.5f));
        list.bordered_rect(pill, 30, gfx::mix(kSurface, kAccent, 0.1f).with_alpha(0.97f), 1.5f,
                           kAccent.with_alpha(0.55f));
        const float cx = pill.x + 32;
        const float cy = pill.cy();
        list.circle(cx, cy, 17, kAccent);
        const float stroke = tween::clamp01(toast_age_ / 0.28f);
        const float first = tween::clamp01(stroke / 0.4f);
        const float second = tween::clamp01((stroke - 0.4f) / 0.6f);
        list.line(cx - 7.5f, cy + 0.5f, cx - 7.5f + 5.0f * first, cy + 0.5f + 5.0f * first, 3.2f,
                  kDeep);
        if (second > 0.0f)
            list.line(cx - 2.5f, cy + 5.5f, cx - 2.5f + 10.0f * second, cy + 5.5f - 11.0f * second,
                      3.2f, kDeep);
        ui::text(list, fonts.semibold, toast_message_, pill.x + 62, centred(cy, 24), 24, kInk);
        list.pop_opacity();
    }

    Context &context_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float drift_ = 0.0f;
    bool close_ = false;

    Column column_ = Column::rail;
    int category_ = 0;
    std::array<int, kCategoryCount> row_{};
    ui::SpringRect focus_;
    tween::Spring marker_;
    ui::Pulse refuse_;

    int previous_ = 0;
    float change_direction_ = 1.0f;
    tween::Timer leave_;
    float content_age_ = 0.0f;

    std::array<Control, kFieldCount> controls_;

    const char *footer_shown_ = "";
    const char *footer_previous_ = "";
    tween::Timer footer_;
    HintMode hints_shown_ = HintMode::rail;
    HintMode hints_previous_ = HintMode::rail;
    tween::Timer hints_;

    Ask ask_ = Ask::none;
    Ask last_ask_ = Ask::quit;
    tween::Spring dialog_;
    int choice_ = 0; // 0: the safe choice
    tween::Spring choice_position_;

    bool sheet_open_ = false;
    tween::Spring sheet_;
    bool picker_open_ = false;
    int picker_index_ = 0; // 0: Automatic, then kTimeZones[index - 1]
    tween::Spring picker_;
    tween::Spring picker_focus_;  // the focus row's top, in list pixels
    tween::Spring picker_scroll_; // how far the list has scrolled
    std::string name_edit_;
    ui::Keyboard keyboard_;

    float save_wait_ = 0.0f;
    float toast_left_ = 0.0f;
    float toast_age_ = 10.0f;
    const char *pending_message_ = "Saved";
    const char *toast_message_ = "Saved";
    tween::Spring toast_;
};

} // namespace

std::unique_ptr<Screen> make_control_room(Context &context)
{
    return std::make_unique<ControlRoom>(context);
}

bool control_room_wants_close(const Screen &room)
{
    return static_cast<const ControlRoom &>(room).wants_close();
}

} // namespace screens
