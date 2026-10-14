// spot5 - Now Playing: what plays on this console, and what comes next.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Adapted from ps5-homebrew-ui src/concepts/player.cpp (GPL-3.0-or-later,
// BlackBearReloaded). Kept: layout numbers, the pseudo-spectrum, artwork as
// play state, slide-through track change, the focus ring that glides between
// the transport and the scrubber's thumb, the spring scrubber with its time
// bubble, the frosted queue that slides away while the layout re-centres.
// Changed: data comes from the Model; buttons, seeks and queue picks go to
// the session (seeks are sent once the user stops stepping); no shuffle /
// repeat / favourite buttons (cspot has no such state to show yet).

#include "screens/now_playing.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <set>
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

// ---- layout (player.cpp) ----
constexpr float kMargin = 96.0f;
constexpr float kWideMargin = 218.0f; // side margin when the queue is hidden
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

constexpr float kQueueX = 1404.0f;
constexpr float kQueueY = 96.0f;
constexpr float kQueueW = 420.0f;
constexpr float kQueueH = 856.0f;
constexpr float kQueueRadius = 36.0f;
constexpr float kQueueListTop = 104.0f; // from the panel's top to the first row
constexpr float kQueueRow = 72.0f;
constexpr float kQueueThumb = 46.0f;

constexpr float kSeekStep = 5.0f;    // left / right on the scrubber
constexpr float kSeekJump = 15.0f;   // L2 / R2 from anywhere
constexpr float kSeekSendAfter = 0.35f; // seconds of no stepping before a seek is sent
constexpr float kSeekTrust = 2.0f;  // the session's position counts again once this close

enum class Zone
{
    scrubber,
    transport,
    queue,
};
enum Control
{
    kPrevious,
    kPlay,
    kNext,
    kControls,
};
constexpr float kControlOffset[kControls] = {-114.0f, 0.0f, 114.0f};


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
    std::uint32_t art = 0; // the real cover once loaded
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
        const bool reduced = context_.settings.reduced_motion;
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
                        model.track.has_palette ? model.track.palette
                                                : style::album_palette(model.track.album),
                        model.track.art};
            apply_palette(previous_.title.empty() && !swap_.running);
            // The cover may already be here (the track started while another
            // screen was showing): then it is simply visible.
            art_in_.snap(model.track.art ? 1.0f : 0.0f);
            travel_ = next_travel_;
            next_travel_ = 1.0f;
        }
        else if (model.track.art != current_.art)
        {
            // The cover arrived after the track: fade it in and ease the
            // colours from the name-based guess to the cover's own.
            current_.art = model.track.art;
            if (model.track.has_palette)
                current_.palette = model.track.palette;
            apply_palette(false);
            art_in_.snap(0.0f);
            art_in_.target = 1.0f;
        }
        art_in_.update(dt, context_.settings.reduced_motion ? 40.0f : 6.0f);
        duration_ = std::max(1.0f, static_cast<float>(model.track.duration_ms) / 1000.0f);
        playing_ = !model.paused;
        queue_ptr_ = model.queue.get();
        // While a seek is on its way, show where it goes, not where the
        // session still is.
        const float session = static_cast<float>(model.position_ms) / 1000.0f;
        if (seek_target_ >= 0.0f)
        {
            if (seek_send_in_ <= 0.0f && std::fabs(session - seek_target_) < kSeekTrust)
                seek_target_ = -1.0f;
            else
                seek_wait_ += dt;
            if (seek_wait_ > 4.0f) // never pinned for long if the session disagrees
                seek_target_ = -1.0f;
        }
        position_ = seek_target_ >= 0.0f ? seek_target_ : session;

        update_shortcuts(model, input, feedback);
        switch (zone_)
        {
        case Zone::scrubber:
            update_scrubber(input, feedback, commands);
            break;
        case Zone::transport:
            update_transport(input, feedback, commands);
            break;
        case Zone::queue:
            update_queue(input, feedback);
            break;
        }
        if (seek_send_in_ > 0.0f)
        {
            seek_send_in_ -= dt;
            if (seek_send_in_ <= 0.0f)
            {
                Request request;
                request.kind = Request::Kind::seek;
                request.ms = static_cast<std::uint32_t>(seek_target_ * 1000.0f);
                context_.requests.push_back(request);
            }
        }
        ask_for_details();
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
        queue_.target = queue_open_ && queue_ptr_ ? 1.0f : 0.0f;
        queue_.update(dt, reduced ? 40.0f : 11.0f);
        scrub_focus_.target = zone_ == Zone::scrubber ? 1.0f : 0.0f;
        scrub_focus_.update(dt, 16.0f);
        queue_focus_.target = zone_ == Zone::queue ? 1.0f : 0.0f;
        queue_focus_.update(dt, 16.0f);
        ring_.target(ring_target());
        ring_.update(dt, 18.0f);
        queue_row_.target = static_cast<float>(queue_row_index_);
        queue_row_.update(dt, 18.0f);
        // The list keeps the focused row in view; left alone it follows the
        // track that is playing.
        const int rows = queue_ptr_ ? static_cast<int>(queue_ptr_->tracks.size()) : 0;
        const float row = static_cast<float>(zone_ == Zone::queue ? queue_row_index_ : std::max(0, playing_row()));
        queue_scroll_.reveal(row * kQueueRow, (row + 1.0f) * kQueueRow, queue_view_height(), kQueueRow,
                             static_cast<float>(rows) * kQueueRow);
        queue_scroll_.update(dt, 12.0f);
        seek_.update(dt, 6.0f);
        seek_hold_ = std::max(0.0f, seek_hold_ - dt);
        bubble_.target = zone_ == Zone::scrubber || seek_hold_ > 0.0f ? 1.0f : 0.0f;
        bubble_.update(dt, 16.0f);
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
        if (queue_.value > 0.01f)
        {
            frame.glass = true;
            draw_queue(frame.overlay, frame.glass_texture);
        }
        draw_hints(frame.overlay);
    }

    bool uses_back() const
    {
        return zone_ != Zone::transport;
    }

  private:
    // Where the main block sits. With the queue open it keeps to the left;
    // when the queue slides away the block widens and re-centres.
    struct Layout
    {
        float x = 0.0f;  // left edge of the artwork, the scrubber and the header
        float w = 0.0f;  // width of the scrubber
        float cx = 0.0f; // centre of the transport row
        float column_x = 0.0f; // the text and the visualizer
        float column_w = 0.0f;
    };

    Layout layout() const
    {
        const float q = queue_.value;
        Layout l;
        l.x = tween::lerp(kWideMargin, kMargin, q);
        l.w = tween::lerp(gfx::kVirtualWidth - 2.0f * kWideMargin, kQueueX - 64.0f - kMargin, q);
        l.cx = l.x + l.w * 0.5f;
        l.column_x = l.x + kArt + kColumnGap;
        l.column_w = l.w - kArt - kColumnGap;
        return l;
    }

    float queue_view_height() const
    {
        return kQueueH - kQueueListTop - 20.0f;
    }

    int playing_row() const
    {
        return queue_ptr_ ? queue_ptr_->playing : -1;
    }

    float appear(int order) const { return tween::stagger(age_, order, 0.07f, 0.55f); }

    float slide(float in, float distance) const
    {
        return context_.settings.reduced_motion ? 0.0f : distance * (1.0f - in);
    }

    // The focus ring's target as a circle: x relative to the transport's
    // centre (it rides along when the layout re-centres), y the centre, w the
    // diameter. On the scrubber it locks onto the thumb.
    Rect ring_target() const
    {
        if (zone_ == Zone::scrubber)
        {
            const Layout l = layout();
            return {(position_ / duration_ - 0.5f) * l.w, kScrubY, 52.0f, 52.0f};
        }
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
        const Layout l = layout();
        const float pan = ui::pan_for_x(l.cx + kControlOffset[control]);
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

    void set_zone(Zone zone, ui::Feedback &feedback)
    {
        zone_ = zone;
        if (zone == Zone::queue)
            queue_row_index_ = std::max(0, playing_row()); // starts on the playing track
        feedback.play(audio::Cue::focus, zone == Zone::scrubber ? 1.1f : zone == Zone::queue ? 0.94f : 1.0f);
    }

    // Buttons that work from every zone.
    void update_shortcuts(const Model &model, const InputFrame &input, ui::Feedback &feedback)
    {
        // Options: the song menu, on the focused queue row or the playing song.
        if (input.is_pressed(Action::menu))
        {
            const int rows = queue_ptr_ ? static_cast<int>(queue_ptr_->tracks.size()) : 0;
            const int playing = playing_row();
            if (zone_ == Zone::queue && queue_row_index_ < rows)
            {
                const Rect row{kQueueX + 14,
                               kQueueY + kQueueListTop - queue_scroll_.offset() +
                                   static_cast<float>(queue_row_index_) * kQueueRow + 3,
                               kQueueW - 28, kQueueRow - 6};
                context_.song_pick = Context::SongPick{queue_ptr_->tracks[static_cast<std::size_t>(queue_row_index_)], row};
            }
            else if (model.has_track)
            {
                // The queue's row has the album and artist links; the track alone has none.
                TrackRow song;
                if (playing >= 0 && playing < rows)
                    song = queue_ptr_->tracks[static_cast<std::size_t>(playing)];
                else
                {
                    song.title = model.track.title;
                    song.artists = model.track.artist;
                    song.album = model.track.album;
                    song.duration_ms = model.track.duration_ms;
                    song.image = model.track.image_url;
                }
                const Layout l = layout();
                context_.song_pick = Context::SongPick{song, {l.column_x - 20, 230, l.column_w + 40, 140}};
            }
        }
        if (input.is_pressed(Action::west) && queue_ptr_)
        {
            queue_open_ = !queue_open_;
            feedback.play(queue_open_ ? audio::Cue::open : audio::Cue::modal_close, 1.0f, 0.4f);
            if (!queue_open_ && zone_ == Zone::queue)
                zone_ = Zone::transport; // the focus never stays on something hidden
        }
        if (!queue_ptr_ && zone_ == Zone::queue)
            zone_ = Zone::transport;
        if (input.is_pressed(Action::jump_prev))
            seek(-kSeekJump, false, feedback);
        if (input.is_pressed(Action::jump_next))
            seek(kSeekJump, false, feedback);
    }

    void update_transport(const InputFrame &input, ui::Feedback &feedback, std::vector<Command> &commands)
    {
        const Layout l = layout();
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int step = input.nav == Direction::right ? 1 : -1;
            const int next = button_ + step;
            if (next >= 0 && next < kControls)
            {
                button_ = next;
                feedback.play(audio::Cue::focus, 1.0f, ui::pan_for_x(l.cx + kControlOffset[next]));
            }
            else if (!input.nav_repeat)
            {
                // Past the last button lies the queue, when it is showing.
                if (step > 0 && queue_open_ && queue_ptr_)
                    set_zone(Zone::queue, feedback);
                else
                    refuse(feedback, static_cast<float>(step));
            }
            break;
        }
        case Direction::up:
            set_zone(Zone::scrubber, feedback);
            break;
        case Direction::down:
            if (queue_open_ && queue_ptr_)
                set_zone(Zone::queue, feedback);
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm))
            activate(button_, feedback, commands);
        if (input.is_pressed(Action::page_prev))
            activate(kPrevious, feedback, commands);
        if (input.is_pressed(Action::page_next))
            activate(kNext, feedback, commands);
    }

    void update_scrubber(const InputFrame &input, ui::Feedback &feedback, std::vector<Command> &commands)
    {
        switch (input.nav)
        {
        case Direction::left:
            seek(-kSeekStep, input.nav_repeat, feedback);
            break;
        case Direction::right:
            seek(kSeekStep, input.nav_repeat, feedback);
            break;
        case Direction::down:
            set_zone(Zone::transport, feedback);
            break;
        case Direction::up:
            if (!input.nav_repeat)
                refuse(feedback, 0.0f);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm))
            activate(kPlay, feedback, commands);
        if (input.is_pressed(Action::back))
        {
            zone_ = Zone::transport;
            feedback.play(audio::Cue::back);
        }
    }

    void update_queue(const InputFrame &input, ui::Feedback &feedback)
    {
        const int rows = static_cast<int>(queue_ptr_->tracks.size());
        switch (input.nav)
        {
        case Direction::up:
        case Direction::down:
        {
            const int next = queue_row_index_ + (input.nav == Direction::down ? 1 : -1);
            if (next >= 0 && next < rows)
            {
                queue_row_index_ = next;
                feedback.play(audio::Cue::focus, 1.08f - 0.012f * static_cast<float>(std::min(next, 20)),
                              ui::pan_for_x(kQueueX + kQueueW * 0.5f));
            }
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
            break;
        }
        case Direction::left:
            set_zone(Zone::transport, feedback);
            break;
        case Direction::right:
            if (!input.nav_repeat)
                refuse(feedback, 1.0f);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm) && queue_row_index_ < rows)
        {
            Request request;
            request.kind = Request::Kind::jump;
            request.index = queue_ptr_->first + queue_row_index_;
            context_.requests.push_back(request);
            next_travel_ = queue_row_index_ >= playing_row() ? 1.0f : -1.0f;
            feedback.play(audio::Cue::select, 1.0f, ui::pan_for_x(kQueueX + kQueueW * 0.5f));
        }
        if (input.is_pressed(Action::back))
        {
            zone_ = Zone::transport;
            feedback.play(audio::Cue::back);
        }
    }

    // Seeks move the thumb at once; the session is told once stepping stops.
    void seek(float delta, bool repeat, ui::Feedback &feedback)
    {
        const float target = std::clamp(position_ + delta, 0.0f, duration_ - 1.0f);
        if (std::fabs(target - position_) < 0.5f)
        {
            if (!repeat)
                refuse(feedback, delta > 0.0f ? 1.0f : -1.0f);
            return;
        }
        position_ = target;
        seek_target_ = target;
        seek_wait_ = 0.0f;
        seek_send_in_ = kSeekSendAfter;
        const float fraction = position_ / duration_;
        const Layout l = layout();
        // The tick rises as the track fills and sits where the thumb is.
        feedback.play(audio::Cue::slider, 0.85f + 0.4f * fraction, ui::pan_for_x(l.x + fraction * l.w));
        seek_.trigger();
        seek_hold_ = 1.1f;
    }

    // Queue rows on screen without details yet: ask once.
    void ask_for_details()
    {
        if (!queue_ptr_ || queue_.value < 0.01f)
            return;
        const int first = std::max(0, static_cast<int>(queue_scroll_.offset() / kQueueRow) - 1);
        const int last = std::min(static_cast<int>(queue_ptr_->tracks.size()), first + 16);
        Request request;
        request.kind = Request::Kind::describe;
        for (int i = first; i < last; ++i)
        {
            const TrackRow &row = queue_ptr_->tracks[static_cast<std::size_t>(i)];
            if (row.title.empty() && !row.uri.empty() && asked_.insert(row.uri).second)
                request.uris.push_back(row.uri);
        }
        if (!request.uris.empty())
            context_.requests.push_back(request);
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
        const Layout l = layout();
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(0);
        list.push_opacity(in);
        const float y = 116.0f - slide(in, 12.0f);
        const float w = ui::text(list, fonts.display, "Playing on " + model.device_name, l.x, y, 32, kWhite);
        constexpr const char *kKbps[] = {"96 kbps", "160 kbps", "320 kbps"};
        const int quality = std::clamp(context_.settings.quality, 0, 2);
        ui::text(list, fonts.regular, kKbps[quality], l.x + w + 24, y, 22, kWhite.with_alpha(0.6f));
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
        // The generated cover stays underneath while the real one fades in.
        const float real = track.art ? (&track == &current_ ? art_in_.value : 1.0f) : 0.0f;
        if (real < 0.99f)
            draw_generated_cover(list, r, track, tint);
        if (real > 0.01f)
            list.image(track.art, r, gfx::kFullUv, tint.with_alpha(real), kArtRadius);
        list.bordered_rect(r, kArtRadius, kClear, 2, kWhite.with_alpha(0.16f));
        list.pop_opacity();
    }

    void draw_artwork(gfx::DrawList &list) const
    {
        const Layout l = layout();
        const bool reduced = context_.settings.reduced_motion;
        const float in = appear(1);
        const float live = play_.value;
        const float pulse = glow_.value;
        Rect art{l.x, kArtY, kArt, kArt};
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
        const Layout l = layout();
        if (alpha <= 0.01f)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float x = l.column_x + dx;
        list.push_opacity(alpha);
        // A long title first drops a size, then takes an ellipsis.
        const float size = fonts.display.measure(track.title, 76) <= l.column_w ? 76.0f : 60.0f;
        ui::text(list, fonts.display, fonts.display.font->fit(track.title, size, l.column_w), x - 3,
                 306, size, kWhite);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(track.artist, 30, l.column_w), x, 356,
                 30, kWhite.with_alpha(0.92f));
        ui::text(list, fonts.regular, fonts.regular.font->fit(track.album, 24, l.column_w), x, 396,
                 24, kWhite.with_alpha(0.62f));
        list.pop_opacity();
    }

    void draw_info(gfx::DrawList &list) const
    {
        const Layout l = layout();
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(2);
        const float live = play_.value;
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.3f);
        list.push_opacity(in);
        const float enter = slide(in, 30.0f);
        const float label_y = 206.0f;
        const float x = l.column_x + enter;
        if (live > 0.01f)
            ui::text(list, fonts.semibold, "NOW PLAYING", x, label_y + 8.0f * (1.0f - live), 18,
                     accent.with_alpha(live), gfx::Align::left, 4.0f);
        if (live < 0.99f)
            ui::text(list, fonts.semibold, "PAUSED", x, label_y - 8.0f * live, 18,
                     kWhite.with_alpha(0.7f * (1.0f - live)), gfx::Align::left, 4.0f);

        if (swap_.running)
        {
            const float t = swap_.progress();
            const float distance = context_.settings.reduced_motion ? 0.0f : 1.0f;
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
        const Layout l = layout();
        const float in = appear(3);
        const Color top = gfx::mix(accent_.value(), kWhite, 0.4f);
        const Color bottom = gfx::mix(body_.value(), accent_.value(), 0.45f);
        const float width = l.column_w / static_cast<float>(kBars) * 0.56f;
        const float pitch = (l.column_w - width) / static_cast<float>(kBars - 1);
        list.push_opacity(in);
        for (int i = 0; i < kBars; ++i)
        {
            const float grow = tween::stagger(age_, i, 0.012f, 0.5f);
            const float level = levels_[static_cast<std::size_t>(i)] * grow;
            const float h = width + level * (kBarsHeight - width);
            const float x = l.column_x + static_cast<float>(i) * pitch;
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
        const Layout l = layout();
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(4);
        const float focus = scrub_focus_.value;
        const float thumb = l.x + tween::clamp01(shown_.value) * l.w;
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.25f);
        char text[16];
        list.push_opacity(in);
        const float y = kScrubY + slide(in, 24.0f);
        // The track thickens when the scrubber has the focus...
        const float thick = 8.0f + 4.0f * focus;
        const Rect bar{l.x, y - thick * 0.5f, l.w, thick};
        list.rounded_rect(bar, thick * 0.5f, kWhite.with_alpha(0.16f + 0.06f * focus));
        const float filled = std::max(thick, thumb - l.x);
        list.glow({bar.x, bar.y, filled, bar.h}, thick * 0.5f, 10.0f + 8.0f * focus,
                  accent.with_alpha(0.22f + 0.2f * focus));
        list.gradient_rect_h({bar.x, bar.y, filled, bar.h}, thick * 0.5f, gfx::mix(body_.value(), accent, 0.5f),
                             accent);
        // ... and the thumb grows.
        const float radius = 9.0f + 6.0f * focus + 3.0f * seek_.value;
        list.shadow({thumb - radius, y - radius + 4, radius * 2, radius * 2}, radius, 10,
                    Color::rgb(0x000000, 0.45f));
        list.circle(thumb, y, radius, kWhite);
        style::format_time(text, sizeof(text), position_);
        ui::text(list, fonts.mono, text, l.x, y + 46, 22, kWhite.with_alpha(0.86f));
        style::format_time(text, sizeof(text), duration_);
        ui::text(list, fonts.mono, text, l.x + l.w, y + 46, 22, kWhite.with_alpha(0.6f), gfx::Align::right);
        // While seeking, the time rides above the thumb.
        const float bubble = tween::clamp01(bubble_.value);
        if (bubble > 0.01f)
        {
            const float cx = std::clamp(thumb, l.x + 44.0f, l.x + l.w - 44.0f);
            const Rect pill{cx - 44, y - 66 + 8.0f * (1.0f - bubble), 88, 36};
            list.push_opacity(bubble);
            list.shadow({pill.x, pill.y + 6, pill.w, pill.h}, 18, 14, Color::rgb(0x000000, 0.4f));
            list.rounded_rect(pill, 18, kWhite);
            style::format_time(text, sizeof(text), shown_.value * duration_ + 0.5f);
            ui::text(list, fonts.mono, text, pill.cx(), pill.cy() + 8, 22, kInk, gfx::Align::center);
            list.pop_opacity();
        }
        list.pop_opacity();
    }

    void draw_skip_icon(gfx::DrawList &list, float cx, float cy, float dir, Color c) const
    {
        side_triangle(list, cx - dir * 6.0f, cy, 24, dir, c);
        list.line(cx + dir * 12.0f, cy - 9, cx + dir * 12.0f, cy + 9, 4.5f, c);
    }

    void draw_transport(gfx::DrawList &list) const
    {
        const Layout l = layout();
        const float in = appear(5);
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.3f);
        list.push_opacity(in);
        const float y = kTransportY + slide(in, 28.0f);
        for (int i = 0; i < kControls; ++i)
        {
            const float cx = l.cx + kControlOffset[i];
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
        const Layout l = layout();
        const float alpha = appear(5);
        if (alpha <= 0.01f)
            return;
        const Rect ring = ring_.value();
        const bool sideways = nudge_direction_ != 0.0f;
        const float cx =
            l.cx + ring.x +
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

    // The three bars that mark the playing row: they read three bars of
    // the big visualizer, so the whole screen moves to one beat.
    void draw_equalizer(gfx::DrawList &list, float cx, float cy, Color color) const
    {
        constexpr int kSource[3] = {2, 9, 17};
        for (int i = 0; i < 3; ++i)
        {
            const float h = 5.0f + 17.0f * levels_[static_cast<std::size_t>(kSource[i])];
            list.rounded_rect({cx - 10.0f + static_cast<float>(i) * 8.0f, cy + 10.0f - h, 5, h}, 2.5f, color);
        }
    }

    void draw_queue(gfx::DrawList &list, std::uint32_t glass) const
    {
        if (!queue_ptr_)
            return;
        const Queue &queue = *queue_ptr_;
        const ui::Fonts &fonts = context_.fonts;
        const float in = appear(6);
        const float open = queue_.value * (context_.settings.reduced_motion ? 1.0f : in);
        const Color accent = gfx::mix(accent_.value(), kWhite, 0.3f);
        // The panel slides in from beyond the right edge.
        const Rect panel{kQueueX + (1.0f - open) * (kQueueW + kMargin + 60.0f), kQueueY, kQueueW, kQueueH};
        list.push_opacity(tween::clamp01(queue_.value * 1.5f) * in);
        list.shadow({panel.x, panel.y + 20, panel.w, panel.h}, kQueueRadius, 56, Color::rgb(0x000000, 0.42f));
        // Frosted glass: the blurred screen, a tint in the album's dark tone,
        // then a hairline of light.
        list.glass(glass, panel, kQueueRadius, kWhite);
        list.rounded_rect(panel, kQueueRadius, gfx::mix(palette_[0].value(), Color::rgb(0x0b0d16), 0.4f).with_alpha(0.5f));
        list.bordered_rect(panel, kQueueRadius, kClear, 1.5f, kWhite.with_alpha(0.2f));

        ui::text(list, fonts.semibold, "QUEUE", panel.x + 32, panel.y + 56, 18, accent, gfx::Align::left, 4.0f);
        const int after = static_cast<int>(queue.tracks.size()) - 1 - queue.playing;
        const std::string count = after > 0 ? std::to_string(after) + (after >= 50 ? "+ up next" : " up next")
                                            : std::string("Last track");
        ui::text(list, fonts.regular, count, panel.x + panel.w - 32, panel.y + 56, 20, kWhite.with_alpha(0.6f),
                 gfx::Align::right);
        list.rounded_rect({panel.x + 32, panel.y + 84, panel.w - 64, 1.5f}, 0.75f, kWhite.with_alpha(0.12f));

        const float top = panel.y + kQueueListTop - 20.0f;
        const float view = queue_view_height() + 20.0f;
        const float origin = panel.y + kQueueListTop - queue_scroll_.offset();
        list.push_clip({panel.x, top, panel.w, view});

        // The highlight springs between rows while the queue has the focus;
        // the playing row keeps a faint plate of its own.
        const float inset = 14.0f;
        const float focus = queue_focus_.value;
        if (queue.playing >= 0)
            list.rounded_rect({panel.x + inset, origin + static_cast<float>(queue.playing) * kQueueRow + 3,
                               panel.w - 2 * inset, kQueueRow - 6},
                              18, kWhite.with_alpha(0.07f));
        if (focus > 0.01f)
        {
            Rect row{panel.x + inset, origin + queue_row_.value * kQueueRow + 3, panel.w - 2 * inset, kQueueRow - 6};
            row.y += ui::shake(nudge_.value, clock_, 7.0f, 9.0f) * (nudge_direction_ == 0.0f ? 1.0f : 0.0f);
            list.glow(row, 18, 14, accent.with_alpha(0.22f * focus));
            list.rounded_rect(row, 18, kWhite.with_alpha(0.16f * focus));
            list.bordered_rect(row, 18, kClear, 2.5f, kWhite.with_alpha(focus));
        }

        for (int i = 0; i < static_cast<int>(queue.tracks.size()); ++i)
        {
            const float y = origin + static_cast<float>(i) * kQueueRow;
            // Rows fade under the edges of the list instead of being cut.
            const float alpha = tween::clamp01((y - top + 24.0f) / 36.0f) *
                                tween::clamp01((top + view - (y + kQueueRow) + 24.0f) / 36.0f);
            if (alpha <= 0.01f)
                continue;
            const TrackRow &track = queue.tracks[static_cast<std::size_t>(i)];
            const bool current = i == queue.playing;
            const bool focused = zone_ == Zone::queue && i == queue_row_index_;
            const float cy = y + kQueueRow * 0.5f;
            list.push_opacity(alpha);
            if (current)
                draw_equalizer(list, panel.x + 46, cy, accent);
            else
                ui::text(list, fonts.mono, std::to_string(queue.first + i + 1), panel.x + 46, cy + 7, 18,
                         kWhite.with_alpha(0.45f), gfx::Align::center);
            const Rect thumb{panel.x + 72, cy - kQueueThumb * 0.5f, kQueueThumb, kQueueThumb};
            const Art art = context_.art && !track.image.empty() ? context_.art(track.image) : Art{};
            if (art.texture)
                list.image(art.texture, thumb, gfx::kFullUv, kWhite, 8);
            else
                list.rounded_rect(thumb, 8, kWhite.with_alpha(0.08f));
            const float tx = thumb.x + thumb.w + 16;
            const float room = panel.x + panel.w - 30 - tx;
            if (track.title.empty())
            {
                list.rounded_rect({tx, cy - 16, room * 0.6f, 13}, 6.5f, kWhite.with_alpha(0.08f));
                list.rounded_rect({tx, cy + 6, room * 0.4f, 11}, 5.5f, kWhite.with_alpha(0.06f));
            }
            else
            {
                const ui::FontRef &face = current ? fonts.semibold : fonts.regular;
                ui::text(list, face, face.font->fit(track.title, 22, room), tx, cy - 2, 22,
                         kWhite.with_alpha(current || focused ? 1.0f : 0.85f));
                ui::text(list, fonts.regular, fonts.regular.font->fit(track.artists, 18, room), tx, cy + 22, 18,
                         kWhite.with_alpha(0.55f));
            }
            list.pop_opacity();
        }
        list.pop_clip();
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list) const
    {
        if (context_.song_menu_open)
            return;
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle glyphs = style::hint_style();
        constexpr const char *kLabels[kControls] = {"Previous", "Play", "Next"};
        const char *play = playing_ ? "Pause" : "Play";
        const char *queue = queue_open_ ? "Hide queue" : "Show queue";
        list.push_opacity(appear(8));
        ui::Hint hints[6];
        int count = 0;
        switch (zone_)
        {
        case Zone::scrubber:
            hints[count++] = {ui::Button::dpad, "Seek 5 s"};
            hints[count++] = {ui::Button::cross, play};
            hints[count++] = {ui::Button::circle, "Back"};
            break;
        case Zone::transport:
            hints[count++] = {ui::Button::cross, button_ == kPlay ? play : kLabels[button_]};
            hints[count++] = {ui::Button::l2, "Seek 15 s", ui::Button::r2};
            hints[count++] = {ui::Button::circle, "Back"};
            break;
        case Zone::queue:
            hints[count++] = {ui::Button::cross, "Play track"};
            hints[count++] = {ui::Button::circle, "Back"};
            break;
        }
        hints[count++] = {ui::Button::options, "More"};
        if (queue_ptr_)
            hints[count++] = {ui::Button::square, queue};
        ui::draw_hints(list, fonts, glyphs, hints, count, 1824, true, style::hint_layout());
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
    Zone zone_ = Zone::transport;

    // ---- queue and seeking ----
    const Queue *queue_ptr_ = nullptr; // this frame's model.queue
    bool queue_open_ = true;
    int queue_row_index_ = 0;
    float seek_target_ = -1.0f;  // seconds; >= 0 while a seek is pending or on its way
    float seek_send_in_ = 0.0f;  // > 0: the seek is sent when this runs out
    float seek_wait_ = 0.0f;
    float seek_hold_ = 0.0f;     // keeps the time bubble up after L2 / R2
    std::set<std::string> asked_; // queue rows whose details were asked for

    // ---- animation ----
    float age_ = 0.0f;
    float clock_ = 0.0f;
    float drift_ = 0.0f;
    std::array<float, kBars> levels_{};
    std::array<float, kBars> peaks_{};
    float energy_ = 0.0f;
    tween::Spring art_in_; // the real cover fading in over the generated one
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
    tween::Spring queue_;
    tween::Spring scrub_focus_;
    tween::Spring queue_focus_;
    tween::Spring queue_row_;
    ui::Scroller queue_scroll_;
    ui::Pulse seek_;
    tween::Spring bubble_;
};

} // namespace

std::unique_ptr<Screen> make_now_playing(Context &context)
{
    return std::make_unique<NowPlaying>(context);
}

bool now_playing_uses_back(const Screen &screen)
{
    return static_cast<const NowPlaying &>(screen).uses_back();
}

} // namespace screens
