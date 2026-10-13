// ps5-spotify - Connects cspot's spirc/TrackPlayer to sceAudioOut.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same contract as cspot's targets/cli/CliPlayer: TrackPlayer pushes 44.1 kHz
// S16 stereo through the data callback (returning 0 = "full, retry later"),
// spirc events pause/flush/reset the buffer, and the output side reports
// when a new track's audio actually reaches the speaker.

#pragma once

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/resampler.hpp"

namespace cspot
{
class SpircHandler;
}

// What is playing, for the app and UI. Copied out under the player's lock.
struct NowPlaying
{
    std::string track_id; // changes with every track that reaches the speaker
    std::string title;
    std::string artist;
    std::string album;
    std::string image_url; // the album cover (LARGE, 640 px)
    std::uint32_t duration_ms = 0;
    std::uint32_t position_ms = 0; // of the audio being heard, not decoded
    bool paused = true;
    bool operator==(const NowPlaying &) const = default;
};

class Player
{
  public:
    explicit Player(std::shared_ptr<cspot::SpircHandler> handler);
    ~Player();
    Player(const Player &) = delete;
    Player &operator=(const Player &) = delete;

    bool ok() const { return handle_ >= 0; }
    NowPlaying now_playing();

    // This console's own level (0..10, perceptual: -4 dB per step near the
    // top) on top of the phone's volume, or alone when follow_phone is off.
    void set_volume(int level, bool follow_phone);

  private:
    struct Chunk
    {
        std::vector<int16_t> pcm; // 48 kHz stereo
        std::string track;
        std::size_t read = 0; // samples already played
    };

    std::size_t on_data(uint8_t *data, std::size_t bytes, const std::string &track);
    void clear_locked();
    void output_loop();
    static void *output_entry(void *self);

    std::shared_ptr<cspot::SpircHandler> handler_;
    int handle_ = -1;
    pthread_t thread_{};
    std::atomic<bool> running_{true};

    std::mutex lock_;
    std::deque<Chunk> chunks_;
    std::size_t buffered_ = 0; // samples in chunks_ not yet played
    Resampler resampler_{44100, 48000};
    std::string stream_track_, play_track_;
    bool paused_ = true, playlist_end_ = false;
    std::atomic<int> volume_{65535}; // spirc volume, 0..65535
    std::atomic<float> local_gain_{1.0f};
    std::atomic<bool> follow_phone_{true};
    NowPlaying now_playing_;
    // Position of what is heard: base + frames played since it was set.
    // PLAYBACK_START fires when the next track starts *streaming* (seconds
    // before it is heard), so its position waits in next_start_ms_ until
    // that track's audio reaches the output.
    std::uint32_t base_ms_ = 0;
    std::uint32_t next_start_ms_ = 0;
    std::uint64_t played_frames_ = 0;
};
