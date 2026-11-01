// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "DistrhoPlugin.hpp"

#include "engine/Engine.hpp"
#include "state/AudioFile.hpp"
#include "state/ChainExchange.hpp"
#include "state/Paths.hpp"
#include "state/Settings.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>

START_NAMESPACE_DISTRHO

using namespace xythos;

// Plugin state keys
static constexpr const char* kStateChain = "chain";

enum StateIndex : uint32_t {
    kStateIndexChain,
    kStateCount
};

class XythosPlugin : public Plugin
{
public:
    XythosPlugin()
        : Plugin(kParamCount, 0, kStateCount)
    {
        fEngine.prepare(getSampleRate(), getBufferSize());

        std::random_device random;
        fInstanceId = 1 + random() % kMaxExchangeValue;
        fEngine.setParameter(kParamInstanceId, static_cast<float>(fInstanceId));
        fExchangeThread = std::thread([this] { watchExchange(); });
        fTunerThread = std::thread([this] { runTuner(); });
    }

    ~XythosPlugin() override
    {
        fStopExchange = true;
        if (fExchangeThread.joinable())
            fExchangeThread.join();
        if (fTunerThread.joinable())
            fTunerThread.join();
        std::remove(chainExchangePath(fInstanceId).c_str());
    }

protected:
    const char* getLabel() const override { return "Xythos"; }
    const char* getDescription() const override { return "Neural Amp Modeler guitar rig"; }
    const char* getMaker() const override { return DISTRHO_PLUGIN_BRAND; }
    const char* getHomePage() const override { return DISTRHO_PLUGIN_URI; }
    const char* getLicense() const override { return "MIT"; }
    uint32_t getVersion() const override
    {
        return d_version(XYTHOS_VERSION_MAJOR, XYTHOS_VERSION_MINOR, XYTHOS_VERSION_PATCH);
    }

    void initAudioPort(const bool input, const uint32_t index, AudioPort& port) override
    {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
    }

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        if (!isSlotParam(index))
        {
            const GlobalParamSpec& spec = globalParamSpec(index);
            parameter.name = spec.name;
            parameter.symbol = spec.symbol;
            parameter.unit = spec.unit;
            parameter.ranges.min = spec.min;
            parameter.ranges.max = spec.max;
            parameter.ranges.def = spec.def;
            parameter.hints = (spec.flags & kParamFlagOutput) ? kParameterIsOutput : kParameterIsAutomatable;
            if (spec.flags & kParamFlagToggle)
                parameter.hints |= kParameterIsBoolean | kParameterIsInteger;
            if (spec.flags & kParamFlagInteger)
                parameter.hints |= kParameterIsInteger;
            if (spec.flags & kParamFlagHidden)
                parameter.hints = (parameter.hints & ~kParameterIsAutomatable) | kParameterIsHidden;
            if (index == kParamBypass)
                parameter.designation = kParameterDesignationBypass;
            return;
        }

        // Slot parameters: generic names, because the block in a slot changes (docs/Architecture.md 4.3)
        const uint32_t offset = index - kSlotParamBase;
        const int slot = static_cast<int>(offset / kSlotParamCount);
        const int param = static_cast<int>(offset % kSlotParamCount);
        char name[32];
        char symbol[32];
        if (param == 0)
        {
            std::snprintf(name, sizeof(name), "Slot %02d Bypass", slot + 1);
            std::snprintf(symbol, sizeof(symbol), "s%02d_bypass", slot + 1);
            parameter.hints = kParameterIsAutomatable | kParameterIsBoolean | kParameterIsInteger;
        }
        else
        {
            std::snprintf(name, sizeof(name), "Slot %02d P%d", slot + 1, param);
            std::snprintf(symbol, sizeof(symbol), "s%02d_p%d", slot + 1, param);
            parameter.hints = kParameterIsAutomatable;
        }
        parameter.name = name;
        parameter.symbol = symbol;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1.0f;
        parameter.ranges.def = 0.0f;
    }

    void initState(const uint32_t index, State& state) override
    {
        switch (index)
        {
        case kStateIndexChain:
            state.key = kStateChain;
            state.defaultValue = "";
            // Not DSP-only: the UI edits the chain and needs it back when it opens
            state.hints = 0;
            break;
        }
    }

    float getParameterValue(const uint32_t index) const override
    {
        return fEngine.getParameter(index);
    }

    void setParameterValue(const uint32_t index, const float value) override
    {
        if (index == kParamInstanceId || index == kParamMidiLastCc || index == kParamMidiCcCount
            || index == kParamMidiLastValue)
            return; // outputs, owned by the plugin
        fEngine.setParameter(index, value);
    }

    void setState(const char* const key, const char* const value) override
    {
        if (std::strcmp(key, kStateChain) == 0)
            applyChain(value);
    }

    String getState(const char* const key) const override
    {
        if (std::strcmp(key, kStateChain) != 0)
            return String();
        std::string json;
        {
            const std::lock_guard<std::mutex> lock(fChainMutex);
            json = fChainJson;
        }
        return String(withLiveLatches(json).c_str());
    }

    // Hosts may call this on any thread, and DPF already counts the plugin as active while it runs, so the audio
    // thread can call run() meanwhile (DPF's VST3 process() also activates the plugin itself). The two never
    // overlap: this waits for a run() in progress, and run() outputs silence while this prepares.
    void activate() override
    {
        const std::lock_guard<std::mutex> lock(fActivateMutex);
        fPreparing.store(true);
        while (fRunning.load())
            std::this_thread::yield();
        fEngine.prepare(getSampleRate(), getBufferSize());
        fPreparing.store(false);
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames,
             const MidiEvent* const midiEvents, const uint32_t midiEventCount) override
    {
        // the handshake with activate(): announce the run, then check (both sequentially consistent, so one of the
        // two always sees the other)
        fRunning.store(true);
        if (fPreparing.load())
        {
            fRunning.store(false);
            for (int ch = 0; ch < 2; ++ch)
                std::memset(outputs[ch], 0, sizeof(float) * frames);
            return;
        }
        runEngine(inputs, outputs, frames, midiEvents, midiEventCount);
        fRunning.store(false);
    }

private:
    void runEngine(const float** const inputs, float** const outputs, const uint32_t frames,
                   const MidiEvent* const midiEvents, const uint32_t midiEventCount)
    {
        // MIDI CCs (learn and mapped controls), applied at the start of the buffer
        for (uint32_t i = 0; i < midiEventCount; ++i)
        {
            const MidiEvent& event = midiEvents[i];
            if (event.size != 3 || (event.data[0] & 0xf0) != 0xb0)
                continue;
            const Engine::CcResult result = fEngine.midiCc(event.data[0] & 0x0f, event.data[1], event.data[2]);
            if (result.param >= 0)
                requestParameterValueChange(static_cast<uint32_t>(result.param), result.value);
        }

        // The host's transport: tempo for tempo-synced blocks, and the metronome's DAW sync
        const TimePosition& time = getTimePosition();
        if (time.bbt.valid)
            fEngine.setTempo(time.bbt.beatsPerMinute);
        Transport transport;
        transport.valid = time.bbt.valid && time.bbt.ticksPerBeat > 0.0;
        transport.playing = time.playing;
        if (transport.valid)
        {
            transport.bpm = time.bbt.beatsPerMinute;
            transport.beatsPerBar = static_cast<int>(std::lround(time.bbt.beatsPerBar));
            transport.beat = (time.bbt.bar - 1) * static_cast<double>(time.bbt.beatsPerBar) + (time.bbt.beat - 1) +
                             time.bbt.tick / time.bbt.ticksPerBeat;
        }

        const int64_t start = fEngine.midiTime();
        fEngine.process(inputs, outputs, frames, &transport);

        // the MIDI Out block's notes, at their frames in this buffer
        fEngine.drainMidi(start + frames, [&](const MidiMessage& message) {
            MidiEvent event {};
            event.frame = static_cast<uint32_t>(std::clamp<int64_t>(message.time - start, 0, frames - 1));
            event.size = message.size;
            std::memcpy(event.data, message.data, message.size);
            writeMidiEvent(event);
        });

        const uint32_t latency = fEngine.latency();
        if (latency != fReportedLatency)
        {
            fReportedLatency = latency;
            setLatency(latency);
        }
    }

    // Non-RT. Builds the chain and keeps its JSON for getState() and the exchange file.
    void applyChain(const char* const json)
    {
        // Settings > Parallel processing (the UI resends the chain when it changes)
        const bool parallel = Settings::load().parallel;
        {
            // the same chain often arrives twice (state message and exchange file)
            const std::lock_guard<std::mutex> lock(fChainMutex);
            if (fChainJson == json && fParallel == parallel)
                return;
        }
        fEngine.setParallelProcessing(parallel);
        std::string error;
        const std::optional<ChainState> chain = ChainState::fromJson(json, &error);
        if (!chain || !fEngine.setChain(*chain, &error))
        {
            d_stderr2("Xythos: ignoring invalid chain state: %s", error.c_str());
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(fChainMutex);
            fChainJson = json;
            fParallel = parallel;
        }
        writeChainExchange(fInstanceId, json);
    }

    // Non-RT. The chain with the footswitches latched now, which only the blocks know (Block::latched()), so a
    // saved project keeps them
    std::string withLiveLatches(const std::string& json) const
    {
        if (json.empty())
            return json;
        std::optional<ChainState> chain = ChainState::fromJson(json);
        if (!chain)
            return json;
        chain->restoreLatches = false;
        for (BlockState& block : chain->blocks)
        {
            if (block.slot >= 0 && block.slot < kMaxBlocks)
                block.latched = static_cast<uint32_t>(fEngine.getParameter(slotLatchParam(block.slot)));
        }
        return chain->toJson();
    }

    // Non-RT. Builds the current chain again with fresh blocks, after one broke (Engine::takeChainFault())
    void rebuildChain()
    {
        std::string json;
        {
            const std::lock_guard<std::mutex> lock(fChainMutex);
            json = fChainJson;
        }
        if (json.empty())
            return;
        d_stderr2("Xythos: a block's output went NaN; rebuilding the chain");
        fEngine.forgetBlocks();
        if (const std::optional<ChainState> chain = ChainState::fromJson(withLiveLatches(json)))
            fEngine.setChain(*chain);
        // the broken blocks play until the new ones swap in: their reports are old news
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fEngine.takeChainFault();
    }

    // Picks up chains the UI sent through the exchange file (see state/ChainExchange.hpp), and rebuilds the chain
    // when a block broke
    void watchExchange()
    {
        uint32_t seenRevision = 0;
        uint32_t seenExport = 0;
        auto lastRebuild = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        while (!fStopExchange)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            // the looper's memory, the first time it's wanted (not reserved in instances that never use it)
            fEngine.serviceLooper();
            // the UI asks to save the loop: it goes to a file the UI then encodes as the user chose
            const auto exportRequest = static_cast<uint32_t>(fEngine.getParameter(kParamLooperExport));
            if (exportRequest != seenExport)
            {
                seenExport = exportRequest;
                const std::string wav = fEngine.looperWav();
                std::error_code error;
                std::filesystem::create_directories(cacheDir(), error);
                const bool ok = !wav.empty() && writeFileAtomic(looperExportFile(fInstanceId), wav);
                fEngine.setParameter(kParamLooperExported, static_cast<float>(exportRequest * 2 + (ok ? 1 : 0)));
            }
            // at most once a second, in case a block keeps breaking
            const auto now = std::chrono::steady_clock::now();
            if (now - lastRebuild >= std::chrono::seconds(1) && fEngine.takeChainFault())
            {
                lastRebuild = now;
                rebuildChain();
            }
            const auto revision = static_cast<uint32_t>(fEngine.getParameter(kParamChainRevision));
            if (revision == seenRevision)
                continue;
            seenRevision = revision;
            if (const std::optional<std::string> json = readChainExchange(fInstanceId))
                applyChain(json->c_str());
        }
    }

    // The tuner's pitch analysis, off the audio thread (docs/Architecture.md 4.9)
    void runTuner()
    {
        while (!fStopExchange)
        {
            const bool on = fEngine.getParameter(kParamTuner) >= 0.5f;
            if (on || fEngine.getParameter(kParamTunerFrequency) != 0.0f)
                fEngine.analyzeTuner();
            std::this_thread::sleep_for(std::chrono::milliseconds(on ? 20 : 50));
        }
    }

    Engine fEngine;
    uint32_t fReportedLatency = 0;
    std::mutex fActivateMutex;               // activate() calls from two threads at once
    std::atomic<bool> fPreparing { false };  // activate() is preparing the engine
    std::atomic<bool> fRunning { false };    // run() is processing

    uint32_t fInstanceId = 0;
    mutable std::mutex fChainMutex;
    std::string fChainJson;
    bool fParallel = true; // the setting the current chain was built with
    std::atomic<bool> fStopExchange { false }; // stops both threads
    std::thread fExchangeThread;
    std::thread fTunerThread;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(XythosPlugin)
};

Plugin* createPlugin()
{
    return new XythosPlugin();
}

END_NAMESPACE_DISTRHO
