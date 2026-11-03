// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The global looper (owner, 2026-11-01): its waveform for the looper page, and the engine's copy of it, which plays on
// through chain changes

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/Engine.hpp"
#include "engine/looper/Looper.hpp"

#include <cmath>
#include <deque>
#include <memory>
#include <set>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

// The loop's memory of the test's looper; run() services it between buffers, as the plugin's thread does
LooperMemory* gMemory = nullptr;

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
        if (gMemory != nullptr)
            gMemory->service();
        testing::AllocationGuard guard;
        looper.process(io, 1, n, click.data());
        if (allocations)
            *allocations += guard.count();
        done += n;
    }
}

// The loop's memory for a test's looper (the last few tests' kept: their loopers may still be around)
LooperMemory* giveMemory(Looper& looper)
{
    static std::deque<std::unique_ptr<LooperMemory>> memories;
    memories.push_back(std::make_unique<LooperMemory>());
    if (memories.size() > 3)
        memories.pop_front();
    gMemory = memories.back().get();
    gMemory->service();
    looper.attach(gMemory);
    return gMemory;
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

namespace {

// Like run(), and keeps what came out and the clicks
void runKeep(Looper& looper, uint32_t frames, float level, std::vector<float>& out, std::vector<float>& clicks)
{
    std::vector<float> buffer(256), click(256);
    uint32_t done = 0;
    while (done < frames)
    {
        const uint32_t n = std::min<uint32_t>(256, frames - done);
        for (uint32_t i = 0; i < n; ++i)
            buffer[i] = level * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * (done + i) / kRate));
        std::fill(click.begin(), click.end(), 0.0f);
        float* io[1] = { buffer.data() };
        if (gMemory != nullptr)
            gMemory->service();
        testing::AllocationGuard guard;
        looper.process(io, 1, n, click.data());
        CHECK(guard.count() == 0);
        out.insert(out.end(), buffer.begin(), buffer.begin() + n);
        clicks.insert(clicks.end(), click.begin(), click.begin() + n);
        done += n;
    }
}

float peakOf(const std::vector<float>& v, size_t from, size_t to)
{
    float peak = 0.0f;
    for (size_t i = from; i < std::min(to, v.size()); ++i)
        peak = std::max(peak, std::fabs(v[i]));
    return peak;
}

// What the looper plays with nothing coming in, over `frames`
float playedPeak(Looper& looper, uint32_t frames)
{
    std::vector<float> out, clicks;
    runKeep(looper, frames, 0.0f, out, clicks);
    return peakOf(out, 0, out.size());
}

} // namespace

TEST_CASE("any pass of the loop can be deleted, and the passes after it move up")
{
    Looper looper;
    countInSetup(looper, 3);
    press(looper, kLooperParamRecDub);
    // pass 1 silent, pass 2 a tone, pass 3 a quieter tone (2 s each)
    run(looper, 96000, 0.0f);
    run(looper, 96000, 0.5f);
    run(looper, 96000 + 16, 0.2f);
    REQUIRE(looper.report().state == LooperState::Playing);
    REQUIRE(looper.report().lanes == 3);
    CHECK(playedPeak(looper, 96000) == doctest::Approx(0.7f).epsilon(0.03));
    const int generation = looper.report().generation;

    // the pass with the loud tone goes; the quiet one is the second lane now, and the waveform a new generation
    looper.deletePass(1);
    CHECK(looper.report().lanes == 2);
    CHECK(looper.report().generation == (generation + 1) % kLooperGenerations);
    CHECK(looper.wave(0, 64) == 0);
    CHECK(std::abs(looper.wave(1, 64) - 114) <= 3); // sqrt(0.2) * 255
    CHECK(looper.wave(2, 64) == 0);
    CHECK(playedPeak(looper, 96000) == doctest::Approx(0.2f).epsilon(0.03));

    // the first pass can go too; deleting the last one left clears the loop
    looper.deletePass(0);
    CHECK(looper.report().lanes == 1);
    CHECK(playedPeak(looper, 96000) == doctest::Approx(0.2f).epsilon(0.03));
    looper.deletePass(5); // no such pass
    CHECK(looper.report().lanes == 1);
    looper.deletePass(0);
    CHECK(looper.report().state == LooperState::Empty);
    CHECK(playedPeak(looper, 9600) == 0.0f);
}

TEST_CASE("a deleted pass's memory is zeroed and used again, and a pass being recorded can't be deleted")
{
    Looper looper;
    LooperMemory* memory = giveMemory(looper);
    looper.prepare(kRate);
    looper.setParam(kLooperParamLatency, 0.0f);
    press(looper, kLooperParamRecDub); // Free mode
    run(looper, 48000, 0.5f);
    press(looper, kLooperParamRecDub);
    run(looper, 256, 0.0f);
    // a minute for the first recording, and a second ready for the next pass
    CHECK(memory->bytes() == (size_t { kLooperMaxSamples } + 48000) * 2 * sizeof(float));
    press(looper, kLooperParamRecDub); // overdub
    run(looper, 24000, 0.3f);
    CHECK(looper.report().pass == 2);
    looper.deletePass(1); // being recorded: stays
    CHECK(looper.report().lanes == 2);
    run(looper, 24000 + 4800, 0.3f); // into pass 3
    CHECK(looper.report().pass == 3);
    press(looper, kLooperParamRecDub); // back to playing
    looper.deletePass(1);
    looper.deletePass(1);
    CHECK(looper.report().lanes == 1);
    CHECK(playedPeak(looper, 48000) == doctest::Approx(0.5f).epsilon(0.03));
    // the passes' lanes came back zeroed, so a new pass over silence adds nothing
    press(looper, kLooperParamRecDub);
    run(looper, 48000, 0.0f);
    press(looper, kLooperParamRecDub);
    CHECK(looper.report().lanes == 2);
    CHECK(playedPeak(looper, 48000) == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(memory->bytes() <= (size_t { kLooperMaxSamples } + 3 * 48000) * 2 * sizeof(float));
}

TEST_CASE("Count In mode counts in to a new pass over a playing loop, on the loop's beat")
{
    Looper looper;
    countInSetup(looper, 1); // a bar of 4 at 120 BPM: a 2 s loop, a beat 24000 samples
    looper.setParam(kLooperParamCountIn, 4.0f);
    looper.setParam(kLooperParamClick, 100.0f);
    press(looper, kLooperParamRecDub);
    run(looper, 4 * 24000 + 96000 + 16, 0.5f); // the count-in and the first pass
    REQUIRE(looper.report().state == LooperState::Playing);
    run(looper, 24000 - 16, 0.0f); // a quarter of the way round
    CHECK(looper.report().position == doctest::Approx(0.25f).epsilon(0.01));

    // Rec/Dub: the loop plays on; four clicks lead to its start, a whole loop and three quarters away (there's no
    // room for them before the next one)
    press(looper, kLooperParamRecDub);
    CHECK(looper.report().state == LooperState::DubCountIn);
    std::vector<float> out, clicks;
    runKeep(looper, 72000 + 96000 - 100, 0.0f, out, clicks);
    CHECK(looper.report().state == LooperState::DubCountIn);
    CHECK(looper.report().count == 4);
    CHECK(peakOf(out, 0, out.size()) > 0.4f);                // the loop played on
    CHECK(peakOf(clicks, 0, 72000 - 10) == 0.0f);            // no clicks until a bar before the start
    for (int k = 0; k < 4; ++k)
        CHECK(peakOf(clicks, 72000 + k * 24000, 72000 + k * 24000 + 200) > 0.1f);

    // the pass starts with the loop and records once round, with the click; then it plays on, no click
    runKeep(looper, 200, 0.3f, out, clicks);
    CHECK(looper.report().state == LooperState::Overdubbing);
    CHECK(looper.report().pass == 2);
    run(looper, 96000 - 100 + 16, 0.3f); // the pass started 100 samples into those 200
    CHECK(looper.report().state == LooperState::Playing);
    CHECK(looper.report().lanes == 2);
    out.clear();
    clicks.clear();
    runKeep(looper, 96000, 0.0f, out, clicks);
    CHECK(peakOf(clicks, 0, clicks.size()) == 0.0f);
    CHECK(std::abs(looper.wave(1, 64) - 140) <= 3); // the new pass: sqrt(0.3) * 255

    // a second press during the count-in calls it off
    press(looper, kLooperParamRecDub);
    CHECK(looper.report().state == LooperState::DubCountIn);
    press(looper, kLooperParamRecDub);
    CHECK(looper.report().state == LooperState::Playing);
}

TEST_CASE("Count In mode counts in to a new pass from Stopped, then starts the loop with it")
{
    Looper looper;
    countInSetup(looper, 2);
    looper.setParam(kLooperParamCountIn, 2.0f);
    looper.setParam(kLooperParamClick, 100.0f);
    press(looper, kLooperParamRecDub);
    run(looper, 2 * 24000 + 2 * 96000 + 16, 0.4f);
    REQUIRE(looper.report().state == LooperState::Playing);
    REQUIRE(looper.report().lanes == 2);
    press(looper, kLooperParamPlayStop);
    run(looper, 4800, 0.0f);
    REQUIRE(looper.report().state == LooperState::Stopped);

    // two clicks in silence, then the loop from its start, and two passes recorded
    press(looper, kLooperParamRecDub);
    std::vector<float> out, clicks;
    runKeep(looper, 48000 - 10, 0.0f, out, clicks);
    CHECK(looper.report().state == LooperState::DubCountIn);
    CHECK(peakOf(out, 0, out.size()) < 0.25f); // only the clicks (on the click bus here)
    CHECK(peakOf(clicks, 0, 200) > 0.1f);
    CHECK(peakOf(clicks, 24000, 24200) > 0.1f);
    run(looper, 20, 0.0f);
    CHECK(looper.report().state == LooperState::Overdubbing);
    CHECK(looper.report().position < 0.01f);
    run(looper, 2 * 96000, 0.1f);
    CHECK(looper.report().state == LooperState::Playing);
    CHECK(looper.report().lanes == 4);
}

TEST_CASE("Play starts the loop where the start marker is, and Clear puts it back at the start")
{
    Looper looper;
    countInSetup(looper, 1);
    press(looper, kLooperParamRecDub);
    run(looper, 96000 + 16, 0.5f);
    press(looper, kLooperParamPlayStop);
    run(looper, 4800, 0.0f);
    press(looper, kLooperParamPlayStop);
    CHECK(looper.report().position == 0.0f);
    press(looper, kLooperParamPlayStop);
    looper.setStart(0.5f);
    run(looper, 4800, 0.0f);
    press(looper, kLooperParamPlayStop);
    CHECK(looper.report().position == doctest::Approx(0.5f));
    press(looper, kLooperParamClear);
    CHECK(looper.start() == 0.0f);
}

TEST_CASE("a click on the timeline while the loop plays plays on from there at once, without a click")
{
    Looper looper;
    countInSetup(looper, 1);
    press(looper, kLooperParamRecDub);
    run(looper, 96000 + 16, 0.5f);
    REQUIRE(looper.report().state == LooperState::Playing);
    std::vector<float> out, clicks;
    runKeep(looper, 9600, 0.0f, out, clicks); // a fifth of the way round
    const float before = looper.report().position;
    looper.setStart(0.6513f);
    CHECK(looper.report().state == LooperState::Playing);
    CHECK(looper.report().position == doctest::Approx(0.6513f).epsilon(0.001));
    runKeep(looper, 4800, 0.0f, out, clicks);
    CHECK(looper.report().position == doctest::Approx(0.6513f + 0.05f).epsilon(0.001));
    CHECK(before < 0.3f);
    // the 220 Hz sine moves at most ~0.015 a sample: the jump crossfades rather than stepping
    float step = 0.0f;
    for (size_t i = 9000; i < 10400; ++i)
        step = std::max(step, std::fabs(out[i] - out[i - 1]));
    CHECK(step < 0.03f);
    CHECK(peakOf(out, 9600, 9900) > 0.2f); // and it's still playing through the fade
}

TEST_CASE("the engine deletes a pass and sets the start when the page asks, not when a project restores them")
{
    Engine engine { testing::testRegistry() };
    engine.prepare(kRate, 256);
    engine.setParameter(kParamLooperWake, 1.0f);
    engine.setParameter(kParamLooperDelete, 17.0f); // as a project might restore it: not a request
    std::vector<float> inL(256), inR(256), outL(256), outR(256);
    const float* in[2] = { inL.data(), inR.data() };
    float* out[2] = { outL.data(), outR.data() };
    auto runFor = [&](int buffers, float level) {
        for (int b = 0; b < buffers; ++b)
        {
            for (int i = 0; i < 256; ++i)
                inL[i] = inR[i] = level * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * (b * 256 + i) / kRate));
            engine.serviceLooper();
            engine.process(in, out, 256);
        }
    };
    auto pressButton = [&](int param) {
        engine.setParameter(looperParam(param), 1.0f);
        runFor(1, 0.0f);
        engine.setParameter(looperParam(param), 0.0f);
        runFor(1, 0.0f); // the release too, or the next press isn't a change
    };
    runFor(1, 0.0f);
    engine.setParameter(looperParam(kLooperParamMode), 1.0f); // Free
    engine.setParameter(looperParam(kLooperParamLatency), 0.0f);
    runFor(1, 0.0f); // the settings first (the footswitches are applied before them)
    LooperReport report;
    auto state = [&] { report.decodeState(engine.getParameter(kParamLooperState)); return report.state; };
    pressButton(kLooperParamRecDub);
    CHECK(state() == LooperState::Recording);
    runFor(188, 0.3f);
    pressButton(kLooperParamRecDub); // a loop
    CHECK(state() == LooperState::Playing);
    pressButton(kLooperParamRecDub); // a second pass
    CHECK(state() == LooperState::Overdubbing);
    runFor(200, 0.2f);
    pressButton(kLooperParamRecDub);
    report.decodeState(engine.getParameter(kParamLooperState));
    REQUIRE(report.lanes >= 2);
    const int lanes = report.lanes;

    engine.setParameter(kParamLooperDelete, 3 * 8 + 1.0f); // the second pass (request 3: 2 was the restored one)
    runFor(1, 0.0f);
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.lanes == lanes - 1);
    runFor(4, 0.0f); // unchanged: nothing more
    report.decodeState(engine.getParameter(kParamLooperState));
    CHECK(report.lanes == lanes - 1);

    // the export is the passes left, summed
    const std::string wav = engine.looperWav();
    CHECK_FALSE(wav.empty());
}
