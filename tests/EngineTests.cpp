// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/BlockRegistry.hpp"
#include "engine/Engine.hpp"
#include "engine/looper/Looper.hpp"
#include "engine/blocks/RhythmBlocks.hpp"

#include <cmath>
#include <thread>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBuffer = 256;

// Runs the engine for `buffers` buffers of a constant input and returns the last output sample per channel
struct Rig {
    Engine engine { testing::testRegistry() };
    std::vector<float> inL = std::vector<float>(kBuffer, 0.25f);
    std::vector<float> inR = std::vector<float>(kBuffer, -0.125f);
    std::vector<float> outL = std::vector<float>(kBuffer, 0.0f);
    std::vector<float> outR = std::vector<float>(kBuffer, 0.0f);

    Rig() { engine.prepare(kRate, kBuffer); }

    void run(int buffers = 40)
    {
        const float* in[2] = { inL.data(), inR.data() };
        float* out[2] = { outL.data(), outR.data() };
        for (int i = 0; i < buffers; ++i)
            engine.process(in, out, kBuffer);
    }

    float left() const { return outL.back(); }
    float right() const { return outR.back(); }
};

ChainState gainChain(int slot, nlohmann::json config = nlohmann::json::object())
{
    ChainState chain;
    BlockState block;
    block.typeId = "test.gain";
    block.slot = slot;
    block.config = std::move(config);
    chain.blocks.push_back(block);
    return chain;
}

// Normalized value of the test block's gain parameter for a gain in dB
float gainParam(float db)
{
    return testing::kGainDescriptor.params[0].toNormalized(db);
}

} // namespace

TEST_CASE("empty chain passes audio through; mono mode copies the left input to both outputs")
{
    Rig rig;
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));
    CHECK(rig.right() == doctest::Approx(0.25f));

    rig.engine.setParameter(kParamInputMode, 1.0f);
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));
    CHECK(rig.right() == doctest::Approx(-0.125f));
}

TEST_CASE("input and output levels")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(kParamInput, 6.0f);
    rig.engine.setParameter(kParamOutput, -12.0f);
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f * dbToGain(-6.0f)).epsilon(0.001));
}

TEST_CASE("a block's slot parameters reach it after the chain swap")
{
    Rig rig;
    rig.engine.setParameter(slotParam(5, 0), gainParam(-6.0f));
    REQUIRE(rig.engine.setChain(gainChain(5)));
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f * dbToGain(-6.0f)).epsilon(0.001));

    // automation of the slot parameter
    rig.engine.setParameter(slotParam(5, 0), gainParam(-12.0f));
    rig.run(2);
    CHECK(rig.left() == doctest::Approx(0.25f * dbToGain(-12.0f)).epsilon(0.001));
}

TEST_CASE("unchanged blocks are reused when the chain is rebuilt")
{
    Rig rig;
    testing::GainBlock::sCreated = 0;
    REQUIRE(rig.engine.setChain(gainChain(0)));
    rig.run(4);
    REQUIRE(rig.engine.setChain(gainChain(0)));
    rig.run(4);
    CHECK(testing::GainBlock::sCreated == 1);

    // a config change makes a new block
    REQUIRE(rig.engine.setChain(gainChain(0, { { "latency", 0 }, { "x", 1 } })));
    rig.run(4);
    CHECK(testing::GainBlock::sCreated == 2);
}

TEST_CASE("slot bypass and global bypass")
{
    Rig rig;
    rig.engine.setParameter(slotParam(0, 0), gainParam(-6.0f));
    REQUIRE(rig.engine.setChain(gainChain(0)));
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f * dbToGain(-6.0f)).epsilon(0.001));

    rig.engine.setParameter(slotBypassParam(0), 1.0f);
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));

    // global bypass ignores everything, including the input level
    rig.engine.setParameter(slotBypassParam(0), 0.0f);
    rig.engine.setParameter(kParamInput, 12.0f);
    rig.engine.setParameter(kParamBypass, 1.0f);
    rig.run();
    CHECK(rig.left() == 0.25f);
    CHECK(rig.right() == -0.125f);
}

TEST_CASE("chain latency is reported")
{
    Rig rig;
    CHECK(rig.engine.latency() == 0);
    REQUIRE(rig.engine.setChain(gainChain(0, { { "latency", 64 } })));
    rig.run(4);
    CHECK(rig.engine.latency() == 64);
}

TEST_CASE("a bypassed block with latency still delays the signal, so the timing doesn't change")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(slotParam(0, 0), gainParam(-6.0f));
    REQUIRE(rig.engine.setChain(gainChain(0, { { "latency", 100 } })));
    std::fill(rig.inL.begin(), rig.inL.end(), 0.0f);
    rig.run(4);
    // where an impulse comes out, and how loud
    auto impulse = [&]() {
        rig.inL[0] = 1.0f;
        std::vector<float> out;
        for (int i = 0; i < 2; ++i)
        {
            rig.run(1);
            out.insert(out.end(), rig.outL.begin(), rig.outL.end());
            rig.inL[0] = 0.0f;
        }
        size_t at = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::fabs(out[i]) > std::fabs(out[at]))
                at = i;
        for (size_t i = 0; i < out.size(); ++i)
            if (i != at)
                CHECK(out[i] == 0.0f);
        return std::make_pair(at, out[at]);
    };
    const auto active = impulse();
    CHECK(active.first == 100);
    CHECK(active.second == doctest::Approx(std::pow(10.0f, -6.0f / 20.0f)));

    rig.engine.setParameter(slotBypassParam(0), 1.0f);
    rig.run(20); // past the bypass ramp
    const auto bypassed = impulse();
    CHECK(bypassed.first == 100);
    CHECK(bypassed.second == doctest::Approx(1.0f));
}

TEST_CASE("a block that fails to configure is left out and reported")
{
    Rig rig;
    rig.engine.setParameter(slotParam(2, 0), gainParam(-6.0f));
    std::vector<int> failed;
    REQUIRE(rig.engine.setChain(gainChain(2, { { "fail", true } }), nullptr, &failed));
    CHECK(failed == std::vector<int> { 2 });
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));
}

TEST_CASE("an invalid chain is rejected and the old one keeps playing")
{
    Rig rig;
    rig.engine.setParameter(slotParam(0, 0), gainParam(-6.0f));
    REQUIRE(rig.engine.setChain(gainChain(0)));
    rig.run();

    std::string error;
    ChainState bad = gainChain(0);
    bad.blocks[0].typeId = "missing";
    CHECK_FALSE(rig.engine.setChain(bad, &error));
    CHECK_FALSE(error.empty());
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f * dbToGain(-6.0f)).epsilon(0.001));
}

TEST_CASE("the output limiter and meters")
{
    Rig rig;
    rig.inL.assign(kBuffer, 0.9f);
    rig.engine.setParameter(kParamOutput, 12.0f);
    rig.run();
    CHECK(rig.left() <= 0.892f);
    CHECK(rig.engine.getParameter(kParamLimiterReduction) > 6.0f);
    CHECK(rig.engine.getParameter(kParamOutPeakL) < 0.0f);
    CHECK(rig.engine.getParameter(kParamInPeakL) == doctest::Approx(gainToDb(0.9f)).epsilon(0.01));

    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.run();
    CHECK(rig.left() > 1.0f);
    CHECK(rig.engine.getParameter(kParamOutClip) == 1.0f);
}

TEST_CASE("the audio thread never allocates, including across chain swaps")
{
    Rig rig;
    rig.run(1);
    for (int i = 0; i < 20; ++i)
    {
        // alternate between two different blocks so every swap creates and retires one
        REQUIRE(rig.engine.setChain(gainChain(i % 2, { { "n", i } })));
        rig.engine.setParameter(slotParam(i % 2, 0), gainParam(static_cast<float>(-i)));
        rig.engine.setParameter(kParamBass, static_cast<float>(i % 12));

        testing::AllocationGuard guard;
        rig.run(4);
        CHECK(guard.count() == 0);
    }
}

TEST_CASE("engine processes buffers larger than its prepared size")
{
    Engine engine { testing::testRegistry() };
    engine.prepare(kRate, 64);
    std::vector<float> in(1000, 0.5f), out(1000, 0.0f);
    const float* ins[2] = { in.data(), in.data() };
    float* outs[2] = { out.data(), out.data() };
    engine.process(ins, outs, 1000);
    CHECK(out.back() == doctest::Approx(0.5f));
}

TEST_CASE("the allocation guard detects allocations")
{
    static std::vector<float>* volatile sink = nullptr;
    testing::AllocationGuard guard;
    sink = new std::vector<float>(16); // two allocations: the vector and its storage
    CHECK(guard.count() == 2);
    delete sink;
}

TEST_CASE("chain edits on another thread while audio runs")
{
    Rig rig;
    std::atomic<bool> running { true };
    std::thread editor([&] {
        for (int i = 0; running && i < 2000; ++i)
        {
            ChainState chain = gainChain(i % kMaxBlocks, { { "n", i % 3 } });
            if (i % 5 == 0)
                chain.blocks.push_back({ "test.gain", (i + 1) % kMaxBlocks });
            rig.engine.setChain(chain);
            rig.engine.setParameter(slotParam(i % kMaxBlocks, 0), gainParam(-6.0f));
        }
    });
    for (int i = 0; i < 4000; ++i)
        rig.run(1);
    running = false;
    editor.join();

    rig.run();
    CHECK(std::isfinite(rig.left()));
}

TEST_CASE("the engine reports which slots are running")
{
    Rig rig;
    rig.run(1);
    CHECK(rig.engine.getParameter(kParamRunningSlotsLow) == 0.0f);
    ChainState chain = gainChain(3);
    chain.blocks.push_back({ "test.gain", 20 });
    REQUIRE(rig.engine.setChain(chain));
    rig.run();
    CHECK(rig.engine.getParameter(kParamRunningSlotsLow) == static_cast<float>(1u << 3));
    CHECK(rig.engine.getParameter(kParamRunningSlotsHigh) == static_cast<float>(1u << 4));
}

TEST_CASE("blocks get the host tempo, and their meters reach the slot meter parameters")
{
    Rig rig;
    REQUIRE(rig.engine.setChain(gainChain(7)));
    rig.run(2);
    CHECK(rig.engine.getParameter(slotMeterParam(7)) == doctest::Approx(120.0f));
    rig.engine.setTempo(93.5);
    rig.run(2);
    CHECK(rig.engine.getParameter(slotMeterParam(7)) == doctest::Approx(93.5f));
}

TEST_CASE("blocks get the song position while the transport plays")
{
    Rig rig;
    REQUIRE(rig.engine.setChain(gainChain(7)));
    rig.run(2);
    testing::GainBlock::sLastSongBeat = -1.0;
    const float* in[2] = { rig.inL.data(), rig.inR.data() };
    float* out[2] = { rig.outL.data(), rig.outR.data() };

    Transport transport;
    transport.valid = true;
    transport.playing = false;
    transport.bpm = 120.0;
    transport.beat = 8.0;
    rig.engine.process(in, out, kBuffer, &transport);
    CHECK(testing::GainBlock::sLastSongBeat == -1.0); // stopped: nothing

    transport.playing = true;
    rig.engine.process(in, out, kBuffer, &transport);
    CHECK(testing::GainBlock::sLastSongBeat == doctest::Approx(8.0));

    // a host buffer longer than the engine's: later chunks get the position at their own start
    std::vector<float> longIn(kBuffer * 3, 0.0f), longOut(kBuffer * 3, 0.0f);
    const float* longIns[2] = { longIn.data(), longIn.data() };
    float* longOuts[2] = { longOut.data(), longOut.data() };
    rig.engine.process(longIns, longOuts, kBuffer * 3, &transport);
    CHECK(testing::GainBlock::sLastSongBeat == doctest::Approx(8.0 + 2.0 * kBuffer * 2.0 / kRate));
}

TEST_CASE("MIDI CCs move mapped parameters and are reported for MIDI learn")
{
    Rig rig;
    ChainState chain = gainChain(2);
    chain.midi.push_back({ 0, 7, slotParam(2, 0) });   // CC 7 on channel 1 -> the block's gain
    chain.midi.push_back({ 3, 20, kParamOutput });      // CC 20 on channel 4 -> output level (-24..24 dB)
    chain.midi.push_back({ 0, 64, slotBypassParam(2) });
    REQUIRE(rig.engine.setChain(chain));
    rig.run(2);

    testing::AllocationGuard guard;
    Engine::CcResult result = rig.engine.midiCc(0, 7, 127);
    CHECK(result.param == static_cast<int>(slotParam(2, 0)));
    CHECK(rig.engine.getParameter(slotParam(2, 0)) == doctest::Approx(1.0f));

    result = rig.engine.midiCc(3, 20, 0);
    CHECK(result.param == static_cast<int>(kParamOutput));
    CHECK(rig.engine.getParameter(kParamOutput) == doctest::Approx(-24.0f));

    rig.engine.midiCc(0, 64, 100);
    CHECK(rig.engine.getParameter(slotBypassParam(2)) == 1.0f);

    // unmapped, or another channel: nothing moves, but it's reported
    CHECK(rig.engine.midiCc(1, 7, 0).param == -1);
    CHECK(rig.engine.getParameter(kParamMidiLastCc) == 1 * 128 + 7);
    const float count = rig.engine.getParameter(kParamMidiCcCount);

    // while learning, CCs only report
    rig.engine.setParameter(kParamMidiLearn, 1.0f);
    CHECK(rig.engine.midiCc(0, 7, 0).param == -1);
    CHECK(rig.engine.getParameter(slotParam(2, 0)) == doctest::Approx(1.0f));
    CHECK(rig.engine.getParameter(kParamMidiCcCount) == count + 1);
    CHECK(guard.count() == 0);
}

TEST_CASE("Program Changes flip learned switches, press footswitch controls, and are reported for MIDI learn")
{
    Rig rig;
    ChainState chain = gainChain(2);
    chain.midi.push_back({ 0, 1, slotBypassParam(2), true });             // PC 1 -> the block on/off
    chain.midi.push_back({ 0, 2, looperParam(kLooperParamRecDub), true }); // PC 2 -> the looper's Rec/Dub
    chain.midi.push_back({ 0, 3, slotParam(2, 0), true });                // PC 3 -> a knob: no use, ignored
    REQUIRE(rig.engine.setChain(chain));
    rig.run(2);

    testing::AllocationGuard guard;
    Engine::ProgramResult result = rig.engine.midiProgram(0, 1);
    CHECK(result.mapped);
    CHECK(result.param == static_cast<int>(slotBypassParam(2)));
    CHECK(result.value == 1.0f);
    CHECK(rig.engine.getParameter(slotBypassParam(2)) == 1.0f);
    CHECK(rig.engine.getParameter(kParamMidiProgramValue) == 1.0f);
    CHECK(rig.engine.getParameter(kParamMidiLastProgram) == 1.0f);
    result = rig.engine.midiProgram(0, 1); // again: back on
    CHECK(result.value == 0.0f);
    CHECK(rig.engine.getParameter(slotBypassParam(2)) == 0.0f);

    // a press is held for one process() call, then let go
    result = rig.engine.midiProgram(0, 2);
    CHECK(result.mapped);
    CHECK(result.param == -1); // nothing for the host: it's 0 again by the next buffer
    CHECK(rig.engine.getParameter(looperParam(kLooperParamRecDub)) == 1.0f);
    rig.run(1);
    CHECK(rig.engine.getParameter(looperParam(kLooperParamRecDub)) == 0.0f);

    // a knob, another channel, an unmapped program: not mapped (a preset's, maybe), but reported
    CHECK_FALSE(rig.engine.midiProgram(0, 3).mapped);
    CHECK(rig.engine.getParameter(slotParam(2, 0)) == 0.0f);
    CHECK_FALSE(rig.engine.midiProgram(1, 1).mapped);
    CHECK(rig.engine.getParameter(kParamMidiLastProgram) == 1 * 128 + 1);
    const float count = rig.engine.getParameter(kParamMidiProgramCount);

    // while learning, they only report, and nothing else may take them
    rig.engine.setParameter(kParamMidiLearn, 1.0f);
    result = rig.engine.midiProgram(0, 1);
    CHECK(result.mapped);
    CHECK(result.param == -1);
    CHECK(rig.engine.getParameter(slotBypassParam(2)) == 0.0f);
    CHECK(rig.engine.getParameter(kParamMidiProgramCount) == count + 1);
    CHECK(guard.count() == 0);
}

namespace {

// Chain: optional pre block (slot 0), branch A (slot 1), branch B (slot 2)
ChainState splitChain(nlohmann::json configA = nlohmann::json::object(),
                      nlohmann::json configB = nlohmann::json::object())
{
    ChainState chain;
    chain.blocks.push_back({ "test.gain", 1, std::move(configA) });
    chain.blocks.push_back({ "test.gain", 2, std::move(configB), {}, false, kBranchB });
    chain.split = SplitState { 0, 2 };
    return chain;
}

} // namespace

TEST_CASE("a split sums its branches at the merge")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(slotParam(1, 0), gainParam(-6.0f));
    rig.engine.setParameter(slotParam(2, 0), gainParam(-12.0f));
    REQUIRE(rig.engine.setChain(splitChain()));
    rig.run();
    const float a = 0.25f * dbToGain(-6.0f);
    const float b = 0.25f * dbToGain(-12.0f);
    CHECK(rig.left() == doctest::Approx(a + b).epsilon(0.001));

    SUBCASE("balance")
    {
        rig.engine.setParameter(kParamMergeBalance, -100.0f);
        rig.run();
        CHECK(rig.left() == doctest::Approx(a).epsilon(0.001));
        rig.engine.setParameter(kParamMergeBalance, 50.0f);
        rig.run();
        CHECK(rig.left() == doctest::Approx(0.5f * a + b).epsilon(0.001));
    }
    SUBCASE("levels and phase invert")
    {
        rig.engine.setParameter(kParamMergeLevelB, 6.0f);
        rig.engine.setParameter(kParamMergeInvertA, 1.0f);
        rig.run();
        CHECK(rig.left() == doctest::Approx(b * dbToGain(6.0f) - a).epsilon(0.001));
    }
    SUBCASE("identical branches with one inverted cancel")
    {
        rig.engine.setParameter(slotParam(2, 0), gainParam(-6.0f));
        rig.engine.setParameter(kParamMergeInvertB, 1.0f);
        rig.run();
        CHECK(std::fabs(rig.left()) < 1e-5f);
    }
    SUBCASE("pan, in Mono mode too: the chain is stereo after the merge")
    {
        rig.engine.setParameter(kParamMergePanA, -100.0f);
        rig.engine.setParameter(kParamMergePanB, 100.0f);
        rig.run();
        CHECK(rig.left() == doctest::Approx(a).epsilon(0.001));
        CHECK(rig.right() == doctest::Approx(b).epsilon(0.001)); // from the left input: Mono

        rig.engine.setParameter(kParamInputMode, 1.0f);
        rig.run();
        CHECK(rig.left() == doctest::Approx(a).epsilon(0.001));
        CHECK(rig.right() == doctest::Approx(-0.125f * dbToGain(-12.0f)).epsilon(0.001));
    }
    SUBCASE("centred in Mono mode, both sides are the same")
    {
        rig.run();
        CHECK(rig.left() == doctest::Approx(a + b).epsilon(0.001));
        CHECK(rig.right() == rig.left());
    }
}

TEST_CASE("Merge Align delays one branch, by fractions of a sample too")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(slotParam(1, 0), gainParam(0.0f));
    rig.engine.setParameter(slotParam(2, 0), gainParam(0.0f));
    rig.engine.setParameter(kParamMergePanA, -100.0f);
    rig.engine.setParameter(kParamMergePanB, 100.0f);
    REQUIRE(rig.engine.setChain(splitChain()));

    // an impulse every buffer: the branch's output shows where it lands
    auto impulseAt = [&](float alignMs) {
        rig.engine.setParameter(kParamMergeAlign, alignMs);
        std::fill(rig.inL.begin(), rig.inL.end(), 0.0f);
        rig.inL[0] = 1.0f;
        rig.run(200); // the align smoother settles
        return std::pair { rig.outL, rig.outR };
    };

    const auto [left0, right0] = impulseAt(0.0f);
    CHECK(left0[0] == doctest::Approx(1.0f));
    CHECK(right0[0] == doctest::Approx(1.0f));

    const float oneMs = static_cast<float>(kRate) / 1000.0f; // 48 samples
    const auto [left1, right1] = impulseAt(1.0f);
    CHECK(left1[0] == doctest::Approx(1.0f));                 // A as it was
    CHECK(right1[static_cast<size_t>(oneMs)] == doctest::Approx(1.0f).epsilon(0.001)); // B 1 ms later
    CHECK(std::fabs(right1[0]) < 1e-4f);

    const auto [left2, right2] = impulseAt(-0.5f);
    CHECK(left2[static_cast<size_t>(oneMs / 2)] == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(right2[0] == doctest::Approx(1.0f));

    // half a sample: the impulse is shared between two samples
    const auto [left3, right3] = impulseAt(0.5f / oneMs);
    CHECK(right3[0] > 0.3f);
    CHECK(right3[1] > 0.3f);

    testing::AllocationGuard guard;
    rig.run(4);
    CHECK(guard.count() == 0);
}

TEST_CASE("in Mono mode the chain turns stereo at a stereo block")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(slotParam(0, 0), gainParam(0.0f));
    rig.engine.setParameter(slotParam(2, 0), gainParam(0.0f));
    ChainState chain;
    chain.blocks.push_back({ "test.gain", 0 });
    chain.blocks.push_back({ "test.widen", 1 });
    REQUIRE(rig.engine.setChain(chain));
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));
    CHECK(rig.right() == doctest::Approx(-0.25f)); // the left input, made stereo

    // in a branch: the other branch, still mono, plays on both sides
    ChainState split;
    split.blocks.push_back({ "test.widen", 1 });
    split.blocks.push_back({ "test.gain", 2, nlohmann::json::object(), {}, false, kBranchB });
    split.split = SplitState { 0, 2 };
    REQUIRE(rig.engine.setChain(split));
    rig.run(100);
    CHECK(rig.left() == doctest::Approx(0.5f));
    CHECK(std::fabs(rig.right()) < 1e-5f);
}

TEST_CASE("empty branches carry the signal; blocks before and after the split run in series")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(slotParam(0, 0), gainParam(-6.0f));
    rig.engine.setParameter(slotParam(3, 0), gainParam(-6.0f));
    ChainState chain;
    chain.blocks.push_back({ "test.gain", 0 });
    chain.blocks.push_back({ "test.gain", 3 });
    chain.split = SplitState { 1, 1 };
    REQUIRE(rig.engine.setChain(chain));
    rig.run();
    // both branches carry the pre block's output, and they sum
    CHECK(rig.left() == doctest::Approx(2.0f * 0.25f * dbToGain(-12.0f)).epsilon(0.001));
}

TEST_CASE("branches with different latencies are time-aligned at the merge")
{
    Rig rig;
    rig.engine.setParameter(kParamLimiter, 0.0f);
    rig.engine.setParameter(slotParam(1, 0), gainParam(0.0f));
    rig.engine.setParameter(slotParam(2, 0), gainParam(0.0f));
    REQUIRE(rig.engine.setChain(splitChain({ { "latency", 100 } }, { { "latency", 30 } })));
    rig.run();
    CHECK(rig.engine.latency() == 100);

    // An impulse arrives once, at the larger latency, with both branches together
    std::fill(rig.inL.begin(), rig.inL.end(), 0.0f);
    rig.run(4);
    rig.inL[0] = 1.0f;
    std::vector<float> out;
    for (int i = 0; i < 2; ++i)
    {
        rig.run(1);
        out.insert(out.end(), rig.outL.begin(), rig.outL.end());
        rig.inL[0] = 0.0f;
    }
    for (size_t i = 0; i < out.size(); ++i)
        CHECK(out[i] == doctest::Approx(i == 100 ? 2.0f : 0.0f));
}

TEST_CASE("branch B gives the same result on the worker thread as in series")
{
    auto render = [](bool parallel) {
        Rig rig;
        rig.engine.setParallelProcessing(parallel);
        rig.engine.setParameter(slotParam(1, 0), gainParam(-3.0f));
        rig.engine.setParameter(slotParam(2, 0), gainParam(-9.0f));
        rig.engine.setParameter(kParamMergeBalance, 20.0f);
        REQUIRE(rig.engine.setChain(splitChain({ { "latency", 7 } })));
        std::vector<float> out;
        for (int i = 0; i < 20; ++i)
        {
            rig.inL[0] = static_cast<float>(i % 3) * 0.1f;
            rig.run(1);
            out.insert(out.end(), rig.outL.begin(), rig.outL.end());
        }
        return out;
    };
    CHECK(render(true) == render(false));
}

TEST_CASE("the audio thread doesn't allocate with a split on the worker thread")
{
    Rig rig;
    rig.run(1);
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(rig.engine.setChain(splitChain({ { "n", i } }, { { "latency", i * 3 } })));
        rig.engine.setParameter(kParamMergePanA, static_cast<float>(i * 10));
        rig.engine.setParameter(kParamInputMode, static_cast<float>(i % 2));

        testing::AllocationGuard guard;
        rig.run(4);
        CHECK(guard.count() == 0);
    }
}

TEST_CASE("split chain edits on another thread while audio runs")
{
    Rig rig;
    std::atomic<bool> running { true };
    std::thread editor([&] {
        for (int i = 0; running && i < 500; ++i)
        {
            ChainState chain = splitChain({ { "n", i % 3 } }, { { "latency", i % 5 } });
            if (i % 3 == 0)
                chain.removeSplit();
            rig.engine.setChain(chain);
        }
    });
    for (int i = 0; i < 2000; ++i)
        rig.run(1);
    running = false;
    editor.join();

    rig.run();
    CHECK(std::isfinite(rig.left()));
}

TEST_CASE("a NaN from the host doesn't stay in the engine")
{
    Rig rig;
    rig.engine.setParameter(slotParam(0, 0), gainParam(0.0f));
    rig.engine.setChain(gainChain(0));
    rig.run();
    rig.inL[10] = std::nanf("");
    rig.inR[20] = INFINITY;
    rig.run(1);
    for (float v : rig.outL)
        CHECK(std::isfinite(v));
    rig.inL[10] = rig.inL[11];
    rig.inR[20] = rig.inR[21];
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));
    CHECK(rig.right() == doctest::Approx(0.25f)); // mono mode: the left input on both sides
    CHECK_FALSE(rig.engine.takeChainFault());
}

TEST_CASE("a block whose state went NaN is silenced, reported and replaced by a fresh one")
{
    Rig rig;
    rig.engine.setParameter(slotParam(0, 0), gainParam(0.0f));
    const ChainState chain = gainChain(0);
    rig.engine.setChain(chain);
    rig.run();
    CHECK_FALSE(rig.engine.takeChainFault());

    testing::GainBlock::sBreakNext = true;
    rig.run();
    for (float v : rig.outL)
        CHECK(v == 0.0f); // silence, not NaN
    CHECK(rig.engine.takeChainFault());
    CHECK_FALSE(rig.engine.takeChainFault()); // reported once

    // the same chain again reuses the broken block; after forgetBlocks() it gets a new one
    const int created = testing::GainBlock::sCreated;
    rig.engine.forgetBlocks();
    REQUIRE(rig.engine.setChain(chain));
    CHECK(testing::GainBlock::sCreated == created + 1);
    rig.run();
    CHECK(rig.left() == doctest::Approx(0.25f));
    rig.engine.takeChainFault(); // the old block's last buffers, before the swap
    rig.run();
    CHECK_FALSE(rig.engine.takeChainFault());
}

TEST_CASE("the host deactivating and activating the plugin keeps the chain playing")
{
    Rig rig;
    rig.engine.setParameter(slotParam(0, 0), gainParam(-6.0f));
    rig.engine.setChain(gainChain(0));
    rig.run();
    const float before = rig.left();
    CHECK(before == doctest::Approx(0.25f * std::pow(10.0f, -6.0f / 20.0f)));
    for (auto [rate, size] : { std::pair { 48000.0, 256u }, { 96000.0, 4096u }, { 44100.0, 64u } })
    {
        rig.engine.prepare(rate, size);
        rig.run();
        CHECK(rig.left() == doctest::Approx(before));
    }
}

TEST_CASE("latched footswitches are reported, and restored from a saved chain")
{
    Engine engine { BlockRegistry::builtin() };
    engine.prepare(kRate, kBuffer);
    std::vector<float> inL(kBuffer, 0.25f), inR(kBuffer, 0.25f), outL(kBuffer), outR(kBuffer);
    const float* in[2] = { inL.data(), inR.data() };
    float* out[2] = { outL.data(), outR.data() };
    auto run = [&](int buffers) {
        for (int i = 0; i < buffers; ++i)
            engine.process(in, out, kBuffer);
    };

    ChainState chain;
    BlockState kill;
    kill.typeId = kKillSwitchBlockDescriptor.typeId;
    kill.slot = 2;
    kill.latched = 1u << kKillParamStutter;
    chain.blocks.push_back(kill);
    // the block is made before the host has restored Latch
    REQUIRE(engine.setChain(chain));
    run(4);
    CHECK(engine.getParameter(slotLatchParam(2)) == 2.0f);
    engine.setParameter(slotParam(2, kKillParamLatch), 1.0f);
    run(4);
    CHECK(engine.getParameter(slotLatchParam(2)) == 2.0f);

    // a press of Kill latches it too
    engine.setParameter(slotParam(2, kKillParamKill), 1.0f);
    run(1);
    engine.setParameter(slotParam(2, kKillParamKill), 0.0f);
    run(1);
    CHECK(engine.getParameter(slotLatchParam(2)) == 3.0f);

    // the UI resending the chain (with old latched state) leaves a kept block alone...
    chain.blocks[0].latched = 0;
    REQUIRE(engine.setChain(chain));
    run(4);
    CHECK(engine.getParameter(slotLatchParam(2)) == 3.0f);

    // ...but a preset being loaded sets it
    chain.restoreLatches = true;
    chain.blocks[0].latched = 1u << kKillParamKill;
    REQUIRE(engine.setChain(chain));
    run(4);
    CHECK(engine.getParameter(slotLatchParam(2)) == 1.0f);

    // a removed block's slot reports nothing
    REQUIRE(engine.setChain(ChainState {}));
    run(4);
    CHECK(engine.getParameter(slotLatchParam(2)) == 0.0f);
}
