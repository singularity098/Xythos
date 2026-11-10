// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Behaviour tests for the third set of pedals (2026-10-23): Sympathetic Strings, Jawari, Grain Cloud, Swarm, Tape
// Stop, Bouncing Ball, Pattern Delay, Chaos Mod, Acoustic Sim, Strum Machine, Hard Tune, Drum Trigger, and the
// instruments (Fart, Trumpet, Violin, Piano)

#include "GuitarSignals.hpp"
#include "TestSupport.hpp"

#include "engine/CpuHints.hpp"
#include "engine/blocks/CloudBlocks.hpp"
#include "engine/blocks/FartSamples.hpp"
#include "engine/blocks/InstrumentBlocks.hpp"
#include "engine/blocks/PlayedBlocks.hpp"
#include "engine/blocks/ResonatorBlocks.hpp"
#include "engine/blocks/TextureBlocks.hpp"
#include "engine/blocks/TimeFxBlocks.hpp"
#include "engine/dsp/Biquad.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

size_t ms(double milliseconds) { return static_cast<size_t>(milliseconds / 1000.0 * kRate); }

std::unique_ptr<Block> make(const BlockDescriptor& type) { return testing::createWithDefaults(type, kRate); }

std::vector<std::vector<float>> sine(double hz, float amplitude, size_t length, int channels = 1)
{
    std::vector<std::vector<float>> x(channels, std::vector<float>(length));
    for (auto& ch : x)
        for (size_t i = 0; i < length; ++i)
            ch[i] = amplitude * static_cast<float>(std::sin(2.0 * M_PI * hz * static_cast<double>(i) / kRate));
    return x;
}

std::vector<std::vector<float>> impulse(size_t length, int channels = 1)
{
    std::vector<std::vector<float>> x(channels, std::vector<float>(length, 0.0f));
    for (auto& ch : x)
        ch[0] = 1.0f;
    return x;
}

// One picked note on the synthetic guitar (no hum or hiss), muted after `seconds`
std::vector<std::vector<float>> picked(double midi, double seconds, double total, int channels = 1)
{
    guitar::Note note;
    note.midi = midi;
    note.start = 0.05;
    note.length = seconds;
    note.string = 2;
    const guitar::Performance p = guitar::perform({ note }, total, { 0.0f, 0.0f });
    return std::vector<std::vector<float>>(channels, p.signal);
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

// Share of x[from, to)'s level above `hz`
double highShare(const std::vector<float>& x, double hz, size_t from, size_t to)
{
    Biquad filter[2];
    for (auto& f : filter)
        f.setHighPass(kRate, hz);
    std::vector<float> high(x);
    for (float& v : high)
        v = filter[1].process(filter[0].process(v));
    return testing::rms(high, from, to) / std::max(1e-12, testing::rms(x, from, to));
}

double midiHz(double midi) { return 440.0 * std::exp2((midi - 69.0) / 12.0); }

// Where the biggest peaks of |x| are, at least `gap` apart and over `floor`
std::vector<size_t> peaks(const std::vector<float>& x, float floor, size_t gap)
{
    std::vector<size_t> found;
    for (size_t i = 0; i + 1 < x.size(); ++i)
    {
        const float a = std::fabs(x[i]);
        if (a < floor || (i > 0 && a < std::fabs(x[i - 1])) || a < std::fabs(x[i + 1]))
            continue;
        if (!found.empty() && i - found.back() < gap)
        {
            if (a > std::fabs(x[found.back()]))
                found.back() = i;
            continue;
        }
        found.push_back(i);
    }
    return found;
}

} // namespace

TEST_CASE("sympathetic strings ring on after a note in their tuning, much less after one out of it")
{
    // key E, Drone: strings on E and B. An E rings them; an F barely does.
    auto ringAfter = [](double midi) {
        auto block = make(kSympatheticBlockDescriptor);
        block->setParam(kSympParamLevel, 100.0f);
        auto x = picked(midi, 0.5, 2.0);
        testing::runBlock(*block, x);
        return testing::rms(x[0], ms(700), ms(1500)); // after the note is muted
    };
    const double e = ringAfter(52.0), f = ringAfter(53.0);
    CAPTURE(e);
    CAPTURE(f);
    CHECK(e > 0.003);
    CHECK(e > 4.0 * f);

    // with the strings at 0 the dry signal passes untouched
    auto block = make(kSympatheticBlockDescriptor);
    block->setParam(kSympParamLevel, 0.0f);
    auto x = sine(330.0, 0.3f, ms(300));
    const auto dry = x;
    testing::runBlock(*block, x);
    for (size_t i = 0; i < x[0].size(); ++i)
        REQUIRE(x[0][i] == doctest::Approx(dry[0][i]).epsilon(1e-5));
}

TEST_CASE("jawari buzzes: harmonics grow with Buzz, and Bloom makes the buzz grow as the note fades")
{
    auto buzz = [](float amount, float bloom, double from, double to) {
        auto block = make(kJawariBlockDescriptor);
        block->setParam(kJawariParamBuzz, amount);
        block->setParam(kJawariParamBloom, bloom);
        // a note fading over 1.5 s
        std::vector<std::vector<float>> x(1, std::vector<float>(ms(1500)));
        for (size_t i = 0; i < x[0].size(); ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            x[0][i] = static_cast<float>(0.4 * std::exp(-t / 0.5) * std::sin(2.0 * M_PI * 220.0 * t));
        }
        const auto dry = x;
        testing::runBlock(*block, x);
        // the buzz's level (over 1.5 kHz) against the note's
        return highShare(x[0], 1500.0, ms(from), ms(to)) * testing::rms(x[0], ms(from), ms(to))
               / testing::rms(dry[0], ms(from), ms(to));
    };
    CHECK(buzz(0.0f, 0.0f, 100, 300) < 0.01);
    CHECK(buzz(60.0f, 0.0f, 100, 300) > 0.05);
    CHECK(buzz(100.0f, 0.0f, 100, 300) > buzz(30.0f, 0.0f, 100, 300));
    // Bloom: more buzz late in the note than early
    CHECK(buzz(60.0f, 0.0f, 1000, 1400) == doctest::Approx(buzz(60.0f, 0.0f, 100, 300)).epsilon(0.1));
    CHECK(buzz(60.0f, 100.0f, 1000, 1400) > 1.3 * buzz(60.0f, 100.0f, 100, 300));
}

TEST_CASE("grain cloud plays on after the input stops, and Pitch shifts the grains")
{
    auto block = make(kGrainCloudBlockDescriptor);
    block->setParam(kGrainParamMix, 100.0f);
    block->setParam(kGrainParamReverse, 0.0f);
    auto x = sine(440.0, 0.4f, ms(1500));
    for (size_t i = ms(500); i < x[0].size(); ++i)
        x[0][i] = 0.0f;
    testing::runBlock(*block, x);
    CHECK(testing::rms(x[0], ms(200), ms(500)) > 0.05);
    CHECK(testing::rms(x[0], ms(550), ms(750)) > 0.02); // the cloud still playing what came before
    CHECK(testing::rms(x[0], ms(1300), ms(1500)) < 1e-4);

    auto up = make(kGrainCloudBlockDescriptor);
    up->setParam(kGrainParamMix, 100.0f);
    up->setParam(kGrainParamReverse, 0.0f);
    up->setParam(kGrainParamPitch, 1.0f); // +12
    auto y = sine(440.0, 0.4f, ms(1000));
    testing::runBlock(*up, y);
    CHECK(toneAt(y[0], 880.0, ms(300), ms(1000)) > 4.0 * toneAt(y[0], 440.0, ms(300), ms(1000)));

    // Freeze keeps the cloud going on what's in the buffer
    auto frozen = make(kGrainCloudBlockDescriptor);
    frozen->setParam(kGrainParamMix, 100.0f);
    auto z = sine(440.0, 0.4f, ms(3000));
    for (size_t i = ms(500); i < z[0].size(); ++i)
        z[0][i] = 0.0f;
    for (size_t t = 0; t < z[0].size(); t += 128)
    {
        if (t >= ms(400))
            frozen->setParam(kGrainParamFreeze, 1.0f);
        float* io[1] = { z[0].data() + t };
        frozen->process(io, 1, static_cast<uint32_t>(std::min<size_t>(128, z[0].size() - t)));
    }
    CHECK(testing::rms(z[0], ms(2500), ms(3000)) > 0.02);
}

TEST_CASE("swarm: Buzz flutters the level, Drift spreads the pitch")
{
    // the level's ups and downs over 5 ms windows
    auto flutter = [](float buzz, float drift) {
        auto block = make(kSwarmBlockDescriptor);
        block->setParam(kSwarmParamMix, 100.0f);
        block->setParam(kSwarmParamBuzz, buzz);
        block->setParam(kSwarmParamDrift, drift);
        auto x = sine(1000.0, 0.3f, ms(1000));
        testing::runBlock(*block, x);
        double lo = 1e9, hi = 0.0;
        for (size_t t = ms(200); t + ms(5) < ms(1000); t += ms(5))
        {
            const double r = testing::rms(x[0], t, t + ms(5));
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        return std::make_pair(hi / std::max(lo, 1e-9), toneAt(x[0], 1000.0, ms(200), ms(1000)));
    };
    const auto calm = flutter(0.0f, 0.0f);
    const auto bees = flutter(100.0f, 0.0f);
    CHECK(bees.first > 2.0 * calm.first);
    // drift spreads the energy away from the exact pitch
    const auto drifting = flutter(0.0f, 60.0f);
    CHECK(drifting.second < 0.8 * calm.second);
}

TEST_CASE("tape stop slows the pitch to a halt, then spins back up and lands back in time")
{
    auto block = make(kTapeStopBlockDescriptor);
    block->setParam(kTapeStopParamStopTime, 500.0f);
    block->setParam(kTapeStopParamStartTime, 300.0f);
    block->setParam(kTapeStopParamCurve, 0.0f);
    auto x = sine(1000.0, 0.5f, ms(2000));
    const auto dry = x;
    // hold Stop from 200 ms to 1000 ms
    for (size_t t = 0; t < x[0].size(); t += 128)
    {
        block->setParam(kTapeStopParamStop, t >= ms(200) && t < ms(1000) ? 1.0f : 0.0f);
        float* io[1] = { x[0].data() + t };
        block->process(io, 1, 128);
    }
    // halfway into the stop the pitch is about half (a linear slope)
    CHECK(toneAt(x[0], 500.0, ms(420), ms(480)) > 3.0 * toneAt(x[0], 1000.0, ms(420), ms(480)));
    // stopped: silent
    CHECK(testing::rms(x[0], ms(750), ms(1000)) < 1e-3);
    // back up, and after the spin-up exactly the live signal again
    double error = 0.0;
    for (size_t i = ms(1400); i < ms(2000); ++i)
        error = std::max(error, static_cast<double>(std::fabs(x[0][i] - dry[0][i])));
    CHECK(error < 1e-4);
    // during the spin-up it plays below pitch, rising
    CHECK(testing::rms(x[0], ms(1100), ms(1300)) > 0.1);
}

TEST_CASE("bouncing ball: the echoes come closer together, each quieter (Fall), or the other way round (Rise)")
{
    auto gaps = [](bool rise) {
        auto block = make(kBounceDelayBlockDescriptor);
        block->setParam(kBounceParamMix, 50.0f); // dry and echoes both at full level
        block->setParam(kBounceParamTime, 300.0f);
        block->setParam(kBounceParamBounces, 1.0f); // 6
        block->setParam(kBounceParamTone, 16000.0f);
        block->setParam(kBounceParamSpread, 0.0f);
        block->setParam(kBounceParamDirection, rise ? 1.0f : 0.0f);
        auto x = impulse(ms(3000));
        testing::runBlock(*block, x);
        return peaks(x[0], 0.01f, ms(10));
    };
    const auto fall = gaps(false);
    REQUIRE(fall.size() == 7); // the dry hit and six bounces
    CHECK(fall[1] - fall[0] == doctest::Approx(ms(300)).epsilon(0.01));
    for (size_t k = 2; k < fall.size(); ++k)
        CHECK(fall[k] - fall[k - 1] < fall[k - 1] - (k >= 2 ? fall[k - 2] : 0));
    const auto rise = gaps(true);
    REQUIRE(rise.size() == 7);
    for (size_t k = 2; k < rise.size(); ++k)
        CHECK(rise[k] - rise[k - 1] > rise[k - 1] - rise[k - 2]);
    CHECK(rise.back() - rise[rise.size() - 2] == doctest::Approx(ms(300)).epsilon(0.01));
}

TEST_CASE("pattern delay plays its taps on the grid at their levels, and Pitch shifts every other one")
{
    auto block = make(kPatternDelayBlockDescriptor);
    block->setParam(kPatternParamMix, 50.0f);
    block->setParam(kPatternParamTime, 100.0f);
    block->setParam(kPatternParamFeedback, 0.0f);
    block->setParam(kPatternParamTone, 16000.0f);
    block->setParam(kPatternParamSpread, 0.0f);
    auto x = impulse(ms(1200));
    testing::runBlock(*block, x);
    // the default pattern: taps 3, 6 and 8, at 70%, 50% and 35% (of the centre's equal-power pan, 0.707)
    const auto found = peaks(x[0], 0.05f, ms(20));
    REQUIRE(found.size() == 4);
    CHECK(found[1] == doctest::Approx(ms(300)).epsilon(0.01));
    CHECK(found[2] == doctest::Approx(ms(600)).epsilon(0.01));
    CHECK(found[3] == doctest::Approx(ms(800)).epsilon(0.01));
    CHECK(std::fabs(x[0][found[1]]) == doctest::Approx(0.7f * std::sqrt(0.5f)).epsilon(0.05));
    CHECK(std::fabs(x[0][found[3]]) == doctest::Approx(0.35f * std::sqrt(0.5f)).epsilon(0.05));

    // Oct Up: an even tap (6) plays an octave up, an odd one (3) as it was
    auto pitched = make(kPatternDelayBlockDescriptor);
    pitched->setParam(kPatternParamMix, 100.0f);
    pitched->setParam(kPatternParamTime, 300.0f);
    pitched->setParam(kPatternParamFeedback, 0.0f);
    pitched->setParam(kPatternParamPitch, 1.0f);
    for (int k = 0; k < kPatternTaps; ++k)
        pitched->setParam(kPatternParamTap1 + k, k == 0 || k == 1 ? 100.0f : 0.0f);
    auto y = sine(440.0, 0.4f, ms(300));
    y[0].resize(ms(1100), 0.0f);
    testing::runBlock(*pitched, y);
    // tap 1 (300-600 ms): at pitch; tap 2 (600-900 ms): an octave up
    CHECK(toneAt(y[0], 440.0, ms(350), ms(550)) > 4.0 * toneAt(y[0], 880.0, ms(350), ms(550)));
    CHECK(toneAt(y[0], 880.0, ms(680), ms(860)) > 4.0 * toneAt(y[0], 440.0, ms(680), ms(860)));
}

TEST_CASE("chaos mod moves the sound without repeating, and Width splits the channels")
{
    auto block = make(kChaosModBlockDescriptor);
    block->setParam(kChaosParamSpeed, 2.0f);
    block->setParam(kChaosParamFilter, 100.0f);
    auto x = sine(1000.0, 0.3f, ms(6000), 2);
    testing::runBlock(*block, x);
    // the level after the filter wanders over a wide range
    double lo = 1e9, hi = 0.0;
    for (size_t t = ms(500); t + ms(50) < x[0].size(); t += ms(50))
    {
        const double r = testing::rms(x[0], t, t + ms(50));
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    CHECK(hi > 3.0 * lo);
    double diff = 0.0;
    for (size_t i = ms(3000); i < ms(6000); ++i)
        diff = std::max(diff, static_cast<double>(std::fabs(x[0][i] - x[1][i])));
    CHECK(diff > 0.02);
}

TEST_CASE("acoustic sim adds the body's low resonance and takes off the pickup's treble peak")
{
    auto gain = [](double hz) {
        auto block = make(kAcousticBlockDescriptor);
        block->setParam(kAcousticParamWidth, 0.0f);
        auto x = sine(hz, 0.1f, ms(400));
        testing::runBlock(*block, x);
        return testing::rms(x[0], ms(200), ms(400)) / (0.1 / std::sqrt(2.0));
    };
    // the dreadnought's air resonance near 100 Hz against the boxy dip, and the humbucker's peak at 2.5 kHz
    CHECK(gain(100.0) > 2.0 * gain(450.0));
    CHECK(gain(2500.0) < gain(1500.0));
}

TEST_CASE("strum machine strums the chord: the notes come in one after another, low to high")
{
    auto block = make(kStrumBlockDescriptor);
    block->setParam(kStrumParamChord, 1.0f); // Major: +4, +7, +12
    block->setParam(kStrumParamStrum, 60.0f);
    block->setParam(kStrumParamDry, 0.0f);
    auto x = picked(57.0, 1.0, 1.2); // A3
    testing::runBlock(*block, x);
    // the note is found ~15 ms after the pick (at 50 ms); the third comes in a gap later, the fifth two
    const double third = midiHz(61.0), fifth = midiHz(64.0);
    const size_t at = ms(50 + 15);
    CHECK(toneAt(x[0], third, at + ms(70), at + ms(115)) > 2.0 * toneAt(x[0], fifth, at + ms(70), at + ms(115)));
    CHECK(toneAt(x[0], fifth, ms(400), ms(900)) > 0.01);
    CHECK(toneAt(x[0], third, ms(400), ms(900)) > 0.01);

    // the chords: diatonic in C major on E is E minor (G, B), on F is F major (A, C)
    CHECK(strumChordNote(0, 0, 0, 64, 0) == 3);
    CHECK(strumChordNote(0, 0, 0, 64, 1) == 7);
    CHECK(strumChordNote(0, 0, 0, 65, 0) == 4);
    CHECK(strumChordNote(0, 0, 0, 71, 1) == 6); // B diminished
}

TEST_CASE("hard tune pulls a sharp note to the nearest note of the scale")
{
    auto block = make(kHardTuneBlockDescriptor);
    block->setParam(kHardTuneParamScale, 0.0f); // chromatic
    block->setParam(kHardTuneParamSpeed, 0.0f);
    const double sharp = 220.0 * std::exp2(0.35 / 12.0); // A3, 35 cents sharp
    std::vector<std::vector<float>> x(1, std::vector<float>(ms(1500)));
    for (size_t i = 0; i < x[0].size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        x[0][i] = static_cast<float>(0.3 * std::sin(2.0 * M_PI * sharp * t) + 0.15 * std::sin(4.0 * M_PI * sharp * t));
    }
    testing::runBlock(*block, x);
    CHECK(toneAt(x[0], 220.0, ms(500), ms(1500)) > 3.0 * toneAt(x[0], sharp, ms(500), ms(1500)));
}

TEST_CASE("drum trigger: low thumps play the kick, bright clicks the snare or hi-hat")
{
    auto hit = [](bool bright) {
        auto block = make(kDrumTriggerBlockDescriptor);
        block->setParam(kDrumParamDry, 0.0f);
        std::vector<std::vector<float>> x(1, std::vector<float>(ms(500), 0.0f));
        Biquad shape;
        if (bright)
            shape.setHighPass(kRate, 4000.0);
        else
            shape.setLowPass(kRate, 150.0);
        uint32_t state = 12345;
        for (size_t i = ms(50); i < ms(70); ++i)
        {
            state = state * 1664525u + 1013904223u;
            const float noise = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
            x[0][i] = shape.process(noise) * (bright ? 1.0f : 3.0f);
        }
        testing::runBlock(*block, x);
        return x;
    };
    const auto kick = hit(false);
    const auto snare = hit(true);
    CHECK(testing::rms(kick[0], ms(60), ms(300)) > 0.05);
    CHECK(highShare(kick[0], 1000.0, ms(80), ms(300)) < 0.2);
    CHECK(testing::rms(snare[0], ms(60), ms(200)) > 0.02);
    CHECK(highShare(snare[0], 1000.0, ms(60), ms(200)) > 0.4);
}

// Where x is first and last louder than `floor` (in samples; {0, 0} if never)
std::pair<size_t, size_t> soundingSpan(const std::vector<float>& x, float floor)
{
    size_t first = x.size(), last = 0;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i]) > floor)
        {
            first = std::min(first, i);
            last = i;
        }
    return first < x.size() ? std::make_pair(first, last) : std::make_pair(size_t { 0 }, size_t { 0 });
}

TEST_CASE("fart plays every sample, and each stops on its own")
{
    REQUIRE(std::string(fartTypeName(0)) == "Rumble");
    for (int type = 0; type <= kFartTypes; ++type)
    {
        CAPTURE(type);
        auto block = make(kFartBlockDescriptor);
        block->setParam(kFartParamType, static_cast<float>(type));
        block->setParam(kFartParamPitch, 1.0f); // Natural: as recorded, at most 7.4 s
        auto x = picked(52.0, 0.3, 8.0);
        testing::runBlock(*block, x);
        CHECK(testing::rms(x[0], ms(60), ms(250)) > 0.005);
        CHECK(testing::rms(x[0], ms(7800), ms(8000)) < 1e-4);
    }
}

TEST_CASE("fart follows the note like a sampler: an octave up plays twice as fast")
{
    auto span = [](double midi, bool natural) {
        auto block = make(kFartBlockDescriptor);
        block->setParam(kFartParamType, static_cast<float>(kFartTypes)); // Mouse, the highest: 0.38 s
        block->setParam(kFartParamOctave, 2.0f);                        // 0: at the note
        block->setParam(kFartParamPitch, natural ? 1.0f : 0.0f);
        auto x = picked(midi, 0.3, 2.0);
        testing::runBlock(*block, x);
        const auto [first, last] = soundingSpan(x[0], 1e-3f);
        return static_cast<double>(last - first) / kRate;
    };
    const double low = span(57.0, false), high = span(69.0, false);
    CAPTURE(low);
    CAPTURE(high);
    // A3 is 220 Hz: the 595 Hz sample at 0.37 times the speed lasts about 1 s; A4 about 0.5 s
    CHECK(low == doctest::Approx(1.03).epsilon(0.1));
    CHECK(low / high == doctest::Approx(2.0).epsilon(0.05));
    // Natural: as recorded, whatever the note
    CHECK(span(57.0, true) == doctest::Approx(0.38).epsilon(0.1));
    CHECK(span(69.0, true) == doctest::Approx(span(57.0, true)).epsilon(0.02));
}

TEST_CASE("fart's Length cuts it short, and Held stops it with the note")
{
    auto play = [](float length, bool held) {
        auto block = make(kFartBlockDescriptor);
        block->setParam(kFartParamType, static_cast<float>(kFartTypes - 1)); // Balloon: 7.4 s as recorded
        block->setParam(kFartParamPitch, 1.0f);
        block->setParam(kFartParamLength, length);
        block->setParam(kFartParamHold, held ? 1.0f : 0.0f);
        auto x = picked(52.0, 0.3, 3.0);
        testing::runBlock(*block, x);
        return x;
    };
    const auto whole = play(100.0f, false);
    CHECK(testing::rms(whole[0], ms(2000), ms(2500)) > 0.01);
    const auto shortened = play(10.0f, false); // 0.74 s
    const auto [first, last] = soundingSpan(shortened[0], 1e-3f);
    CHECK(static_cast<double>(last - first) / kRate == doctest::Approx(0.74).epsilon(0.05));
    const auto held = play(100.0f, true);
    CHECK(testing::rms(held[0], ms(150), ms(300)) > 0.01);
    CHECK(testing::rms(held[0], ms(600), ms(3000)) < 1e-4);
}

TEST_CASE("fart's Random plays at any note, Tone filters, and the samples' WAV reader rejects others")
{
    for (double midi : { 28.0, 40.0, 52.0, 64.0, 76.0, 88.0 })
    {
        CAPTURE(midi);
        auto block = make(kFartBlockDescriptor);
        auto x = picked(midi, 0.3, 1.0);
        testing::runBlock(*block, x);
        CHECK(testing::rms(x[0], ms(60), ms(300)) > 0.005);
    }
    auto bright = [](float tone) {
        auto block = make(kFartBlockDescriptor);
        block->setParam(kFartParamType, 16.0f); // Squish
        block->setParam(kFartParamPitch, 1.0f);
        block->setParam(kFartParamTone, tone);
        auto x = picked(52.0, 0.3, 1.0);
        testing::runBlock(*block, x);
        return highShare(x[0], 1000.0, ms(60), ms(500));
    };
    CHECK(bright(0.0f) < 0.5 * bright(50.0f));
    CHECK(bright(100.0f) > bright(50.0f));

    const unsigned char junk[] = "RIFF\x10\0\0\0WAVEdata\xff\xff\xff\xff";
    CHECK(pcmFromWav(junk, sizeof(junk)).frames == 0);
    CHECK(pcmFromWav(nullptr, 0).frames == 0);
    const EmbeddedFile none = builtinFartSample("no such fart");
    CHECK(none.data == nullptr);
}

TEST_CASE("trumpet and violin play the note you play")
{
    for (const BlockDescriptor* type : { &kTrumpetBlockDescriptor, &kViolinBlockDescriptor })
    {
        CAPTURE(std::string(type->typeId));
        auto block = make(*type);
        auto x = picked(57.0, 1.2, 1.4);
        testing::runBlock(*block, x);
        const double hz = midiHz(57.0);
        const double at = toneAt(x[0], hz, ms(500), ms(1100));
        CHECK(at > 0.02);
        CHECK(at > 4.0 * toneAt(x[0], hz * std::exp2(1.0 / 12.0), ms(500), ms(1100)));
    }
}

TEST_CASE("piano strikes the note, damps it when you let go, and Sustain lets it ring")
{
    auto play = [](bool sustain) {
        auto block = make(kPianoBlockDescriptor);
        block->setParam(kPianoParamSustain, sustain ? 1.0f : 0.0f);
        auto x = picked(57.0, 0.4, 1.5);
        testing::runBlock(*block, x);
        return x;
    };
    const auto damped = play(false), held = play(true);
    const double hz = midiHz(57.0);
    CHECK(toneAt(damped[0], hz, ms(100), ms(400)) > 0.02);
    // its partials are stretched a little sharp of the harmonics (inharmonicity), but the second is near 2f
    CHECK(toneAt(damped[0], 2.0 * hz, ms(100), ms(400)) > 0.002);
    // after the note ends (at 450 ms): damped, or ringing on with Sustain
    CHECK(testing::rms(damped[0], ms(900), ms(1400)) < 0.1 * testing::rms(held[0], ms(900), ms(1400)));
    CHECK(testing::rms(held[0], ms(900), ms(1400)) > 0.005);
}

TEST_CASE("third set: CPU use" * doctest::skip(std::getenv("XYTHOS_CPU") == nullptr))
{
    // as hosts do on their audio threads: denormals flushed to zero
    const FloatState csr = flushDenormals();
    for (const BlockDescriptor* type :
         { &kSympatheticBlockDescriptor, &kJawariBlockDescriptor, &kGrainCloudBlockDescriptor, &kSwarmBlockDescriptor,
           &kTapeStopBlockDescriptor, &kBounceDelayBlockDescriptor, &kPatternDelayBlockDescriptor,
           &kChaosModBlockDescriptor, &kAcousticBlockDescriptor, &kStrumBlockDescriptor, &kHardTuneBlockDescriptor,
           &kDrumTriggerBlockDescriptor, &kFartBlockDescriptor, &kTrumpetBlockDescriptor, &kViolinBlockDescriptor,
           &kPianoBlockDescriptor })
    {
        auto block = make(*type);
        auto x = picked(57.0, 0.3, 10.0, 2);
        const auto start = std::chrono::steady_clock::now();
        testing::runBlock(*block, x, 256);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%-14s %5.2f%% of a core (stereo, 48 kHz)\n", type->typeId, 100.0 * seconds / 10.0);
    }
    setFloatState(csr);
}

namespace {

void writeWav(const std::string& path, const std::vector<std::vector<float>>& x)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    REQUIRE(f != nullptr);
    const auto channels = static_cast<uint16_t>(x.size());
    const auto frames = static_cast<uint32_t>(x[0].size());
    const uint32_t rate = static_cast<uint32_t>(kRate), bytes = frames * channels * 2;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(channels);
    u32(rate);
    u32(rate * channels * 2);
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    for (uint32_t i = 0; i < frames; ++i)
        for (uint16_t ch = 0; ch < channels; ++ch)
        {
            const auto v = static_cast<int16_t>(std::lround(std::clamp(x[ch][i], -1.0f, 1.0f) * 32000.0f));
            std::fwrite(&v, 2, 1, f);
        }
    std::fclose(f);
}

// A short lick on the synthetic guitar: single notes, a slide of notes and one left to ring
std::vector<guitar::Note> demoLick()
{
    std::vector<guitar::Note> notes;
    const double lick[][2] = { { 57, 0.25 }, { 60, 0.25 }, { 62, 0.25 }, { 64, 0.5 }, { 67, 0.25 }, { 64, 0.25 },
                               { 62, 0.5 }, { 60, 0.25 }, { 57, 1.2 } };
    double time = 0.2;
    for (const auto& [midi, length] : lick)
    {
        guitar::Note note;
        note.midi = midi;
        note.start = time;
        note.length = length - 0.02;
        note.string = midi < 59 ? 2 : (midi < 64 ? 3 : 4);
        note.velocity = 0.9f;
        notes.push_back(note);
        time += length;
    }
    return notes;
}

} // namespace

TEST_CASE("third set: demo renders" * doctest::skip(std::getenv("XYTHOS_DEMO_DIR") == nullptr))
{
    const std::string dir = std::getenv("XYTHOS_DEMO_DIR");
    const guitar::Performance lick = guitar::perform(demoLick(), 5.0, { 0.0001f, 0.0001f, 0.35f, 7 });
    const std::vector<std::vector<float>> dry = { lick.signal, lick.signal };
    writeWav(dir + "/dry.wav", dry);

    struct Demo {
        const BlockDescriptor* type;
        std::vector<std::pair<int, float>> settings;
    };
    const Demo demos[] = {
        { &kSympatheticBlockDescriptor, { { kSympParamKey, 9.0f }, { kSympParamTuning, 3.0f }, { kSympParamLevel, 80.0f } } },
        { &kJawariBlockDescriptor, {} },
        { &kGrainCloudBlockDescriptor, { { kGrainParamPitch, 5.0f }, { kGrainParamMix, 60.0f } } },
        { &kSwarmBlockDescriptor, { { kSwarmParamBuzz, 80.0f }, { kSwarmParamDrift, 40.0f }, { kSwarmParamMix, 70.0f } } },
        { &kBounceDelayBlockDescriptor, {} },
        { &kPatternDelayBlockDescriptor, { { kPatternParamPitch, 4.0f } } },
        { &kChaosModBlockDescriptor, { { kChaosParamSpeed, 2.0f } } },
        { &kAcousticBlockDescriptor, {} },
        { &kStrumBlockDescriptor, { { kStrumParamKey, 9.0f }, { kStrumParamScale, 1.0f } } },
        { &kHardTuneBlockDescriptor, { { kHardTuneParamKey, 9.0f }, { kHardTuneParamScale, 4.0f }, { kHardTuneParamSpeed, 0.0f } } },
        { &kFartBlockDescriptor, {} },
        { &kTrumpetBlockDescriptor, {} },
        { &kViolinBlockDescriptor, { { kViolinParamEnsemble, 1.0f } } },
        { &kPianoBlockDescriptor, {} },
    };
    for (const Demo& demo : demos)
    {
        auto block = make(*demo.type);
        for (const auto& [param, value] : demo.settings)
            block->setParam(param, value);
        auto x = dry;
        testing::runBlock(*block, x);
        writeWav(dir + "/" + demo.type->typeId + ".wav", x);
    }

    // tape stop: held from 1.6 s to 2.9 s
    {
        auto block = make(kTapeStopBlockDescriptor);
        auto x = dry;
        for (size_t t = 0; t < x[0].size(); t += 128)
        {
            block->setParam(kTapeStopParamStop, t >= ms(1600) && t < ms(2900) ? 1.0f : 0.0f);
            float* io[2] = { x[0].data() + t, x[1].data() + t };
            block->process(io, 2, static_cast<uint32_t>(std::min<size_t>(128, x[0].size() - t)));
        }
        writeWav(dir + "/tapestop.wav", x);
    }

    // drum trigger: muted thumps on the low string and bright slaps, a simple beat
    {
        std::vector<guitar::Note> hits;
        std::vector<double> slaps;
        for (int k = 0; k < 16; ++k)
        {
            const double t = 0.2 + 0.25 * k;
            if (k % 4 == 2)
            {
                slaps.push_back(t);
                continue;
            }
            guitar::Note note;
            note.midi = 40;
            note.start = t;
            note.length = 0.1;
            note.palmMute = true;
            note.velocity = k % 4 == 0 ? 1.0f : 0.6f;
            hits.push_back(note);
        }
        guitar::Performance p = guitar::perform(hits, 4.5, { 0.0001f, 0.0001f, 0.35f, 3 });
        Biquad bright;
        bright.setHighPass(kRate, 3000.0);
        uint32_t state = 99;
        for (const double t : slaps)
            for (size_t i = ms(t * 1000.0); i < ms(t * 1000.0 + 25.0); ++i)
            {
                state = state * 1664525u + 1013904223u;
                const float noise = static_cast<float>(state >> 8) / 8388608.0f - 1.0f;
                const float fade = 1.0f - static_cast<float>(i - ms(t * 1000.0)) / static_cast<float>(ms(25.0));
                p.signal[i] += 0.6f * bright.process(noise) * fade;
            }
        std::vector<std::vector<float>> x = { p.signal, p.signal };
        writeWav(dir + "/drumtrigger-dry.wav", x);
        auto block = make(kDrumTriggerBlockDescriptor);
        block->setParam(kDrumParamDry, 20.0f);
        testing::runBlock(*block, x);
        writeWav(dir + "/drumtrigger.wav", x);
    }
}
