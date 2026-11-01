// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "FuzzTarget.hpp"

#include "state/Paths.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace xythos::fuzz {

namespace fs = std::filesystem;

namespace {

void setEnv(const char* name, const std::string& value)
{
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

} // namespace

const std::string& scratchDir()
{
    static const std::string dir = [] {
        std::random_device random;
        const fs::path path = fs::temp_directory_path() / ("xythos-fuzz-" + std::to_string(random()));
        fs::create_directories(path / "config");
        fs::create_directories(path / "cache");
        fs::create_directories(path / "files");
        setEnv("XYTHOS_CONFIG_DIR", pathToUtf8(path / "config"));
        setEnv("XYTHOS_CACHE_DIR", pathToUtf8(path / "cache"));
        return pathToUtf8(path);
    }();
    return dir;
}

std::string writeScratchFile(const std::string& name, std::string_view bytes)
{
    const fs::path path = pathFromUtf8(scratchDir()) / "files" / pathFromUtf8(name);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return pathToUtf8(path);
}

} // namespace xythos::fuzz

#ifdef XYTHOS_FUZZ_LIBFUZZER
namespace {
xythos::fuzz::TargetFn gTarget = nullptr;
}

bool xythos::fuzz::registerTarget(const char* name, TargetFn fn)
{
    if (std::string_view(name) == XYTHOS_FUZZ_ONLY)
        gTarget = fn;
    return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    return gTarget(data, size);
}
#endif
