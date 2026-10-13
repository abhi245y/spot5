// ps5-spotify - DualSense buttons through scePad.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Trimmed from ps5-homebrew-ui src/platform/ps5/pad.cpp (GPL-3.0-or-later):
// only "which buttons were newly pressed since the last poll".

#pragma once

#include <cstdint>

namespace platform
{

namespace button
{
constexpr std::uint32_t kCross = 0x00004000;
constexpr std::uint32_t kCircle = 0x00002000;
} // namespace button

class Pad
{
  public:
    Pad() = default;
    Pad(const Pad &) = delete;
    Pad &operator=(const Pad &) = delete;
    ~Pad();

    // Opens the initial user's controller. Safe to call again after failure.
    bool open();
    bool is_open() const { return handle_ >= 0; }

    // Buttons that went down since the last call (bit mask of button::k*).
    std::uint32_t poll_pressed();

  private:
    int handle_ = -1;
    std::uint32_t held_ = 0;
};

} // namespace platform
