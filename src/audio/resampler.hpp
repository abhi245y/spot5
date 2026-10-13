// ps5-spotify - Streaming stereo S16 resampler (Spotify 44.1 kHz -> PS5 48 kHz).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// sceAudioOut only opens at 48 kHz (44.1 kHz is refused with 0x80260008).
// ponytail: linear interpolation; audible as slight dullness/aliasing on
// bright music. Swap for a polyphase/windowed-sinc filter if it sounds off.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class Resampler
{
  public:
    Resampler(int from, int to) : from_(from), to_(to) {}

    // Appends the resampled frames of `frames` interleaved stereo input
    // frames to `out`. Keeps the last frame and phase, so blocks join
    // seamlessly.
    void process(const std::int16_t *in, std::size_t frames, std::vector<std::int16_t> &out)
    {
        // Frame -1 is the previous block's last frame.
        auto at = [&](std::size_t i, int c) -> double {
            return i == 0 ? last_[c] : in[(i - 1) * 2 + c];
        };
        // Position is exact: pos_ counts 1/to_ steps of an input frame, so
        // the output is identical however the input is split into blocks.
        const std::int64_t end = static_cast<std::int64_t>(frames) * to_;
        for (; pos_ < end; pos_ += from_)
        {
            auto i = static_cast<std::size_t>(pos_ / to_);
            double t = static_cast<double>(pos_ % to_) / to_;
            for (int c = 0; c < 2; c++)
                out.push_back(static_cast<std::int16_t>(at(i, c) * (1 - t) + at(i + 1, c) * t));
        }
        pos_ -= end;
        if (frames > 0)
        {
            last_[0] = in[(frames - 1) * 2];
            last_[1] = in[(frames - 1) * 2 + 1];
        }
    }

  private:
    std::int64_t from_, to_;
    std::int64_t pos_ = 0;
    std::int16_t last_[2] = {0, 0};
};
