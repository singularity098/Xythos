// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "engine/BlockRegistry.hpp"
#include "engine/Params.hpp"
#include "ui/ParamHelp.hpp"

#include <cstring>
#include <string>

using namespace xythos;

namespace {

// Long enough to explain what the control does and how to use it, not just restate its name
constexpr size_t kMinHelpLength = 80;

} // namespace

TEST_CASE("every block type has help")
{
    for (const BlockDescriptor* type : BlockRegistry::builtin().types())
    {
        const char* help = blockTypeHelp(type->typeId);
        CAPTURE(std::string(type->typeId));
        REQUIRE(help != nullptr);
        CHECK(std::strlen(help) >= 2 * kMinHelpLength);
    }
}

TEST_CASE("every block control has help")
{
    for (const BlockDescriptor* type : BlockRegistry::builtin().types())
    {
        for (int i = 0; i < type->numParams; ++i)
        {
            const ParamSpec& spec = type->params[static_cast<size_t>(i)];
            const char* help = blockParamHelp(type->typeId, spec.name);
            CAPTURE(std::string(type->typeId) + " / " + spec.name);
            REQUIRE(help != nullptr);
            CHECK(std::strlen(help) >= kMinHelpLength);
        }
    }
}

TEST_CASE("every global control has help")
{
    for (uint32_t index = 0; index < kParamCount; ++index)
    {
        if (isSlotParam(index))
            continue;
        const GlobalParamSpec& spec = globalParamSpec(index);
        // outputs and internal parameters aren't controls
        if (spec.flags & kParamFlagOutput)
            continue;
        if (index == kParamChainRevision || index == kParamMidiLearn || index == kParamLooperWake)
            continue;
        const char* help = globalParamHelp(index);
        CAPTURE(std::string(spec.name));
        REQUIRE(help != nullptr);
        CHECK(std::strlen(help) >= kMinHelpLength);
    }
}
