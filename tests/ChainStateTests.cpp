// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "state/ChainState.hpp"
#include "engine/BlockRegistry.hpp"
#include "engine/blocks/SynthBlocks.hpp"
#include "engine/looper/Looper.hpp"
#include "state/ChainMigration.hpp"

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

TEST_CASE("Program Change mappings round-trip, sit beside CCs, and only switch things")
{
    ChainState chain;
    chain.blocks.push_back({ "test.gain", 0 });
    chain.blocks.push_back({ "looper", 1 });
    chain.setMidiMapping({ 0, 3, slotBypassParam(0), true }); // PC 3 -> the gain block on/off
    chain.setMidiMapping({ 0, 3, slotParam(0, 0) });          // CC 3 is another message: both stay
    REQUIRE(chain.midi.size() == 2);
    CHECK(chain.midiMappingFor(slotBypassParam(0))->program);
    CHECK_FALSE(chain.midiMappingFor(slotParam(0, 0))->program);

    const std::optional<ChainState> back = ChainState::fromJson(chain.toJson());
    REQUIRE(back);
    CHECK(*back == chain);
    // a CC mapping's JSON is as before (no "pc")
    CHECK(chain.toJson().find("\"pc\":true") != std::string::npos);
    ChainState ccOnly;
    ccOnly.setMidiMapping({ 0, 7, kParamOutput });
    CHECK(ccOnly.toJson().find("pc") == std::string::npos);

    // the same PC again takes it from the other parameter
    chain.setMidiMapping({ 0, 3, kParamLimiter, true });
    CHECK(chain.midiMappingFor(slotBypassParam(0)) == nullptr);
    CHECK(chain.midiMappingFor(slotParam(0, 0)) != nullptr);

    // what a Program Change does: bypass and switches flip, footswitch controls are pressed, knobs take none
    const BlockRegistry& registry = BlockRegistry::builtin();
    ChainState looper;
    looper.blocks.push_back({ "looper", 4 });
    CHECK(looper.programAction(slotBypassParam(4), registry) == ProgramAction::Flip);
    CHECK(looper.programAction(slotParam(4, 0), registry) == ProgramAction::Press);  // Rec/Dub
    CHECK(looper.programAction(slotParam(4, 5), registry) == ProgramAction::Flip);   // Reverse
    CHECK(looper.programAction(slotParam(4, 3), registry) == ProgramAction::None);   // Level
    CHECK(looper.programAction(slotParam(4, 7), registry) == ProgramAction::None);   // Mode (a choice)
    CHECK(looper.programAction(slotBypassParam(5), registry) == ProgramAction::None); // an empty slot
    CHECK(looper.programAction(kParamLimiter, registry) == ProgramAction::Flip);
    CHECK(looper.programAction(kParamInput, registry) == ProgramAction::None);
    CHECK(looper.programAction(looperParam(kLooperParamRecDub), registry) == ProgramAction::Press);
    CHECK(looper.programAction(looperParam(kLooperParamTap), registry) == ProgramAction::Press);
    CHECK(looper.programAction(looperParam(kLooperParamReverse), registry) == ProgramAction::Flip);
    CHECK(looper.programAction(looperParam(kLooperParamBpm), registry) == ProgramAction::None);
    CHECK(looper.programAction(kParamOutPeakL, registry) == ProgramAction::None);

    CHECK(flippedValue(slotBypassParam(0), 0.0f) == 1.0f);
    CHECK(flippedValue(slotBypassParam(0), 1.0f) == 0.0f);
    CHECK(flippedValue(kParamLimiter, 1.0f) == 0.0f);
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

TEST_CASE("a Poly Chip (removed) becomes a Chip Synth in Poly, keeping its settings and MIDI mappings")
{
    // as the Poly Chip saved it: Wave, Voices, Focus, Arp, Arp Rate, Release, Crush, Octave, Mix, Quantize,
    // Pick Filter, Track (normalized)
    ChainState saved;
    BlockState old;
    old.typeId = kRetiredPolyChipType;
    old.slot = 3;
    old.bypass = true;
    old.config = { { "lookahead", 10.0 } };
    const float values[] = { 0.333f, 0.6f, 0.7f, 0.5f, 0.4f, 0.2f, 0.75f, 0.666f, 0.8f, 1.0f, 0.25f, 0.0f };
    for (size_t p = 0; p < std::size(values); ++p)
        old.params[p] = values[p];
    saved.blocks.push_back(old);
    saved.midi.push_back({ 0, 20, slotParam(3, 6) });
    saved.midi.push_back({ 0, 21, slotParam(3, 1) });
    saved.midi.push_back({ 0, 22, slotBypassParam(3) });
    std::optional<ChainState> chain = ChainState::fromJson(saved.toJson());
    REQUIRE(chain);
    CHECK_FALSE(chain->validate(BlockRegistry::builtin()));
    const std::vector<int> slots = retirePolyChips(*chain);
    REQUIRE(slots == std::vector<int> { 3 });
    CHECK(chain->validate(BlockRegistry::builtin()));
    const BlockState& block = chain->blocks[0];
    CHECK(block.typeId == kChipSynthBlockDescriptor.typeId);
    CHECK(block.bypass);
    CHECK(polyFromConfig(block.config));
    CHECK(block.config.value("lookahead", 0.0) == 10.0);
    CHECK(block.params[kChipParamWave] == doctest::Approx(0.333f));
    CHECK(block.params[kChipParamCrush] == doctest::Approx(0.75f));
    CHECK(block.params[kChipParamOctave] == doctest::Approx(0.666f));
    CHECK(block.params[kChipParamMix] == doctest::Approx(0.8f));
    CHECK(block.params[kChipParamQuantize] == doctest::Approx(1.0f));
    CHECK(block.params[kChipParamPickFilter] == doctest::Approx(0.25f));
    CHECK(block.params[kChipParamTrack] == doctest::Approx(0.0f));
    CHECK(block.params[kChipParamArp] == doctest::Approx(0.0f));   // Off
    CHECK(block.params[kChipParamVibrato] == doctest::Approx(0.0f)); // the Poly Chip had none
    // Crush's mapping follows it, Voices' goes, the bypass's stays
    REQUIRE(chain->midi.size() == 2);
    CHECK(chain->midi[0].param == slotParam(3, kChipParamCrush));
    CHECK(chain->midi[1].param == slotBypassParam(3));
}
