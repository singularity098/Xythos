// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// Hum Eliminator (owner, 2026-11-14): takes a single-coil pickup's mains hum away by learning it and subtracting it
// (dsp/HumCanceller.hpp), so the guitar itself goes through untouched. Goes first in the chain. No config.
extern const BlockDescriptor kHumEliminatorBlockDescriptor;

enum HumEliminatorParam {
    kHumParamAmount, // %: how much of the learnt hum is taken away
    kHumParamAdapt,  // s: how fast the learnt hum follows changes while you're not playing
    kHumParamMains,  // choice: 60 Hz, 50 Hz
    kHumParamCount
};

} // namespace xythos
