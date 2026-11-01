// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/BlockRegistry.hpp"
#include "engine/MidiOutput.hpp"
#include "engine/Params.hpp"
#include "engine/ParallelWorker.hpp"
#include "engine/SpscQueue.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/Limiter.hpp"
#include "engine/dsp/Meter.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/ToneStack.hpp"
#include "engine/looper/Looper.hpp"
#include "engine/metronome/Metronome.hpp"
#include "engine/tuner/Tuner.hpp"
#include "state/ChainState.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xythos {

// The audio engine: input stage, signal chain (with an optional split), tone stack, output stage, looper, metronome,
// limiter and meters, and the tuner's input.
//
// Threads: process() runs on the audio thread and never allocates, locks or frees. setChain() and prepare()
// run on other threads; a new chain is built there and handed to the audio thread lock-free, which swaps it in
// with a short fade out/in. Blocks that didn't change are reused, so their state carries over.
class Engine {
public:
    static constexpr double kSwapFadeSeconds = 0.005;   // each of fade-out and fade-in
    static constexpr double kBypassRampSeconds = 0.005;

    explicit Engine(const BlockRegistry& registry = BlockRegistry::builtin());
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Non-RT, audio stopped
    void prepare(double sampleRate, uint32_t maxFrames);

    // Non-RT. Builds the chain and queues it for the audio thread. Blocks whose configure() fails are left out
    // (audio passes through them); their slots are listed in `failedSlots`.
    bool setChain(const ChainState& chain, std::string* error = nullptr, std::vector<int>* failedSlots = nullptr);

    // Any thread. True once after the chain's output went non-finite (NaN or infinity): a block's state is broken
    // (a NaN never leaves recursive filters by itself). The engine outputs silence for it meanwhile; the caller
    // rebuilds the chain with fresh blocks (forgetBlocks(), then setChain()).
    bool takeChainFault() { return fChainFault.exchange(false, std::memory_order_acq_rel); }

    // Non-RT. The next setChain() creates every block anew instead of reusing unchanged ones.
    void forgetBlocks();

    // Any thread. Index from Params.hpp; slot parameters are normalized 0..1.
    void setParameter(uint32_t index, float value);
    float getParameter(uint32_t index) const;

    // RT. The host tempo for tempo-synced blocks; call before process() when the host reports one.
    void setTempo(double bpm) { fTempo = bpm > 0.0 ? bpm : 120.0; }

    // RT. Applies a MIDI CC through the active chain's MIDI map, and reports it for MIDI learn. Returns the host
    // parameter it changed (with its new value), or -1.
    struct CcResult {
        int param = -1;
        float value = 0.0f;
    };
    CcResult midiCc(int channel, int cc, int value);

    // RT. Two inputs and two outputs; they may alias. `transport` is the host's, for the metronome's DAW sync.
    void process(const float* const* inputs, float* const* outputs, uint32_t frames,
                 const Transport* transport = nullptr);

    // Total latency of the active chain, in samples
    uint32_t latency() const { return fLatency.load(std::memory_order_relaxed); }

    // RT. MIDI the blocks sent (MIDI Out), timed in samples since prepare(): midiTime() is the time of the next
    // process() call's first sample. After process(), drainMidi() hands over (in time order) every message before
    // `end`; later ones wait for their buffer (a block's MIDI is delayed by the latency of the blocks after it).
    int64_t midiTime() const { return fTime; }

    template <typename F>
    void drainMidi(int64_t end, F&& f)
    {
        int n = 0;
        while (n < fMidiQueued && fMidiQueue[static_cast<size_t>(n)].time < end)
            f(fMidiQueue[static_cast<size_t>(n++)]);
        for (int k = n; k < fMidiQueued; ++k)
            fMidiQueue[static_cast<size_t>(k - n)] = fMidiQueue[static_cast<size_t>(k)];
        fMidiQueued -= n;
    }

    // Non-RT, any one thread. Runs the tuner's analysis on the latest input and publishes the result in the
    // tuner output parameters. Call it every 20 ms or so while the tuner is on.
    void analyzeTuner();

    // Non-RT, call it now and then (the plugin does from its exchange thread). Gives the looper its loop's memory
    // once it's wanted in this run: the UI pulsed Looper Wake (it opened the looper), or one of its footswitches was
    // pressed (a MIDI footswitch). Until then the looper passes audio through. From then on it keeps a lane ready for
    // the next pass (LooperMemory::service(): 23 MB for a first recording, the loop's length for each pass after it)
    // and zeroes the passes deleted. Returns whether it has the memory.
    bool serviceLooper();

    // Non-RT. The loop as a float WAV (encodeWav()), or "" if there's none to save: no memory, empty, or being
    // recorded (it's only read while it plays or is stopped, when the audio thread doesn't write it)
    std::string looperWav() const;

    // Any thread. Whether branch B of a split runs on a second core (default on). Takes effect from the next
    // setChain() if no worker thread was started yet.
    void setParallelProcessing(bool enabled) { fParallel.store(enabled, std::memory_order_relaxed); }

private:
    struct Node {
        std::shared_ptr<Block> block;
        const BlockDescriptor* type;
        int slot;
        // for a block with latency: delays the signal while it's bypassed, so the chain's latency stays the same
        mutable AlignmentDelay bypassDelay;
        // the latency of the blocks after it: its MIDI is delayed as much, to stay with its audio
        uint32_t midiDelay = 0;
        // a kept block's latched footswitches from a preset being loaded, set on the audio thread; -1 for none
        mutable int32_t restoreLatched = -1;
    };

    // The chain in four sections: before the split, the two branches, and after the merge. Without a split,
    // everything is in `pre`.
    struct Graph {
        std::vector<Node> pre, branchA, branchB, post;
        bool split = false;
        uint32_t latency = 0;
        AlignmentDelay alignA, alignB;        // delays the branch with less latency so both arrive together
        ParallelWorker* worker = nullptr; // runs branch B, if parallel processing is on
        std::array<int16_t, 16 * 128> midiMap; // channel * 128 + cc -> host parameter, -1 if unmapped

        Graph() { midiMap.fill(-1); }

        template <typename F>
        void forEachNode(F&& f) const
        {
            for (const std::vector<Node>* nodes : { &pre, &branchA, &branchB, &post })
                for (const Node& node : *nodes)
                    f(node);
        }
    };

    // What the non-RT side last built for each slot, so unchanged blocks can be reused
    struct CachedBlock {
        std::shared_ptr<Block> block;
        std::string typeId;
        std::string config;
    };

    // Audio-thread state per slot; slots outlive graphs
    struct SlotState {
        const Block* block = nullptr;
        std::array<float, kParamsPerSlot> applied {};
        bool applyAll = true;   // push every parameter on the next buffer (new block in this slot)
        float bypassMix = 0.0f; // 0 = active, 1 = bypassed
    };

    enum class Fade { Idle, Out, In };

    void processChunk(const float* const* inputs, float* const* outputs, uint32_t frames,
                      const Transport* transport, uint32_t offset);
    Metronome::Settings metronomeSettings() const;
    // Both return the channel count after them: a Mono chain turns stereo at a stereo block and at the merge
    uint32_t processChain(uint32_t numChannels, uint32_t frames);
    uint32_t processNodes(const std::vector<Node>& nodes, float* const* work, float* const* scratch,
                          uint32_t numChannels, uint32_t frames, MidiOutput& midi, float* click);
    void collectMidi(const MidiOutput& midi);
    void merge(uint32_t channelsA, uint32_t channelsB, uint32_t frames);
    float mergeGain(int branch, int channel) const;
    float mergeAlignSamples() const;
    static void runBranchB(void* engine);
    void updateLatency(Graph& graph);
    void swapGraph();
    void drainRetired();
    // RT. Replaces NaN and infinite samples with silence; returns whether there were any
    static bool silenceNonFinite(float* const* io, uint32_t numChannels, uint32_t frames);
    float param(uint32_t index) const { return fParams[index].load(std::memory_order_relaxed); }
    void processLooper(float* const* work, uint32_t& numChannels, uint32_t frames);

    const BlockRegistry& fRegistry;
    std::mutex fMutex; // non-RT callers only
    std::mutex fTunerMutex; // analyzeTuner() vs prepare()

    double fSampleRate = 48000.0;
    uint32_t fMaxFrames = 0;

    std::array<std::atomic<float>, kParamCount> fParams;

    // non-RT side
    std::array<CachedBlock, kMaxBlocks> fCache;

    // handover
    std::atomic<Graph*> fPending { nullptr };
    SpscQueue<Graph*, 8> fRetired;
    std::atomic<uint32_t> fLatency { 0 };
    std::atomic<bool> fParallel { true };
    std::atomic<bool> fChainFault { false };
    std::unique_ptr<ParallelWorker> fWorker; // started when a chain first needs it

    // audio thread side
    static constexpr int kMidiQueue = 2048;
    MidiOutput fMidiOut[2];               // what the blocks send in a chunk: the main thread's, and branch B's
    MidiSounding fMidiSounding {};        // the notes each slot has sounding
    std::array<MidiMessage, kMidiQueue> fMidiQueue {}; // waiting for the host, in time order
    int fMidiQueued = 0;
    int64_t fTime = 0, fChunkTime = 0;    // samples since prepare(): the next buffer's start, the chunk's
    bool fMidiEndAll = false;             // end every sounding note (after prepare())
    Graph* fActive = nullptr;
    Graph* fNext = nullptr;
    Fade fFade = Fade::Idle;
    float fFadeGain = 1.0f;
    float fFadeStep = 1.0f;
    float fRampStep = 1.0f;
    float fGlobalBypassMix = 0.0f;
    double fTempo = 120.0;
    bool fSongPlaying = false; // the host transport plays; fSongBeat is the position at the current chunk's start
    double fSongBeat = 0.0;
    uint32_t fCcCount = 0;
    float fTunerMuteMix = 0.0f; // 1 = guitar muted by the tuner
    bool fTunerOn = false;
    std::array<SlotState, kMaxBlocks> fSlots {};

    Smoother fInputGain, fOutputGain;
    Smoother fMergeGain[2][2]; // [branch][channel]
    Smoother fMergeAlign;      // samples; > 0 delays branch B, < 0 branch A
    DelayLine fAlignLines[2][2]; // [branch][channel]
    uint32_t fJobChannels = 0, fJobFrames = 0; // branch B job, for the worker
    uint32_t fJobChannelsOut = 0;              // branch B's channels after it
    ToneStack fToneStack;
    Limiter fLimiter;
    Tuner fTuner;
    Metronome fMetronome;
    // The global looper (owner, 2026-11-01): after the output level, so it records what's heard and plays on whatever
    // the chain becomes; its clicks join the metronome's. fLooperApplied holds the values it last took.
    Looper fLooper;
    std::array<float, kLooperParamCount> fLooperApplied {};
    bool fLooperApplyAll = true;
    // its loop's memory, kept by serviceLooper() and handed over through fLooperMemoryReady
    LooperMemory fLooperMemory;
    std::atomic<bool> fLooperMemoryReady { false };
    float fLooperDeleteSeen = 0.0f, fLooperStartSeen = 0.0f; // the page's last requests (audio thread)
    std::atomic<bool> fLooperWanted { false }; // a footswitch was pressed before the memory came
    bool fLooperAttached = false;              // audio thread
    std::atomic<uint32_t> fLooperLength { 0 }; // the loop's length and state, for looperWav()
    std::atomic<int> fLooperState { 0 };
    Meter fInMeters[2], fOutMeters[2];

    std::vector<float> fWork[2], fDry[2], fScratch[2];
    std::vector<float> fRigInput[2]; // the chain input, for blocks keyed from it (Block::setRigInput)
    std::vector<float> fBranch[2], fBranchScratch[2]; // branch B of a split
    std::vector<float> fClick;
    std::vector<float> fBlockClick[2]; // clicks blocks send past the chain (the looper's): the main thread's, branch B's
};

} // namespace xythos
