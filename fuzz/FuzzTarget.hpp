// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

// Fuzz targets: each fuzz/*.cpp defines one with XYTHOS_FUZZ_TARGET(name) { ... } over (data, size).
//
// Built two ways (see fuzz/README.md):
// - with XYTHOS_FUZZ=ON (clang): one libFuzzer binary per target, xythos-fuzz-<name>, with ASan and UBSan;
// - always, with the tests: xythos-fuzz-replay, every target in one binary without libFuzzer. It replays the seed
//   corpus in fuzz/corpus/<name>/ and a fixed number of deterministic mutations of it (ctest runs it, so every CI
//   platform and compiler covers the targets), and reproduces a crash file from a libFuzzer run anywhere.
//
// A target returns 0. A broken invariant calls FUZZ_CHECK, which aborts with a message, so both builds report it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace xythos::fuzz {

using TargetFn = int (*)(const uint8_t* data, size_t size);

bool registerTarget(const char* name, TargetFn fn);

// A private temporary folder for targets that read or write files. The first call also points the plugin's config
// and cache folders (XYTHOS_CONFIG_DIR, XYTHOS_CACHE_DIR) into it, so a run never touches the user's own.
const std::string& scratchDir();

// Writes `bytes` to a file in scratchDir() named `name`, and returns its path (UTF-8)
std::string writeScratchFile(const std::string& name, std::string_view bytes);

[[noreturn]] inline void checkFailed(const char* what, const char* file, int line)
{
    std::fprintf(stderr, "FUZZ_CHECK failed: %s (%s:%d)\n", what, file, line);
    std::fflush(stderr);
    std::abort();
}

// Reads typed values from the fuzzer's bytes; runs out gracefully (zeros) when they're used up
class Input {
public:
    Input(const uint8_t* data, size_t size) : fData(data), fSize(size) {}

    bool empty() const { return fPos >= fSize; }
    size_t remaining() const { return fSize - fPos; }

    uint8_t byte() { return fPos < fSize ? fData[fPos++] : 0; }
    bool flag() { return (byte() & 1) != 0; }

    uint32_t u32()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = v << 8 | byte();
        return v;
    }

    // In [lo, hi]
    int range(int lo, int hi)
    {
        if (hi <= lo)
            return lo;
        const uint32_t span = static_cast<uint32_t>(hi - lo) + 1;
        const uint32_t raw = span <= 256 ? byte() : (span <= 65536 ? (byte() << 8 | byte()) : u32());
        return lo + static_cast<int>(raw % span);
    }

    // In [0, 1]
    float unit() { return static_cast<float>(u32() & 0xffffff) / static_cast<float>(0xffffff); }

    // Any float bit pattern: NaN, infinities, denormals included
    float anyFloat()
    {
        const uint32_t bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }

    std::string text(size_t maxLength)
    {
        const size_t n = std::min<size_t>(range(0, static_cast<int>(maxLength)), remaining());
        std::string s(reinterpret_cast<const char*>(fData + fPos), n);
        fPos += n;
        return s;
    }

    std::string_view rest()
    {
        std::string_view s(reinterpret_cast<const char*>(fData + fPos), remaining());
        fPos = fSize;
        return s;
    }

private:
    const uint8_t* fData;
    size_t fSize;
    size_t fPos = 0;
};

} // namespace xythos::fuzz

#define FUZZ_CHECK(cond)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
            ::xythos::fuzz::checkFailed(#cond, __FILE__, __LINE__);                                                   \
    } while (false)

// Registers the target: the replay binary keeps them all; a libFuzzer binary (built with XYTHOS_FUZZ_ONLY="name")
// runs the one it's named after from LLVMFuzzerTestOneInput (FuzzSupport.cpp)
#define XYTHOS_FUZZ_TARGET(name)                                                                                      \
    static int xythosFuzz_##name(const uint8_t* data, size_t size);                                                   \
    [[maybe_unused]] static const bool xythosFuzzRegistered_##name =                                                  \
        ::xythos::fuzz::registerTarget(#name, xythosFuzz_##name);                                                     \
    static int xythosFuzz_##name(const uint8_t* data, size_t size)
