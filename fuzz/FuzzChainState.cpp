// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The chain state JSON: what the host restores from a project, the chain exchange file and presets hand the DSP.
// Parsing must never crash, and a parsed chain must survive a round trip unchanged.

#include "FuzzTarget.hpp"

#include "engine/BlockRegistry.hpp"
#include "state/ChainState.hpp"

using namespace xythos;

XYTHOS_FUZZ_TARGET(chain_state)
{
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    std::string error;
    const std::optional<ChainState> chain = ChainState::fromJson(text, &error);
    if (!chain)
        return 0;

    const std::string json = chain->toJson();
    const std::optional<ChainState> again = ChainState::fromJson(json, &error);
    FUZZ_CHECK(again.has_value());
    FUZZ_CHECK(again->toJson() == json);

    if (!chain->validate(BlockRegistry::builtin(), &error))
        return 0;

    // a valid chain's editing operations keep it valid
    ChainState edited = *chain;
    const int count = static_cast<int>(edited.blocks.size());
    for (int i = 0; i < count; ++i)
        (void)edited.sectionOf(i);
    for (Section section : { Section::Pre, Section::A, Section::B, Section::Post })
        (void)edited.sectionIndices(section);
    if (count > 0)
    {
        edited.move(count - 1, edited.sectionOf(0), 0);
        FUZZ_CHECK(edited.blocks.size() == static_cast<size_t>(count));
        FUZZ_CHECK(edited.validate(BlockRegistry::builtin(), &error));
        if (!edited.split)
        {
            edited.addSplit(0, count);
            FUZZ_CHECK(edited.validate(BlockRegistry::builtin(), &error));
        }
        edited.removeSplit();
        FUZZ_CHECK(edited.validate(BlockRegistry::builtin(), &error));
        edited.remove(0);
        FUZZ_CHECK(edited.blocks.size() == static_cast<size_t>(count - 1));
        FUZZ_CHECK(edited.validate(BlockRegistry::builtin(), &error));
    }
    return 0;
}
