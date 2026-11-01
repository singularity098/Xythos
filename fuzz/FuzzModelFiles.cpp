// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// NAM models (.nam) and impulse responses (.wav) from the user's library or a shared preset: loaded the way the NAM
// and IR blocks load them (NAM Core's parsers, then our resampling and normalization), then a few buffers through
// the block. Loading may fail; it must not crash, and a block that loaded must stay finite.

#include "FuzzTarget.hpp"

#include "engine/BlockRegistry.hpp"
#include "state/Library.hpp"

#include <cmath>
#include <vector>

using namespace xythos;

namespace {

void loadAndRun(const char* typeId, const std::string& path, double rate, bool mustBeFinite)
{
    const BlockDescriptor* type = BlockRegistry::builtin().find(typeId);
    FUZZ_CHECK(type != nullptr);
    std::unique_ptr<Block> block = type->create();
    constexpr uint32_t kFrames = 64;
    block->prepare(rate, kFrames);
    if (!block->configure(nlohmann::json { { "path", path } }))
        return;
    std::vector<float> left(kFrames), right(kFrames);
    for (int buffer = 0; buffer < 4; ++buffer)
    {
        for (uint32_t i = 0; i < kFrames; ++i)
        {
            left[i] = buffer == 0 && i == 0 ? 1.0f : 0.25f * std::sin(0.05f * static_cast<float>(i + buffer * kFrames));
            right[i] = left[i];
        }
        float* io[2] = { left.data(), right.data() };
        block->process(io, 2, kFrames);
    }
    // a loaded IR gives a finite output. (A NAM model's output is the model's: one that blows up is caught by the
    // engine's chain fault, Engine::takeChainFault().)
    for (uint32_t i = 0; mustBeFinite && i < kFrames; ++i)
        FUZZ_CHECK(std::isfinite(left[i]) && std::isfinite(right[i]));
}

} // namespace

XYTHOS_FUZZ_TARGET(nam_model)
{
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    (void)describeNamFile(bytes);
    loadAndRun("nam", fuzz::writeScratchFile("model.nam", bytes), 48000.0, false);
    return 0;
}

XYTHOS_FUZZ_TARGET(ir_wav)
{
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    loadAndRun("ir", fuzz::writeScratchFile("ir.wav", bytes), 48000.0, true);
    return 0;
}
