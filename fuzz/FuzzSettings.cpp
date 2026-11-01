// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// settings.json (hand edits, other versions, a half-written file) and the update check's cache file and GitHub
// release JSON: never crash, and what's read survives a round trip.

#include "FuzzTarget.hpp"

#include "state/Settings.hpp"
#include "state/UpdateCheck.hpp"

using namespace xythos;

XYTHOS_FUZZ_TARGET(settings)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const Settings settings = Settings::fromJson(text);
    const std::string json = settings.toJson();
    FUZZ_CHECK(Settings::fromJson(json).toJson() == json);
    return 0;
}

XYTHOS_FUZZ_TARGET(update_check)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    const UpdateCache cache = UpdateCache::fromJson(text);
    FUZZ_CHECK(UpdateCache::fromJson(cache.toJson()) == cache);

    if (const std::optional<std::string> version = releaseVersion(text))
        FUZZ_CHECK(parseVersion(*version).has_value());

    // versions: parse, print, parse again; "newer" is a strict order
    if (const std::optional<Version> version = parseVersion(text))
    {
        const std::string printed = versionString(*version);
        FUZZ_CHECK(parseVersion(printed) == version);
        FUZZ_CHECK(!isNewerVersion(printed, printed));
    }
    const size_t half = size / 2;
    const std::string_view a = text.substr(0, half), b = text.substr(half);
    FUZZ_CHECK(!(isNewerVersion(a, b) && isNewerVersion(b, a)));
    (void)updateToShow(cache, a, b);
    return 0;
}
