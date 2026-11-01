// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// A preset's preset.json (shared presets come from anyone): parsing never crashes, and a parsed preset survives a
// round trip unchanged.

#include "FuzzTarget.hpp"

#include "state/Preset.hpp"

using namespace xythos;

XYTHOS_FUZZ_TARGET(preset_json)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    std::string error;
    const std::optional<Preset> preset = Preset::fromJson(text, &error);
    if (!preset)
        return 0;
    const std::string json = preset->toJson();
    const std::optional<Preset> again = Preset::fromJson(json, &error);
    FUZZ_CHECK(again.has_value());
    FUZZ_CHECK(again->toJson() == json);
    return 0;
}
