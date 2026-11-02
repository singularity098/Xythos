// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/Engine.hpp"
#include "engine/blocks/NamBlock.hpp"

#include "NAM/get_dsp.h"

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;
constexpr uint32_t kBuffer = 128;

std::string exampleModel(const char* name)
{
    return std::string(XYTHOS_EXAMPLE_MODELS) + "/" + name;
}

// Processes a 220 Hz sine through the block and returns the output. If `allocations` is given, it receives the
// number of heap allocations made inside process().
std::vector<float> render(Block& block, uint32_t buffers = 40, int* allocations = nullptr)
{
    std::vector<float> out;
    out.reserve(static_cast<size_t>(buffers) * kBuffer); // so only the block's own allocations are counted
    std::vector<float> buf(kBuffer);
    uint32_t t = 0;
    for (uint32_t b = 0; b < buffers; ++b)
    {
        for (uint32_t i = 0; i < kBuffer; ++i, ++t)
            buf[i] = 0.3f * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * t / kRate));
        float* io[2] = { buf.data(), buf.data() };
        {
            testing::AllocationGuard guard;
            block.process(io, 1, kBuffer);
            if (allocations)
                *allocations += guard.count();
        }
        out.insert(out.end(), buf.begin(), buf.end());
    }
    return out;
}

float rms(const std::vector<float>& x, size_t from)
{
    double sum = 0.0;
    for (size_t i = from; i < x.size(); ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return static_cast<float>(std::sqrt(sum / static_cast<double>(x.size() - from)));
}

std::unique_ptr<Block> loadBlock(const std::string& path, double a2Size = 1.0, bool tone = true)
{
    std::unique_ptr<Block> block = kNamBlockDescriptor.create();
    block->prepare(kRate, kBuffer);
    nlohmann::json config = { { "path", path }, { "a2Size", a2Size } };
    if (tone)
        config[kNamToneKey] = true;
    if (!block->configure(config))
        return nullptr;
    for (int p = 0; p < kNamBlockDescriptor.numParams; ++p)
        block->setParam(p, kNamBlockDescriptor.params[p].def);
    return block;
}

} // namespace

TEST_CASE("NAM block loads A1 and A2 models and processes audio without allocating")
{
    for (const char* name : { "wavenet_a1_standard.nam", "A2.nam", "lstm.nam" })
    {
        CAPTURE(name);
        std::unique_ptr<Block> block = loadBlock(exampleModel(name));
        REQUIRE(block);

        int allocations = 0;
        const std::vector<float> out = render(*block, 40, &allocations);
        CHECK(allocations == 0);

        bool finite = true;
        for (float v : out)
            finite &= std::isfinite(v);
        CHECK(finite);
        CHECK(rms(out, out.size() / 2) > 1e-4f);
    }
}

TEST_CASE("NAM block switches an A2 model between Full and Lite")
{
    std::unique_ptr<Block> full = loadBlock(exampleModel("slimmable_wavenet.nam"), 1.0);
    std::unique_ptr<Block> lite = loadBlock(exampleModel("slimmable_wavenet.nam"), 0.0);
    REQUIRE(full);
    REQUIRE(lite);
    const std::vector<float> a = render(*full);
    const std::vector<float> b = render(*lite);
    CHECK(a != b);
}

TEST_CASE("NAM block rejects missing and invalid files")
{
    CHECK_FALSE(loadBlock(exampleModel("does-not-exist.nam")));
    CHECK_FALSE(loadBlock(""));
    CHECK_FALSE(loadBlock(std::string(XYTHOS_EXAMPLE_MODELS) + "/../README.md"));
}

TEST_CASE("NAM block output level follows its parameters")
{
    std::unique_ptr<Block> block = loadBlock(exampleModel("wavenet_a1_standard.nam"));
    REQUIRE(block);
    block->setParam(kNamParamNormalize, 0.0f);
    // long enough for the 20 ms gain smoothing to settle
    const float base = rms(render(*block, 200), 12800);
    block->setParam(kNamParamOutput, -6.0f);
    const float quieter = rms(render(*block, 200), 12800);
    CHECK(20.0f * std::log10(quieter / base) == doctest::Approx(-6.0f).epsilon(0.02));
}

TEST_CASE("NAM block tone controls shape the output; blocks from before them stay flat")
{
    for (const bool tone : { true, false })
    {
        CAPTURE(tone);
        std::unique_ptr<Block> block = loadBlock(exampleModel("wavenet_a1_standard.nam"), 1.0, tone);
        REQUIRE(block);
        block->setParam(kNamParamNormalize, 0.0f);
        const float base = rms(render(*block, 200), 12800);
        block->setParam(kNamParamBass, 12.0f); // a shelf at 120 Hz: the model's 220 Hz fundamental rises a little
        int allocations = 0;
        const std::vector<float> out = render(*block, 200, &allocations);
        CHECK(allocations == 0);
        const float boosted = rms(out, 12800);
        if (tone)
            CHECK(20.0f * std::log10(boosted / base) > 0.5f);
        else
            CHECK(boosted == doctest::Approx(base).epsilon(1e-4));
    }
}

TEST_CASE("NAM block in the engine, including through the plugin's state format")
{
    Engine engine;
    engine.prepare(kRate, kBuffer);
    ChainState chain;
    BlockState block;
    block.typeId = "nam";
    block.slot = 0;
    block.config = { { "path", exampleModel("A2.nam") } };
    chain.blocks.push_back(block);
    for (int p = 0; p < kNamBlockDescriptor.numParams; ++p)
        engine.setParameter(slotParam(0, p), kNamBlockDescriptor.params[p].toNormalized(kNamBlockDescriptor.params[p].def));

    const std::optional<ChainState> parsed = ChainState::fromJson(chain.toJson());
    REQUIRE(parsed);
    std::vector<int> failed;
    REQUIRE(engine.setChain(*parsed, nullptr, &failed));
    CHECK(failed.empty());

    std::vector<float> in(kBuffer, 0.0f), out(kBuffer, 0.0f);
    for (uint32_t i = 0; i < kBuffer; ++i)
        in[i] = 0.3f * static_cast<float>(std::sin(2.0 * M_PI * 220.0 * i / kRate));
    const float* ins[2] = { in.data(), in.data() };
    float* outs[2] = { out.data(), out.data() };
    float energy = 0.0f;
    {
        testing::AllocationGuard guard;
        for (int b = 0; b < 50; ++b)
        {
            engine.process(ins, outs, kBuffer);
            energy += std::fabs(out[kBuffer / 2]);
        }
        CHECK(guard.count() == 0);
    }
    CHECK(energy > 0.0f);
}

TEST_CASE("two NAM amps in a split, panned hard in stereo mode, each reach only their own side")
{
    // the owner heard no difference between the speakers; each side must be exactly its amp alone
    auto renderChain = [](const ChainState& chain, bool parallel) {
        Engine engine;
        engine.setParallelProcessing(parallel);
        engine.prepare(kRate, kBuffer);
        engine.setParameter(kParamInputMode, 1.0f);
        engine.setParameter(kParamLimiter, 0.0f);
        engine.setParameter(kParamMergePanA, -100.0f);
        engine.setParameter(kParamMergePanB, 100.0f);
        for (int slot = 0; slot < 2; ++slot)
            for (int p = 0; p < kNamBlockDescriptor.numParams; ++p)
                engine.setParameter(slotParam(slot, p),
                                    kNamBlockDescriptor.params[p].toNormalized(kNamBlockDescriptor.params[p].def));
        REQUIRE(engine.setChain(chain));

        std::vector<float> in(kBuffer), left, right, outL(kBuffer), outR(kBuffer);
        uint32_t t = 0;
        for (int b = 0; b < 100; ++b)
        {
            for (uint32_t i = 0; i < kBuffer; ++i, ++t)
                in[i] = 0.3f * static_cast<float>(std::sin(2.0 * M_PI * 110.0 * t / kRate));
            const float* ins[2] = { in.data(), in.data() };
            float* outs[2] = { outL.data(), outR.data() };
            engine.process(ins, outs, kBuffer);
            left.insert(left.end(), outL.begin(), outL.end());
            right.insert(right.end(), outR.begin(), outR.end());
        }
        return std::pair { left, right };
    };
    auto amp = [](int slot, const char* model, int branch) {
        BlockState block;
        block.typeId = "nam";
        block.slot = slot;
        block.branch = branch;
        block.config = { { "path", exampleModel(model) }, { "tone", true } };
        return block;
    };

    ChainState split;
    split.blocks = { amp(0, "lstm.nam", kBranchA), amp(1, "wavenet.nam", kBranchB) };
    split.split = SplitState { 0, 2 };
    ChainState onlyA, onlyB;
    onlyA.blocks = { amp(0, "lstm.nam", kBranchA) };
    onlyB.blocks = { amp(1, "wavenet.nam", kBranchA) };

    for (const bool parallel : { false, true })
    {
        CAPTURE(parallel);
        const auto [left, right] = renderChain(split, parallel);
        const auto [aLeft, aRight] = renderChain(onlyA, parallel);
        const auto [bLeft, bRight] = renderChain(onlyB, parallel);
        const size_t from = left.size() / 2;
        std::vector<float> diffA(left.size()), diffB(right.size()), sides(left.size());
        for (size_t i = 0; i < left.size(); ++i)
        {
            diffA[i] = left[i] - aLeft[i];
            diffB[i] = right[i] - bRight[i];
            sides[i] = left[i] - right[i];
        }
        CHECK(rms(left, from) > 1e-3f);
        CHECK(rms(right, from) > 1e-3f);
        CHECK(rms(diffA, from) < 1e-4f); // the merge gains settle smoothly
        CHECK(rms(diffB, from) < 1e-4f);
        CHECK(rms(sides, from) > 1e-3f);
    }
}

TEST_CASE("NAM block removes DC from the model output")
{
    std::unique_ptr<Block> block = loadBlock(exampleModel("wavenet_a1_standard.nam"));
    REQUIRE(block);
    std::vector<float> silence(kBuffer, 0.0f);
    float* io[2] = { silence.data(), silence.data() };
    for (int b = 0; b < 2000; ++b) // ~5 s
    {
        std::fill(silence.begin(), silence.end(), 0.0f);
        block->process(io, 1, kBuffer);
    }
    double mean = 0.0;
    for (float v : silence)
        mean += v;
    CHECK(std::fabs(mean / kBuffer) < 1e-4);
}

TEST_CASE("a model file with fewer weights than its config needs fails to load instead of reading past them")
{
    // patches/nam/: NAM Core's loaders read weights unchecked; a short file read past the end of the vector
    for (const char* name : { "lstm.nam", "wavenet.nam", "slimmable_wavenet.nam" })
    {
        CAPTURE(name);
        std::ifstream in(exampleModel(name));
        nlohmann::json config = nlohmann::json::parse(in);
        auto& weights = config.at("weights");
        REQUIRE(weights.size() > 2);
        for (const size_t keep : { weights.size() / 2, weights.size() - 1, size_t(0) })
        {
            CAPTURE(keep);
            nlohmann::json shorter = config;
            shorter["weights"] = nlohmann::json(std::vector<float>(weights.begin(), weights.begin() + keep));
            CHECK_THROWS_AS(nam::get_dsp(shorter), std::runtime_error);
        }
    }
}
