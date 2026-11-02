// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/ChainMigration.hpp"

#include "engine/blocks/LooperBlock.hpp"
#include "engine/blocks/NamBlock.hpp"

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

} // namespace xythos
