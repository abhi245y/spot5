// ps5-spotify - Where the app's files live, and its settings.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

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
// Shipped with the app (read-only).
constexpr const char *kCertBundle = "/app0/assets/x509_bundle.bin";
} // namespace paths

struct Config
{
    // Spotify client id the saved credentials belong to (never in the repo).
    std::string client_id;
};

// Reads paths::kConfig. Returns false and sets `error` to a message for the
// user when the file is missing or invalid.
bool load_config(Config &config, std::string &error);

// Whole file as a string; empty if it cannot be read.
std::string read_file(const std::string &path);

// Replaces the file's contents. Returns false if it cannot be written.
bool write_file(const std::string &path, const std::string &contents);

} // namespace app
