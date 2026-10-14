// spot5 - Spotify's text, made drawable with the app's fonts.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The baked fonts have no typographic quotes, so titles and descriptions with
// ’ ‘ “ ” drew "?". They become their plain ASCII forms here, once, as
// replies are parsed.

#pragma once

#include <string>

namespace service
{

inline std::string drawable(std::string text)
{
    static const struct
    {
        const char *from;
        const char *to;
    } kSwaps[] = {{"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}, {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""}};
    for (const auto &swap : kSwaps)
        for (std::size_t at = text.find(swap.from); at != std::string::npos; at = text.find(swap.from, at))
            text.replace(at, 3, swap.to);
    return text;
}

} // namespace service
