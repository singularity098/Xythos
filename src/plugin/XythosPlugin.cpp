// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "DistrhoPlugin.hpp"

#include "engine/Engine.hpp"
#include "engine/BlockRegistry.hpp"
#include "state/AudioFile.hpp"
#include "state/ChainExchange.hpp"
#include "state/ChainMigration.hpp"
#include "state/Library.hpp"
#include "state/Paths.hpp"
#include "state/Preset.hpp"
#include "state/Settings.hpp"

#include <algorithm>
#include <array>
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
            || index == kParamMidiLastValue || index == kParamMidiLastProgram || index == kParamMidiProgramCount
            || index == kParamMidiProgramValue || index == kParamPresetProgram)
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
        // MIDI CCs and Program Changes (learn and mapped controls), applied at the start of the buffer
        for (uint32_t i = 0; i < midiEventCount; ++i)
        {
            const MidiEvent& event = midiEvents[i];
            const uint8_t status = event.data[0] & 0xf0;
            if (event.size == 3 && status == 0xb0)
            {
                const Engine::CcResult result = fEngine.midiCc(event.data[0] & 0x0f, event.data[1], event.data[2]);
                if (result.param >= 0)
                    requestParameterValueChange(static_cast<uint32_t>(result.param), result.value);
            }
            else if (event.size >= 2 && status == 0xc0)
            {
                const Engine::ProgramResult result = fEngine.midiProgram(event.data[0] & 0x0f, event.data[1] & 0x7f);
                if (result.param >= 0)
                    requestParameterValueChange(static_cast<uint32_t>(result.param), result.value);
                else if (!result.mapped)
                    fPendingProgram.store(event.data[1] & 0x7f, std::memory_order_relaxed); // a preset's, maybe
            }
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

        // the values a preset loaded for a Program Change set, for the host (requests only work from here)
        if (fNotifyPending.exchange(false, std::memory_order_acquire))
        {
            int sent = 0;
            for (uint32_t index = 0; index < kParamCount; ++index)
            {
                if (!fNotifyHost[index].load(std::memory_order_relaxed))
                    continue;
                if (sent == kMaxNotifyPerRun)
                {
                    fNotifyPending.store(true, std::memory_order_relaxed); // the rest next time
                    break;
                }
                fNotifyHost[index].store(false, std::memory_order_relaxed);
                requestParameterValueChange(index, fEngine.getParameter(index));
                ++sent;
            }
        }

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

    // Whether a library file exists (for a file inside a zip: whether the zip does)
    static bool libraryFileExists(const std::string& path)
    {
        std::error_code ec;
        const std::optional<ArchivePath> archive = splitArchivePath(path);
        return std::filesystem::exists(pathFromUtf8(archive ? archive->archive : path), ec);
    }

    // Non-RT. Sets a parameter for a preset being loaded, and has run() tell the host
    void setLoadedValue(uint32_t index, float value)
    {
        fEngine.setParameter(index, value);
        fNotifyHost[index].store(true, std::memory_order_relaxed);
    }

    // Non-RT (the exchange thread). A Program Change no MIDI mapping took: loads the preset assigned to it
    // (Settings::programPresets), the way the UI opens one, so a footswitch changes presets with the window closed.
    // An open UI sees Preset Program change and takes the chain from the exchange file.
    void loadProgramPreset(int program)
    {
        const Settings settings = Settings::load();
        const auto assigned = settings.programPresets.find(program);
        if (assigned == settings.programPresets.end())
            return;
        const std::string& path = assigned->second;

        // a file that moved is found by its content, as the UI does, in the libraries' last scans
        LibraryIndex nam, ir;
        nam.load(LibraryIndex::defaultPath(LibraryKind::Nam));
        ir.load(LibraryIndex::defaultPath(LibraryKind::Ir));
        std::string error;
        std::optional<LoadedPreset> loaded = loadPresetFile(path, [&](const std::string& hash) {
            for (const LibraryIndex* index : { &nam, &ir })
            {
                if (const LibraryEntry* entry = index->findByHash(hash); entry && libraryFileExists(entry->path))
                    return entry->path;
            }
            return std::string();
        }, &error);
        if (!loaded || !loaded->preset.chain.validate(BlockRegistry::builtin(), &error))
        {
            d_stderr2("Xythos: couldn't load the preset for program %d: %s", program, error.c_str());
            return;
        }

        ChainState chain = std::move(loaded->preset.chain);
        retireLoopers(chain); // a preset's Looper block: the global looper keeps its own settings
        migrateBlocks(chain);
        for (uint32_t index : presetGlobalParams())
        {
            // values a preset doesn't have go back to their defaults
            const GlobalParamSpec& spec = globalParamSpec(index);
            const auto it = loaded->preset.globals.find(spec.symbol);
            setLoadedValue(index, it != loaded->preset.globals.end() ? it->second : spec.def);
        }
        for (const BlockState& block : chain.blocks)
        {
            setLoadedValue(slotBypassParam(block.slot), block.bypass ? 1.0f : 0.0f);
            for (int p = 0; p < kParamsPerSlot; ++p)
                setLoadedValue(slotParam(block.slot, p), block.params[static_cast<size_t>(p)]);
        }
        fNotifyPending.store(true, std::memory_order_release);
        chain.restoreLatches = true;
        applyChain(chain.toJson().c_str());

        fPresetLoads = (fPresetLoads + 1) % (kMaxExchangeValue / 128);
        fEngine.setParameter(kParamPresetProgram, static_cast<float>(fPresetLoads * 128 + static_cast<uint32_t>(program)));
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
            // a footswitch's Program Change for a preset
            if (const int program = fPendingProgram.exchange(-1, std::memory_order_relaxed); program >= 0)
                loadProgramPreset(program);
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

    std::atomic<int> fPendingProgram { -1 }; // a Program Change for the exchange thread (a preset's), -1 for none
    uint32_t fPresetLoads = 0;               // presets loaded for Program Changes (exchange thread)
    // the parameters a preset loaded for a Program Change set, for run() to tell the host about
    static constexpr int kMaxNotifyPerRun = 64; // hosts' queues of parameter changes are finite
    std::array<std::atomic<bool>, kParamCount> fNotifyHost {};
    std::atomic<bool> fNotifyPending { false };

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(XythosPlugin)
};

Plugin* createPlugin()
{
    return new XythosPlugin();
}

END_NAMESPACE_DISTRHO
