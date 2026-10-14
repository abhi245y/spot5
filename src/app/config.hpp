// spot5 - Where the app's files live, and its settings.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include "screens/model.hpp"

namespace app
{

namespace paths
{
// Login and settings live in the install folder for now, so they are easy to
// replace over FTP while testing. For the release, move them to the title's
// private storage (/download0/...), which FTP cannot read while it is closed.
constexpr const char *kDataDir = "/app0";
const std::string kConfig = std::string(kDataDir) + "/config.json";
const std::string kCredentials = std::string(kDataDir) + "/cspot-creds.json";
const std::string kSettings = std::string(kDataDir) + "/settings.json";
const std::string kHome = std::string(kDataDir) + "/home.json"; // last Home page
const std::string kLibrary = std::string(kDataDir) + "/library.json"; // last Your Library
// Downloaded covers, by URL hash. ponytail: never pruned (directory listing
// does not work under /app0); a few MB a month. Prune with an index if it grows.
const std::string kCovers = std::string(kDataDir) + "/covers";
// Shipped with the app (read-only).
constexpr const char *kCertBundle = "/app0/assets/x509_bundle.bin";
} // namespace paths

struct Config
{
    // Spotify client id logins are made for. Default: librespot's public
    // keymaster id (core/src/config.rs); config.json "client_id" overrides it.
    std::string client_id = "65b708073fc0480ea92a077233ca87bd";
};

// Reads paths::kConfig, which is optional. Returns false and sets `error` to
// a message for the user only when the file is there but unreadable.
bool load_config(Config &config, std::string &error);

// Control Room's settings; defaults for anything missing or unreadable.
screens::Settings load_settings();
// Written to a temporary file and renamed, so a crash never leaves half a file.
bool save_settings(const screens::Settings &settings);

// Whole file as a string; empty if it cannot be read.
std::string read_file(const std::string &path);

// Replaces the file's contents. Returns false if it cannot be written.
bool write_file(const std::string &path, const std::string &contents);

} // namespace app
