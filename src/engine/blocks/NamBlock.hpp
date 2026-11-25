// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// A local .nam model (A1 or A2).
//
// Config: {"path": "/abs/file.nam", "a2Size": 1.0, "tone": true}. a2Size is 1.0 for A2-Full and 0.0 for A2-Lite; it's
// config rather than a host parameter because switching it isn't real-time safe. Models that aren't slimmable ignore it.
//
// Bass, Middle and Treble shape the model's output (the global tone stack's EQ), for sculpting each side of a split.
// They came after the first NAM blocks, whose unused slots saved 0 (-12 dB): a block without "tone" in its config
// stays flat whatever they're set to, and the UI moves such blocks to flat controls and adds "tone"
// (migrateBlocks()).
//
// Not done yet: resampling when the host rate differs from the model's training rate (the model runs at the host
// rate, which changes its tone slightly). See docs/Architecture.md 4.5.
extern const BlockDescriptor kNamBlockDescriptor;

enum NamParam {
    kNamParamInput,     // dB
    kNamParamOutput,    // dB
    kNamParamNormalize, // toggle
    kNamParamBass,      // dB
    kNamParamMiddle,    // dB
    kNamParamTreble,    // dB
    kNamParamCount
};

// The config key that marks a NAM block as having its tone controls
constexpr const char* kNamToneKey = "tone";

} // namespace xythos
