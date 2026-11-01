// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The whole audio engine driven like a host would, by the fuzzer's bytes: a chain of any built-in blocks (with a
// split, bypasses, MIDI mappings, settings), then a run of host events in any order: audio buffers of any size and
// content (silence, noise, full scale, huge values, denormals), parameter changes (in range, or anywhere), MIDI CCs,
// tempo and transport, chain changes, the plugin's chain fault rebuild, the looper's memory.
//
// The engine must never crash, never output a non-finite sample (it silences a broken chain and reports it), and
// never write past the buffers it was given.

#include "FuzzTarget.hpp"

#include "engine/BlockRegistry.hpp"
#include "engine/Engine.hpp"
#include "engine/Params.hpp"

#include <cmath>
#include <vector>

using namespace xythos;

namespace {

constexpr uint32_t kMaxFrames = 512;
constexpr int kMaxChainBlocks = 6; // enough for splits and interactions, few enough to stay fast
constexpr float kGuard = 12345.0f; // written after each buffer, must survive process()

ChainState makeChain(fuzz::Input& in)
{
    const auto& types = BlockRegistry::builtin().types();
    ChainState chain;
    const int count = in.range(0, kMaxChainBlocks);
    uint32_t usedSlots = 0;
    for (int i = 0; i < count; ++i)
    {
        BlockState block;
        const BlockDescriptor* type = types[static_cast<size_t>(in.range(0, static_cast<int>(types.size()) - 1))];
        block.typeId = type->typeId;
        int slot = in.range(0, kMaxBlocks - 1);
        while (usedSlots & (1u << slot))
            slot = (slot + 1) % kMaxBlocks;
        usedSlots |= 1u << slot;
        block.slot = slot;
        block.bypass = in.range(0, 7) == 0;
        for (int p = 0; p < type->numParams; ++p)
            block.params[static_cast<size_t>(p)] = type->params[p].toValue(in.unit());
        block.config = nlohmann::json::object();
        if (type->lookahead && in.flag())
            block.config["lookahead"] = in.range(0, 30);
        chain.blocks.push_back(std::move(block));
    }
    if (count >= 2 && in.flag())
    {
        const int begin = in.range(0, count - 1);
        const int end = in.range(begin + 1, count);
        chain.addSplit(begin, end);
        for (int i = begin; i < end; ++i)
            chain.blocks[static_cast<size_t>(i)].branch = in.flag() ? kBranchB : kBranchA;
    }
    const int mappings = in.range(0, 2);
    for (int i = 0; i < mappings; ++i)
        chain.setMidiMapping({ in.range(0, 15), in.range(0, 127), static_cast<uint32_t>(in.range(0, kParamCount - 1)) });
    return chain;
}

void applyChain(Engine& engine, const ChainState& chain)
{
    std::string error;
    if (!chain.validate(BlockRegistry::builtin(), &error))
        return;
    engine.setChain(chain, &error);
    // the plugin pushes the host's values for the chain's slots
    for (const BlockState& block : chain.blocks)
    {
        const BlockDescriptor* type = BlockRegistry::builtin().find(block.typeId);
        engine.setParameter(slotBypassParam(block.slot), block.bypass ? 1.0f : 0.0f);
        for (int p = 0; p < type->numParams; ++p)
            engine.setParameter(slotParam(block.slot, p), type->params[p].toNormalized(block.params[static_cast<size_t>(p)]));
    }
}

float sampleFor(int kind, fuzz::Input& in, uint32_t i)
{
    switch (kind)
    {
    case 0: return 0.0f;
    case 1: return 0.3f * std::sin(0.031f * static_cast<float>(i)); // a steady tone
    case 2: return (in.byte() - 128) / 128.0f;                         // noise from the fuzzer
    case 3: return (i / 16) % 2 ? 1.0f : -1.0f;                        // full-scale square
    case 4: return (i % 2 ? 1.0f : -1.0f) * 1e-40f;                    // denormals
    case 5: return (i % 2 ? 1.0f : -1.0f) * 1e6f;                      // far too loud
    default: return in.anyFloat();                                     // anything, NaN included
    }
}

} // namespace

XYTHOS_FUZZ_TARGET(engine)
{
    fuzz::Input in(data, size);
    static constexpr double kRates[] = { 44100.0, 48000.0, 96000.0, 22050.0 };
    const double rate = kRates[in.range(0, 3)];

    Engine engine;
    engine.setParallelProcessing(in.flag());
    engine.prepare(rate, kMaxFrames);
    ChainState chain = makeChain(in);
    applyChain(engine, chain);

    std::vector<float> inL(kMaxFrames), inR(kMaxFrames), outL(kMaxFrames + 1), outR(kMaxFrames + 1);
    Transport transport;
    int events = 0;
    while (!in.empty() && events++ < 64)
    {
        switch (in.range(0, 9))
        {
        case 0: case 1: case 2: case 3: {
            const auto frames = static_cast<uint32_t>(in.range(1, static_cast<int>(kMaxFrames)));
            const int kind = in.range(0, 6);
            for (uint32_t i = 0; i < frames; ++i)
            {
                inL[i] = sampleFor(kind, in, i);
                inR[i] = in.flag() ? inL[i] : -inL[i];
            }
            outL[frames] = kGuard;
            outR[frames] = kGuard;
            const float* inputs[2] = { inL.data(), inR.data() };
            float* outputs[2] = { outL.data(), outR.data() };
            engine.process(inputs, outputs, frames, transport.valid ? &transport : nullptr);
            for (uint32_t i = 0; i < frames; ++i)
                FUZZ_CHECK(std::isfinite(outL[i]) && std::isfinite(outR[i]));
            FUZZ_CHECK(outL[frames] == kGuard && outR[frames] == kGuard);
            if (transport.playing)
                transport.beat += frames / rate * transport.bpm / 60.0;
            // the plugin's exchange thread, between buffers
            engine.drainMidi(engine.midiTime(), [](const auto&) {});
            if (engine.takeChainFault())
            {
                engine.forgetBlocks();
                applyChain(engine, chain);
            }
            break;
        }
        case 4: {
            // a host parameter: in its range (normalized for slots), or now and then anything finite
            const auto index = static_cast<uint32_t>(in.range(0, kParamCount - 1));
            float value = in.unit();
            if (!isSlotParam(index))
            {
                const GlobalParamSpec& spec = globalParamSpec(index);
                value = spec.min + value * (spec.max - spec.min);
            }
            if (in.range(0, 15) == 0)
            {
                value = in.anyFloat();
                if (!std::isfinite(value))
                    value = 0.0f;
            }
            engine.setParameter(index, value);
            (void)engine.getParameter(static_cast<uint32_t>(in.range(0, kParamCount - 1)));
            break;
        }
        case 5: {
            const Engine::CcResult result = engine.midiCc(in.range(0, 15), in.range(0, 127), in.range(0, 127));
            FUZZ_CHECK(result.param < static_cast<int>(kParamCount));
            break;
        }
        case 6:
            engine.setTempo(in.range(0, 400) + in.unit());
            transport.valid = in.flag();
            transport.playing = in.flag();
            transport.bpm = in.range(20, 300);
            transport.beat = in.range(0, 1000) + in.unit();
            transport.beatsPerBar = in.range(1, 16);
            break;
        case 7:
            // a new chain (blocks it shares with the old one are kept)
            chain = makeChain(in);
            applyChain(engine, chain);
            break;
        case 8:
            engine.analyzeTuner();
            break;
        case 9:
            // the looper's memory, on the exchange thread (23 MB: only now and then)
            if (in.range(0, 7) == 0)
            {
                engine.setParameter(kParamLooperWake, 1.0f);
                engine.serviceLooper();
                (void)engine.looperWav();
            }
            break;
        }
    }
    return 0;
}
