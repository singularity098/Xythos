// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"
#include "engine/looper/Looper.hpp"
#include "state/ChainState.hpp"

#include <array>
#include <optional>
#include <vector>

namespace xythos {

// What every chain from outside (a project, a preset) goes through before it's used: the UI does it, and the DSP
// too when it loads a preset for a MIDI Program Change itself.

// Controls added to a block type after it was released: blocks saved before them lack the config key, and their
// new controls hold the host's unused values (0), so the DSP ignores them until the key is there
struct Migration {
    const BlockDescriptor* type;
    const char* key;
    int first, last; // the controls added
};

// A block's migration: the slot and which one (a type can have several)
struct MigratedSlot {
    int slot;
    const Migration* migration;
};

// Sets a migrated block's new controls to their defaults (NAM tone: 0 dB, flat)
void setMigratedDefaults(BlockState& block, const Migration& migration);

// Blocks from before their type's later controls get them at their defaults, and the config marks them, so the DSP
// applies them. Returns the migrations done.
std::vector<MigratedSlot> migrateBlocks(ChainState& chain);

// The retired Looper block's settings as the global looper's, in LooperParam order (real values); the footswitches
// are left out
using LooperSettings = std::array<float, kLooperParamCount>;

// Takes Looper blocks out of a chain (owner, 2026-11-01: the looper is global now): MIDI mappings to their controls
// move to the global looper's, and the first one's settings are returned (as it had them: one saved before Count In
// or Latency existed gets Free mode and the default latency, as the DSP ran it)
std::optional<LooperSettings> retireLoopers(ChainState& chain);

} // namespace xythos
