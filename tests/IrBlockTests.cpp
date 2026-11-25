// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/blocks/IrBlock.hpp"
#include "engine/dsp/Biquad.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <fstream>
#include <string>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBuffer = 128;

// Writes a 32-bit float WAV; channels are given as separate vectors of equal length
std::string writeWav(const std::string& name, double rate, const std::vector<std::vector<float>>& channels)
{
    const std::string path = (std::filesystem::temp_directory_path() / ("xythos-test-" + name)).string();
    std::ofstream out(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const uint16_t numChannels = static_cast<uint16_t>(channels.size());
    const uint32_t frames = static_cast<uint32_t>(channels[0].size());
    const uint32_t dataSize = frames * numChannels * 4;
    out.write("RIFF", 4);
    u32(36 + dataSize);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3); // IEEE float
    u16(numChannels);
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate) * numChannels * 4);
    u16(numChannels * 4);
    u16(32);
    out.write("data", 4);
    u32(dataSize);
    for (uint32_t i = 0; i < frames; ++i)
        for (const std::vector<float>& ch : channels)
            out.write(reinterpret_cast<const char*>(&ch[i]), 4);
    return path;
}

std::vector<float> impulseAt(size_t length, size_t at, float value = 1.0f)
{
    std::vector<float> x(length, 0.0f);
    x[at] = value;
    return x;
}

std::unique_ptr<Block> loadIr(const std::string& path)
{
    std::unique_ptr<Block> block = kIrBlockDescriptor.create();
    block->prepare(kRate, kBuffer);
    if (!block->configure({ { "path", path } }))
        return nullptr;
    for (int p = 0; p < kIrBlockDescriptor.numParams; ++p)
        block->setParam(p, kIrBlockDescriptor.params[p].def);
    return block;
}

// Feeds a unit impulse at sample 0 and returns `length` samples of output for each channel
std::vector<std::vector<float>> impulseResponse(Block& block, uint32_t numChannels, size_t length,
                                                int* allocations = nullptr)
{
    std::vector<std::vector<float>> out(numChannels);
    std::vector<float> buf[2] = { std::vector<float>(kBuffer), std::vector<float>(kBuffer) };
    for (auto& ch : out)
        ch.reserve(length + kBuffer);
    for (size_t t = 0; t < length; t += kBuffer)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            std::fill(buf[ch].begin(), buf[ch].end(), 0.0f);
            if (t == 0)
                buf[ch][0] = 1.0f;
        }
        float* io[2] = { buf[0].data(), buf[1].data() };
        {
            testing::AllocationGuard guard;
            block.process(io, numChannels, kBuffer);
            if (allocations)
                *allocations += guard.count();
        }
        for (uint32_t ch = 0; ch < numChannels; ++ch)
            out[ch].insert(out[ch].end(), buf[ch].begin(), buf[ch].end());
    }
    return out;
}

size_t peakIndex(const std::vector<float>& x)
{
    size_t best = 0;
    for (size_t i = 1; i < x.size(); ++i)
        if (std::fabs(x[i]) > std::fabs(x[best]))
            best = i;
    return best;
}

} // namespace

TEST_CASE("IR block convolves with zero latency and doesn't allocate")
{
    std::unique_ptr<Block> block = loadIr(writeWav("delta.wav", kRate, { impulseAt(256, 0) }));
    REQUIRE(block);
    int allocations = 0;
    const auto out = impulseResponse(*block, 1, 4096, &allocations);
    CHECK(allocations == 0);
    CHECK(peakIndex(out[0]) == 0);
    CHECK(out[0][0] > 0.9f);
}

TEST_CASE("IR block follows a delayed impulse, also when the file's rate differs from the host's")
{
    std::unique_ptr<Block> same = loadIr(writeWav("delay48.wav", kRate, { impulseAt(4800, 1000) }));
    REQUIRE(same);
    CHECK(peakIndex(impulseResponse(*same, 1, 8192)[0]) == 1000);

    // 441 samples at 44.1 kHz is 10 ms, which is 480 samples at 48 kHz
    std::unique_ptr<Block> resampled = loadIr(writeWav("delay44.wav", 44100.0, { impulseAt(4410, 441) }));
    REQUIRE(resampled);
    const size_t at = peakIndex(impulseResponse(*resampled, 1, 8192)[0]);
    CHECK(at >= 479);
    CHECK(at <= 481);
}

TEST_CASE("IR block uses both channels of a stereo IR")
{
    std::unique_ptr<Block> block =
        loadIr(writeWav("stereo.wav", kRate, { impulseAt(1024, 100), impulseAt(1024, 300) }));
    REQUIRE(block);
    const auto out = impulseResponse(*block, 2, 4096);
    CHECK(peakIndex(out[0]) == 100);
    CHECK(peakIndex(out[1]) == 300);
}

TEST_CASE("IR block mix blends the dry signal back in")
{
    std::unique_ptr<Block> block = loadIr(writeWav("delay-mix.wav", kRate, { impulseAt(1024, 500) }));
    REQUIRE(block);
    block->setParam(kIrParamMix, 0.0f);
    // let the mix smoothing settle
    impulseResponse(*block, 1, 48000);
    const auto out = impulseResponse(*block, 1, 2048);
    CHECK(peakIndex(out[0]) == 0);
    CHECK(std::fabs(out[0][500]) < 0.01f);
}

TEST_CASE("IR block rejects missing and invalid files")
{
    CHECK_FALSE(loadIr(""));
    CHECK_FALSE(loadIr((std::filesystem::temp_directory_path() / "xythos-no-such.wav").string()));
    const std::string bad = (std::filesystem::temp_directory_path() / "xythos-test-bad.wav").string();
    std::ofstream(bad) << "not a wav file";
    CHECK_FALSE(loadIr(bad));
}

// FFTConvolver's reset() unloads the IR; the block's reset() (which the engine calls when the host reactivates the
// plugin) must keep it
TEST_CASE("IR block keeps its IR when the host reactivates the plugin")
{
    std::unique_ptr<Block> block = loadIr(writeWav("delay-reactivate.wav", kRate, { impulseAt(1024, 300) }));
    REQUIRE(block);
    impulseResponse(*block, 1, 4096);
    block->prepare(kRate, kBuffer);
    int allocations = 0;
    {
        testing::AllocationGuard guard;
        block->reset();
        allocations = guard.count();
    }
    CHECK(allocations == 0);
    const auto out = impulseResponse(*block, 1, 4096);
    CHECK(peakIndex(out[0]) == 300);
    CHECK(out[0][300] > 0.9f);
    CHECK(std::fabs(out[0][0]) < 0.01f); // and no dry signal
}

namespace {

// A cab-like IR: 90 Hz to about 5 kHz with a presence bump, scaled by `scale`
std::vector<float> cabLikeIr(double rate, float scale)
{
    Biquad hp, lp1, lp2, peak;
    hp.setHighPass(rate, 90.0);
    lp1.setLowPass(rate, 5000.0);
    lp2.setLowPass(rate, 5000.0);
    peak.setPeak(rate, 2500.0, 5.0, 1.0);
    std::vector<float> ir(static_cast<size_t>(rate * 0.1));
    for (size_t i = 0; i < ir.size(); ++i)
        ir[i] = scale * peak.process(lp2.process(lp1.process(hp.process(i == 0 ? 1.0f : 0.0f))));
    return ir;
}

// Output level over input level, in dB, for noise shaped like an amp's output (falling above 800 Hz)
double levelChangeDb(Block& block)
{
    std::mt19937 rng(7);
    std::normal_distribution<float> noise;
    Biquad shape;
    shape.setLowPass(kRate, 800.0, 0.5);
    std::vector<float> buf(kBuffer);
    double in = 0.0, out = 0.0;
    for (int b = 0; b < 1500; ++b)
    {
        for (float& v : buf)
            v = shape.process(noise(rng));
        double blockIn = 0.0;
        for (float v : buf)
            blockIn += static_cast<double>(v) * v;
        float* io[1] = { buf.data() };
        block.process(io, 1, kBuffer);
        if (b < 100) // the IR's start and the gain smoothing
            continue;
        in += blockIn;
        for (float v : buf)
            out += static_cast<double>(v) * v;
    }
    return 10.0 * std::log10(out / in);
}

} // namespace

// Normalize used to give the IR unit energy, which left a cab 4 to 8 dB louder than its input on a guitar
TEST_CASE("IR block normalizes a cab to about unity on a guitar amp's spectrum, whatever its file's level")
{
    for (float scale : { 0.05f, 1.0f, 8.0f })
    {
        CAPTURE(scale);
        std::unique_ptr<Block> block = loadIr(writeWav("cab-level.wav", kRate, { cabLikeIr(kRate, scale) }));
        REQUIRE(block);
        CHECK(std::fabs(levelChangeDb(*block)) < 1.5);
    }
    // and at another file rate
    std::unique_ptr<Block> block = loadIr(writeWav("cab-level-44.wav", 44100.0, { cabLikeIr(44100.0, 1.0f) }));
    REQUIRE(block);
    CHECK(std::fabs(levelChangeDb(*block)) < 1.5);
}

// Settings > Normalized levels
TEST_CASE("IR block plays at the normalized level the settings give")
{
    std::unique_ptr<Block> block = loadIr(writeWav("cab-setting.wav", kRate, { cabLikeIr(kRate, 1.0f) }));
    REQUIRE(block);
    const double unity = levelChangeDb(*block);
    block->setNormalizeLevels({ -18.0f, -6.0f });
    CHECK(levelChangeDb(*block) - unity == doctest::Approx(-6.0).epsilon(0.02));
    // without Normalize the setting does nothing: the file's own level
    block->setParam(kIrParamNormalize, 0.0f);
    const double raw = levelChangeDb(*block);
    block->setNormalizeLevels({ -18.0f, 6.0f });
    CHECK(levelChangeDb(*block) == doctest::Approx(raw).epsilon(0.001));
}
