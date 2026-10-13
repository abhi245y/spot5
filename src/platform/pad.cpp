// ps5-spotify - DualSense buttons through scePad.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/pad.hpp"

#include <cstddef>

#include "platform/ps5/system.hpp"

namespace
{

// scePadRead sample layout (120 bytes), as in ps5-homebrew-ui/ProsperoLight.
struct RawPadSample
{
    std::uint32_t buttons;
    std::uint8_t sticks_and_triggers[6];
    std::uint8_t reserved0[66];
    std::int32_t connected;
    std::uint64_t timestamp_us;
    std::uint8_t extension[16];
    std::uint8_t connected_count;
    std::uint8_t reserved1[15];
};
static_assert(sizeof(RawPadSample) == 120);
static_assert(offsetof(RawPadSample, connected) == 0x4c);

constexpr int kUserServiceAlreadyInitialized = static_cast<int>(0x80960003);

} // namespace

extern "C"
{
    int sceUserServiceInitialize(const void *params);
    int sceUserServiceGetInitialUser(int *user);
    int scePadInit(void);
    int scePadOpen(int user, int type, int index, const void *parameters);
    int scePadRead(int handle, RawPadSample *samples, int count);
    int scePadClose(int handle);
}

namespace platform
{

Pad::~Pad()
{
    if (handle_ >= 0)
        scePadClose(handle_);
}

bool Pad::open()
{
    if (handle_ >= 0)
        return true;
    int init = sceUserServiceInitialize(nullptr);
    if (init != 0 && init != kUserServiceAlreadyInitialized)
        hui::sys::log("[APP] pad: sceUserServiceInitialize=0x%08x", static_cast<unsigned>(init));
    int user = -1;
    if (sceUserServiceGetInitialUser(&user) != 0)
    {
        hui::sys::log("[APP] pad: no initial user");
        return false;
    }
    scePadInit();
    // The pad service can lag behind title start; retry briefly.
    for (int attempt = 0; attempt < 20 && handle_ < 0; attempt++)
    {
        handle_ = scePadOpen(user, 0, 0, nullptr);
        if (handle_ < 0)
            hui::sys::sleep_us(50000);
    }
    if (handle_ < 0)
        hui::sys::log("[APP] pad: scePadOpen=0x%08x", static_cast<unsigned>(handle_));
    return handle_ >= 0;
}

std::uint32_t Pad::poll_pressed()
{
    if (handle_ < 0)
        return 0;
    // scePadRead returns every sample buffered since the last call, so a tap
    // shorter than the poll interval is still seen.
    RawPadSample samples[64];
    int count = scePadRead(handle_, samples, 64);
    std::uint32_t pressed = 0;
    for (int i = 0; i < count; i++)
    {
        if (!samples[i].connected)
            continue;
        pressed |= samples[i].buttons & ~held_;
        held_ = samples[i].buttons;
    }
    return pressed;
}

} // namespace platform
