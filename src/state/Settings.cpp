// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/Settings.hpp"

#include "state/ChainState.hpp" // jsonFloat()
#include "state/Paths.hpp"

#include <nlohmann/json.hpp>

namespace xythos {

using nlohmann::json;

// Stored as [{ "program": n, "path": "..." }], since JSON object keys are strings
static json programPresetsJson(const std::map<int, std::string>& programs)
{
    json list = json::array();
    for (const auto& [program, path] : programs)
        list.push_back({ { "program", program }, { "path", path } });
    return list;
}

std::string Settings::toJson() const
{
    return json {
        { "version", kVersion },
        { "namDir", namDir },
        { "irDir", irDir },
        { "a2Lite", a2Lite },
        { "normalize", normalize },
        { "parallel", parallel },
        { "accent", accent },
        { "accentColor", accentColor },
        { "highlightStyle", inkMode },
        { "highlightColors", inkColors },
        { "savedColors", savedColors },
        { "favorites", favorites },
        { "embedFiles", embedFiles },
        { "defaultPreset", defaultPreset },
        { "recentPresets", recentPresets },
        { "checkUpdates", checkUpdates },
        { "dismissedUpdate", dismissedUpdate },
        { "cabImages", cabImages },
        { "pedalImages", pedalImages },
        { "ampImages", ampImages },
        { "blockDefaults", blockDefaults },
        { "programPresets", programPresetsJson(programPresets) },
    }.dump(2);
}

Settings Settings::fromJson(std::string_view text)
{
    Settings settings;
    try
    {
        const json root = json::parse(text);
        if (!root.is_object())
            return settings;
        auto get = [&root](const char* key, auto& value) {
            const auto it = root.find(key);
            if (it == root.end())
                return;
            try
            {
                using T = std::remove_reference_t<decltype(value)>;
                if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
                    value = jsonInt<T>(*it);
                else
                    value = it->get<T>();
            }
            catch (const json::exception&)
            {
            }
        };
        get("namDir", settings.namDir);
        get("irDir", settings.irDir);
        get("a2Lite", settings.a2Lite);
        get("normalize", settings.normalize);
        get("parallel", settings.parallel);
        get("accent", settings.accent);
        get("accentColor", settings.accentColor);
        get("highlightStyle", settings.inkMode);
        get("highlightColors", settings.inkColors);
        get("savedColors", settings.savedColors);
        get("favorites", settings.favorites);
        get("embedFiles", settings.embedFiles);
        get("defaultPreset", settings.defaultPreset);
        get("recentPresets", settings.recentPresets);
        get("checkUpdates", settings.checkUpdates);
        get("dismissedUpdate", settings.dismissedUpdate);
        get("cabImages", settings.cabImages);
        get("pedalImages", settings.pedalImages);
        get("ampImages", settings.ampImages);
        if (const auto programs = root.find("programPresets"); programs != root.end() && programs->is_array())
        {
            for (const json& item : *programs)
            {
                try
                {
                    const int program = jsonInt<int>(item.at("program"));
                    const std::string path = item.at("path").get<std::string>();
                    if (program >= 0 && program <= 127 && !path.empty())
                        settings.programPresets[program] = path;
                }
                catch (const json::exception&)
                {
                }
            }
        }
        if (const auto defaults = root.find("blockDefaults"); defaults != root.end())
        {
            try
            {
                std::map<std::string, std::map<std::string, float>> read;
                if (!defaults->is_object())
                    throw json::type_error::create(302, "blockDefaults isn't an object", nullptr);
                for (const auto& [typeId, controls] : defaults->items())
                {
                    if (!controls.is_object())
                        throw json::type_error::create(302, "a block's defaults aren't an object", nullptr);
                    for (const auto& [control, value] : controls.items())
                        read[typeId][control] = jsonFloat(value);
                }
                settings.blockDefaults = std::move(read);
            }
            catch (const json::exception&)
            {
            }
        }
    }
    catch (const json::exception&)
    {
    }
    return settings;
}

void Settings::addRecentPreset(const std::string& path)
{
    std::erase(recentPresets, path);
    recentPresets.insert(recentPresets.begin(), path);
    if (recentPresets.size() > kMaxRecentPresets)
        recentPresets.resize(kMaxRecentPresets);
}

int Settings::programFor(const std::string& presetPath) const
{
    for (const auto& [program, path] : programPresets)
    {
        if (path == presetPath)
            return program;
    }
    return -1;
}

void Settings::assignProgram(const std::string& presetPath, int program)
{
    std::erase_if(programPresets, [&presetPath](const auto& entry) { return entry.second == presetPath; });
    if (program >= 0 && program <= 127 && !presetPath.empty())
        programPresets[program] = presetPath;
}

std::filesystem::path Settings::defaultPath()
{
    return configDir() / "settings.json";
}

Settings Settings::load(const std::filesystem::path& path)
{
    const std::optional<std::string> text = readFile(path);
    return text ? fromJson(*text) : Settings {};
}

bool Settings::save(const std::filesystem::path& path) const
{
    return writeFileAtomic(path, toJson());
}

} // namespace xythos
