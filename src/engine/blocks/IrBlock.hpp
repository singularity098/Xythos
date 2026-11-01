// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// A cab impulse response from a local WAV file (16/24/32-bit PCM or 32-bit float, mono or stereo).
// Zero-latency partitioned convolution. If the file's sample rate differs from the host's, the IR is resampled.
//
// Config: {"path": "/abs/file.wav"}
extern const BlockDescriptor kIrBlockDescriptor;

enum IrParam {
    kIrParamLevel,     // dB
    kIrParamMix,       // %
    kIrParamLowCut,    // Hz
    kIrParamHighCut,   // Hz
    kIrParamNormalize, // toggle
    kIrParamCount
};

// Longest IR used; longer files are truncated
constexpr double kIrMaxSeconds = 10.0;
// Samples past this (+60 dB) mark a damaged file, which isn't loaded
constexpr float kIrMaxPeak = 1000.0f;

} // namespace xythos
