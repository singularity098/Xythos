// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/Engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace xythos {

namespace {

constexpr uint32_t kDefaultMaxFrames = 2048;
constexpr float kMaxMergeAlignMs = 5.0f; // the Merge Align parameter's range

float clampDb(float db)
{
    return std::clamp(db, -60.0f, 6.0f);
}

uint32_t sectionLatency(const auto& nodes)
{
    uint32_t latency = 0;
    for (const auto& node : nodes)
        latency += node.block->latency();
    return latency;
}

} // namespace

Engine::Engine(const BlockRegistry& registry)
    : fRegistry(registry)
{
    for (uint32_t i = 0; i < kParamCount; ++i)
        fParams[i].store(0.0f, std::memory_order_relaxed);
    for (uint32_t i = 0; i < kParamCount; ++i)
    {
        if (!isSlotParam(i))
            fParams[i].store(globalParamSpec(i).def, std::memory_order_relaxed);
    }

    fActive = new Graph();
    prepare(fSampleRate, kDefaultMaxFrames);
}

Engine::~Engine()
{
    drainRetired();
    delete fPending.exchange(nullptr);
    delete fNext;
    delete fActive;
}

void Engine::prepare(double sampleRate, uint32_t maxFrames)
{
    const std::lock_guard<std::mutex> lock(fMutex);

    fSampleRate = sampleRate;
    fMaxFrames = std::max<uint32_t>(maxFrames, 1);

    // Audio is stopped, so finish any pending swap right away
    drainRetired();
    if (Graph* pending = fPending.exchange(nullptr))
    {
        delete fNext;
        fNext = pending;
    }
    if (fNext)
    {
        delete fActive;
        fActive = fNext;
        fNext = nullptr;
    }
    fFade = Fade::Idle;
    fFadeGain = 1.0f;
    // the host restarts audio: what was waiting goes, and every sounding note ends at the next buffer
    fMidiQueued = 0;
    fMidiEndAll = true;

    std::unordered_set<Block*> blocks;
    fActive->forEachNode([&](const Node& node) { blocks.insert(node.block.get()); });
    for (const CachedBlock& cached : fCache)
    {
        if (cached.block)
            blocks.insert(cached.block.get());
    }
    for (Block* block : blocks)
    {
        block->prepare(fSampleRate, fMaxFrames);
        block->reset();
    }

    fLooper.setHasCountIn(true);
    fLooper.prepare(fSampleRate);
    fLooperApplyAll = true;

    // block latencies may depend on the sample rate
    updateLatency(*fActive);
    fLatency.store(fActive->latency, std::memory_order_relaxed);
    for (SlotState& slot : fSlots)
        slot = SlotState {};
    fActive->forEachNode([this](const Node& node) {
        SlotState& slot = fSlots[node.slot];
        slot.block = node.block.get();
        slot.applyAll = true;
        slot.bypassMix = param(slotBypassParam(node.slot)) >= 0.5f ? 1.0f : 0.0f;
    });

    fFadeStep = static_cast<float>(1.0 / (kSwapFadeSeconds * fSampleRate));
    fRampStep = static_cast<float>(1.0 / (kBypassRampSeconds * fSampleRate));
    fGlobalBypassMix = param(kParamBypass) >= 0.5f ? 1.0f : 0.0f;
    fTunerOn = false;
    fTunerMuteMix = 0.0f;
    {
        const std::lock_guard<std::mutex> tunerLock(fTunerMutex);
        fTuner.prepare(fSampleRate);
    }
    fMetronome.prepare(fSampleRate);
    fClick.assign(fMaxFrames, 0.0f);
    for (std::vector<float>& click : fBlockClick)
        click.assign(fMaxFrames, 0.0f);

    fInputGain.prepare(fSampleRate);
    fInputGain.snap(dbToGain(param(kParamInput)));
    fOutputGain.prepare(fSampleRate);
    fOutputGain.snap(dbToGain(param(kParamOutput)));
    fToneStack.setGains(param(kParamBass), param(kParamMiddle), param(kParamTreble));
    fToneStack.prepare(fSampleRate);
    fLimiter.prepare(fSampleRate);
    for (int branch = 0; branch < 2; ++branch)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            fMergeGain[branch][ch].prepare(fSampleRate);
            fMergeGain[branch][ch].snap(mergeGain(branch, ch));
            fAlignLines[branch][ch].prepare(static_cast<size_t>(std::ceil(kMaxMergeAlignMs * 0.001 * fSampleRate)) + 4);
        }
    }
    fMergeAlign.prepare(fSampleRate, 0.05);
    fMergeAlign.snap(mergeAlignSamples());
    for (int ch = 0; ch < 2; ++ch)
    {
        fInMeters[ch].prepare(fSampleRate);
        fOutMeters[ch].prepare(fSampleRate);
        fWork[ch].assign(fMaxFrames, 0.0f);
        fDry[ch].assign(fMaxFrames, 0.0f);
        fScratch[ch].assign(fMaxFrames, 0.0f);
        fRigInput[ch].assign(fMaxFrames, 0.0f);
        fBranch[ch].assign(fMaxFrames, 0.0f);
        fBranchScratch[ch].assign(fMaxFrames, 0.0f);
    }
}

bool Engine::setChain(const ChainState& chain, std::string* error, std::vector<int>* failedSlots)
{
    if (!chain.validate(fRegistry, error))
        return false;

    const std::lock_guard<std::mutex> lock(fMutex);
    drainRetired();

    auto graph = std::make_unique<Graph>();
    std::array<CachedBlock, kMaxBlocks> cache;

    graph->split = chain.split.has_value();
    for (size_t i = 0; i < chain.blocks.size(); ++i)
    {
        const BlockState& state = chain.blocks[i];
        const BlockDescriptor* type = fRegistry.find(state.typeId);
        std::string config = state.config.dump();

        CachedBlock& previous = fCache[state.slot];
        std::shared_ptr<Block> block;
        int32_t restoreLatched = -1;
        if (previous.block && previous.typeId == state.typeId && previous.config == config)
        {
            block = previous.block;
            if (chain.restoreLatches)
                restoreLatched = static_cast<int32_t>(state.latched & 0xffffu);
        }
        else
        {
            block = type->create();
            block->prepare(fSampleRate, fMaxFrames);
            if (!block->configure(state.config))
            {
                if (failedSlots)
                    failedSlots->push_back(state.slot);
                continue;
            }
            block->setLatched(state.latched);
        }

        std::vector<Node>* section = &graph->pre;
        switch (chain.sectionOf(static_cast<int>(i)))
        {
        case Section::Pre: section = &graph->pre; break;
        case Section::A: section = &graph->branchA; break;
        case Section::B: section = &graph->branchB; break;
        case Section::Post: section = &graph->post; break;
        }
        section->push_back(Node { block, type, state.slot, AlignmentDelay {}, 0, restoreLatched });
        cache[state.slot] = { std::move(block), state.typeId, std::move(config) };
    }

    fCache = std::move(cache);
    for (const MidiMapping& mapping : chain.midi)
        graph->midiMap[mapping.channel * 128 + mapping.cc] = static_cast<int16_t>(mapping.param);
    updateLatency(*graph);

    if (graph->split && !graph->branchB.empty() && fParallel.load(std::memory_order_relaxed))
    {
        if (!fWorker)
            fWorker = std::make_unique<ParallelWorker>();
        graph->worker = fWorker.get();
    }

    // A graph still pending was never seen by the audio thread, so it can be deleted here
    delete fPending.exchange(graph.release(), std::memory_order_acq_rel);
    return true;
}

void Engine::forgetBlocks()
{
    const std::lock_guard<std::mutex> lock(fMutex);
    fCache = {};
}

bool Engine::silenceNonFinite(float* const* io, uint32_t numChannels, uint32_t frames)
{
    bool found = false;
    for (uint32_t ch = 0; ch < numChannels; ++ch)
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (!std::isfinite(io[ch][i]))
            {
                io[ch][i] = 0.0f;
                found = true;
            }
        }
    }
    return found;
}

void Engine::setParameter(uint32_t index, float value)
{
    if (index < kParamCount)
        fParams[index].store(value, std::memory_order_relaxed);
}

float Engine::getParameter(uint32_t index) const
{
    return index < kParamCount ? param(index) : 0.0f;
}

Engine::CcResult Engine::midiCc(int channel, int cc, int value)
{
    CcResult result;
    if (channel < 0 || channel > 15 || cc < 0 || cc > 127)
        return result;

    fCcCount = (fCcCount + 1) % (1u << 24);
    fParams[kParamMidiLastCc].store(static_cast<float>(channel * 128 + cc), std::memory_order_relaxed);
    fParams[kParamMidiLastValue].store(static_cast<float>(std::clamp(value, 0, 127)), std::memory_order_relaxed);
    fParams[kParamMidiCcCount].store(static_cast<float>(fCcCount), std::memory_order_relaxed);

    // While learning, the CC only teaches; it doesn't move whatever it was mapped to before
    if (param(kParamMidiLearn) >= 0.5f)
        return result;

    // the graph about to be swapped in already has the newest map
    const Graph* graph = fNext ? fNext : fActive;
    const int index = graph->midiMap[channel * 128 + cc];
    if (index < 0)
        return result;

    const float v = midiCcToValue(static_cast<uint32_t>(index), value);
    fParams[index].store(v, std::memory_order_relaxed);
    result.param = index;
    result.value = v;
    return result;
}

void Engine::process(const float* const* inputs, float* const* outputs, uint32_t frames,
                     const Transport* transport)
{
    uint32_t done = 0;
    while (done < frames)
    {
        const uint32_t n = std::min(frames - done, fMaxFrames);
        const float* in[2] = { inputs[0] + done, inputs[1] + done };
        float* out[2] = { outputs[0] + done, outputs[1] + done };
        fChunkTime = fTime;
        processChunk(in, out, n, transport, done);
        fTime += n;
        done += n;
    }

    const Metronome::Status& metronome = fMetronome.status();
    fParams[kParamMetroBeat].store(static_cast<float>(metronome.beat), std::memory_order_relaxed);
    fParams[kParamMetroBeatCount].store(static_cast<float>(metronome.beatCount), std::memory_order_relaxed);
    fParams[kParamMetroBar].store(static_cast<float>(metronome.bar & 0xffffffu), std::memory_order_relaxed);
    fParams[kParamMetroLiveTempo].store(static_cast<float>(metronome.bpm), std::memory_order_relaxed);
    fParams[kParamMetroSilent].store(metronome.silent ? 1.0f : 0.0f, std::memory_order_relaxed);

    fParams[kParamInPeakL].store(clampDb(gainToDb(fInMeters[0].level())), std::memory_order_relaxed);
    fParams[kParamInPeakR].store(clampDb(gainToDb(fInMeters[1].level())), std::memory_order_relaxed);
    fParams[kParamOutPeakL].store(clampDb(gainToDb(fOutMeters[0].level())), std::memory_order_relaxed);
    fParams[kParamOutPeakR].store(clampDb(gainToDb(fOutMeters[1].level())), std::memory_order_relaxed);
    fParams[kParamInClip].store(fInMeters[0].clipping() || fInMeters[1].clipping() ? 1.0f : 0.0f,
                                std::memory_order_relaxed);
    uint32_t running = 0;
    std::array<uint32_t, kMaxBlocks> latched {};
    fActive->forEachNode([&](const Node& node) {
        running |= 1u << node.slot;
        fParams[slotMeterParam(node.slot)].store(node.block->meter(), std::memory_order_relaxed);
        latched[node.slot] = node.block->latched() & 0xffffu;
    });
    for (int slot = 0; slot < kMaxBlocks; ++slot)
        fParams[slotLatchParam(slot)].store(static_cast<float>(latched[slot]), std::memory_order_relaxed);
    fParams[kParamRunningSlotsLow].store(static_cast<float>(running & 0xffffu), std::memory_order_relaxed);
    fParams[kParamRunningSlotsHigh].store(static_cast<float>(running >> 16), std::memory_order_relaxed);
    fParams[kParamOutClip].store(fOutMeters[0].clipping() || fOutMeters[1].clipping() ? 1.0f : 0.0f,
                                 std::memory_order_relaxed);
}

void Engine::analyzeTuner()
{
    const std::lock_guard<std::mutex> lock(fTunerMutex);
    PitchDetector::Result result;
    if (param(kParamTuner) >= 0.5f)
        result = fTuner.analyze();
    fParams[kParamTunerFrequency].store(static_cast<float>(result.frequency), std::memory_order_relaxed);
    fParams[kParamTunerClarity].store(static_cast<float>(result.clarity), std::memory_order_relaxed);
}

Metronome::Settings Engine::metronomeSettings() const
{
    auto integer = [this](uint32_t index) { return static_cast<int>(std::lround(param(index))); };
    Metronome::Settings settings;
    settings.on = param(kParamMetronome) >= 0.5f;
    settings.sync = param(kParamMetroSync) >= 0.5f;
    settings.bpm = param(kParamMetroTempo);
    settings.beats = std::clamp(integer(kParamMetroBeats), 1, Metronome::kMaxBeats);
    settings.subdivision = static_cast<MetroSubdivision>(
        std::clamp(integer(kParamMetroSubdivision), 0, static_cast<int>(MetroSubdivision::Count) - 1));
    settings.sound = static_cast<MetroSound>(std::clamp(integer(kParamMetroSound), 0,
                                                        static_cast<int>(MetroSound::Count) - 1));
    settings.volume = std::clamp(param(kParamMetroVolume) / 100.0f, 0.0f, 1.0f);
    settings.subVolume = std::clamp(param(kParamMetroSubVolume) / 100.0f, 0.0f, 1.0f);
    for (int i = 0; i < Metronome::kMaxBeats; ++i)
        settings.levels[i] = integer(kParamMetroAccent1 + static_cast<uint32_t>(i));
    settings.trainer = static_cast<MetroTrainer>(std::clamp(integer(kParamMetroTrainer), 0, 2));
    settings.trainerStart = param(kParamMetroTrainerStart);
    settings.trainerEnd = param(kParamMetroTrainerEnd);
    settings.trainerStep = param(kParamMetroTrainerStep);
    settings.trainerEvery = integer(kParamMetroTrainerEvery);
    settings.trainerBars = integer(kParamMetroTrainerBars);
    settings.gap = param(kParamMetroGap) >= 0.5f;
    settings.gapPlay = integer(kParamMetroGapPlay);
    settings.gapMute = integer(kParamMetroGapMute);
    return settings;
}

void Engine::processLooper(float* const* work, uint32_t& numChannels, uint32_t frames)
{
    // the controls it hasn't taken yet (only changes: Tap sets the BPM inside it, which the host's value mustn't undo)
    for (int p = 0; p < kLooperParamCount; ++p)
    {
        const float value = param(looperParam(p));
        if (fLooperApplyAll || value != fLooperApplied[static_cast<size_t>(p)])
        {
            fLooperApplied[static_cast<size_t>(p)] = value;
            fLooper.setParam(p, value);
        }
    }
    fLooperApplyAll = false;

    // a loop recorded in stereo plays in stereo whatever the chain is now
    if (numChannels == 1 && (fLooper.hasLoop() || fLooper.report().state == LooperState::Recording))
    {
        std::memcpy(work[1], work[0], sizeof(float) * frames);
        numChannels = 2;
    }
    fLooper.process(work, numChannels, frames, fBlockClick[0].data());

    const LooperReport report = fLooper.report();
    fParams[kParamLooperState].store(report.encodeState(), std::memory_order_relaxed);
    fParams[kParamLooperPosition].store(report.position, std::memory_order_relaxed);
    fParams[kParamLooperCount].store(static_cast<float>(report.count), std::memory_order_relaxed);
    fParams[kParamLooperLength].store(report.seconds, std::memory_order_relaxed);
    float cells[kParamLooperCellEnd - kParamLooperCellBase];
    fLooper.streamCells(cells, static_cast<int>(std::size(cells)));
    for (uint32_t k = 0; k < std::size(cells); ++k)
        fParams[kParamLooperCellBase + k].store(cells[k], std::memory_order_relaxed);
}

void Engine::processChunk(const float* const* inputs, float* const* outputs, uint32_t frames,
                          const Transport* transport, uint32_t offset)
{
    const bool stereo = param(kParamInputMode) >= 0.5f;
    const uint32_t numChannels = stereo ? 2 : 1;

    // Copy first: inputs and outputs may be the same buffers. A NaN from the host would stay in every filter.
    for (int ch = 0; ch < 2; ++ch)
        std::memcpy(fDry[ch].data(), inputs[ch], sizeof(float) * frames);
    float* dry[2] = { fDry[0].data(), fDry[1].data() };
    silenceNonFinite(dry, 2, frames);

    // Input stage
    fInputGain.setTarget(dbToGain(param(kParamInput)));
    for (uint32_t i = 0; i < frames; ++i)
    {
        const float g = fInputGain.next();
        fWork[0][i] = fDry[0][i] * g;
        fWork[1][i] = fDry[1][i] * g;
    }
    fInMeters[0].process(fWork[0].data(), frames);
    fInMeters[1].process(fWork[stereo ? 1 : 0].data(), frames);

    // The tuner reads the input before the chain
    const bool tunerOn = param(kParamTuner) >= 0.5f;
    if (tunerOn && !fTunerOn)
        fTuner.restart();
    fTunerOn = tunerOn;
    if (tunerOn)
        fTuner.push(fWork[0].data(), stereo ? fWork[1].data() : nullptr, frames);

    // Song position for blocks that lock to the beat
    fSongPlaying = transport && transport->valid && transport->playing;
    if (fSongPlaying)
        fSongBeat = transport->beat + offset * transport->bpm / 60.0 / fSampleRate;

    // Blocks keyed from the rig input read this copy (the chain processes fWork in place). Both channels: a Mono
    // chain can turn stereo on the way.
    for (uint32_t ch = 0; ch < 2; ++ch)
        std::memcpy(fRigInput[ch].data(), fWork[stereo ? ch : 0].data(), sizeof(float) * frames);

    // MIDI the blocks send in this chunk
    for (MidiOutput& midi : fMidiOut)
    {
        midi.clear();
        midi.setNode(fChunkTime, 0, &fMidiSounding);
    }
    if (fMidiEndAll)
    {
        fMidiEndAll = false;
        for (int slot = 0; slot < kMaxBlocks; ++slot)
            fMidiOut[0].endSlot(slot, fChunkTime);
    }

    for (std::vector<float>& click : fBlockClick)
        std::fill(click.begin(), click.begin() + frames, 0.0f);

    // Chain, with a fade out/in around graph swaps
    if (fFade == Fade::Idle)
    {
        if (Graph* pending = fPending.exchange(nullptr, std::memory_order_acq_rel))
        {
            fNext = pending;
            fFade = Fade::Out;
        }
    }

    uint32_t outChannels = processChain(numChannels, frames);

    if (fFade != Fade::Idle)
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (fFade == Fade::Out)
            {
                fFadeGain = std::max(0.0f, fFadeGain - fFadeStep);
            }
            else if (fFade == Fade::In)
            {
                fFadeGain = std::min(1.0f, fFadeGain + fFadeStep);
                if (fFadeGain >= 1.0f)
                    fFade = Fade::Idle;
            }
            for (uint32_t ch = 0; ch < outChannels; ++ch)
                fWork[ch][i] *= fFadeGain;
        }
        if (fFade == Fade::Out && fFadeGain <= 0.0f)
        {
            swapGraph();
            fFade = Fade::In;
        }
    }
    collectMidi(fMidiOut[0]);
    collectMidi(fMidiOut[1]);

    // Output stage
    float* work[2] = { fWork[0].data(), fWork[1].data() };
    // A block whose state went NaN outputs NaN from then on: silence it here, before it reaches the filters below,
    // and have the chain rebuilt with fresh blocks
    if (silenceNonFinite(work, outChannels, frames))
        fChainFault.store(true, std::memory_order_release);
    fToneStack.setGains(param(kParamBass), param(kParamMiddle), param(kParamTreble));
    fToneStack.process(work, outChannels, frames);

    // Output level, and the tuner's mute (which only silences the guitar, not the metronome)
    fOutputGain.setTarget(dbToGain(param(kParamOutput)));
    const float muteTarget = tunerOn && param(kParamTunerMute) >= 0.5f ? 1.0f : 0.0f;
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (fTunerMuteMix < muteTarget)
            fTunerMuteMix = std::min(muteTarget, fTunerMuteMix + fRampStep);
        else if (fTunerMuteMix > muteTarget)
            fTunerMuteMix = std::max(muteTarget, fTunerMuteMix - fRampStep);
        const float g = fOutputGain.next() * (1.0f - fTunerMuteMix);
        for (uint32_t ch = 0; ch < outChannels; ++ch)
            fWork[ch][i] *= g;
    }

    // The looper, on what's heard; it turns the output stereo while it holds a loop
    processLooper(work, outChannels, frames);

    // Metronome, before the limiter so clicks can't clip the output
    std::fill(fClick.begin(), fClick.begin() + frames, 0.0f);
    fMetronome.process(fClick.data(), frames, metronomeSettings(), transport, offset);
    // and the clicks blocks sent (the looper's count-in), which skip the blocks after them and the output level
    for (uint32_t i = 0; i < frames; ++i)
        fClick[i] += fBlockClick[0][i] + fBlockClick[1][i];
    for (uint32_t ch = 0; ch < outChannels; ++ch)
        for (uint32_t i = 0; i < frames; ++i)
            fWork[ch][i] += fClick[i];

    float reduction = 1.0f;
    if (param(kParamLimiter) >= 0.5f)
        reduction = fLimiter.process(work, outChannels, frames);
    else
        fLimiter.reset();
    fParams[kParamLimiterReduction].store(-gainToDb(reduction, -24.0f), std::memory_order_relaxed);

    // the output stage can't make a NaN from finite input, but if it ever did it must not stay in its filters
    if (silenceNonFinite(work, outChannels, frames))
    {
        fToneStack.reset();
        fLimiter.reset();
    }

    if (outChannels == 1)
        std::memcpy(fWork[1].data(), fWork[0].data(), sizeof(float) * frames);

    // Global bypass: crossfade to the untouched input (which the tuner mutes too)
    const float bypassTarget = param(kParamBypass) >= 0.5f ? 1.0f : 0.0f;
    if (fTunerMuteMix > 0.0f)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (uint32_t i = 0; i < frames; ++i)
                fDry[ch][i] *= 1.0f - fTunerMuteMix;
    }
    for (uint32_t i = 0; i < frames; ++i)
    {
        if (fGlobalBypassMix < bypassTarget)
            fGlobalBypassMix = std::min(bypassTarget, fGlobalBypassMix + fRampStep);
        else if (fGlobalBypassMix > bypassTarget)
            fGlobalBypassMix = std::max(bypassTarget, fGlobalBypassMix - fRampStep);

        for (int ch = 0; ch < 2; ++ch)
        {
            // exact at the ends, so fully bypassed really is the untouched input
            if (fGlobalBypassMix == 0.0f)
                outputs[ch][i] = fWork[ch][i];
            else if (fGlobalBypassMix == 1.0f)
                outputs[ch][i] = fDry[ch][i];
            else
                outputs[ch][i] = fWork[ch][i] + (fDry[ch][i] - fWork[ch][i]) * fGlobalBypassMix;
        }
    }

    fOutMeters[0].process(outputs[0], frames);
    fOutMeters[1].process(outputs[1], frames);
}

uint32_t Engine::processChain(uint32_t numChannels, uint32_t frames)
{
    float* work[2] = { fWork[0].data(), fWork[1].data() };
    float* scratch[2] = { fScratch[0].data(), fScratch[1].data() };
    Graph& graph = *fActive;

    numChannels = processNodes(graph.pre, work, scratch, numChannels, frames, fMidiOut[0], fBlockClick[0].data());
    if (graph.split)
    {
        for (uint32_t ch = 0; ch < numChannels; ++ch)
            std::memcpy(fBranch[ch].data(), work[ch], sizeof(float) * frames);
        fJobChannels = numChannels;
        fJobFrames = frames;

        // Branch B on the worker (if any) while this thread runs branch A
        const bool parallel = graph.worker && !graph.branchB.empty() && fParallel.load(std::memory_order_relaxed);
        if (parallel)
            graph.worker->start(&Engine::runBranchB, this);
        const uint32_t channelsA = processNodes(graph.branchA, work, scratch, numChannels, frames, fMidiOut[0], fBlockClick[0].data());
        graph.alignA.process(work, channelsA, frames);
        if (parallel)
            graph.worker->wait();
        else
            runBranchB(this);

        // stereo from here on, in Mono mode too, so the branches can be panned apart
        merge(channelsA, fJobChannelsOut, frames);
        numChannels = 2;
    }
    return processNodes(graph.post, work, scratch, numChannels, frames, fMidiOut[0], fBlockClick[0].data());
}

void Engine::runBranchB(void* context)
{
    Engine& engine = *static_cast<Engine*>(context);
    float* branch[2] = { engine.fBranch[0].data(), engine.fBranch[1].data() };
    float* scratch[2] = { engine.fBranchScratch[0].data(), engine.fBranchScratch[1].data() };
    engine.fJobChannelsOut = engine.processNodes(engine.fActive->branchB, branch, scratch, engine.fJobChannels,
                                                 engine.fJobFrames, engine.fMidiOut[1],
                                                 engine.fBlockClick[1].data());
    engine.fActive->alignB.process(branch, engine.fJobChannelsOut, engine.fJobFrames);
}

float Engine::mergeAlignSamples() const
{
    return static_cast<float>(std::clamp(param(kParamMergeAlign), -kMaxMergeAlignMs, kMaxMergeAlignMs) * 0.001 *
                              fSampleRate);
}

float Engine::mergeGain(int branch, int channel) const
{
    const float balance = std::clamp(param(kParamMergeBalance) / 100.0f, -1.0f, 1.0f);
    float gain = branch == kBranchA ? std::min(1.0f, 1.0f - balance) : std::min(1.0f, 1.0f + balance);
    gain *= dbToGain(param(branch == kBranchA ? kParamMergeLevelA : kParamMergeLevelB));
    if (param(branch == kBranchA ? kParamMergeInvertA : kParamMergeInvertB) >= 0.5f)
        gain = -gain;

    // Pan as a balance control: the far side is turned down, the near side stays at full level
    const float pan = std::clamp(param(branch == kBranchA ? kParamMergePanA : kParamMergePanB) / 100.0f, -1.0f, 1.0f);
    if (channel == 0 && pan > 0.0f)
        gain *= 1.0f - pan;
    else if (channel == 1 && pan < 0.0f)
        gain *= 1.0f + pan;
    return gain;
}

void Engine::merge(uint32_t channelsA, uint32_t channelsB, uint32_t frames)
{
    // a branch still mono (Mono mode) plays on both sides
    if (channelsA == 1)
        std::memcpy(fWork[1].data(), fWork[0].data(), sizeof(float) * frames);
    if (channelsB == 1)
        std::memcpy(fBranch[1].data(), fBranch[0].data(), sizeof(float) * frames);

    for (int ch = 0; ch < 2; ++ch)
    {
        fMergeGain[kBranchA][ch].setTarget(mergeGain(kBranchA, ch));
        fMergeGain[kBranchB][ch].setTarget(mergeGain(kBranchB, ch));
    }
    fMergeAlign.setTarget(mergeAlignSamples());
    for (uint32_t i = 0; i < frames; ++i)
    {
        // Align: one branch is delayed (fractionally, so it can be turned smoothly) to line up with the other.
        // A delay of 1 reads back the sample just written, so 0 ms passes the branches through exactly.
        const float align = fMergeAlign.next();
        const float delayA = 1.0f + std::max(0.0f, -align);
        const float delayB = 1.0f + std::max(0.0f, align);
        for (int ch = 0; ch < 2; ++ch)
        {
            DelayLine& lineA = fAlignLines[kBranchA][ch];
            DelayLine& lineB = fAlignLines[kBranchB][ch];
            lineA.write(fWork[ch][i]);
            lineB.write(fBranch[ch][i]);
            fWork[ch][i] = lineA.readFractional(delayA) * fMergeGain[kBranchA][ch].next() +
                           lineB.readFractional(delayB) * fMergeGain[kBranchB][ch].next();
        }
    }
}

void Engine::updateLatency(Graph& graph)
{
    for (std::vector<Node>* nodes : { &graph.pre, &graph.branchA, &graph.branchB, &graph.post })
        for (Node& node : *nodes)
            node.bypassDelay.prepare(node.block->latency());
    const uint32_t a = sectionLatency(graph.branchA);
    const uint32_t b = sectionLatency(graph.branchB);
    const uint32_t branches = std::max(a, b);
    graph.alignA.prepare(graph.split ? branches - a : 0);
    graph.alignB.prepare(graph.split ? branches - b : 0);
    graph.latency = sectionLatency(graph.pre) + branches + sectionLatency(graph.post);

    // each block's MIDI waits for the latency after it: the rest of its section, then what follows the section
    auto delays = [](std::vector<Node>& nodes, uint32_t after) {
        uint32_t later = after;
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        {
            it->midiDelay = later;
            later += it->block->latency();
        }
    };
    const uint32_t post = sectionLatency(graph.post);
    delays(graph.post, 0);
    delays(graph.branchA, post + graph.alignA.delay());
    delays(graph.branchB, post + graph.alignB.delay());
    delays(graph.pre, post + (graph.split ? branches : 0));
}

uint32_t Engine::processNodes(const std::vector<Node>& nodes, float* const* work, float* const* scratch,
                              uint32_t numChannels, uint32_t frames, MidiOutput& midi, float* click)
{
    for (const Node& node : nodes)
    {
        SlotState& slot = fSlots[node.slot];
        Block& block = *node.block;

        // a Mono chain turns stereo at a block that makes stereo (even while it's bypassed, so the layout of the
        // chain alone decides it)
        if (numChannels == 1 && node.type->stereo)
        {
            std::memcpy(work[1], work[0], sizeof(float) * frames);
            numChannels = 2;
        }

        for (int p = 0; p < node.type->numParams; ++p)
        {
            const float normalized = param(slotParam(node.slot, p));
            if (slot.applyAll || normalized != slot.applied[p])
            {
                block.setParam(p, node.type->params[p].toValue(normalized));
                slot.applied[p] = normalized;
            }
        }
        slot.applyAll = false;
        if (node.restoreLatched >= 0)
        {
            block.setLatched(static_cast<uint32_t>(node.restoreLatched));
            node.restoreLatched = -1;
        }
        block.setTempo(fTempo);
        if (fSongPlaying)
            block.setSongPosition(fSongBeat);
        const float* rig[2] = { fRigInput[0].data(), fRigInput[1].data() };
        block.setRigInput(rig, numChannels);
        midi.setNode(fChunkTime + node.midiDelay, node.slot, &fMidiSounding);
        block.setMidiOutput(&midi);
        block.setClickOutput(click);

        const float bypassTarget = param(slotBypassParam(node.slot)) >= 0.5f ? 1.0f : 0.0f;
        if (node.bypassDelay.delay() > 0)
        {
            // a block with latency: its dry signal is delayed as much, so bypassing it doesn't shift the timing
            for (uint32_t ch = 0; ch < numChannels; ++ch)
                std::memcpy(scratch[ch], work[ch], sizeof(float) * frames);
            node.bypassDelay.process(scratch, numChannels, frames);
            if (slot.bypassMix == bypassTarget && bypassTarget != 0.0f)
            {
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    std::memcpy(work[ch], scratch[ch], sizeof(float) * frames);
                midi.endSlot(node.slot, fChunkTime + node.midiDelay); // nothing hangs while it's bypassed
                continue;
            }
            block.process(work, numChannels, frames);
            if (slot.bypassMix == bypassTarget)
                continue;
            float mix = slot.bypassMix;
            for (uint32_t i = 0; i < frames; ++i)
            {
                mix = mix < bypassTarget ? std::min(bypassTarget, mix + fRampStep)
                                         : std::max(bypassTarget, mix - fRampStep);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    work[ch][i] += (scratch[ch][i] - work[ch][i]) * mix;
            }
            slot.bypassMix = mix;
            continue;
        }
        if (slot.bypassMix == bypassTarget)
        {
            if (bypassTarget == 0.0f)
                block.process(work, numChannels, frames);
            else
                midi.endSlot(node.slot, fChunkTime + node.midiDelay); // nothing hangs while it's bypassed
            continue;
        }

        // Ramping between bypassed and active: process a copy and crossfade it with the dry signal
        for (uint32_t ch = 0; ch < numChannels; ++ch)
            std::memcpy(scratch[ch], work[ch], sizeof(float) * frames);
        block.process(scratch, numChannels, frames);

        float mix = slot.bypassMix;
        for (uint32_t i = 0; i < frames; ++i)
        {
            mix = mix < bypassTarget ? std::min(bypassTarget, mix + fRampStep)
                                     : std::max(bypassTarget, mix - fRampStep);
            for (uint32_t ch = 0; ch < numChannels; ++ch)
                work[ch][i] = scratch[ch][i] + (work[ch][i] - scratch[ch][i]) * mix;
        }
        slot.bypassMix = mix;
    }
    return numChannels;
}

void Engine::swapGraph()
{
    Graph* old = fActive;
    fActive = fNext;
    fNext = nullptr;

    // a block that is gone (or rebuilt) ends its notes
    std::array<bool, kMaxBlocks> kept {};
    fActive->forEachNode([&](const Node& node) {
        kept[static_cast<size_t>(node.slot)] = fSlots[node.slot].block == node.block.get();
    });
    for (int slot = 0; slot < kMaxBlocks; ++slot)
        if (!kept[static_cast<size_t>(slot)])
            fMidiOut[0].endSlot(slot, fChunkTime);

    fActive->forEachNode([this](const Node& node) {
        SlotState& slot = fSlots[node.slot];
        if (slot.block != node.block.get())
        {
            // A new block in this slot: apply all its parameters and start at its bypass state, no ramp
            slot.block = node.block.get();
            slot.applyAll = true;
            slot.bypassMix = param(slotBypassParam(node.slot)) >= 0.5f ? 1.0f : 0.0f;
        }
    });

    fLatency.store(fActive->latency, std::memory_order_relaxed);

    // Freed on a non-RT thread. The queue can't fill up in practice (each swap follows one setChain(), which
    // drains it); if it ever does, leaking is better than freeing on the audio thread.
    fRetired.push(old);
}

void Engine::collectMidi(const MidiOutput& midi)
{
    // into the queue, in time order (each block's messages are already in order; blocks interleave)
    for (int i = 0; i < midi.count() && fMidiQueued < kMidiQueue; ++i)
    {
        const MidiMessage& m = midi.at(i);
        int k = fMidiQueued++;
        while (k > 0 && fMidiQueue[static_cast<size_t>(k - 1)].time > m.time)
        {
            fMidiQueue[static_cast<size_t>(k)] = fMidiQueue[static_cast<size_t>(k - 1)];
            --k;
        }
        fMidiQueue[static_cast<size_t>(k)] = m;
    }
}

void Engine::drainRetired()
{
    Graph* graph;
    while (fRetired.pop(graph))
        delete graph;
}

} // namespace xythos
