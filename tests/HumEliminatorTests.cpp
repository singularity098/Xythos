// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Behaviour tests for the Hum Eliminator (blocks/HumEliminatorBlock.*, dsp/HumCanceller.hpp)

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/blocks/HumEliminatorBlock.hpp"
#include "engine/dsp/HumCanceller.hpp"

#include <cmath>
#include <random>
#include <vector>

using namespace xythos;
using namespace ::testing;

namespace {

// A single coil's buzz like the owner's (2026-11-14 recordings): harmonics of the mains up to ~12 kHz, the odd ones
// stronger, most of the energy between 2 and 10 kHz. The mains wanders by up to `drift` Hz over a few seconds, the
// level by 1 dB. About -72 dBFS RMS.
std::vector<float> buzz(double rate, double mains, size_t length, double drift = 0.01, unsigned seed = 1)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> phaseDist(0.0, 2.0 * M_PI);
    const int harmonics = static_cast<int>(std::min(12000.0, 0.45 * rate) / mains);
    std::vector<double> amp(harmonics + 1), phase(harmonics + 1);
    for (int h = 1; h <= harmonics; ++h)
    {
        const double f = h * mains;
        const double band = f < 2000.0 ? 0.3 : (f < 10000.0 ? 1.0 : 0.4);
        amp[h] = 2.2e-5 * band * (h % 2 == 1 ? 1.0 : 0.25);
        phase[h] = phaseDist(rng);
    }
    std::vector<float> y(length);
    double cycles = 0.0;
    for (size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        cycles += (mains + drift * std::sin(2.0 * M_PI * t / 6.0 + 1.0)) / rate;
        const double gain = std::pow(10.0, 0.05 * std::sin(2.0 * M_PI * t / 5.0) / 1.0);
        double s = 0.0;
        for (int h = 1; h <= harmonics; ++h)
            s += amp[h] * std::sin(2.0 * M_PI * h * cycles + phase[h]);
        y[i] = static_cast<float>(gain * s);
    }
    return y;
}

// Plucked notes: harmonics falling off with their number, dying away, muted at the end. {start s, length s, Hz}
struct Note {
    double start, length, hz;
};

std::vector<float> guitar(double rate, size_t length, std::initializer_list<Note> notes)
{
    std::vector<float> y(length, 0.0f);
    for (const Note& note : notes)
    {
        const auto from = static_cast<size_t>(note.start * rate);
        const auto to = std::min(length, static_cast<size_t>((note.start + note.length) * rate));
        for (size_t i = from; i < to; ++i)
        {
            const double t = static_cast<double>(i - from) / rate;
            const double left = static_cast<double>(to - i) / rate;
            const double env = 0.3 * std::exp(-t / 1.5) * std::min(1.0, left / 0.01) * std::min(1.0, t / 0.002);
            double s = 0.0;
            for (int k = 1; k <= 24 && k * note.hz < 0.45 * rate; ++k)
                s += std::sin(2.0 * M_PI * k * note.hz * t) / k * std::exp(-t * k * 0.4);
            y[i] += static_cast<float>(env * s);
        }
    }
    return y;
}

std::vector<float> noise(size_t length, double rms, unsigned seed = 2)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> dist(0.0f, static_cast<float>(rms));
    std::vector<float> y(length);
    for (float& v : y)
        v = dist(rng);
    return y;
}

std::vector<float> sum(std::initializer_list<const std::vector<float>*> parts)
{
    std::vector<float> y((*parts.begin())->size(), 0.0f);
    for (const std::vector<float>* p : parts)
        for (size_t i = 0; i < y.size(); ++i)
            y[i] += (*p)[i];
    return y;
}

double rmsOf(const std::vector<float>& a, const std::vector<float>& b, double rate, double from, double to)
{
    const auto i0 = static_cast<size_t>(from * rate), i1 = std::min(a.size(), static_cast<size_t>(to * rate));
    double s = 0.0;
    for (size_t i = i0; i < i1; ++i)
    {
        const double d = static_cast<double>(a[i]) - (b.empty() ? 0.0 : b[i]);
        s += d * d;
    }
    return std::sqrt(s / static_cast<double>(i1 - i0));
}

double db(double ratio) { return 20.0 * std::log10(ratio); }

// Runs the block at its defaults (Mains as given) over `in`, mono
std::vector<float> eliminate(const std::vector<float>& in, double rate, float mains = 0.0f, int* allocations = nullptr)
{
    auto block = createWithDefaults(kHumEliminatorBlockDescriptor, rate);
    block->setParam(kHumParamMains, mains);
    std::vector<std::vector<float>> io { in };
    runBlock(*block, io, 128, allocations);
    return io[0];
}

} // namespace

TEST_CASE("hum eliminator: takes a single coil's buzz away while nothing plays")
{
    for (const double rate : { 44100.0, 48000.0, 96000.0 })
    {
        CAPTURE(rate);
        const size_t length = static_cast<size_t>(6.0 * rate);
        const std::vector<float> hum = buzz(rate, 60.0, length);
        const std::vector<float> hiss = noise(length, 3e-5); // -90 dB
        const std::vector<float> clean = hiss;
        const std::vector<float> in = sum({ &hum, &hiss });
        int allocations = 0;
        const std::vector<float> out = eliminate(in, rate, 0.0f, &allocations);
        CHECK(allocations == 0);
        const double before = rmsOf(hum, {}, rate, 2.0, 6.0);
        const double after = rmsOf(out, clean, rate, 2.0, 6.0);
        // learnt within a second
        CHECK(db(rmsOf(out, clean, rate, 1.0, 2.0) / rmsOf(hum, {}, rate, 1.0, 2.0)) < -12.0);
        CHECK(db(after / before) < -20.0);
    }
}

TEST_CASE("hum eliminator: 50 Hz mains, and the mains anywhere within a tenth of a hertz")
{
    constexpr double rate = 48000.0;
    const size_t length = static_cast<size_t>(6.0 * rate);
    for (const auto& [mains, choice] : { std::pair { 50.0, 1.0f }, std::pair { 60.0, 0.0f } })
        for (const double offset : { -0.08, 0.05 })
        {
            CAPTURE(mains);
            CAPTURE(offset);
            const std::vector<float> hum = buzz(rate, mains + offset, length, 0.005);
            const std::vector<float> hiss = noise(length, 3e-5);
            const std::vector<float> in = sum({ &hum, &hiss });
            const std::vector<float> out = eliminate(in, rate, choice);
            CHECK(db(rmsOf(out, hiss, rate, 3.0, 6.0) / rmsOf(hum, {}, rate, 3.0, 6.0)) < -15.0);
        }
}

TEST_CASE("hum eliminator: set to the wrong mains, it doesn't add hum")
{
    constexpr double rate = 48000.0;
    const size_t length = static_cast<size_t>(6.0 * rate);
    const std::vector<float> hum = buzz(rate, 50.0, length);
    const std::vector<float> hiss = noise(length, 3e-5);
    const std::vector<float> in = sum({ &hum, &hiss });
    const std::vector<float> out = eliminate(in, rate, 0.0f); // 60 Hz
    CHECK(db(rmsOf(out, hiss, rate, 1.0, 6.0) / rmsOf(hum, {}, rate, 1.0, 6.0)) < 1.0);
}

TEST_CASE("hum eliminator: leaves the guitar as it was")
{
    constexpr double rate = 48000.0;
    const size_t length = static_cast<size_t>(10.0 * rate);
    // B2 and E4 repeat at nearly the mains' period (123.5 Hz ~ 2 x 60, 329.6 Hz ~ 5.5 x 60), the A an octave
    // multiple of 55; a chord, then single notes with gaps
    const std::vector<float> notes = guitar(rate, length,
                                            { { 1.5, 2.5, 123.47 }, { 1.5, 2.5, 329.63 }, { 1.5, 2.5, 196.0 },
                                              { 4.2, 0.6, 110.0 }, { 5.0, 1.2, 82.41 }, { 6.5, 3.0, 246.94 } });
    const std::vector<float> hiss = noise(length, 3e-5);

    SUBCASE("with no hum, nothing changes")
    {
        const std::vector<float> in = sum({ &notes, &hiss });
        const std::vector<float> out = eliminate(in, rate);
        CHECK(db(rmsOf(out, in, rate, 0.0, 10.0) / rmsOf(in, {}, rate, 0.0, 10.0)) < -60.0);
    }

    SUBCASE("with hum, only the hum goes")
    {
        // the mains drifting up to 0.004 Hz/s, twice what the owner's recordings show
        const std::vector<float> hum = buzz(rate, 60.0, length, 0.004);
        const std::vector<float> in = sum({ &notes, &hum, &hiss });
        const std::vector<float> clean = sum({ &notes, &hiss });
        const std::vector<float> out = eliminate(in, rate);
        // what came out differs from the guitar by no more than the hum did, all through (under notes, where the
        // mains drifts out of step with what was learnt, within 2 dB, until the subtraction has faded out)
        for (double t = 0.5; t + 0.5 <= 10.0; t += 0.5)
        {
            CAPTURE(t);
            CHECK(rmsOf(out, clean, rate, t, t + 0.5) < 1.26 * rmsOf(hum, {}, rate, t, t + 0.5));
        }
        // and much less in the gaps and once the notes have gone
        CHECK(db(rmsOf(out, clean, rate, 0.8, 1.5) / rmsOf(hum, {}, rate, 0.8, 1.5)) < -12.0);
        CHECK(db(rmsOf(out, clean, rate, 4.0, 4.2) / rmsOf(hum, {}, rate, 4.0, 4.2)) < -6.0);
    }
}

TEST_CASE("hum eliminator: after a long note, the hum is found again at once")
{
    constexpr double rate = 48000.0;
    const size_t length = static_cast<size_t>(10.0 * rate);
    const std::vector<float> notes = guitar(rate, length, { { 2.0, 4.0, 146.83 } });
    const std::vector<float> hum = buzz(rate, 60.0, length, 0.012);
    const std::vector<float> hiss = noise(length, 3e-5);
    const std::vector<float> in = sum({ &notes, &hum, &hiss });
    const std::vector<float> clean = sum({ &notes, &hiss });
    const std::vector<float> out = eliminate(in, rate);
    // the mains has moved during the note, by many cycles of the high harmonics
    CHECK(db(rmsOf(out, clean, rate, 6.1, 6.6) / rmsOf(hum, {}, rate, 6.1, 6.6)) < -10.0);
    CHECK(db(rmsOf(out, clean, rate, 6.6, 8.0) / rmsOf(hum, {}, rate, 6.6, 8.0)) < -15.0);
}

TEST_CASE("hum eliminator: Amount scales what's taken away, and a stereo signal is handled per channel")
{
    constexpr double rate = 48000.0;
    const size_t length = static_cast<size_t>(4.0 * rate);
    const std::vector<float> hum = buzz(rate, 60.0, length);
    const std::vector<float> other = buzz(rate, 60.0, length, 0.01, 9);
    auto block = createWithDefaults(kHumEliminatorBlockDescriptor, rate);
    block->setParam(kHumParamAmount, 50.0f);
    std::vector<std::vector<float>> io { hum, other };
    runBlock(*block, io, 64);
    // half of it left on both sides
    CHECK(rmsOf(io[0], {}, rate, 3.0, 4.0) / rmsOf(hum, {}, rate, 3.0, 4.0) == doctest::Approx(0.5).epsilon(0.08));
    CHECK(rmsOf(io[1], {}, rate, 3.0, 4.0) / rmsOf(other, {}, rate, 3.0, 4.0) == doctest::Approx(0.5).epsilon(0.08));
}

TEST_CASE("hum eliminator: starts from silence, and finds the phase again after being bypassed a while")
{
    constexpr double rate = 48000.0;
    const size_t length = static_cast<size_t>(16.0 * rate);
    std::vector<float> hum = buzz(rate, 60.0, length, 0.01);
    for (size_t i = 0; i < static_cast<size_t>(2.0 * rate); ++i)
        hum[i] = 0.0f; // digital silence, then the guitar's volume comes up
    HumCanceller canceller;
    canceller.prepare(rate);
    std::vector<float> out(length, 0.0f);
    for (size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / rate;
        if (t >= 10.0 && t < 12.0)
            continue; // bypassed: not processed
        out[i] = canceller.process(hum[i]);
    }
    CHECK(db(rmsOf(out, {}, rate, 8.0, 10.0) / rmsOf(hum, {}, rate, 8.0, 10.0)) < -20.0);
    CHECK(db(rmsOf(out, {}, rate, 12.5, 16.0) / rmsOf(hum, {}, rate, 12.5, 16.0)) < -15.0);
}
