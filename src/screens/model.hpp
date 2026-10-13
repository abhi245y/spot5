// ps5-spotify - What the screens show, and what they ask the app to do.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The only contract between the UI (src/screens, kit only: also builds on a
// PC for previews) and the app (cspot, audio, console services).

#pragma once

#include <cstdint>
#include <string>

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
};

struct Model
{
    Connection connection = Connection::connecting;
    Step step = Step::certificates;
    std::string message; // why: Error/Reconnecting
    std::string device_name = "PS5";

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
};

} // namespace screens
