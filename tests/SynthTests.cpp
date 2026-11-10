// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Behaviour tests for the synths in SynthBlocks and the Pick Filter (the original Synth block is otherwise tested in
// PedalTests.cpp)

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/BlockRegistry.hpp"
#include "engine/blocks/MoreSynthBlocks.hpp"
#include "engine/blocks/SynthBlock.hpp"
#include "engine/blocks/SynthBlocks.hpp"
#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/PickFilter.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

// A guitar-like tone: a fundamental and strong harmonics, steady, or fading if `decaySeconds` > 0
std::vector<std::vector<float>> note(double hz, size_t length, int channels = 1, double decaySeconds = 0.0)
{
    std::vector<std::vector<float>> x(channels, std::vector<float>(length));
    for (auto& ch : x)
    {
        for (size_t i = 0; i < length; ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            const double fade = decaySeconds > 0.0 ? std::exp(-t / decaySeconds) : 1.0;
            ch[i] = static_cast<float>(fade * (0.2 * std::sin(2 * M_PI * hz * t) + 0.15 * std::sin(2 * M_PI * 2 * hz * t + 1.0)
                                               + 0.1 * std::sin(2 * M_PI * 3 * hz * t + 2.0)));
        }
    }
    return x;
}

std::unique_ptr<Block> make(const BlockDescriptor& type)
{
    return testing::createWithDefaults(type, kRate);
}

// Amplitude of one frequency in x[from, to) (a single-bin DFT with a Hann window)
double toneAt(const std::vector<float>& x, double hz, size_t from, size_t to)
{
    double re = 0.0, im = 0.0;
    const double n = static_cast<double>(to - from);
    for (size_t i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i - from) / n);
        const double angle = 2.0 * M_PI * hz * static_cast<double>(i) / kRate;
        re += w * x[i] * std::cos(angle);
        im -= w * x[i] * std::sin(angle);
    }
    return 4.0 * std::sqrt(re * re + im * im) / n;
}

// Level in a band around `hz` (+-3%): for the detuned saws
double bandAt(const std::vector<float>& x, double hz, size_t from, size_t to)
{
    double power = 0.0;
    for (double f = hz * 0.97; f <= hz * 1.03; f += 2.0)
    {
        const double a = toneAt(x, f, from, to);
        power += a * a;
    }
    return std::sqrt(power);
}

// How much of x[from, to) is above `hz` (RMS after a high-pass, against the whole)
double highShare(const std::vector<float>& x, double hz, size_t from, size_t to)
{
    xythos::Biquad filter[2];
    for (auto& f : filter)
        f.setHighPass(kRate, hz);
    std::vector<float> high(x);
    for (float& v : high)
        v = filter[1].process(filter[0].process(v));
    return testing::rms(high, from, to) / testing::rms(x, from, to);
}

size_t ms(double milliseconds)
{
    return static_cast<size_t>(milliseconds / 1000.0 * kRate);
}

} // namespace

TEST_CASE("every synth plays the note you play, and an octave down when asked")
{
    for (const BlockDescriptor* type : { &kSuperSawBlockDescriptor, &kFmSynthBlockDescriptor, &kFoldSynthBlockDescriptor,
                                         &kAcidSynthBlockDescriptor, &kStringSynthBlockDescriptor,
                                         &kChipSynthBlockDescriptor })
    {
        const std::string typeId = type->typeId;
        CAPTURE(typeId);
        // the octave knob is at the same place in each: find it by name
        int octave = -1;
        for (int p = 0; p < type->numParams; ++p)
            if (std::string(type->params[p].name) == "Octave")
                octave = p;
        REQUIRE(octave >= 0);
        for (const float choice : { 2.0f, 1.0f }) // 0, -1
        {
            CAPTURE(choice);
            auto block = make(*type);
            block->setParam(octave, choice);
            auto x = note(196.0, 48000);
            testing::runBlock(*block, x);
            const double hz = choice == 2.0f ? 196.0 : 98.0;
            const double played = bandAt(x[0], hz, 24000, 48000);
            CHECK(played > 0.01);
            // nothing much a semitone either side: it's on the note
            CHECK(played > 3.0 * toneAt(x[0], hz * std::exp2(-1.0 / 12.0), 24000, 48000));
            CHECK(played > 3.0 * toneAt(x[0], hz * std::exp2(1.0 / 12.0), 24000, 48000));
        }
    }
}

TEST_CASE("super saw: detuned saws, spread in stereo")
{
    auto block = make(kSuperSawBlockDescriptor);
    block->setParam(kSuperSawParamCutoff, 12000.0f);
    block->setParam(kSuperSawParamWidth, 100.0f);
    auto x = note(196.0, 48000, 2);
    testing::runBlock(*block, x);
    double difference = 0.0;
    for (size_t i = 24000; i < 48000; ++i)
        difference = std::max(difference, static_cast<double>(std::fabs(x[0][i] - x[1][i])));
    CHECK(difference > 0.02);

    // no detune and no width: one saw, the same both sides, with all its harmonics
    auto plain = make(kSuperSawBlockDescriptor);
    plain->setParam(kSuperSawParamCutoff, 12000.0f);
    plain->setParam(kSuperSawParamDetune, 0.0f);
    plain->setParam(kSuperSawParamWidth, 0.0f);
    auto y = note(196.0, 48000, 2);
    testing::runBlock(*plain, y);
    for (size_t i = 24000; i < 48000; i += 101)
        CHECK(y[0][i] == doctest::Approx(y[1][i]).epsilon(1e-5));
    const double fundamental = toneAt(y[0], 196.0, 24000, 48000);
    CHECK(toneAt(y[0], 392.0, 24000, 48000) == doctest::Approx(fundamental / 2.0).epsilon(0.15));
}

TEST_CASE("FM synth: the ratio sets the harmonics, and the brightness decays after the pick")
{
    // 1:2 makes odd harmonics only; 1:1 all of them
    auto odd = make(kFmSynthBlockDescriptor);
    odd->setParam(kFmParamRatio, 2.0f);
    odd->setParam(kFmParamSustain, 100.0f);
    auto x = note(196.0, 48000);
    testing::runBlock(*odd, x);
    const double fundamental = toneAt(x[0], 196.0, 24000, 48000);
    CHECK(toneAt(x[0], 588.0, 24000, 48000) > 0.2 * fundamental);
    CHECK(toneAt(x[0], 392.0, 24000, 48000) < 0.02 * fundamental);

    auto all = make(kFmSynthBlockDescriptor);
    all->setParam(kFmParamRatio, 1.0f);
    all->setParam(kFmParamSustain, 100.0f);
    auto y = note(196.0, 48000);
    testing::runBlock(*all, y);
    CHECK(toneAt(y[0], 392.0, 24000, 48000) > 0.2 * toneAt(y[0], 196.0, 24000, 48000));

    // Index 0: a pure sine
    auto pure = make(kFmSynthBlockDescriptor);
    pure->setParam(kFmParamIndex, 0.0f);
    auto z = note(196.0, 48000);
    testing::runBlock(*pure, z);
    CHECK(toneAt(z[0], 392.0, 24000, 48000) < 0.01 * toneAt(z[0], 196.0, 24000, 48000));

    // brightness falls to Sustain (0) over Decay (100 ms)
    auto pluck = make(kFmSynthBlockDescriptor);
    pluck->setParam(kFmParamDecay, 100.0f);
    pluck->setParam(kFmParamSustain, 0.0f);
    auto p = note(196.0, 48000);
    testing::runBlock(*pluck, p);
    const double early = toneAt(p[0], 392.0, ms(30), ms(90)) / toneAt(p[0], 196.0, ms(30), ms(90));
    const double late = toneAt(p[0], 392.0, ms(600), ms(900)) / toneAt(p[0], 196.0, ms(600), ms(900));
    CHECK(late < 0.1 * early);
}

TEST_CASE("fold synth folds harder with Fold, and Pluck dies away")
{
    auto brightness = [](float fold) {
        auto block = make(kFoldSynthBlockDescriptor);
        block->setParam(kFoldParamFold, fold);
        block->setParam(kFoldParamEnv, 0.0f);
        block->setParam(kFoldParamTone, 16000.0f);
        auto x = note(196.0, 48000);
        testing::runBlock(*block, x);
        return highShare(x[0], 1500.0, 24000, 48000);
    };
    CHECK(brightness(100.0f) > 5.0 * brightness(0.0f));

    auto pluck = make(kFoldSynthBlockDescriptor);
    pluck->setParam(kFoldParamGate, 1.0f);
    pluck->setParam(kFoldParamDecay, 100.0f);
    auto x = note(196.0, 48000); // a steady note: only the ping makes it fade
    testing::runBlock(*pluck, x);
    CHECK(testing::rms(x[0], ms(30), ms(80)) > 0.01);
    CHECK(testing::rms(x[0], ms(700), ms(900)) < 0.01 * testing::rms(x[0], ms(30), ms(80)));
}

TEST_CASE("acid synth's filter snaps open on the pick and closes")
{
    auto block = make(kAcidSynthBlockDescriptor);
    block->setParam(kAcidParamOctave, 2.0f);
    block->setParam(kAcidParamDecay, 100.0f);
    block->setParam(kAcidParamEnvMod, 100.0f);
    auto x = note(110.0, 48000);
    testing::runBlock(*block, x);
    CHECK(highShare(x[0], 1500.0, ms(20), ms(80)) > 3.0 * highShare(x[0], 1500.0, ms(600), ms(900)));
}

TEST_CASE("string machine swells in, rings on, and spreads in stereo")
{
    auto block = make(kStringSynthBlockDescriptor); // 300 ms attack, 1.2 s release
    auto x = note(196.0, 72000, 2);
    for (auto& ch : x)
        std::fill(ch.begin() + 48000, ch.end(), 0.0f);
    testing::runBlock(*block, x);
    const double full = testing::rms(x[0], ms(800), ms(1000));
    CHECK(full > 0.02);
    CHECK(testing::rms(x[0], 0, ms(40)) < 0.25 * full);
    // the input stops at 1 s: still ringing 300 ms later
    CHECK(testing::rms(x[0], ms(1250), ms(1350)) > 0.3 * full);
    double difference = 0.0;
    for (size_t i = 24000; i < 48000; ++i)
        difference = std::max(difference, static_cast<double>(std::fabs(x[0][i] - x[1][i])));
    CHECK(difference > 0.005);
}

TEST_CASE("chip synth snaps to semitones and arpeggiates")
{
    // a slightly sharp A (445 Hz) plays as A 440 when quantized
    auto block = make(kChipSynthBlockDescriptor);
    block->setParam(kChipParamQuantize, 1.0f); // (off by default)
    block->setParam(kChipParamVibrato, 0.0f);
    block->setParam(kChipParamWave, 2.0f); // a square: no even harmonics of its own
    auto x = note(445.0, 48000);
    testing::runBlock(*block, x);
    CHECK(toneAt(x[0], 440.0, 24000, 48000) > 4.0 * toneAt(x[0], 445.0, 24000, 48000));
    CHECK(toneAt(x[0], 880.0, 24000, 48000) < 0.1 * toneAt(x[0], 440.0, 24000, 48000));

    // the octave arpeggio adds the octave
    block->setParam(kChipParamArp, 1.0f);
    auto y = note(445.0, 48000);
    testing::runBlock(*block, y);
    CHECK(toneAt(y[0], 880.0, 24000, 48000) > 0.3 * toneAt(y[0], 440.0, 24000, 48000));
}

// ---------------------------------------------------------------------------------------------------------------------
// Poly tracking (the "poly" config; owner, 2026-11-10: in place of the Poly Chip): every note of a chord gets a voice.
// The Chip Synth in Poly is tested the way the Poly Chip was.

namespace {

// Several guitar-like notes at once (with the note() helper's harmonics), or pure sines
std::vector<std::vector<float>> chord(const std::vector<double>& notes, size_t length, bool harmonics = true)
{
    std::vector<std::vector<float>> x(1, std::vector<float>(length, 0.0f));
    for (const double hz : notes)
    {
        const auto one = note(hz, length);
        for (size_t i = 0; i < length; ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            x[0][i] += harmonics ? one[0][i] : static_cast<float>(0.2 * std::sin(2 * M_PI * hz * t));
        }
    }
    return x;
}

std::unique_ptr<Block> makePoly(const BlockDescriptor& type)
{
    auto block = make(type);
    block->configure(nlohmann::json { { "poly", true } });
    return block;
}

// The Chip Synth in Poly with a square (no even harmonics of its own), no crush or vibrato, notes snapped to
// semitones (off by default) and the Poly Chip's Pick Filter (15 ms)
std::unique_ptr<Block> polyChip()
{
    auto block = makePoly(kChipSynthBlockDescriptor);
    block->setParam(kChipParamWave, 2.0f);
    block->setParam(kChipParamCrush, 0.0f);
    block->setParam(kChipParamVibrato, 0.0f);
    block->setParam(kChipParamQuantize, 1.0f);
    block->setParam(kChipParamPickFilter, 15.0f);
    return block;
}

double semitone(int midi)
{
    return 440.0 * std::exp2((midi - 69) / 12.0);
}

} // namespace

TEST_CASE("chip synth in poly plays one note, without the string's harmonics")
{
    for (const int midi : { 40, 55, 69, 81 }) // E2, G3, A4, A5
    {
        CAPTURE(midi);
        const double hz = semitone(midi);
        auto block = polyChip();
        auto x = note(hz * std::exp2(0.2 / 12.0), 48000); // 20 cents sharp: snapped to the semitone
        testing::runBlock(*block, x);
        const double played = toneAt(x[0], hz, 24000, 48000);
        CHECK(played > 0.05);
        if (midi >= 69) // a 0.5 s window can't tell 20 cents apart on low notes
            CHECK(played > 4.0 * toneAt(x[0], hz * std::exp2(0.2 / 12.0), 24000, 48000));
        CHECK(toneAt(x[0], 2.0 * hz, 24000, 48000) < 0.1 * played); // no octave voice from the 2nd harmonic
        CHECK(played > 5.0 * toneAt(x[0], hz * std::exp2(-1.0 / 12.0), 24000, 48000));
        CHECK(played > 5.0 * toneAt(x[0], hz * std::exp2(1.0 / 12.0), 24000, 48000));
    }
}

TEST_CASE("chip synth in poly plays every note of a chord")
{
    // an open E major chord, low strings: E2 B2 E3 G#3 B3 (the upper E and B are octaves of lower notes, so they're
    // heard as harmonics); and a C major triad higher up
    for (const auto& notes : { std::vector<int> { 40, 47, 56 }, std::vector<int> { 60, 64, 67 } })
    {
        auto block = polyChip();
        std::vector<double> hz;
        for (const int midi : notes)
            hz.push_back(semitone(midi));
        auto x = chord(hz, 48000);
        testing::runBlock(*block, x);
        double weakest = 1.0;
        for (const int midi : notes)
        {
            CAPTURE(midi);
            const double level = toneAt(x[0], semitone(midi), 24000, 48000);
            CHECK(level > 0.04);
            weakest = std::min(weakest, level);
        }
        // not a note of the chord, nor a semitone off one
        for (const int off : { -1, 1, 2 })
            CHECK(toneAt(x[0], semitone(notes[1] + off), 24000, 48000) < 0.2 * weakest);
    }
}

TEST_CASE("chip synth in poly falls silent after the chord")
{
    auto block = polyChip();
    auto x = chord({ semitone(52), semitone(56), semitone(59) }, 48000);
    std::fill(x[0].begin() + 24000, x[0].end(), 0.0f);
    testing::runBlock(*block, x);
    CHECK(testing::rms(x[0], 12000, 24000) > 0.05);
    CHECK(testing::rms(x[0], 24000 + ms(400), 48000) < 1e-4);
}

namespace {

// A plucked string as a magnetic pickup hears it (Karplus-Strong from a pick-shaped start, read as velocity), with
// a 2 ms noise click for the pick itself
std::vector<std::vector<float>> pickedString(double hz, size_t length)
{
    std::mt19937 rng(static_cast<uint32_t>(hz));
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const auto period = static_cast<size_t>(std::lround(kRate / hz));
    std::vector<float> string(period);
    const double pick = 0.2 * static_cast<double>(period);
    double mean = 0.0;
    for (size_t i = 0; i < period; ++i)
    {
        const double p = static_cast<double>(i);
        string[i] = static_cast<float>(p < pick ? p / pick : (static_cast<double>(period) - p) / (static_cast<double>(period) - pick));
        mean += string[i];
    }
    for (float& v : string)
        v -= static_cast<float>(mean / static_cast<double>(period));
    std::vector<std::vector<float>> x(1, std::vector<float>(length));
    size_t q = 0;
    float previous = 0.0f, last = 0.0f;
    for (size_t i = 0; i < length; ++i)
    {
        const float v = string[q];
        string[q] = 0.997f * 0.5f * (v + previous);
        previous = v;
        q = (q + 1) % period;
        const float velocity = (v - last) * static_cast<float>(period) / 8.0f;
        last = v;
        x[0][i] = 0.3f * velocity + (i < 96 ? 0.2f * noise(rng) * (1.0f - static_cast<float>(i) / 96.0f) : 0.0f);
    }
    return x;
}

// The loudest tone at a semitone above `midi` that a square on it doesn't have (its odd harmonics), leaving out
// frequencies the window can't tell from the note's own
double strayNotes(const std::vector<float>& x, int midi, size_t from, size_t to)
{
    const double resolution = 3.0 * kRate / static_cast<double>(to - from);
    double stray = 0.0;
    for (int m = midi + 1; m <= 88; ++m)
    {
        const double hz = semitone(m);
        bool own = false;
        for (int h = 1; h <= 25; h += 2)
            own |= std::fabs(hz - h * semitone(midi)) < resolution;
        if (!own)
            stray = std::max(stray, toneAt(x, hz, from, to));
    }
    return stray;
}

} // namespace

TEST_CASE("chip synth in poly: the pick filter keeps the pick attack from sounding as high notes")
{
    for (const int midi : { 40, 50, 55, 64 })
    {
        CAPTURE(midi);
        const double hz = semitone(midi);
        // the attack: the first 150 ms
        auto raw = polyChip();
        raw->setParam(kChipParamPickFilter, 0.0f);
        auto x = pickedString(hz, 48000);
        testing::runBlock(*raw, x);
        const double unfiltered = strayNotes(x[0], midi, 0, ms(150));

        auto filtered = polyChip(); // the Poly Chip's default Pick Filter
        auto y = pickedString(hz, 48000);
        testing::runBlock(*filtered, y);
        const double clean = strayNotes(y[0], midi, 0, ms(150));
        CAPTURE(unfiltered);
        CAPTURE(clean);
        // (the note finder waits for a single note to be found before it starts others, whatever the Pick Filter,
        // so without it the attack is nearly as clean: this was unfiltered > 3 * clean with the filter bank)
        CHECK(clean < 0.01); // the analysis window's own floor around the note
        CHECK(unfiltered < 0.05);
        // and the note itself still plays
        CHECK(toneAt(y[0], hz, ms(200), ms(700)) > 0.05);
    }
}

TEST_CASE("chip synth in poly with quantize off plays the pitch you play")
{
    const double sharp = 440.0 * std::exp2(0.4 / 12.0); // 40 cents sharp
    for (const bool quantize : { true, false })
    {
        CAPTURE(quantize);
        auto block = polyChip();
        block->setParam(kChipParamQuantize, quantize ? 1.0f : 0.0f);
        auto x = note(sharp, 48000);
        testing::runBlock(*block, x);
        const double atNote = toneAt(x[0], 440.0, 24000, 48000);
        const double atPlayed = toneAt(x[0], sharp, 24000, 48000);
        if (quantize)
            CHECK(atNote > 4.0 * atPlayed);
        else
            CHECK(atPlayed > 4.0 * atNote);
    }
}

TEST_CASE("chip synth in poly bends smoothly across semitones with quantize off")
{
    // a sine bending up two semitones over a second: the voice moves from band to band without dropping out
    auto block = polyChip();
    block->setParam(kChipParamQuantize, 0.0f);
    std::vector<std::vector<float>> x(1, std::vector<float>(ms(1500)));
    double phase = 0.0;
    for (size_t i = 0; i < x[0].size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        const double bend = std::clamp(t - 0.25, 0.0, 1.0) * 2.0;
        phase += 220.0 * std::exp2(bend / 12.0) / kRate;
        x[0][i] = static_cast<float>(0.2 * std::sin(2.0 * M_PI * phase));
    }
    testing::runBlock(*block, x);
    const double held = testing::rms(x[0], ms(100), ms(250));
    double lowest = held;
    for (size_t from = ms(250); from + ms(20) <= ms(1250); from += ms(10))
        lowest = std::min(lowest, testing::rms(x[0], from, from + ms(20)));
    CHECK(held > 0.05);
    CHECK(lowest > 0.6 * held);
    // and it ends on the bent note
    const double target = 220.0 * std::exp2(2.0 / 12.0);
    CHECK(toneAt(x[0], target, ms(1300), ms(1500)) > 4.0 * toneAt(x[0], 220.0 * std::exp2(1.0 / 12.0), ms(1300), ms(1500)));
}

TEST_CASE("the synths in poly play every note of a chord, each with its own voice")
{
    // C3 G3 E4: far enough apart that no note is a harmonic of another
    const std::vector<int> notes { 48, 55, 64 };
    // (not the Super Saw: its detuned saws beat, so a note's level in a short window wanders)
    for (const BlockDescriptor* type : { &kSynthBlockDescriptor, &kFmSynthBlockDescriptor, &kChipSynthBlockDescriptor,
                                         &kWavetableBlockDescriptor, &kSubBassBlockDescriptor })
    {
        const std::string typeId = type->typeId;
        CAPTURE(typeId);
        auto block = makePoly(*type);
        auto x = chord({ semitone(48), semitone(55), semitone(64) }, 48000);
        testing::runBlock(*block, x);
        double loudest = 0.0, weakest = 1.0;
        for (const int midi : notes)
        {
            const double level = bandAt(x[0], semitone(midi), 24000, 48000);
            loudest = std::max(loudest, level);
            weakest = std::min(weakest, level);
        }
        CHECK(loudest > 0.01);
        CHECK(weakest > 0.15 * loudest);
    }
}

TEST_CASE("poly tracking: Mono is the default, and Poly takes the lookahead in whole hops")
{
    CHECK_FALSE(polyFromConfig(nlohmann::json::object()));
    CHECK_FALSE(polyFromConfig(nlohmann::json { { "poly", false } }));
    CHECK(polyFromConfig(nlohmann::json { { "poly", true } }));
    for (const BlockDescriptor* type : BlockRegistry::builtin().types())
    {
        if (!type->poly)
            continue;
        const std::string typeId = type->typeId;
        CAPTURE(typeId);
        auto block = make(*type);
        block->configure(nlohmann::json { { "poly", true }, { "lookahead", 10.0 }, { "hq", false } });
        // 10 ms in hops of 256 samples at 48 kHz (5.3 ms): two
        CHECK(block->latency() == 512);
        block->configure(nlohmann::json { { "poly", false }, { "lookahead", 10.0 } });
        CHECK(block->latency() == 480);
    }
}

TEST_CASE("pick filter holds a new note until its pitch is steady")
{
    PickFilter pick;
    pick.prepare(kRate);
    pick.setMs(10.0f); // 480 samples
    // an unsteady attack: estimates jumping about every 4 ms are never accepted
    for (int i = 0; i < 2000; ++i)
    {
        const double estimate = i % 192 == 0 ? (i % 384 == 0 ? 880.0 : 330.0) : 0.0;
        CHECK_FALSE(pick.process(true, estimate));
    }
    CHECK_FALSE(pick.open());
    // a steady note is accepted once it has held for 10 ms
    int accepted = -1;
    for (int i = 0; i < 1000 && accepted < 0; ++i)
    {
        if (pick.process(true, i % 192 == 0 ? 220.0 * (1.0 + 0.001 * i / 192) : 0.0))
            accepted = i;
    }
    CHECK(accepted >= 480);
    CHECK(accepted <= 481);
    CHECK(pick.open());
    // a bend follows at once
    CHECK(pick.process(true, 225.0));
    CHECK(pick.hz() == 225.0);
    // a jump to another note waits, the old one playing on
    CHECK_FALSE(pick.process(true, 330.0));
    CHECK(pick.hz() == 225.0);
    // silence closes it
    pick.process(false, 0.0);
    CHECK_FALSE(pick.open());

    // 0 ms passes every estimate straight through
    PickFilter off;
    off.prepare(kRate);
    CHECK(off.open());
    CHECK(off.process(true, 440.0));
    CHECK(off.hz() == 440.0);
    CHECK(off.process(true, 880.0));
    CHECK(off.hz() == 880.0);
}

TEST_CASE("every mono synth has a pick filter that holds the note back, then plays it")
{
    for (const BlockDescriptor* type : { &kSynthBlockDescriptor, &kSuperSawBlockDescriptor, &kFmSynthBlockDescriptor,
                                         &kFoldSynthBlockDescriptor, &kAcidSynthBlockDescriptor,
                                         &kStringSynthBlockDescriptor, &kChipSynthBlockDescriptor })
    {
        CAPTURE(std::string(type->typeId));
        // the last two controls: Pick Filter, then Track
        const int last = type->numParams - 2;
        REQUIRE(std::string(type->params[static_cast<size_t>(last)].name) == "Pick Filter");
        CHECK(type->params[static_cast<size_t>(last)].def == 0.0f); // off by default: synths sound as they did
        REQUIRE(std::string(type->params[static_cast<size_t>(last + 1)].name) == "Track");
        CHECK(type->params[static_cast<size_t>(last + 1)].def == 1.0f); // Rig In (owner, 2026-10-24)
        CHECK(type->lookahead);

        // when the synth starts sounding, and how loud it plays once the note has settled
        const auto run = [&](float pickMs, size_t& start, double& level) {
            auto block = make(*type);
            block->setParam(last, pickMs);
            auto x = note(220.0, 48000);
            int allocations = 0;
            testing::runBlock(*block, x, 128, &allocations);
            CHECK(allocations == 0);
            float peak = 0.0f;
            for (const float v : x[0])
                peak = std::max(peak, std::fabs(v));
            start = 0;
            while (start < x[0].size() && std::fabs(x[0][start]) < 0.05f * peak)
                ++start;
            double sum = 0.0;
            for (size_t i = ms(400); i < ms(900); ++i)
                sum += static_cast<double>(x[0][i]) * x[0][i];
            level = std::sqrt(sum / static_cast<double>(ms(500)));
        };
        size_t startOff = 0, startOn = 0;
        double levelOff = 0.0, levelOn = 0.0;
        run(0.0f, startOff, levelOff);
        run(40.0f, startOn, levelOn);
        CAPTURE(startOff);
        CAPTURE(startOn);
        CHECK(startOn >= startOff + ms(25));
        CHECK(levelOn > 0.7 * levelOff);
        CHECK(levelOn < 1.4 * levelOff);
    }
}

namespace {

// Runs a pitch-following block on `input`, tracking `rig` (both mono); returns the output
std::vector<float> runTrackingRig(Block& block, std::vector<float> input, const std::vector<float>& rig)
{
    int allocations = 0;
    for (size_t t = 0; t < input.size(); t += 128)
    {
        const auto n = static_cast<uint32_t>(std::min<size_t>(128, input.size() - t));
        float* io[1] = { input.data() + t };
        const float* key[1] = { rig.data() + t };
        testing::AllocationGuard guard;
        block.setRigInput(key, 1);
        block.process(io, 1, n);
        allocations += guard.count();
    }
    CHECK(allocations == 0);
    return input;
}

} // namespace

TEST_CASE("tracking the rig, pitch-following blocks play at the level of their own input")
{
    // Rig In is the default; the sound still comes from the signal path: a volume pedal (or a gate) before the
    // block turns it down (or off) even though the rig's input plays on
    std::vector<float> rig = note(220.0, 48000)[0];
    for (float& s : rig)
        s *= 0.05f;
    std::vector<float> half = rig;
    for (float& s : half)
        s *= 0.5f;
    const std::vector<float> silent(rig.size(), 0.0f);

    for (const BlockDescriptor* type : { &kSynthBlockDescriptor, &kSuperSawBlockDescriptor })
    for (const bool poly : { false, true })
    {
        CAPTURE(std::string(type->typeId));
        CAPTURE(poly);
        const auto run = [&](const std::vector<float>& input) {
            auto block = poly ? makePoly(*type) : make(*type);
            const std::vector<float> out = runTrackingRig(*block, input, rig);
            return testing::rms(out, 24000, 48000);
        };
        const double full = run(rig);
        REQUIRE(full > 1e-3);
        CHECK(run(half) / full == doctest::Approx(0.5).epsilon(0.15));
        CHECK(run(silent) < 1e-6);
    }
}
