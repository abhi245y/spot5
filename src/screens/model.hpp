// ps5-spotify - What the screens show, and what they ask the app to do.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only contract between the UI (src/screens, kit only: also builds on a
// PC for previews) and the app (cspot, audio, console services).

#pragma once

#include <cstdint>
#include <string>

#include "screens/style.hpp"

namespace screens
{

enum class Connection : std::uint8_t
{
    connecting,
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

    bool has_track = false; // the phone picked this device and sent a track
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
};

} // namespace screens
