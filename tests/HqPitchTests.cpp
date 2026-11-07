// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The pitch blocks' HQ mode (spectral shifting, dsp/SpectralShifter.hpp; the "hq" block config)

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/BlockRegistry.hpp"
#include "engine/blocks/AmbientBlocks.hpp"
#include "engine/blocks/DelayFxBlocks.hpp"
#include "engine/blocks/PitchBlocks.hpp"
#include "engine/dsp/SpectralShifter.hpp"

#include <cmath>
#include <random>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

std::unique_ptr<Block> makeHq(const BlockDescriptor& type, bool hq = true, double rate = kRate)
{
    std::unique_ptr<Block> block = type.create();
    block->prepare(rate, 256);
    block->configure(nlohmann::json { { "hq", hq } });
    for (int p = 0; p < type.numParams; ++p)
        block->setParam(p, type.params[p].def);
    return block;
}

// The amplitude of a sine at `hz` in x[from, to) (Hann-windowed DFT at one frequency)
double toneAt(const std::vector<float>& x, double hz, size_t from, size_t to)
{
    double re = 0.0, im = 0.0, weights = 0.0;
    const double n = static_cast<double>(to - from);
    for (size_t i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i - from) / n);
        const double phase = 2.0 * M_PI * hz * static_cast<double>(i) / kRate;
        re += x[i] * w * std::cos(phase);
        im += x[i] * w * std::sin(phase);
        weights += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / weights;
}

// The energy of x[48000, 96000) within 1.5% of the first `harmonics` harmonics of `hz` (1 Hz steps over a
// Hann-windowed DFT overcount a tone by 1.5)
double energyNear(const std::vector<float>& x, double hz, int harmonics)
{
    double near = 0.0;
    int last = -1;
    for (int h = 1; h <= harmonics; ++h)
    {
        const double g = hz * h;
        for (int f = std::max(last + 1, static_cast<int>(g * 0.985)); f <= static_cast<int>(g * 1.015) + 1; ++f)
        {
            const double a = toneAt(x, f, 48000, 96000);
            near += a * a / 2.0 / 1.5;
            last = f;
        }
    }
    return near;
}

std::vector<std::vector<float>> harmonicNote(double hz, size_t length)
{
    std::vector<std::vector<float>> x(1, std::vector<float>(length));
    for (size_t i = 0; i < length; ++i)
    {
        double v = 0.0;
        for (int h = 1; h <= 6; ++h)
            v += 0.15 / h * std::sin(2.0 * M_PI * hz * h * static_cast<double>(i) / kRate + h);
        x[0][i] = static_cast<float>(v);
    }
    return x;
}

const BlockDescriptor* const kHqBlocks[] = { &kPitchShifterBlockDescriptor, &kWhammyBlockDescriptor,
                                             &kPolyOctaveBlockDescriptor,   &kHarmonizerBlockDescriptor,
                                             &kDetuneBlockDescriptor,       &kCapoBlockDescriptor,
                                             &kShimmerBlockDescriptor,      &kPitchDelayBlockDescriptor };

} // namespace

TEST_CASE("the hq config")
{
    CHECK_FALSE(hqFromConfig(nlohmann::json::object()));
    CHECK_FALSE(hqFromConfig(nlohmann::json()));
    CHECK_FALSE(hqFromConfig(nlohmann::json { { "hq", false } }));
    CHECK(hqFromConfig(nlohmann::json { { "hq", true } }));
    CHECK(hqFromConfig(nlohmann::json { { "hq", 1.0 } }));
    CHECK_FALSE(hqFromConfig(nlohmann::json { { "hq", "yes" } }));

    // exactly these blocks offer it (the UI shows their Quality button)
    int count = 0;
    for (const BlockDescriptor* type : BlockRegistry::builtin().types())
        if (type->hq)
        {
            ++count;
            CHECK(std::find(std::begin(kHqBlocks), std::end(kHqBlocks), type) != std::end(kHqBlocks));
        }
    CHECK(count == static_cast<int>(std::size(kHqBlocks)));
}

TEST_CASE("HQ latency: the shifting blocks report it, the tail blocks hide it")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        SpectralShifter shifter;
        shifter.prepare(rate);
        const uint32_t expected = shifter.latency();
        CHECK(expected > 0.12 * rate); // at least the analysis window
        CHECK(expected < 0.16 * rate);
        for (const BlockDescriptor* type : kHqBlocks)
        {
            CAPTURE(type->typeId);
            CHECK(makeHq(*type, false, rate)->latency() == 0);
            const bool tail = type == &kShimmerBlockDescriptor || type == &kPitchDelayBlockDescriptor;
            CHECK(makeHq(*type, true, rate)->latency() == (tail ? 0u : expected));
        }
    }
    // with the harmonizer's lookahead, both add up
    auto harm = kHarmonizerBlockDescriptor.create();
    harm->prepare(kRate, 256);
    harm->configure(nlohmann::json { { "hq", true }, { "lookahead", 10.0 } });
    SpectralShifter shifter;
    shifter.prepare(kRate);
    CHECK(harm->latency() == shifter.latency() + 480);
}

TEST_CASE("HQ: the dry signal is delayed by exactly the reported latency")
{
    auto block = makeHq(kPitchShifterBlockDescriptor);
    block->setParam(kPitchParamLevel1, 0.0f);
    block->setParam(kPitchParamDry, 100.0f);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<std::vector<float>> x(2, std::vector<float>(24000));
    for (auto& ch : x)
        for (float& v : ch)
            v = dist(rng);
    const auto input = x;
    testing::runBlock(*block, x, 100); // a buffer size that doesn't divide the chunk
    const size_t latency = block->latency();
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t i = latency; i < x[0].size(); i += 97)
            REQUIRE(x[ch][i] == input[ch][i - latency]);
}

TEST_CASE("HQ pitch shifting a single note is clean, at every pitch")
{
    for (const double hz : { 82.4, 196.0, 329.6 })
    {
        for (const double shift : { 7.0, 12.0, -12.0 })
        {
            CAPTURE(hz);
            CAPTURE(shift);
            auto block = makeHq(kPitchShifterBlockDescriptor);
            block->setParam(kPitchParamShift1, static_cast<float>(shift));
            block->setParam(kPitchParamDry, 0.0f);
            auto x = harmonicNote(hz, 96000);
            testing::runBlock(*block, x);
            const double total = testing::rms(x[0], 48000, 96000);
            const double ratio = std::exp2(shift / 12.0);
            double exact = 0.0;
            for (int h = 1; h <= 6; ++h)
            {
                const double a = toneAt(x[0], hz * ratio * h, 48000, 96000);
                exact += a * a / 2.0;
            }
            const double near = energyNear(x[0], hz * ratio, 6);
            CHECK(exact / (total * total) > 0.8); // and mostly right on pitch
            CHECK(near / (total * total) > 0.95);
            // and at the input's level
            CHECK(total == doctest::Approx(testing::rms(harmonicNote(hz, 96000)[0], 48000, 96000)).epsilon(0.15));
        }
    }
}

TEST_CASE("HQ poly octave shifts chords cleanly")
{
    const double notes[] = { 82.41, 123.47, 164.81, 207.65, 246.94, 329.63 };
    auto block = makeHq(kPolyOctaveBlockDescriptor);
    block->setParam(kPolyOctParamDry, 0.0f);
    block->setParam(kPolyOctParamSub1, 100.0f);
    std::vector<std::vector<float>> x(1, std::vector<float>(96000));
    for (size_t i = 0; i < x[0].size(); ++i)
    {
        double v = 0.0;
        for (const double hz : notes)
            for (int h = 1; h <= 4; ++h)
                v += 0.1 / h * std::sin(2.0 * M_PI * hz * h * static_cast<double>(i) / kRate + h);
        x[0][i] = static_cast<float>(v);
    }
    const double inputRms = testing::rms(x[0], 48000, 96000);
    testing::runBlock(*block, x);
    const double total = testing::rms(x[0], 48000, 96000);
    CHECK(total == doctest::Approx(inputRms).epsilon(0.15));
    // energy within 1.5% of the shifted partials (1 Hz steps over a Hann-windowed DFT overcount a tone by 1.5)
    std::vector<bool> taken(2000, false);
    double near = 0.0;
    for (const double hz : notes)
        for (int h = 1; h <= 4; ++h)
        {
            const double g = hz * h * 0.5;
            for (int f = static_cast<int>(g * 0.985); f <= static_cast<int>(g * 1.015) + 1; ++f)
            {
                if (taken[static_cast<size_t>(f)])
                    continue;
                taken[static_cast<size_t>(f)] = true;
                const double a = toneAt(x[0], f, 48000, 96000);
                near += a * a / 2.0 / 1.5;
            }
        }
    CHECK(near / (total * total) > 0.9);
}

TEST_CASE("HQ pitch delay: the repeats still land on time")
{
    auto block = makeHq(kPitchDelayBlockDescriptor);
    block->setParam(kPitchDelayParamTime, 400.0f);
    block->setParam(kPitchDelayParamShift, 12.0f);
    block->setParam(kPitchDelayParamFeedback, 0.0f);
    block->setParam(kPitchDelayParamMix, 100.0f);
    block->setParam(kPitchDelayParamTone, 20000.0f);
    // a 60 ms note at the start; the repeat should start 400 ms later
    auto x = harmonicNote(220.0, 48000);
    for (size_t i = 2880; i < x[0].size(); ++i)
        x[0][i] = 0.0f;
    testing::runBlock(*block, x);
    const size_t at = 19200;
    CHECK(testing::rms(x[0], at - 2400, at - 480) < 0.02 * testing::rms(x[0], at + 480, at + 2400));
    CHECK(toneAt(x[0], 440.0, at + 480, at + 2400) > 0.05);
}

TEST_CASE("HQ blocks: finite, quiet for silence, no allocation, mono and stereo")
{
    for (const BlockDescriptor* type : kHqBlocks)
    {
        for (const int channels : { 1, 2 })
        {
            CAPTURE(type->typeId);
            CAPTURE(channels);
            auto block = makeHq(*type);
            // every voice up, so every shifter runs
            for (int p = 0; p < type->numParams; ++p)
                if (std::string(type->params[p].unit) == "%")
                    block->setParam(p, 100.0f);
            if (type == &kWhammyBlockDescriptor)
                block->setParam(kWhammyParamPosition, 50.0f);
            std::mt19937 rng(7);
            std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
            std::vector<std::vector<float>> x(channels, std::vector<float>(48000, 0.0f));
            for (auto& ch : x)
                for (size_t i = 0; i < 24000; ++i)
                    ch[i] = (i / 4000) % 2 ? 0.0f : dist(rng);
            std::vector<float> rig = x[0];
            const float* rigPtr[2] = { rig.data(), rig.data() };
            int allocations = 0;
            float peak = 0.0f;
            bool finite = true;
            for (size_t t = 0; t < x[0].size(); t += 128)
            {
                float* io[2] = { x[0].data() + t, x[channels - 1].data() + t };
                const float* key[2] = { rigPtr[0] + t, rigPtr[1] + t };
                testing::AllocationGuard guard;
                block->setRigInput(key, static_cast<uint32_t>(channels));
                block->process(io, static_cast<uint32_t>(channels), 128);
                allocations += guard.count();
            }
            for (const auto& ch : x)
                for (const float v : ch)
                {
                    finite = finite && std::isfinite(v);
                    peak = std::max(peak, std::fabs(v));
                }
            CHECK(finite);
            CHECK(peak < 8.0f);
            CHECK(allocations == 0);
            // the reverb and delay ring on; the others fall silent once the input has passed through
            if (type != &kShimmerBlockDescriptor && type != &kPitchDelayBlockDescriptor)
                CHECK(testing::rms(x[0], 40000, 48000) < 1e-3);
        }
    }
}

TEST_CASE("HQ voices turned down cost nothing and come back cleanly")
{
    // Poly Octave with only Up 1: the other three shifters are skipped; turning Sub 1 up later brings it in
    auto block = makeHq(kPolyOctaveBlockDescriptor);
    block->setParam(kPolyOctParamSub1, 0.0f);
    block->setParam(kPolyOctParamUp1, 100.0f);
    block->setParam(kPolyOctParamDry, 0.0f);
    auto x = harmonicNote(196.0, 96000);
    std::vector<std::vector<float>> first(1, std::vector<float>(x[0].begin(), x[0].begin() + 48000));
    std::vector<std::vector<float>> second(1, std::vector<float>(x[0].begin() + 48000, x[0].end()));
    testing::runBlock(*block, first);
    CHECK(toneAt(first[0], 392.0, 24000, 48000) > 0.1);
    CHECK(toneAt(first[0], 98.0, 24000, 48000) < 0.003);
    block->setParam(kPolyOctParamSub1, 100.0f);
    testing::runBlock(*block, second);
    CHECK(toneAt(second[0], 98.0, 24000, 48000) > 0.1);
    for (const float v : second[0])
        REQUIRE(std::isfinite(v));
}

TEST_CASE("HQ harmonizer: a third above in the key")
{
    // A3 in C major: the third above is C4 (3 semitones), not C#4
    auto block = makeHq(kHarmonizerBlockDescriptor);
    block->setParam(kHarmParamDry, 0.0f);
    auto x = harmonicNote(220.0, 96000);
    testing::runBlock(*block, x);
    const double c4 = toneAt(x[0], 220.0 * std::exp2(3.0 / 12.0), 48000, 96000);
    const double cSharp4 = toneAt(x[0], 220.0 * std::exp2(4.0 / 12.0), 48000, 96000);
    CHECK(c4 > 0.08);
    CHECK(cSharp4 < 0.1 * c4);
}
