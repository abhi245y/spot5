// ps5-spotify - Where the app's files live, and its settings.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/config.hpp"

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
    {
        error = "Missing " + paths::kConfig;
        return false;
    }
    auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.contains("client_id") || !json["client_id"].is_string())
    {
        error = paths::kConfig + " is invalid (needs \"client_id\")";
        return false;
    }
    config.client_id = json["client_id"].get<std::string>();
    return true;
}

} // namespace app
