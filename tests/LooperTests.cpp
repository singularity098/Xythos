// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The global looper (owner, 2026-11-01): its waveform for the looper page, and the engine's copy of it, which plays on
// through chain changes

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/Engine.hpp"
#include "engine/looper/Looper.hpp"

#include <cmath>
#include <set>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

void press(Looper& looper, int param)
{
    looper.setParam(param, 1.0f);
    looper.setParam(param, 0.0f);
}

// Runs `frames` of a tone (amplitude `level`, 0 = silence) through the looper, mono, in 256-sample buffers
void run(Looper& looper, uint32_t frames, float level, int* allocations = nullptr)
{
    std::vector<float> buffer(256);
    std::vector<float> click(256);
    uint32_t done = 0;
    while (done < frames)
    {
        const uint32_t n = std::min<uint32_t>(256, frames - done);
        for (uint32_t i = 0; i < n; ++i)
            buffer[i] = level * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * (done + i) / kRate));
        float* io[1] = { buffer.data() };
        testing::AllocationGuard guard;
        looper.process(io, 1, n, click.data());
        if (allocations)
            *allocations += guard.count();
        done += n;
    }
}

// The loop's memory for a test's looper
struct Memory {
    std::vector<float> left = std::vector<float>(kLooperMaxSamples), right = std::vector<float>(kLooperMaxSamples);
};

void giveMemory(Looper& looper)
{
    static Memory memory[4];
    static int next = 0;
    Memory& m = memory[next++ % 4];
    std::fill(m.left.begin(), m.left.end(), 0.0f);
    std::fill(m.right.begin(), m.right.end(), 0.0f);
    looper.attach(m.left.data(), m.right.data());
}

void countInSetup(Looper& looper, int loops)
{
    giveMemory(looper);
    looper.setHasCountIn(true);
    looper.prepare(kRate);
    looper.setParam(kLooperParamMode, 0.0f);
    looper.setParam(kLooperParamBpm, 120.0f); // a beat is 24000 samples
    looper.setParam(kLooperParamBeats, 4.0f);
    looper.setParam(kLooperParamCountIn, 0.0f);
    looper.setParam(kLooperParamMeasures, 1.0f); // a 2 s loop
    looper.setParam(kLooperParamLoops, static_cast<float>(loops));
    looper.setParam(kLooperParamClick, 0.0f);
    looper.setParam(kLooperParamLatency, 0.0f);
}

} // namespace

TEST_CASE("a looper cell and the looper's state survive the trip through a float")
{
    const LooperCell cell = decodeLooperCell(encodeLooperCell(63, 7, 127, 255));
    CHECK(cell.generation == 63);
    CHECK(cell.lane == 7);
    CHECK(cell.column == 127);
    CHECK(cell.peak == 255);
    CHECK(encodeLooperCell(63, 7, 127, 255) == 16777215.0f);

    LooperReport r;
    r.state = LooperState::Overdubbing;
    r.pass = 8;
    r.lanes = 8;
    r.generation = 63;
    LooperReport back;
    back.decodeState(r.encodeState());
    CHECK(back.state == LooperState::Overdubbing);
    CHECK(back.pass == 8);
    CHECK(back.lanes == 8);
    CHECK(back.generation == 63);
}

TEST_CASE("each pass of a Count In looper loop gets a lane of its own waveform")
{
    Looper looper;
    countInSetup(looper, 3);
    press(looper, kLooperParamRecDub);
    // pass 1 loud, pass 2 quiet, pass 3 silent (each run goes a little past its pass, into the next one's first
    // column: the passes turn on the sample after the 96000th)
    int allocations = 0;
    run(looper, 96000 + 128, 0.8f, &allocations);
    CHECK(looper.report().state == LooperState::Overdubbing);
    CHECK(looper.report().pass == 2);
    run(looper, 96000, 0.1f, &allocations);
    CHECK(looper.report().pass == 3);
    run(looper, 96000, 0.0f, &allocations);
    CHECK(looper.report().state == LooperState::Playing);
    CHECK(looper.report().pass == 0);
    CHECK(allocations == 0);

    CHECK(looper.report().lanes == 3);
    for (int c = 1; c < kLooperWaveColumns - 1; ++c)
    {
        CAPTURE(c);
        CHECK(looper.wave(0, c) > 200);                         // sqrt(0.8) * 255
        CHECK(std::abs(looper.wave(1, c) - 81) <= 3);           // sqrt(0.1) * 255
        CHECK(looper.wave(2, c) == 0);
    }
}

TEST_CASE("a Free loop's waveform is spread over the columns when it closes")
{
    Looper looper;
    giveMemory(looper);
    looper.prepare(kRate);
    looper.setParam(kLooperParamLatency, 0.0f);
    press(looper, kLooperParamRecDub); // Free mode: no Count In (setHasCountIn() not called)
    run(looper, 48000 * 9, 0.5f);      // 9 s: the peaks merge a few times on the way
    run(looper, 48000 * 3, 0.0f);      // the last quarter silent
    CHECK(looper.report().state == LooperState::Recording);
    CHECK(looper.report().pass == 1);
    CHECK(looper.report().seconds == doctest::Approx(12.0).epsilon(0.01));
    press(looper, kLooperParamRecDub);
    CHECK(looper.report().state == LooperState::Playing);
    for (int c = 2; c < kLooperWaveColumns * 3 / 4 - 2; ++c)
        CHECK(looper.wave(0, c) > 150);
    for (int c = kLooperWaveColumns * 3 / 4 + 2; c < kLooperWaveColumns; ++c)
        CHECK(looper.wave(0, c) == 0);

    // an overdub started mid-loop is a pass of its own, round to where it started
    run(looper, 48000 * 6, 0.0f);
    press(looper, kLooperParamRecDub);
    run(looper, 48000 * 12 - 4800, 0.3f);
    CHECK(looper.report().pass == 2);
    run(looper, 9600, 0.3f);
    CHECK(looper.report().pass == 3);
}

TEST_CASE("the looper streams every cell to the UI, and Clear starts a new generation")
{
    Looper looper;
    countInSetup(looper, 2);
    press(looper, kLooperParamRecDub);
    run(looper, 96000 * 2, 0.5f);
    REQUIRE(looper.report().lanes == 2);
    const int generation = looper.report().generation;

    // within a few seconds of streaming, every cell has been sent with its peak
    std::set<int> seen;
    float cells[8];
    for (int k = 0; k < 2000 && seen.size() < 2u * kLooperWaveColumns; ++k)
    {
        run(looper, 256, 0.0f);
        looper.streamCells(cells, 8);
        for (int i = 1; i < 8; ++i)
        {
            const LooperCell cell = decodeLooperCell(cells[i]);
            CHECK(cell.generation == generation);
            CHECK(cell.peak == looper.wave(cell.lane, cell.column));
            seen.insert(cell.lane * kLooperWaveColumns + cell.column);
        }
    }
    CHECK(seen.size() == 2u * kLooperWaveColumns);

    press(looper, kLooperParamClear);
    CHECK(looper.report().state == LooperState::Empty);
    CHECK(looper.report().lanes == 0);
    CHECK(looper.report().generation == (generation + 1) % kLooperGenerations);
    CHECK(looper.wave(0, 10) == 0);
}

TEST_CASE("the global looper records what's heard and plays on through chain changes")
{
    Engine engine { testing::testRegistry() };
    engine.prepare(kRate, 256);
    engine.setParameter(kParamLooperWake, 1.0f); // the UI opened the looper
    {
        std::vector<float> l(256), r(256);
        const float* i[2] = { l.data(), r.data() };
        float* o[2] = { l.data(), r.data() };
        engine.process(i, o, 256); // the audio thread sees the pulse
    }
    engine.setParameter(kParamLooperWake, 0.0f);
    REQUIRE(engine.serviceLooper());
    engine.setParameter(looperParam(kLooperParamMode), 1.0f); // Free
    engine.setParameter(looperParam(kLooperParamLatency), 0.0f);
    std::vector<float> inL(256), inR(256), outL(256), outR(256);
    const float* in[2] = { inL.data(), inR.data() };
    float* out[2] = { outL.data(), outR.data() };
    auto runFor = [&](int buffers, float level) {
        float peak = 0.0f;
        for (int b = 0; b < buffers; ++b)
        {
            for (int i = 0; i < 256; ++i)
                inL[i] = inR[i] = level * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * (b * 256 + i) / kRate));
            testing::AllocationGuard guard;
            engine.process(in, out, 256);
            CHECK(guard.count() == 0);
            for (int i = 0; i < 256; ++i)
                peak = std::max(peak, std::fabs(outL[i]));
        }
        return peak;
    };
    auto setButton = [&](int param, float value) { engine.setParameter(looperParam(param), value); };

    runFor(4, 0.0f);
    setButton(kLooperParamRecDub, 1.0f);
    runFor(1, 0.3f);
    setButton(kLooperParamRecDub, 0.0f);
    runFor(187, 0.3f); // about a second
    LooperReport report;
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.state == LooperState::Recording);
    setButton(kLooperParamRecDub, 1.0f);
    runFor(1, 0.0f);
    setButton(kLooperParamRecDub, 0.0f);
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.state == LooperState::Playing);
    CHECK(engine.getParameter(kParamLooperLength) == doctest::Approx(1.0).epsilon(0.02));

    // a new chain (another preset) and the guitar silent: the loop still plays
    REQUIRE(engine.setChain(ChainState {}));
    CHECK(runFor(200, 0.0f) > 0.2f);

    // its waveform reached the outputs
    bool any = false;
    for (uint32_t k = kParamLooperCellBase; k < kParamLooperCellEnd; ++k)
        any = any || decodeLooperCell(engine.getParameter(k)).peak > 100;
    CHECK(any);

    // Level 0 silences it; Clear empties it
    setButton(kLooperParamLevel, 0.0f);
    runFor(40, 0.0f);
    CHECK(runFor(10, 0.0f) < 0.01f);
    setButton(kLooperParamClear, 1.0f);
    runFor(1, 0.0f);
    setButton(kLooperParamClear, 0.0f);
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.state == LooperState::Empty);
}

TEST_CASE("the looper's footswitches can be mapped to MIDI")
{
    CHECK(isMidiMappable(looperParam(kLooperParamRecDub)));
    CHECK(isMidiMappable(looperParam(kLooperParamLatency)));
    CHECK_FALSE(isMidiMappable(kParamLooperState));
    CHECK(midiCcToValue(looperParam(kLooperParamRecDub), 127) == 1.0f);
    CHECK(midiCcToValue(looperParam(kLooperParamRecDub), 0) == 0.0f);
}

TEST_CASE("the global looper takes no memory until it's wanted, and a footswitch press wakes it")
{
    Engine engine { testing::testRegistry() };
    engine.prepare(kRate, 256);
    engine.setParameter(looperParam(kLooperParamMode), 1.0f); // Free
    CHECK_FALSE(engine.serviceLooper()); // never opened: nothing reserved

    std::vector<float> inL(256, 0.3f), inR(256, 0.3f), outL(256), outR(256);
    const float* in[2] = { inL.data(), inR.data() };
    float* out[2] = { outL.data(), outR.data() };
    LooperReport report;

    // a MIDI footswitch presses Rec/Dub: passed through for now, and the looper is wanted
    engine.setParameter(looperParam(kLooperParamRecDub), 1.0f);
    for (int b = 0; b < 4; ++b)
    {
        testing::AllocationGuard guard;
        engine.process(in, out, 256);
        CHECK(guard.count() == 0);
    }
    CHECK(outL[100] == doctest::Approx(0.3f).epsilon(0.01)); // the dry signal passes
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.state == LooperState::Empty);

    // the plugin's thread reserves the memory; the press that asked for it starts the recording
    CHECK(engine.serviceLooper());
    engine.process(in, out, 256);
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.state == LooperState::Recording);

    // the looper is ephemeral: a project reopened (the pulse long over, so saved as 0) has no memory until it's wanted
    Engine reopened { testing::testRegistry() };
    reopened.prepare(kRate, 256);
    reopened.setParameter(kParamLooperWake, globalParamSpec(kParamLooperWake).def);
    reopened.process(in, out, 256);
    CHECK_FALSE(reopened.serviceLooper());
}
