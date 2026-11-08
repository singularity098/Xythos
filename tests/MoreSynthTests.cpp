// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Behaviour tests for the fourth set of synths (MoreSynthBlocks)

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/blocks/MoreSynthBlocks.hpp"
#include "engine/dsp/Biquad.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

// A guitar-like tone: a fundamental and strong harmonics, steady
std::vector<std::vector<float>> note(double hz, size_t length, int channels = 1)
{
    std::vector<std::vector<float>> x(channels, std::vector<float>(length));
    for (auto& ch : x)
    {
        for (size_t i = 0; i < length; ++i)
        {
            const double t = static_cast<double>(i) / kRate;
            ch[i] = static_cast<float>(0.2 * std::sin(2 * M_PI * hz * t) + 0.15 * std::sin(2 * M_PI * 2 * hz * t + 1.0)
                                       + 0.1 * std::sin(2 * M_PI * 3 * hz * t + 2.0));
        }
    }
    return x;
}

// Notes one after another, `seconds` each, then silence for `tail` seconds
std::vector<float> melody(const std::vector<double>& hz, double seconds, double tail = 0.0)
{
    std::vector<float> x;
    for (const double f : hz)
    {
        const auto part = note(f, static_cast<size_t>(seconds * kRate))[0];
        x.insert(x.end(), part.begin(), part.end());
    }
    x.resize(x.size() + static_cast<size_t>(tail * kRate), 0.0f);
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

// The strongest frequency between lo and hi (1 Hz steps), in x[from, to)
double peakBetween(const std::vector<float>& x, double lo, double hi, size_t from, size_t to)
{
    double best = lo, bestLevel = -1.0;
    for (double f = lo; f <= hi; f += 1.0)
    {
        const double level = toneAt(x, f, from, to);
        if (level > bestLevel)
        {
            bestLevel = level;
            best = f;
        }
    }
    return best;
}

// Upward zero crossings per second in x[from, to): the pitch, for a sine-like signal
double crossingRate(const std::vector<float>& x, size_t from, size_t to)
{
    int crossings = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            ++crossings;
    return crossings * kRate / static_cast<double>(to - from);
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

const BlockDescriptor* const kNoteSynths[] = {
    &kThereminBlockDescriptor, &kSyncLeadBlockDescriptor, &kPdSynthBlockDescriptor, &kWavetableBlockDescriptor,
    &kSubBassBlockDescriptor,  &kLaserBlockDescriptor,    &kHarmonicPainterBlockDescriptor,
};

int paramNamed(const BlockDescriptor& type, const char* name)
{
    for (int p = 0; p < type.numParams; ++p)
        if (std::string(type.params[p].name) == name)
            return p;
    return -1;
}

} // namespace

TEST_CASE("each new synth plays the note you play, and an octave down when asked")
{
    for (const BlockDescriptor* type : kNoteSynths)
    {
        CAPTURE(std::string(type->typeId));
        const int octave = paramNamed(*type, "Octave");
        REQUIRE(octave >= 0);
        for (const float choice : { 2.0f, 1.0f }) // 0, -1
        {
            CAPTURE(choice);
            auto block = make(*type);
            block->setParam(octave, choice);
            auto x = note(196.0, 48000);
            int allocations = 0;
            testing::runBlock(*block, x, 128, &allocations);
            CHECK(allocations == 0);
            const double hz = choice == 2.0f ? 196.0 : 98.0;
            const double played = toneAt(x[0], hz, 24000, 48000);
            CHECK(played > 0.01);
            CHECK(played > 3.0 * toneAt(x[0], hz * std::exp2(-1.0 / 12.0), 24000, 48000));
            CHECK(played > 3.0 * toneAt(x[0], hz * std::exp2(1.0 / 12.0), 24000, 48000));
        }
    }
}

TEST_CASE("each new synth has a pick filter and Track as its last controls, and a lookahead")
{
    for (const BlockDescriptor* type : { &kThereminBlockDescriptor, &kSyncLeadBlockDescriptor, &kPdSynthBlockDescriptor,
                                         &kWavetableBlockDescriptor, &kSubBassBlockDescriptor, &kLaserBlockDescriptor,
                                         &kDroneBlockDescriptor, &kHarmonicPainterBlockDescriptor })
    {
        CAPTURE(std::string(type->typeId));
        CHECK(type->category == Category::Synth);
        const auto last = static_cast<size_t>(type->numParams - 2);
        CHECK(std::string(type->params[last].name) == "Pick Filter");
        CHECK(type->params[last].def == 0.0f);
        CHECK(std::string(type->params[last + 1].name) == "Track");
        CHECK(type->params[last + 1].def == 1.0f);
        CHECK(type->lookahead);
    }

    // the pick filter holds a new note back, then plays it at the same level
    for (const BlockDescriptor* type : kNoteSynths)
    {
        CAPTURE(std::string(type->typeId));
        const int last = type->numParams - 2;
        const auto run = [&](float pickMs, size_t& start, double& level) {
            auto block = make(*type);
            block->setParam(last, pickMs);
            auto x = note(220.0, 48000);
            testing::runBlock(*block, x);
            float peak = 0.0f;
            for (const float v : x[0])
                peak = std::max(peak, std::fabs(v));
            start = 0;
            while (start < x[0].size() && std::fabs(x[0][start]) < 0.05f * peak)
                ++start;
            level = testing::rms(x[0], ms(400), ms(900));
        };
        size_t startOff = 0, startOn = 0;
        double levelOff = 0.0, levelOn = 0.0;
        run(0.0f, startOff, levelOff);
        run(40.0f, startOn, levelOn);
        CHECK(startOn >= startOff + ms(25));
        CHECK(levelOn > 0.7 * levelOff);
        CHECK(levelOn < 1.4 * levelOff);
    }
}

TEST_CASE("theremin: a pure sine at Timbre 0, gliding between notes")
{
    auto pure = make(kThereminBlockDescriptor);
    pure->setParam(kThereminParamTimbre, 0.0f);
    pure->setParam(kThereminParamVibrato, 0.0f);
    pure->setParam(kThereminParamWobble, 0.0f);
    auto x = note(220.0, 48000);
    testing::runBlock(*pure, x);
    const double fundamental = toneAt(x[0], 220.0, 24000, 48000);
    CHECK(fundamental > 0.05);
    CHECK(toneAt(x[0], 440.0, 24000, 48000) < 0.01 * fundamental);
    CHECK(toneAt(x[0], 660.0, 24000, 48000) < 0.01 * fundamental);

    // Timbre adds harmonics
    auto warm = make(kThereminBlockDescriptor);
    warm->setParam(kThereminParamTimbre, 100.0f);
    auto y = note(220.0, 48000);
    testing::runBlock(*warm, y);
    CHECK(toneAt(y[0], 440.0, 24000, 48000) + toneAt(y[0], 660.0, 24000, 48000)
          > 0.1 * toneAt(y[0], 220.0, 24000, 48000));

    // a long glide passes through the notes between, where a short one jumps
    const auto midGlide = [](float glideMs) {
        auto block = make(kThereminBlockDescriptor);
        block->setParam(kThereminParamGlide, glideMs);
        block->setParam(kThereminParamVibrato, 0.0f);
        block->setParam(kThereminParamWobble, 0.0f);
        block->setParam(kThereminParamTimbre, 0.0f);
        std::vector<std::vector<float>> m { melody({ 220.0, 440.0 }, 0.6) };
        testing::runBlock(*block, m);
        // 100-300 ms after the change (allowing the tracker its few periods)
        return crossingRate(m[0], ms(700), ms(900));
    };
    const double slow = midGlide(600.0), fast = midGlide(2.0);
    CAPTURE(slow);
    CAPTURE(fast);
    CHECK(fast > 420.0);
    CHECK(slow < 400.0);
    CHECK(slow > 230.0);
}

TEST_CASE("theremin: no new attack on a new note, the level carries on")
{
    auto block = make(kThereminBlockDescriptor);
    std::vector<std::vector<float>> m { melody({ 220.0, 330.0, 247.0 }, 0.5) };
    testing::runBlock(*block, m);
    double lowest = 1.0, highest = 0.0;
    for (size_t t = ms(300); t < ms(1450); t += ms(25))
    {
        const double level = testing::rms(m[0], t, t + ms(25));
        lowest = std::min(lowest, level);
        highest = std::max(highest, level);
    }
    CHECK(lowest > 0.6 * highest);
}

TEST_CASE("sync lead: a clean saw at Sync 1, synced harmonics above, no aliasing")
{
    auto plain = make(kSyncLeadBlockDescriptor);
    plain->setParam(kSyncParamSync, 1.0f);
    plain->setParam(kSyncParamEnv, 0.0f);
    plain->setParam(kSyncParamSub, 0.0f);
    plain->setParam(kSyncParamTone, 16000.0f);
    auto x = note(196.0, 48000);
    testing::runBlock(*plain, x);
    const double fundamental = toneAt(x[0], 196.0, 24000, 48000);
    CHECK(toneAt(x[0], 392.0, 24000, 48000) == doctest::Approx(fundamental / 2.0).epsilon(0.15));
    CHECK(toneAt(x[0], 588.0, 24000, 48000) == doctest::Approx(fundamental / 3.0).epsilon(0.15));

    // Sync 3.5: the harmonics around 3.5 times the note are strong, yet it's still the note's pitch
    auto synced = make(kSyncLeadBlockDescriptor);
    synced->setParam(kSyncParamSync, 3.5f);
    synced->setParam(kSyncParamEnv, 0.0f);
    synced->setParam(kSyncParamSub, 0.0f);
    synced->setParam(kSyncParamTone, 16000.0f);
    auto y = note(196.0, 48000);
    testing::runBlock(*synced, y);
    const double h1 = toneAt(y[0], 196.0, 24000, 48000);
    CHECK(toneAt(y[0], 196.0 * 3, 24000, 48000) + toneAt(y[0], 196.0 * 4, 24000, 48000) > 0.6 * h1);
    CHECK(toneAt(y[0], 196.0 * 1.5, 24000, 48000) < 0.05 * h1);

    // a high note synced hard: little between the harmonics (the steps are band-limited)
    auto high = make(kSyncLeadBlockDescriptor);
    high->setParam(kSyncParamSync, 7.3f);
    high->setParam(kSyncParamEnv, 0.0f);
    high->setParam(kSyncParamSub, 0.0f);
    high->setParam(kSyncParamTone, 16000.0f);
    high->setParam(kSyncParamOctave, 3.0f); // +1: 1046 Hz, the synced saw near 7.6 kHz
    auto z = note(523.25, 48000);
    testing::runBlock(*high, z);
    const double top = toneAt(z[0], 1046.5, 24000, 48000);
    double between = 0.0;
    for (int k = 1; k < 8; ++k)
        between = std::max(between, toneAt(z[0], 1046.5 * (k + 0.5), 24000, 48000));
    CHECK(between < 0.03 * top);

    // each pick sweeps the sync up, then it settles
    auto swept = make(kSyncLeadBlockDescriptor);
    swept->setParam(kSyncParamEnv, 100.0f);
    swept->setParam(kSyncParamDecay, 100.0f);
    swept->setParam(kSyncParamTone, 16000.0f);
    swept->setParam(kSyncParamSub, 0.0f);
    auto s = note(196.0, 48000);
    testing::runBlock(*swept, s);
    CHECK(highShare(s[0], 3000.0, ms(30), ms(90)) > 1.5 * highShare(s[0], 3000.0, ms(600), ms(900)));
}

TEST_CASE("phase distortion: a cosine at no distortion, a saw at full, and the DCW falls after the pick")
{
    auto pure = make(kPdSynthBlockDescriptor);
    pure->setParam(kPdParamAmount, 0.0f);
    pure->setParam(kPdParamEnv, 0.0f);
    pure->setParam(kPdParamDetune, 0.0f);
    auto x = note(196.0, 48000);
    testing::runBlock(*pure, x);
    const double fundamental = toneAt(x[0], 196.0, 24000, 48000);
    CHECK(fundamental > 0.05);
    CHECK(toneAt(x[0], 392.0, 24000, 48000) < 0.01 * fundamental);

    for (const float wave : { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f })
    {
        CAPTURE(wave);
        auto bent = make(kPdSynthBlockDescriptor);
        bent->setParam(kPdParamWave, wave);
        bent->setParam(kPdParamAmount, 100.0f);
        bent->setParam(kPdParamEnv, 0.0f);
        bent->setParam(kPdParamDetune, 0.0f);
        auto y = note(196.0, 48000);
        testing::runBlock(*bent, y);
        CHECK(highShare(y[0], 1000.0, 24000, 48000) > 0.15);
    }

    // the square has odd harmonics only
    auto square = make(kPdSynthBlockDescriptor);
    square->setParam(kPdParamWave, 1.0f);
    square->setParam(kPdParamAmount, 90.0f);
    square->setParam(kPdParamEnv, 0.0f);
    square->setParam(kPdParamDetune, 0.0f);
    auto q = note(196.0, 48000);
    testing::runBlock(*square, q);
    CHECK(toneAt(q[0], 588.0, 24000, 48000) > 0.15 * toneAt(q[0], 196.0, 24000, 48000));
    CHECK(toneAt(q[0], 392.0, 24000, 48000) < 0.02 * toneAt(q[0], 196.0, 24000, 48000));

    auto pluck = make(kPdSynthBlockDescriptor);
    pluck->setParam(kPdParamAmount, 0.0f);
    pluck->setParam(kPdParamEnv, 100.0f);
    pluck->setParam(kPdParamSustain, 0.0f);
    pluck->setParam(kPdParamDecay, 100.0f);
    auto p = note(196.0, 48000);
    testing::runBlock(*pluck, p);
    CHECK(highShare(p[0], 1000.0, ms(30), ms(90)) > 3.0 * highShare(p[0], 1000.0, ms(600), ms(900)));

    // Detune: two lines, apart in stereo
    auto wide = make(kPdSynthBlockDescriptor);
    auto w = note(196.0, 48000, 2);
    testing::runBlock(*wide, w);
    double difference = 0.0;
    for (size_t i = 24000; i < 48000; ++i)
        difference = std::max(difference, static_cast<double>(std::fabs(w[0][i] - w[1][i])));
    CHECK(difference > 0.01);
}

TEST_CASE("wavetable: band-limited waves, and the position changes the sound")
{
    // a copy keeps only the harmonics it's for
    for (int table = 0; table < 4; ++table)
    {
        for (const int harmonics : { 4, 32 })
        {
            CAPTURE(table);
            CAPTURE(harmonics);
            int size = 0;
            const float* wave = wavetableWave(table, 3, harmonics, size);
            REQUIRE(size > 0);
            double above = 0.0, total = 0.0;
            for (int h = 1; h <= 2 * harmonics + 4; ++h)
            {
                double re = 0.0, im = 0.0;
                for (int i = 0; i < size; ++i)
                {
                    re += wave[i] * std::cos(2 * M_PI * h * i / size);
                    im += wave[i] * std::sin(2 * M_PI * h * i / size);
                }
                const double power = re * re + im * im;
                total += power;
                if (h > harmonics)
                    above += power;
            }
            CHECK(total > 0.0);
            CHECK(above < 1e-6 * total);
            float peak = 0.0f;
            for (int i = 0; i < size; ++i)
                peak = std::max(peak, std::fabs(wave[i]));
            CHECK(peak < 1.6f);
        }
    }

    // PWM's first wave is a square: no even harmonics
    int size = 0;
    const float* square = wavetableWave(2, 0, 256, size);
    double even = 0.0, odd = 0.0;
    for (int h = 1; h <= 8; ++h)
    {
        double re = 0.0, im = 0.0;
        for (int i = 0; i < size; ++i)
        {
            re += square[i] * std::cos(2 * M_PI * h * i / size);
            im += square[i] * std::sin(2 * M_PI * h * i / size);
        }
        (h % 2 == 0 ? even : odd) += re * re + im * im;
    }
    CHECK(even < 1e-6 * odd);

    // the position moves through the table: different harmonics at each end
    const auto spectrum = [](float position) {
        auto block = make(kWavetableBlockDescriptor);
        block->setParam(kWavetableParamPosition, position);
        block->setParam(kWavetableParamEnv, 0.0f);
        auto x = note(196.0, 48000);
        testing::runBlock(*block, x);
        std::vector<double> h;
        for (int k = 1; k <= 12; ++k)
            h.push_back(toneAt(x[0], 196.0 * k, 24000, 48000));
        return h;
    };
    const auto a = spectrum(0.0f), b = spectrum(100.0f);
    double change = 0.0, size2 = 0.0;
    for (size_t k = 0; k < a.size(); ++k)
    {
        change += std::fabs(a[k] - b[k]);
        size2 += a[k] + b[k];
    }
    CHECK(change > 0.15 * size2);

    // a high note on a bright wave: nothing between its harmonics
    auto high = make(kWavetableBlockDescriptor);
    high->setParam(kWavetableParamTable, 3.0f); // Digital, first wave: a saw
    high->setParam(kWavetableParamPosition, 0.0f);
    high->setParam(kWavetableParamEnv, 0.0f);
    high->setParam(kWavetableParamOctave, 3.0f);
    auto z = note(659.25, 48000);
    testing::runBlock(*high, z);
    const double top = toneAt(z[0], 1318.5, 24000, 48000);
    double between = 0.0;
    for (int k = 1; k < 8; ++k)
        between = std::max(between, toneAt(z[0], 1318.5 * (k + 0.5), 24000, 48000));
    CHECK(between < 0.02 * top);
}

TEST_CASE("sub bass: an octave down by default, darker with the cutoff down")
{
    auto block = make(kSubBassBlockDescriptor);
    auto x = note(196.0, 48000);
    testing::runBlock(*block, x);
    CHECK(toneAt(x[0], 98.0, 24000, 48000) > 0.02);
    CHECK(toneAt(x[0], 98.0, 24000, 48000) > 3.0 * toneAt(x[0], 196.0 * std::exp2(1.0 / 12.0), 24000, 48000));

    const auto brightness = [](float cutoff) {
        auto b = make(kSubBassBlockDescriptor);
        b->setParam(kSubBassParamCutoff, cutoff);
        b->setParam(kSubBassParamEnv, 0.0f);
        auto y = note(196.0, 48000);
        testing::runBlock(*b, y);
        return highShare(y[0], 600.0, 24000, 48000);
    };
    CHECK(brightness(3000.0f) > 4.0 * brightness(150.0f));

    // resonance rings at the cutoff
    const auto atCutoff = [](float resonance) {
        auto b = make(kSubBassBlockDescriptor);
        b->setParam(kSubBassParamCutoff, 800.0f);
        b->setParam(kSubBassParamEnv, 0.0f);
        b->setParam(kSubBassParamResonance, resonance);
        auto y = note(196.0, 48000);
        testing::runBlock(*b, y);
        // the nearest harmonic of 98 Hz to 800 Hz: 784 Hz
        return toneAt(y[0], 784.0, 24000, 48000) / toneAt(y[0], 98.0, 24000, 48000);
    };
    CHECK(atCutoff(95.0f) > 2.0 * atCutoff(0.0f));
}

TEST_CASE("laser: each pick dives from high down to the note, Repeat fires again, the echo trails")
{
    auto block = make(kLaserBlockDescriptor);
    block->setParam(kLaserParamEcho, 0.0f);
    block->setParam(kLaserParamSpeed, 300.0f);
    auto x = note(220.0, 48000);
    testing::runBlock(*block, x);
    // find where it starts
    size_t start = 0;
    while (start < x[0].size() && std::fabs(x[0][start]) < 0.01f)
        ++start;
    REQUIRE(start < ms(100));
    const double early = crossingRate(x[0], start, start + ms(20));
    const double late = crossingRate(x[0], ms(700), ms(900));
    CAPTURE(early);
    CAPTURE(late);
    CHECK(late == doctest::Approx(220.0).epsilon(0.03));
    CHECK(early > 2.5 * late); // 24 semitones up at the start: four times

    // Repeat 4 Hz: high again 250 ms later
    auto repeat = make(kLaserBlockDescriptor);
    repeat->setParam(kLaserParamEcho, 0.0f);
    repeat->setParam(kLaserParamSpeed, 300.0f);
    repeat->setParam(kLaserParamRepeat, 4.0f);
    auto y = note(220.0, 48000);
    testing::runBlock(*repeat, y);
    double highest = 0.0;
    for (size_t t = ms(600); t < ms(900); t += ms(10))
        highest = std::max(highest, crossingRate(y[0], t, t + ms(20)));
    CHECK(highest > 2.0 * 220.0);

    // the echo rings on after the note
    const auto tail = [](float echo) {
        auto b = make(kLaserBlockDescriptor);
        b->setParam(kLaserParamEcho, echo);
        b->setParam(kLaserParamRelease, 10.0f);
        std::vector<std::vector<float>> m { melody({ 220.0 }, 0.5, 1.0) };
        testing::runBlock(*b, m);
        return testing::rms(m[0], ms(700), ms(1000));
    };
    CHECK(tail(70.0f) > 0.005);
    CHECK(tail(0.0f) < 0.1 * tail(70.0f));
}

TEST_CASE("drone keeper finds the root from what you play")
{
    float weights[12] = {};
    CHECK(droneRoot(weights) == -1);
    weights[9] = 1.0f; // A
    CHECK(droneRoot(weights) == 9);
    // A minor pentatonic, E played as much as A: still A (E's fifth, B, is missing; A's fifth is E)
    weights[0] = 0.5f;
    weights[2] = 0.5f;
    weights[4] = 1.0f;
    weights[7] = 0.5f;
    CHECK(droneRoot(weights) == 9);
    // G major's notes, G heard most: G
    float major[12] = {};
    for (const int pc : { 7, 9, 11, 0, 2, 4, 6 })
        major[pc] = 0.5f;
    major[7] = 1.0f;
    CHECK(droneRoot(major) == 7);
}

TEST_CASE("drone keeper holds a drone on the root under your guitar, and moves when the key does")
{
    // the drone alone: the output minus the guitar, which passes
    const auto droneOf = [](Block& block, std::vector<float> input) {
        std::vector<std::vector<float>> x { input };
        int allocations = 0;
        testing::runBlock(block, x, 128, &allocations);
        CHECK(allocations == 0);
        for (size_t i = 0; i < input.size(); ++i)
            x[0][i] -= input[i];
        return x[0];
    };

    // Level 0: the guitar as it was
    {
        auto silent = make(kDroneBlockDescriptor);
        silent->setParam(kDroneParamLevel, 0.0f);
        const auto input = melody({ 220.0 }, 1.0);
        const auto d = droneOf(*silent, input);
        CHECK(testing::rms(d, 0, d.size()) < 1e-6);
    }

    auto block = make(kDroneBlockDescriptor);
    block->setParam(kDroneParamMemory, 2.0f);
    block->setParam(kDroneParamFade, 0.5f);
    // A for 3 s, then D for 6 s, then silence
    const auto input = melody({ 220.0, 220.0, 220.0, 146.83, 146.83, 146.83, 146.83, 146.83, 146.83 }, 1.0, 3.0);
    const auto d = droneOf(*block, input);
    const auto second = [](double s) { return static_cast<size_t>(s * kRate); };
    // Mid register: A2 (110 Hz) and its fifth E3
    const double onA = toneAt(d, 110.0, second(2.0), second(3.0));
    CHECK(onA > 0.005);
    CHECK(toneAt(d, 164.81, second(2.0), second(3.0)) > 0.2 * onA);
    CHECK(onA > 4.0 * toneAt(d, 73.42, second(2.0), second(3.0)));
    // by the end of the D: D2 (73.4 Hz)
    const double onD = toneAt(d, 73.42, second(8.0), second(9.0));
    CHECK(onD > 0.005);
    CHECK(onD > 4.0 * toneAt(d, 110.0, second(8.0), second(9.0)));
    // faded out after you stop (Fade 0.5 s)
    CHECK(testing::rms(d, second(11.0), second(12.0)) < 0.02 * testing::rms(d, second(8.0), second(9.0)));

    // Hold keeps it going
    auto held = make(kDroneBlockDescriptor);
    held->setParam(kDroneParamMemory, 2.0f);
    held->setParam(kDroneParamFade, 0.5f);
    held->setParam(kDroneParamHold, 1.0f);
    const auto h = droneOf(*held, melody({ 220.0, 220.0 }, 1.0, 3.0));
    CHECK(testing::rms(h, second(4.0), second(5.0)) > 0.5 * testing::rms(h, second(1.5), second(2.0)));
}

TEST_CASE("harmonic painter: odd/even, tilt, stretch and spread")
{
    const auto harmonics = [](float tilt, float oddEven, float stretch) {
        auto block = make(kHarmonicPainterBlockDescriptor);
        block->setParam(kHarmonicParamTilt, tilt);
        block->setParam(kHarmonicParamOddEven, oddEven);
        block->setParam(kHarmonicParamStretch, stretch);
        block->setParam(kHarmonicParamShimmer, 0.0f);
        block->setParam(kHarmonicParamSpread, 0.0f);
        auto x = note(196.0, 48000);
        testing::runBlock(*block, x);
        return x[0];
    };

    const auto odd = harmonics(-6.0f, -100.0f, 0.0f);
    CHECK(toneAt(odd, 588.0, 24000, 48000) > 0.2 * toneAt(odd, 196.0, 24000, 48000));
    CHECK(toneAt(odd, 392.0, 24000, 48000) < 0.01 * toneAt(odd, 196.0, 24000, 48000));
    const auto even = harmonics(-6.0f, 100.0f, 0.0f);
    CHECK(toneAt(even, 588.0, 24000, 48000) < 0.01 * toneAt(even, 196.0, 24000, 48000));
    CHECK(toneAt(even, 392.0, 24000, 48000) > 0.2 * toneAt(even, 196.0, 24000, 48000));

    // -6 dB per octave: the 4th harmonic at a quarter; -18: far less
    const auto saw = harmonics(-6.0f, 0.0f, 0.0f);
    CHECK(toneAt(saw, 784.0, 24000, 48000) == doctest::Approx(toneAt(saw, 196.0, 24000, 48000) / 4.0).epsilon(0.1));
    const auto dark = harmonics(-18.0f, 0.0f, 0.0f);
    CHECK(toneAt(dark, 784.0, 24000, 48000) < 0.03 * toneAt(dark, 196.0, 24000, 48000));

    // Stretch: the 8th harmonic sharp of eight times the note
    const auto stretched = harmonics(0.0f, 0.0f, 100.0f);
    const double eighth = peakBetween(stretched, 1500.0, 2000.0, 24000, 48000);
    CHECK(eighth > 1568.0 + 30.0);

    // Spread: odd and even harmonics apart in stereo
    auto wide = make(kHarmonicPainterBlockDescriptor);
    wide->setParam(kHarmonicParamSpread, 100.0f);
    auto w = note(196.0, 48000, 2);
    testing::runBlock(*wide, w);
    CHECK(toneAt(w[0], 588.0, 24000, 48000) > 4.0 * toneAt(w[1], 588.0, 24000, 48000));
    CHECK(toneAt(w[1], 392.0, 24000, 48000) > 4.0 * toneAt(w[0], 392.0, 24000, 48000));
}
