// Persistent settings (JSON in %APPDATA%\MoonUp\settings.json) and profile -> engine config.
#pragma once
#include "engine/config.h"
#include "third_party/json.hpp"

namespace sw {

using json = nlohmann::json;

json DefaultProfile(const std::string& name);
json DefaultSettings();
json LoadSettings();
bool SaveSettings(const json& s);
// Fills missing keys from defaults (forward compatible with older files).
json MergeDefaults(const json& defaults, const json& value);

EngineConfig ProfileToConfig(const json& profile);
// Picks the profile whose "match" equals the executable name, else the active profile.
json ProfileForExe(const json& settings, const std::string& exeName, std::string* profileId = nullptr);

}  // namespace sw
