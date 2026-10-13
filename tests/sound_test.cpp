// ps5-spotify - Console sound test: can sceAudioOut play Spotify-format audio?
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Three tests, each logged with raw return codes (stdout goes to the app log,
// see runtime/runtime_shims.c, and every line is mirrored to klog):
//   1. open the output at 44100 Hz (Spotify's rate) and play a 2 s tone
//   2. open it at 48000 Hz (known to work in ps5-homebrew-ui), 2 s tone
//   3. decode a bundled 44.1 kHz Ogg Vorbis clip and play it, at 44100 if
//      test 1 opened, otherwise resampled to 48000
// Then the app closes itself.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "third_party/stb/vorbis.h"

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);
    int sceKernelUsleep(unsigned int microseconds);
    int sceSystemServiceLoadExec(const char *path, const char **arguments);
    int sceUserServiceInitialize(const void *params);
    int sceAudioOutInit(void);
    int sceAudioOutOpen(int user, int type, int index, unsigned int grain_frames,
                        unsigned int frequency, unsigned int format);
    int sceAudioOutOutput(int handle, const void *samples);
    int sceAudioOutClose(int handle);
}

namespace
{

constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);
constexpr int kSystemUser = 0xff;
constexpr int kPortMain = 0;
constexpr unsigned kFormatS16Stereo = 1;
constexpr int kGrain = 256;

void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
void trace(const char *format, ...)
{
    char line[384];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line) - 1, format, arguments);
    va_end(arguments);
    std::strcat(line, "\n");
    std::fputs(line, stdout);
    sceKernelDebugOutText(0, line);
}

// Plays interleaved stereo S16 frames on an open port, one grain at a time
// (sceAudioOutOutput blocks per grain, which paces the loop).
int play(int handle, const std::int16_t *frames, std::size_t count)
{
    std::int16_t grain[kGrain * 2];
    int errors = 0;
    for (std::size_t at = 0; at < count; at += kGrain)
    {
        std::size_t n = count - at < kGrain ? count - at : kGrain;
        std::memset(grain, 0, sizeof(grain));
        std::memcpy(grain, frames + at * 2, n * 2 * sizeof(std::int16_t));
        if (sceAudioOutOutput(handle, grain) < 0)
            errors++;
    }
    sceAudioOutOutput(handle, nullptr); // drain
    return errors;
}

std::vector<std::int16_t> tone(int rate, double hz, double seconds, bool left = true,
                               bool right = true)
{
    std::vector<std::int16_t> out(static_cast<std::size_t>(rate * seconds) * 2);
    for (std::size_t i = 0; i < out.size() / 2; i++)
    {
        auto s = static_cast<std::int16_t>(8000 * std::sin(2 * M_PI * hz * i / rate));
        out[i * 2] = left ? s : 0;
        out[i * 2 + 1] = right ? s : 0;
    }
    return out;
}

// ponytail: linear interpolation, audible aliasing on bright material;
// swap for a windowed-sinc/polyphase resampler if it sounds dull or harsh.
std::vector<std::int16_t> resample(const std::int16_t *in, std::size_t frames, int from, int to)
{
    std::size_t outFrames = frames * static_cast<std::size_t>(to) / from;
    std::vector<std::int16_t> out(outFrames * 2);
    for (std::size_t i = 0; i < outFrames; i++)
    {
        double pos = static_cast<double>(i) * from / to;
        std::size_t a = static_cast<std::size_t>(pos);
        std::size_t b = a + 1 < frames ? a + 1 : a;
        double t = pos - a;
        for (int c = 0; c < 2; c++)
            out[i * 2 + c] =
                static_cast<std::int16_t>(in[a * 2 + c] * (1 - t) + in[b * 2 + c] * t);
    }
    return out;
}

// Opens a port, logs the result, plays the frames, closes. Returns open rc.
int run_test(const char *name, int rate, const std::int16_t *frames, std::size_t count)
{
    int handle = sceAudioOutOpen(kSystemUser, kPortMain, 0, kGrain, rate, kFormatS16Stereo);
    trace("[SPT] %s: sceAudioOutOpen(rate=%d)=0x%08x", name, rate, static_cast<unsigned>(handle));
    if (handle < 0)
        return handle;
    int errors = play(handle, frames, count);
    int closed = sceAudioOutClose(handle);
    trace("[SPT] %s: played %zu frames, output errors=%d, close=0x%08x", name, count, errors,
        static_cast<unsigned>(closed));
    return handle;
}

std::vector<unsigned char> read_file(const char *path)
{
    std::vector<unsigned char> data;
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return data;
    unsigned char buf[16384];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

[[noreturn]] void quit()
{
    trace("[SPT] done, closing");
    std::fflush(nullptr);
    int refused = sceSystemServiceLoadExec("exit", nullptr);
    trace("[SPT] quit refused rc=0x%x", static_cast<unsigned>(refused));
    for (;;)
        sceKernelUsleep(100000);
}

} // namespace

// libc++ calls this instead of throwing in a no-exceptions build; the SDK
// does not provide it. Log and park (exit() crashes a native title).
namespace std
{
inline namespace __1
{
[[noreturn]] void __libcpp_verbose_abort(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stdout, format, arguments);
    va_end(arguments);
    trace("[SPT] libc++ abort");
    for (;;)
        sceKernelUsleep(100000);
}
} // namespace __1
} // namespace std

extern "C" int spt_exception_probe();

int main()
{
    trace("[SPT] sound test start");
    // 0 = exceptions work; a crash here also answers the question (log ends).
    trace("[SPT] test0 exceptions: starting");
    trace("[SPT] test0 exceptions: result=%d", spt_exception_probe());
    trace("[SPT] sceUserServiceInitialize=0x%08x",
        static_cast<unsigned>(sceUserServiceInitialize(nullptr)));
    int init = sceAudioOutInit();
    trace("[SPT] sceAudioOutInit=0x%08x", static_cast<unsigned>(init));
    if (init != 0 && init != kAlreadyInitialized)
        quit();

    auto t44 = tone(44100, 440, 2);
    bool native44 = run_test("test1 tone 44.1k", 44100, t44.data(), t44.size() / 2) >= 0;
    sceKernelUsleep(1000000);

    auto t48 = tone(48000, 440, 2);
    run_test("test2 tone 48k", 48000, t48.data(), t48.size() / 2);
    sceKernelUsleep(1000000);

    auto ogg = read_file("/app0/assets/test.ogg (copy tests/test.ogg into assets/ to run)");
    int channels = 0, rate = 0;
    short *pcm = nullptr;
    int frames = ogg.empty() ? -1
                             : stb_vorbis_decode_memory(ogg.data(), static_cast<int>(ogg.size()),
                                                        &channels, &rate, &pcm);
    trace("[SPT] test3 ogg: file=%zu bytes, decoded frames=%d channels=%d rate=%d", ogg.size(),
        frames, channels, rate);
    if (frames > 0 && channels == 2)
    {
        if (native44 && rate == 44100)
            run_test("test3 ogg native", 44100, pcm, frames);
        else
        {
            auto out = resample(pcm, frames, rate, 48000);
            run_test("test3 ogg resampled", 48000, out.data(), out.size() / 2);
        }
    }
    std::free(pcm);
    sceKernelUsleep(1000000);

    // Channel order: left-only low tone, pause, right-only high tone.
    auto left = tone(48000, 440, 2, true, false);
    run_test("test4 LEFT only 440 Hz", 48000, left.data(), left.size() / 2);
    sceKernelUsleep(1000000);
    auto right = tone(48000, 880, 2, false, true);
    run_test("test4 RIGHT only 880 Hz", 48000, right.data(), right.size() / 2);
    // The log is readable over FTP only while the title runs.
    trace("[SPT] all tests finished; closing in 60 s");
    sceKernelUsleep(60000000);
    quit();
}
