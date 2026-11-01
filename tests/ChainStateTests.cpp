// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "state/ChainState.hpp"

using namespace xythos;

TEST_CASE("chain state round-trips through JSON")
{
    ChainState chain;
    BlockState block;
    block.typeId = "test.gain";
    block.slot = 3;
    block.config = { { "file", "My Amp.nam" } };
    block.params[0] = 0.25f;
    block.bypass = true;
    chain.blocks.push_back(block);
    block.slot = 4;
    block.image = "My Pedal.png";
    block.latched = 0b10;
    chain.blocks.push_back(block);

    const std::optional<ChainState> parsed = ChainState::fromJson(chain.toJson());
    REQUIRE(parsed);
    CHECK(*parsed == chain);

    // a preset being loaded says so; nothing else does
    CHECK(chain.toJson().find("restoreLatches") == std::string::npos);
    chain.restoreLatches = true;
    const std::optional<ChainState> loading = ChainState::fromJson(chain.toJson());
    REQUIRE(loading);
    CHECK(loading->restoreLatches);
}

TEST_CASE("empty state is an empty chain; bad JSON is rejected")
{
    const std::optional<ChainState> empty = ChainState::fromJson("");
    REQUIRE(empty);
    CHECK(empty->blocks.empty());

    std::string error;
    CHECK_FALSE(ChainState::fromJson("{not json", &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(ChainState::fromJson(R"({"version": 99, "blocks": []})"));
}

TEST_CASE("validation catches bad slots, unknown types and too many blocks")
{
    const BlockRegistry& registry = testing::testRegistry();

    ChainState chain;
    chain.blocks.push_back({ "test.gain", 0 });
    CHECK(chain.validate(registry));

    chain.blocks.push_back({ "test.gain", 0 });
    CHECK_FALSE(chain.validate(registry));

    chain.blocks.back().slot = kMaxBlocks;
    CHECK_FALSE(chain.validate(registry));

    chain.blocks.back() = { "no.such.block", 1 };
    CHECK_FALSE(chain.validate(registry));

    ChainState full;
    for (int i = 0; i <= kMaxBlocks; ++i)
        full.blocks.push_back({ "test.gain", i % kMaxBlocks });
    CHECK_FALSE(full.validate(registry));
}

TEST_CASE("freeSlot finds the lowest unused slot")
{
    ChainState chain;
    CHECK(chain.freeSlot() == 0);
    chain.blocks.push_back({ "test.gain", 0 });
    chain.blocks.push_back({ "test.gain", 2 });
    CHECK(chain.freeSlot() == 1);

    ChainState full;
    for (int i = 0; i < kMaxBlocks; ++i)
        full.blocks.push_back({ "test.gain", i });
    CHECK(full.freeSlot() == -1);
}

TEST_CASE("MIDI mappings and the preset name round-trip and validate")
{
    ChainState chain;
    chain.blocks.push_back({ "test.gain", 0 });
    chain.preset = "/presets/Clean.xythos";
    chain.setMidiMapping({ 0, 7, slotParam(0, 0) });
    chain.setMidiMapping({ 2, 11, kParamOutput });
    REQUIRE(chain.midi.size() == 2);

    const std::optional<ChainState> back = ChainState::fromJson(chain.toJson());
    REQUIRE(back);
    CHECK(*back == chain);
    CHECK(back->validate(testing::testRegistry()));

    // one mapping per CC and per parameter
    chain.setMidiMapping({ 0, 7, kParamInput });
    CHECK(chain.midi.size() == 2);
    CHECK(chain.midiMappingFor(slotParam(0, 0)) == nullptr);
    chain.setMidiMapping({ 5, 5, kParamInput });
    CHECK(chain.midi.size() == 2);
    CHECK(chain.midiMappingFor(kParamInput)->cc == 5);
    chain.clearMidiMappings(kParamInput);
    CHECK(chain.midiMappingFor(kParamInput) == nullptr);

    // outputs can't be mapped
    chain.midi.push_back({ 0, 1, kParamOutPeakL });
    CHECK_FALSE(chain.validate(testing::testRegistry()));
}

namespace {

ChainState chainOf(int count)
{
    ChainState chain;
    for (int i = 0; i < count; ++i)
        chain.blocks.push_back({ "test.gain", i });
    return chain;
}

std::vector<int> slots(const ChainState& chain, Section section)
{
    std::vector<int> result;
    for (int index : chain.sectionIndices(section))
        result.push_back(chain.blocks[index].slot);
    return result;
}

} // namespace

TEST_CASE("a split round-trips through JSON, and version 1 chains still load")
{
    ChainState chain = chainOf(4);
    chain.addSplit(1, 3);
    chain.blocks[2].branch = kBranchB;
    const std::optional<ChainState> parsed = ChainState::fromJson(chain.toJson());
    REQUIRE(parsed);
    CHECK(*parsed == chain);
    CHECK(parsed->validate(testing::testRegistry()));

    const std::optional<ChainState> v1 =
        ChainState::fromJson(R"({"version": 1, "blocks": [{"type": "test.gain", "slot": 0}]})");
    REQUIRE(v1);
    CHECK(v1->blocks.size() == 1);
    CHECK_FALSE(v1->split);
}

TEST_CASE("split validation")
{
    const BlockRegistry& registry = testing::testRegistry();
    ChainState chain = chainOf(3);
    chain.split = SplitState { 1, 4 };
    CHECK_FALSE(chain.validate(registry));
    chain.split = SplitState { 2, 1 };
    CHECK_FALSE(chain.validate(registry));
    chain.split = SplitState { 1, 2 };
    CHECK(chain.validate(registry));
    chain.blocks[0].branch = kBranchB; // outside the split
    CHECK_FALSE(chain.validate(registry));
    chain.blocks[0].branch = kBranchA;
    chain.blocks[1].branch = 2;
    CHECK_FALSE(chain.validate(registry));
}

TEST_CASE("sections of a chain with and without a split")
{
    ChainState chain = chainOf(5);
    CHECK(slots(chain, Section::Pre) == std::vector<int> { 0, 1, 2, 3, 4 });
    CHECK(slots(chain, Section::A).empty());

    chain.addSplit(1, 4);
    chain.blocks[2].branch = kBranchB;
    CHECK(slots(chain, Section::Pre) == std::vector<int> { 0 });
    CHECK(slots(chain, Section::A) == std::vector<int> { 1, 3 });
    CHECK(slots(chain, Section::B) == std::vector<int> { 2 });
    CHECK(slots(chain, Section::Post) == std::vector<int> { 4 });
}

TEST_CASE("inserting, removing and moving blocks keeps the split consistent")
{
    ChainState chain = chainOf(2);
    chain.addSplit(1, 1); // empty branches between slot 0 and slot 1

    chain.insert(Section::B, 0, { "test.gain", 10 });
    chain.insert(Section::A, 0, { "test.gain", 11 });
    chain.insert(Section::A, 99, { "test.gain", 12 });
    chain.insert(Section::Pre, 0, { "test.gain", 13 });
    chain.insert(Section::Post, 99, { "test.gain", 14 });
    CHECK(chain.validate(testing::testRegistry()));
    CHECK(slots(chain, Section::Pre) == std::vector<int> { 13, 0 });
    CHECK(slots(chain, Section::A) == std::vector<int> { 11, 12 });
    CHECK(slots(chain, Section::B) == std::vector<int> { 10 });
    CHECK(slots(chain, Section::Post) == std::vector<int> { 1, 14 });

    // move the Pre block 0 to the end of branch B, then branch A's first block to after the merge
    chain.move(1, Section::B, 1);
    CHECK(slots(chain, Section::Pre) == std::vector<int> { 13 });
    CHECK(slots(chain, Section::B) == std::vector<int> { 10, 0 });
    chain.move(chain.sectionIndices(Section::A)[0], Section::Post, 0);
    CHECK(slots(chain, Section::A) == std::vector<int> { 12 });
    CHECK(slots(chain, Section::Post) == std::vector<int> { 11, 1, 14 });

    // moving within a section, to a later position
    chain.move(chain.sectionIndices(Section::Post)[0], Section::Post, 3);
    CHECK(slots(chain, Section::Post) == std::vector<int> { 1, 14, 11 });

    const BlockState removed = chain.remove(chain.sectionIndices(Section::B)[0]);
    CHECK(removed.slot == 10);
    CHECK(removed.branch == kBranchA);
    CHECK(slots(chain, Section::B) == std::vector<int> { 0 });
    CHECK(chain.validate(testing::testRegistry()));

    // removing the split makes the branches serial: A then B
    chain.removeSplit();
    CHECK_FALSE(chain.split);
    CHECK(slots(chain, Section::Pre) == std::vector<int> { 13, 12, 0, 1, 14, 11 });
    CHECK(chain.validate(testing::testRegistry()));
}

TEST_CASE("numbers past the float range load clamped, so the chain still saves and loads again")
{
    const std::optional<ChainState> chain = ChainState::fromJson(
        R"({"version": 1, "blocks": [{"type": "delay", "slot": 0, "params": [1e300, -1e300, 0.5]}]})");
    REQUIRE(chain);
    CHECK(chain->blocks[0].params[0] == FLT_MAX);
    CHECK(chain->blocks[0].params[1] == -FLT_MAX);
    const std::optional<ChainState> again = ChainState::fromJson(chain->toJson());
    REQUIRE(again);
    CHECK(*again == *chain);
}

TEST_CASE("whole numbers written as huge decimals don't load, rather than overflowing")
{
    CHECK_FALSE(ChainState::fromJson(R"({"version": 3e31, "blocks": []})"));
    CHECK_FALSE(ChainState::fromJson(R"({"version": 1, "blocks": [{"type": "delay", "slot": -1e300}]})"));
    CHECK_FALSE(ChainState::fromJson(R"({"version": 1, "blocks": [], "midi": [{"channel": 0, "cc": 7, "param": -5.0}]})"));
    // a decimal that fits still reads, as before
    const std::optional<ChainState> chain = ChainState::fromJson(R"({"version": 1.0, "blocks": [{"type": "delay", "slot": 3.0}]})");
    REQUIRE(chain);
    CHECK(chain->blocks[0].slot == 3);
}
