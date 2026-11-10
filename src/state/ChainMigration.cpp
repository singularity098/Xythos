// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/ChainMigration.hpp"

#include "engine/blocks/LooperBlock.hpp"
#include "engine/blocks/NamBlock.hpp"
#include "engine/blocks/SynthBlocks.hpp"

namespace xythos {

namespace {

constexpr Migration kMigrations[] = {
    { &kNamBlockDescriptor, kNamToneKey, kNamParamBass, kNamParamTreble }, // NamBlock.hpp
};

} // namespace

void setMigratedDefaults(BlockState& block, const Migration& migration)
{
    for (int p = migration.first; p <= migration.last; ++p)
    {
        const ParamSpec& spec = migration.type->params[static_cast<size_t>(p)];
        block.params[static_cast<size_t>(p)] = spec.toNormalized(spec.def);
    }
}

std::vector<MigratedSlot> migrateBlocks(ChainState& chain)
{
    std::vector<MigratedSlot> changed;
    for (BlockState& block : chain.blocks)
    {
        for (const Migration& migration : kMigrations)
        {
            if (block.typeId != migration.type->typeId || block.config.contains(migration.key))
                continue;
            setMigratedDefaults(block, migration);
            block.config[migration.key] = true;
            changed.push_back({ static_cast<int>(block.slot), &migration });
        }
    }
    return changed;
}

std::optional<LooperSettings> retireLoopers(ChainState& chain)
{
    std::optional<LooperSettings> settings;
    for (int i = static_cast<int>(chain.blocks.size()) - 1; i >= 0; --i)
    {
        const BlockState& block = chain.blocks[static_cast<size_t>(i)];
        if (block.typeId != kLooperBlockDescriptor.typeId)
            continue;
        LooperSettings values {};
        for (int p = 0; p < kLooperParamCount; ++p)
            values[static_cast<size_t>(p)] = kLooperBlockDescriptor.params[static_cast<size_t>(p)].toValue(block.params[static_cast<size_t>(p)]);
        if (!block.config.contains(kLooperCountInKey))
        {
            for (int p = kLooperParamMode; p <= kLooperParamClick; ++p)
                values[static_cast<size_t>(p)] = kLooperBlockDescriptor.params[static_cast<size_t>(p)].def;
            values[kLooperParamMode] = 1.0f; // it was a Free looper
        }
        if (!block.config.contains(kLooperLatencyKey))
            values[kLooperParamLatency] = kLooperBlockDescriptor.params[kLooperParamLatency].def;
        settings = values; // the first one's, as the loop goes from the last to the first
        for (MidiMapping& mapping : chain.midi)
            for (int p = 0; p < kLooperParamCount; ++p)
                if (mapping.param == slotParam(block.slot, p))
                    mapping.param = looperParam(p);
        chain.remove(i);
    }
    return settings;
}

std::vector<int> retirePolyChips(ChainState& chain)
{
    // the Poly Chip's controls, in its slot order, and where each went on the Chip Synth (-1: gone)
    constexpr int kWent[] = {
        kChipParamWave,       // Wave
        -1,                   // Voices
        -1,                   // Focus
        -1,                   // Arp (Off, Up, Down: not the Chip Synth's)
        -1,                   // Arp Rate
        -1,                   // Release
        kChipParamCrush,      // Crush
        kChipParamOctave,     // Octave
        kChipParamMix,        // Mix
        kChipParamQuantize,   // Quantize
        kChipParamPickFilter, // Pick Filter
        kChipParamTrack,      // Track
    };
    constexpr int kOldCount = static_cast<int>(sizeof(kWent) / sizeof(kWent[0]));
    const BlockDescriptor& chip = kChipSynthBlockDescriptor;
    std::vector<int> changed;
    for (BlockState& block : chain.blocks)
    {
        if (block.typeId != kRetiredPolyChipType)
            continue;
        std::array<float, kParamsPerSlot> params {};
        for (int p = 0; p < chip.numParams; ++p)
        {
            const ParamSpec& spec = chip.params[static_cast<size_t>(p)];
            params[static_cast<size_t>(p)] = spec.toNormalized(spec.def);
        }
        params[kChipParamVibrato] = chip.params[kChipParamVibrato].toNormalized(0.0f);
        for (int p = 0; p < kOldCount; ++p)
            if (kWent[p] >= 0)
                params[static_cast<size_t>(kWent[p])] = block.params[static_cast<size_t>(p)];
        block.params = params;
        block.typeId = chip.typeId;
        if (!block.config.is_object())
            block.config = nlohmann::json::object();
        block.config["poly"] = true;
        block.latched = 0;
        // MIDI mappings follow the controls; ones to controls that went go too
        for (size_t m = chain.midi.size(); m-- > 0;)
        {
            MidiMapping& mapping = chain.midi[m];
            for (int p = 0; p < kParamsPerSlot; ++p)
            {
                if (mapping.param != slotParam(block.slot, p))
                    continue;
                const int to = p < kOldCount ? kWent[p] : -1;
                if (to >= 0)
                    mapping.param = slotParam(block.slot, to);
                else
                    chain.midi.erase(chain.midi.begin() + static_cast<std::ptrdiff_t>(m));
                break;
            }
        }
        changed.push_back(block.slot);
    }
    return changed;
}

} // namespace xythos
