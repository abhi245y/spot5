// spot5 - Time zones for Home's morning and evening shelves.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Home only needs the local time, sent as an IANA zone name. The console
// keeps its zone as a UTC offset in minutes plus a summer-time flag, so
// Automatic maps the console's current offset to a zone without summer time
// at that offset; the list in Control Room is real cities, summer time and
// all. Names are ASCII: the app's fonts are Latin.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace screens
{

struct TimeZone
{
    const char *iana;
    const char *city;
    int minutes;      // standard (winter) offset from UTC
    bool summer_time; // moves an hour forward part of the year
};

// By offset; within an offset the zone without summer time comes first.
inline constexpr TimeZone kTimeZones[] = {
    {"Pacific/Honolulu", "Honolulu", -600, false},
    {"America/Anchorage", "Anchorage", -540, true},
    {"America/Los_Angeles", "Los Angeles", -480, true},
    {"America/Phoenix", "Phoenix", -420, false},
    {"America/Denver", "Denver", -420, true},
    {"America/Mexico_City", "Mexico City", -360, false},
    {"America/Chicago", "Chicago", -360, true},
    {"America/Bogota", "Bogota", -300, false},
    {"America/New_York", "New York", -300, true},
    {"America/Caracas", "Caracas", -240, false},
    {"America/Halifax", "Halifax", -240, true},
    {"America/St_Johns", "St. John's", -210, true},
    {"America/Sao_Paulo", "Sao Paulo", -180, false},
    {"America/Argentina/Buenos_Aires", "Buenos Aires", -180, false},
    {"Atlantic/South_Georgia", "South Georgia", -120, false},
    {"Atlantic/Cape_Verde", "Cape Verde", -60, false},
    {"Atlantic/Azores", "Azores", -60, true},
    {"Etc/UTC", "UTC", 0, false},
    {"Atlantic/Reykjavik", "Reykjavik", 0, false},
    {"Europe/London", "London", 0, true},
    {"Africa/Lagos", "Lagos", 60, false},
    {"Europe/Paris", "Paris", 60, true},
    {"Europe/Berlin", "Berlin", 60, true},
    {"Africa/Johannesburg", "Johannesburg", 120, false},
    {"Europe/Athens", "Athens", 120, true},
    {"Africa/Cairo", "Cairo", 120, true},
    {"Europe/Istanbul", "Istanbul", 180, false},
    {"Europe/Moscow", "Moscow", 180, false},
    {"Asia/Riyadh", "Riyadh", 180, false},
    {"Africa/Nairobi", "Nairobi", 180, false},
    {"Asia/Tehran", "Tehran", 210, false},
    {"Asia/Dubai", "Dubai", 240, false},
    {"Asia/Kabul", "Kabul", 270, false},
    {"Asia/Karachi", "Karachi", 300, false},
    {"Asia/Kolkata", "Kolkata", 330, false},
    {"Asia/Colombo", "Colombo", 330, false},
    {"Asia/Kathmandu", "Kathmandu", 345, false},
    {"Asia/Dhaka", "Dhaka", 360, false},
    {"Asia/Yangon", "Yangon", 390, false},
    {"Asia/Bangkok", "Bangkok", 420, false},
    {"Asia/Jakarta", "Jakarta", 420, false},
    {"Asia/Singapore", "Singapore", 480, false},
    {"Asia/Hong_Kong", "Hong Kong", 480, false},
    {"Australia/Perth", "Perth", 480, false},
    {"Asia/Tokyo", "Tokyo", 540, false},
    {"Asia/Seoul", "Seoul", 540, false},
    {"Australia/Darwin", "Darwin", 570, false},
    {"Australia/Adelaide", "Adelaide", 570, true},
    {"Australia/Brisbane", "Brisbane", 600, false},
    {"Australia/Sydney", "Sydney", 600, true},
    {"Pacific/Noumea", "Noumea", 660, false},
    {"Pacific/Fiji", "Fiji", 720, false},
    {"Pacific/Auckland", "Auckland", 720, true},
    {"Pacific/Tongatapu", "Tonga", 780, false},
};
inline constexpr int kTimeZoneCount = static_cast<int>(sizeof(kTimeZones) / sizeof(kTimeZones[0]));

// "UTC+5:30", "UTC-3", "UTC+0".
inline std::string utc_label(int minutes)
{
    char text[16];
    const int size = std::abs(minutes);
    if (size % 60)
        std::snprintf(text, sizeof(text), "UTC%c%d:%02d", minutes < 0 ? '-' : '+', size / 60, size % 60);
    else
        std::snprintf(text, sizeof(text), "UTC%c%d", minutes < 0 ? '-' : '+', size / 60);
    return text;
}

// The zone Automatic sends for the console's current offset: one without
// summer time at that offset, else UTC.
inline const TimeZone &zone_for_offset(int minutes)
{
    for (const TimeZone &zone : kTimeZones)
        if (zone.minutes == minutes && !zone.summer_time)
            return zone;
    for (const TimeZone &zone : kTimeZones)
        if (zone.iana == std::string_view("Etc/UTC"))
            return zone;
    return kTimeZones[0];
}

// The list entry for a saved name, or -1 (Automatic, or a name not listed).
inline int zone_index(const std::string &iana)
{
    for (int i = 0; i < kTimeZoneCount; ++i)
        if (iana == kTimeZones[i].iana)
            return i;
    return -1;
}

} // namespace screens
