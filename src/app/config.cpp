// spot5 - Where the app's files live, and its settings.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/config.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "nlohmann/json.hpp"

namespace app
{

std::string read_file(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool write_file(const std::string &path, const std::string &contents)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return static_cast<bool>(file);
}

bool load_config(Config &config, std::string &error)
{
    std::string text = read_file(paths::kConfig);
    if (text.empty())
        return true; // no file: the defaults
    auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() ||
        (json.contains("client_id") && (!json["client_id"].is_string() || json["client_id"].get<std::string>().empty())))
    {
        error = paths::kConfig + " is invalid (\"client_id\" must be a non-empty string)";
        return false;
    }
    config.client_id = json.value("client_id", config.client_id);
    return true;
}

screens::Settings load_settings()
{
    screens::Settings settings;
    auto json = nlohmann::json::parse(read_file(paths::kSettings), nullptr, false);
    if (json.is_discarded() || !json.is_object())
        return settings;
    settings.quality = std::clamp(json.value("quality", settings.quality), 0, 2);
    settings.volume = std::clamp(json.value("volume", settings.volume), 0, 10);
    settings.phone_volume = json.value("phone_volume", settings.phone_volume);
    settings.device_name = json.value("device_name", settings.device_name);
    if (settings.device_name.empty() || settings.device_name.size() > 64)
        settings.device_name = "PS5";
    settings.reduced_motion = json.value("reduced_motion", settings.reduced_motion);
    settings.resolution = std::clamp(json.value("resolution", settings.resolution), 0, 2);
    settings.timezone = json.value("timezone", settings.timezone);
    if (settings.timezone.size() > 64)
        settings.timezone.clear(); // not a zone name: back to Automatic
    return settings;
}

bool save_settings(const screens::Settings &settings)
{
    const nlohmann::json json = {{"quality", settings.quality},
                                 {"volume", settings.volume},
                                 {"phone_volume", settings.phone_volume},
                                 {"device_name", settings.device_name},
                                 {"reduced_motion", settings.reduced_motion},
                                 {"resolution", settings.resolution},
                                 {"timezone", settings.timezone}};
    const std::string temporary = paths::kSettings + ".tmp";
    return write_file(temporary, json.dump(2)) &&
           std::rename(temporary.c_str(), paths::kSettings.c_str()) == 0;
}

} // namespace app
