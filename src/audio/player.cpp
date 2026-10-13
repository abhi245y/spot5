// ps5-spotify - Connects cspot's spirc/TrackPlayer to sceAudioOut.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "audio/player.hpp"

#include <algorithm>
#include <variant>

#include "Logger.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"

extern "C"
{
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
    int sceKernelUsleep(unsigned int microseconds);
}

namespace
{
constexpr int kGrain = 256;                      // frames per sceAudioOutOutput
constexpr std::size_t kCapacity = 4 * 48000 * 2; // 4 s of 48 kHz stereo samples
} // namespace

Player::Player(std::shared_ptr<cspot::SpircHandler> handler) : handler_(std::move(handler))
{
    sceAudioOutInit();
    // The console only opens 48 kHz (44.1 kHz is refused), hence the resampler.
    handle_ = sceAudioOutOpen(0xff, 0, 0, kGrain, 48000, 1 /* S16 stereo */);
    CSPOT_LOG(info, "sceAudioOutOpen=0x%08x", static_cast<unsigned>(handle_));

    handler_->getTrackPlayer()->setDataCallback(
        [this](uint8_t *data, size_t bytes, std::string_view track) {
            return on_data(data, bytes, std::string(track));
        });

    handler_->setEventHandler([this](std::unique_ptr<cspot::SpircHandler::Event> event) {
        using Type = cspot::SpircHandler::EventType;
        std::scoped_lock guard(lock_);
        switch (event->eventType)
        {
        case Type::PLAY_PAUSE:
            paused_ = std::get<bool>(event->data);
            now_playing_.paused = paused_;
            break;
        case Type::FLUSH:
        case Type::DISC:
        case Type::SEEK:
            clear_locked();
            break;
        case Type::PLAYBACK_START:
            // Paused until the PLAY_PAUSE that follows; re-notify playback.
            paused_ = true;
            playlist_end_ = false;
            clear_locked();
            stream_track_.clear();
            play_track_.clear();
            break;
        case Type::DEPLETED:
            playlist_end_ = true;
            break;
        case Type::VOLUME:
            volume_ = std::get<int>(event->data);
            break;
        case Type::TRACK_INFO: {
            auto info = std::get<cspot::TrackInfo>(event->data);
            now_playing_.title = info.name;
            now_playing_.artist = info.artist;
            now_playing_.album = info.album;
            break;
        }
        default:
            break;
        }
    });

    if (handle_ < 0)
        return;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&thread_, &attr, &Player::output_entry, this) != 0)
    {
        CSPOT_LOG(error, "audio output thread failed");
        sceAudioOutClose(handle_);
        handle_ = -1;
    }
    pthread_attr_destroy(&attr);
}

Player::~Player()
{
    running_ = false;
    if (handle_ >= 0)
    {
        pthread_join(thread_, nullptr);
        sceAudioOutClose(handle_);
    }
}

NowPlaying Player::now_playing()
{
    std::scoped_lock guard(lock_);
    return now_playing_;
}

std::size_t Player::on_data(uint8_t *data, std::size_t bytes, const std::string &track)
{
    std::scoped_lock guard(lock_);
    if (stream_track_ != track)
    {
        // As CliPlayer: one track playing and the next streaming; refuse a
        // third (stale data after a seek or skip).
        if (play_track_ != track && play_track_ != stream_track_)
            return 0;
        stream_track_ = track;
    }
    // Check room before resampling: the resampler keeps state, so a block
    // must be processed exactly once. Upper bound of its 48 kHz output:
    std::size_t room_needed = (bytes / 4 * 48000 / 44100 + 2) * 2;
    if (buffered_ + room_needed > kCapacity)
        return 0; // full: TrackPlayer sleeps 50 ms and offers it again
    Chunk chunk;
    chunk.track = track;
    resampler_.process(reinterpret_cast<const int16_t *>(data), bytes / 4, chunk.pcm);
    buffered_ += chunk.pcm.size();
    chunks_.push_back(std::move(chunk));
    return bytes;
}

void Player::clear_locked()
{
    chunks_.clear();
    buffered_ = 0;
    resampler_ = Resampler(44100, 48000);
}

void *Player::output_entry(void *self)
{
    static_cast<Player *>(self)->output_loop();
    return nullptr;
}

void Player::output_loop()
{
    int16_t grain[kGrain * 2];
    while (running_)
    {
        std::string reached;
        bool ended = false, have = false;
        {
            std::scoped_lock guard(lock_);
            if (!paused_ && buffered_ >= kGrain * 2)
            {
                std::size_t filled = 0;
                while (filled < kGrain * 2 && !chunks_.empty())
                {
                    Chunk &chunk = chunks_.front();
                    if (chunk.track != play_track_)
                    {
                        play_track_ = chunk.track;
                        reached = chunk.track;
                    }
                    std::size_t n = std::min(kGrain * 2 - filled, chunk.pcm.size() - chunk.read);
                    std::copy_n(chunk.pcm.data() + chunk.read, n, grain + filled);
                    chunk.read += n;
                    filled += n;
                    if (chunk.read == chunk.pcm.size())
                        chunks_.pop_front();
                }
                buffered_ -= filled;
                have = true;
            }
            else if (!paused_ && playlist_end_ && chunks_.empty())
            {
                playlist_end_ = false;
                ended = true;
            }
        }
        // Outside the lock: these call back into spirc.
        if (!reached.empty())
            handler_->notifyAudioReachedPlayback(reached);
        if (ended)
            handler_->notifyAudioEnded();
        if (!have)
        {
            sceKernelUsleep(10000);
            continue;
        }
        // Software volume (spirc sends 0..65535).
        int volume = volume_;
        if (volume < 65535)
            for (auto &s : grain)
                s = static_cast<int16_t>(s * volume / 65535);
        sceAudioOutOutput(handle_, grain);
    }
    sceAudioOutOutput(handle_, nullptr);
}
