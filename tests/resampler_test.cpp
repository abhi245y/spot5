// spot5 - Host check for src/audio/resampler.hpp.
// SPDX-License-Identifier: GPL-3.0-or-later
// Build/run: clang++ -std=c++17 -Isrc tests/resampler_test.cpp -o /tmp/rt && /tmp/rt
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "audio/resampler.hpp"

int main()
{
    // 1 s of a 440 Hz stereo sine at 44.1 kHz, fed in random-sized chunks,
    // must equal the same input fed in one block and come out ~48000 frames.
    std::vector<std::int16_t> in(44100 * 2);
    for (int i = 0; i < 44100; i++)
        in[i * 2] = in[i * 2 + 1] = static_cast<std::int16_t>(10000 * std::sin(2 * M_PI * 440 * i / 44100.0));
    std::vector<std::int16_t> whole, chunked;
    Resampler a(44100, 48000), b(44100, 48000);
    a.process(in.data(), 44100, whole);
    for (std::size_t at = 0; at < 44100;)
    {
        std::size_t n = 1 + std::rand() % 777;
        if (at + n > 44100)
            n = 44100 - at;
        b.process(in.data() + at * 2, n, chunked);
        at += n;
    }
    assert(whole == chunked);
    assert(std::abs(static_cast<long>(whole.size() / 2) - 48000) <= 1);
    // Output is the same sine at 48 kHz: compare against the ideal.
    double worst = 0;
    for (std::size_t i = 1; i < whole.size() / 2; i++)
        worst = std::fmax(worst, std::fabs(whole[i * 2] - 10000 * std::sin(2 * M_PI * 440 * (i - 1) / 48000.0)));
    std::printf("frames=%zu worst error=%.0f (of 10000)\n", whole.size() / 2, worst);
    assert(worst < 100);
    std::puts("resampler OK");
}
