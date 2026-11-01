// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Loading a .xythos preset file, which people share: a zip with preset.json, embedded model/IR files and images.
//
// preset_file: the input is the file itself (any bytes: the zip reader sees damaged archives).
// preset_entries: the input is the zip's contents, split at "\n--\n": preset.json, then entry name and entry
// bytes in turn. The target zips them, so the fuzzer works on the paths and JSON rather than on CRCs.
//
// Either way loading must not crash, and the files it extracts must land in the cache's presets folder and the
// user's image folders: never anywhere a preset names.

#include "FuzzTarget.hpp"

#include "state/Paths.hpp"
#include "state/Preset.hpp"

#include <miniz.h>

#include <filesystem>

using namespace xythos;
namespace fs = std::filesystem;

namespace {

bool inside(const fs::path& dir, const fs::path& path)
{
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(dir, ec);
    const fs::path b = fs::weakly_canonical(path, ec);
    auto ai = a.begin(), bi = b.begin();
    for (; ai != a.end(); ++ai, ++bi)
    {
        if (bi == b.end() || *ai != *bi)
            return false;
    }
    return bi != b.end();
}

void load(const std::string& path)
{
    std::string error;
    const std::optional<LoadedPreset> loaded = loadPresetFile(
        path, [](const std::string&) { return std::string(); }, &error);
    if (!loaded)
        return;
    FUZZ_CHECK(loaded->sources.size() == loaded->preset.chain.blocks.size());
    for (size_t i = 0; i < loaded->sources.size(); ++i)
    {
        if (loaded->sources[i] != FileSource::Embedded)
            continue;
        const std::string file = loaded->preset.chain.blocks[i].config.value("path", std::string());
        FUZZ_CHECK(inside(cacheDir() / "presets", pathFromUtf8(file)));
    }
    // the round trip keeps whatever loaded
    const std::string json = loaded->preset.toJson();
    const std::optional<Preset> again = Preset::fromJson(json);
    FUZZ_CHECK(again.has_value() && again->toJson() == json);
}

} // namespace

XYTHOS_FUZZ_TARGET(preset_file)
{
    load(fuzz::writeScratchFile("fuzz.xythos", std::string_view(reinterpret_cast<const char*>(data), size)));
    return 0;
}

XYTHOS_FUZZ_TARGET(preset_entries)
{
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    std::vector<std::string_view> parts;
    for (size_t at = 0;;)
    {
        const size_t next = input.find("\n--\n", at);
        parts.push_back(input.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (next == std::string_view::npos)
            break;
        at = next + 4;
    }

    mz_zip_archive zip {};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        return 0;
    bool ok = mz_zip_writer_add_mem(&zip, "preset.json", parts[0].data(), parts[0].size(), MZ_BEST_SPEED);
    for (size_t i = 1; ok && i + 1 < parts.size(); i += 2)
    {
        const std::string name(parts[i]);
        if (name.empty() || name.find('\0') != std::string::npos)
            continue;
        // miniz refuses some names (absolute, too long); the rest go in as they are
        mz_zip_writer_add_mem(&zip, name.c_str(), parts[i + 1].data(), parts[i + 1].size(), MZ_BEST_SPEED);
    }
    void* buffer = nullptr;
    size_t length = 0;
    if (ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &length))
        load(fuzz::writeScratchFile("entries.xythos", std::string_view(static_cast<const char*>(buffer), length)));
    mz_free(buffer);
    mz_zip_writer_end(&zip);
    return 0;
}
