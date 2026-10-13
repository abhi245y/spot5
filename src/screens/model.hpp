// ps5-spotify - What the screens show, and what they ask the app to do.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only contract between the UI (src/screens, kit only: also builds on a
// PC for previews) and the app (cspot, audio, console services).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "screens/style.hpp"

namespace screens
{

enum class Connection : std::uint8_t
{
    connecting,
    pairing, // nothing saved: waiting for the phone to pick this device
    ready,
    reconnecting,
    error,
};

// Honest loading steps, in order, for the launch screen's loader.
enum class Step : std::uint8_t
{
    certificates,
    login,
    connect,
    start,
    home,    // fetching Home (skipped when the saved one is under an hour old)
    artwork, // loading every cover on Home (Model::artwork_done / artwork_total)
    done,
};
constexpr int kStepCount = static_cast<int>(Step::done);

struct Track
{
    std::string id; // changes on every new track: drives the cover slide
    std::string title;
    std::string artist;
    std::string album;
    std::uint32_t duration_ms = 0;
    std::string image_url;
    std::uint32_t art = 0;       // cover texture (RGBA, top row first); 0 until loaded
    bool has_palette = false;    // palette below comes from the cover
    style::AlbumPalette palette; // valid when has_palette
};

// One thing on Home: a playlist, album, artist, show...
struct Card
{
    std::string uri;
    std::string title;
    std::string subtitle;    // "Playlist · Spotify", "Artist"...
    std::string description; // longer text for the details sheet; may be empty
    std::string image;       // ~300 px cover, for cards
    std::string image_large; // ~640 px cover, for the hero and details
    bool round = false;      // artists are drawn in a circle, as on the phone
};

struct Shelf
{
    std::string title;
    bool shortcuts = false; // the phone's 2x4 grid of small tiles at the top
    std::vector<Card> cards;
};

// One song in a playlist page or the queue. Only uri is known until its
// metadata has arrived (title empty: draw a placeholder row).
struct TrackRow
{
    std::string uri;
    std::string title;
    std::string artists; // "A, B & C"
    std::string album;
    std::uint32_t duration_ms = 0;
    std::string image; // ~300 px album cover
};

// A playlist/album opened from Home: its card and every track (resolved on
// open; metadata arrives a window at a time).
struct PlaylistPage
{
    Card card;
    std::vector<TrackRow> tracks;
    bool loading = true;  // the track list is still being resolved
    std::string error;    // set when it cannot be shown or played
};

// What plays next, for Now Playing's queue panel: a window of the queue
// around the playing track.
struct Queue
{
    std::vector<TrackRow> tracks;
    int playing = -1; // row of the playing track in `tracks`
    int first = 0;    // queue position of tracks[0] (for Request::jump)
};

// The personalised Home page, as Spotify's own apps get it (casita).
struct HomePage
{
    std::vector<Shelf> shelves;
};

// What a screen wants played: a playlist/album/... from its first track (or
// from track `index` of its page), in order or shuffled.
struct PlayRequest
{
    std::string uri;
    bool shuffle = false;
    int index = 0;
};

// What a screen asks the app for (Context::requests), besides Commands.
struct Request
{
    enum class Kind
    {
        play,     // play: PlayRequest
        open,     // open card's page (its track list)
        describe, // fetch details for uris (rows on screen without a title)
        seek,     // seek the playing track to ms
        jump,     // play queue track `index` (absolute, Queue::first + row)
    };
    Kind kind = Kind::play;
    PlayRequest play;
    Card card;
    std::vector<std::string> uris;
    std::uint32_t ms = 0;
    int index = 0;
};

// What the user can change in Control Room. The app loads and saves it
// (settings.json) and applies it; the screens edit it live through
// Context::settings and raise Context::settings_changed.
struct Settings
{
    int quality = 2;              // 0: 96, 1: 160, 2: 320 kbps
    int volume = 10;              // 0..10, on top of the phone's volume
    bool phone_volume = true;     // the phone's volume buttons control this console
    std::string device_name = "PS5";
    bool reduced_motion = false;
    int resolution = 2;           // 0: 1080p, 1: 1440p, 2: 4K (the display opens at it on launch)
};

// Facts for Control Room's About page.
struct About
{
    std::string version;   // the app's contentVersion
    std::string engine;    // cspot branch@commit it was built from
    std::string address;   // the console's IP address
};

struct Model
{
    Connection connection = Connection::connecting;
    Step step = Step::certificates;
    std::string message; // why: Error/Reconnecting
    std::string device_name = "PS5";
    std::string username; // the signed-in Spotify account
    About about;

    std::shared_ptr<const HomePage> home; // null until the first load (or the cache)
    std::shared_ptr<const PlaylistPage> page; // the page last opened from Home
    std::shared_ptr<const Queue> queue;       // null when nothing is queued
    bool home_loading = false;            // a fetch is running
    int artwork_done = 0;                 // launch: covers loaded so far...
    int artwork_total = 0;                // ... of this many
    std::string notice;                   // a short message, e.g. "This can't be played here yet"
    std::uint32_t notice_id = 0;          // changes with each new notice: show it once
    std::string busy;                     // an action under way ("Shuffling Focus Mix"); empty when idle

    bool has_track = false; // a track is loaded (from the phone or from Home)
    Track track;
    bool paused = true;
    std::uint32_t position_ms = 0;
};

enum class Command : std::uint8_t
{
    retry,
    play_pause,
    next,
    previous,
    quit,
    forget_login, // delete the saved login (Control Room > Account)
    refresh_home, // fetch Home again (R3); the app limits how often
    clear_cache,  // delete saved covers and Home (Control Room > About), then fetch again
};

} // namespace screens
