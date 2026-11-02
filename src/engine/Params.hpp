// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <cstdint>

namespace xythos {

constexpr int kMaxBlocks = 32;
constexpr int kParamsPerSlot = 16;
constexpr int kSlotParamCount = 1 + kParamsPerSlot; // bypass + block parameters

// Host parameter indices. These are the DAW's automation IDs, so existing indices must never move:
// parameters added in later milestones go after the slots (see kFirstLaterParam).
enum GlobalParam : uint32_t {
    kParamInputMode,        // 0 = mono, 1 = stereo
    kParamInput,            // dB
    kParamBass,             // dB
    kParamMiddle,           // dB
    kParamTreble,           // dB
    kParamOutput,           // dB
    kParamBypass,           // global bypass
    kParamLimiter,          // output limiter on/off

    // outputs (DSP -> UI)
    kParamInPeakL,          // dBFS
    kParamInPeakR,
    kParamOutPeakL,
    kParamOutPeakR,
    kParamInClip,           // 1 while the input has recently clipped
    kParamOutClip,
    kParamLimiterReduction, // dB of gain reduction (positive)

    kGlobalParamCount
};

constexpr uint32_t kSlotParamBase = kGlobalParamCount;
constexpr uint32_t kFirstLaterParam = kSlotParamBase + kMaxBlocks * kSlotParamCount;

// Parameters added after the slot block. Append only.
enum LaterParam : uint32_t {
    // outputs: which slots have a block running in the audio engine, as two 16-bit masks (floats hold these exactly)
    kParamRunningSlotsLow = kFirstLaterParam,
    kParamRunningSlotsHigh,

    // chain exchange for hosts that don't deliver UI state (see state/ChainExchange.hpp)
    kParamInstanceId,       // output: this plugin instance's random ID
    kParamChainRevision,    // input, hidden: bumped by the UI after writing the exchange file

    // outputs: one meter per slot, for blocks that show a level (Block::meter(), e.g. compressor gain reduction)
    kParamSlotMeterBase,
    kParamSlotMeterEnd = kParamSlotMeterBase + kMaxBlocks,

    // MIDI learn: the UI asks for learning by setting kParamMidiLearn to 1; the DSP reports every CC it receives
    // as channel * 128 + cc in kParamMidiLastCc (value in kParamMidiLastValue) and bumps kParamMidiCcCount, so the
    // UI can map it, and can pass mapped values on to the host in formats where the DSP can't
    kParamMidiLearn = kParamSlotMeterEnd, // input, hidden
    kParamMidiLastCc,                     // output, hidden
    kParamMidiCcCount,                    // output, hidden
    kParamMidiLastValue,                  // output, hidden: 0..127

    // merge of the split (M6)
    kParamMergeBalance,     // -100 (only A) .. 100 (only B) %; both at full level in the middle
    kParamMergeLevelA,      // dB
    kParamMergeLevelB,
    kParamMergePanA,        // -100 (left) .. 100 (right) %; the chain is stereo after a merge, in Mono mode too
    kParamMergePanB,
    kParamMergeInvertA,     // phase invert
    kParamMergeInvertB,

    // tuner (M8)
    kParamTuner,            // on/off
    kParamTunerMute,        // mute the guitar while the tuner is on
    kParamTunerReference,   // A4 in Hz
    kParamTunerStrobe,      // hidden, UI only: the strobe view (Aurora) instead of the needle (Arc)
    kParamTunerFrequency,   // output: detected frequency in Hz, 0 when there's no clear note
    kParamTunerClarity,     // output: 0..1

    // metronome (M9). Tempo, volume, on/off and sync are automatable; the rest are hidden settings.
    kParamMetronome,        // on/off (start/stop, or armed to follow the DAW's transport)
    kParamMetroTempo,       // BPM
    kParamMetroVolume,      // %
    kParamMetroSync,        // follow the DAW's tempo, meter and transport
    kParamMetroBeats,       // beats per bar, 1..16
    kParamMetroNoteValue,   // 0..3: the beat is a half, quarter, eighth or sixteenth note
    kParamMetroGrouping,    // UI only: bit i set = a group starts at beat i (0 = no grouping)
    kParamMetroSubdivision, // MetroSubdivision
    kParamMetroSound,       // 0 Classic, 1 Digital, 2 Beep
    kParamMetroSubVolume,   // %, relative to the volume
    kParamMetroTrainer,     // 0 off, 1 step, 2 gradual
    kParamMetroTrainerStart, // BPM
    kParamMetroTrainerEnd,   // BPM
    kParamMetroTrainerStep,  // BPM per step
    kParamMetroTrainerEvery, // bars per step
    kParamMetroTrainerBars,  // bars for the gradual ramp
    kParamMetroGap,         // gap trainer on/off
    kParamMetroGapPlay,     // bars
    kParamMetroGapMute,     // bars
    kParamMetroAccent1,     // 16 beats: 0 mute, 1 soft, 2 normal, 3 accent
    kParamMetroAccent16 = kParamMetroAccent1 + 15,
    kParamMetroBeat,        // output: beat being heard (0-based), -1 when stopped
    kParamMetroBeatCount,   // output: counts beats, so the UI can pulse on every one
    kParamMetroBar,         // output: bar (0-based)
    kParamMetroLiveTempo,   // output: tempo being played (the trainer changes it; the DAW's in sync mode)
    kParamMetroSilent,      // output: 1 while the gap trainer mutes the click

    kParamMergeAlign,       // ms: > 0 delays branch B, < 0 delays branch A, to line up two amps' timing

    // Outputs, hidden: each slot's latched footswitches (Block::latched(), bit i = parameter i), so the UI can show
    // them and save them in presets
    kParamSlotLatchBase,
    kParamSlotLatchEnd = kParamSlotLatchBase + kMaxBlocks,

    // added after the latches (each new one goes last: the indices are DAW automation IDs)
    kParamTunerRunes = kParamSlotLatchEnd, // hidden, UI only: the Runes view (owner, 2026-10-31); over Tuner Strobe
    kParamTunerChevrons,    // hidden, unused: picked Chevrons over Hive, until Hive was removed (owner, 2026-11-01)

    // The global looper (owner, 2026-11-01; was a block): its 16 controls in LooperParam order, real values. The
    // footswitches (Rec/Dub, Play/Stop, Clear, Tap) are 1 while held and act on the press. Then its outputs.
    kParamLooperBase,
    kParamLooperEnd = kParamLooperBase + 16,
    kParamLooperState = kParamLooperEnd, // output, hidden: LooperReport::encodeState()
    kParamLooperPosition,   // output, hidden: 0..1 round the loop (a timed recording: how much is done)
    kParamLooperCount,      // output, hidden: counting in, the click being counted (1..); else 0
    kParamLooperLength,     // output, hidden: the loop's length in seconds
    kParamLooperCellBase,   // outputs, hidden: waveform cells (encodeLooperCell()), the first the one written last
    kParamLooperCellEnd = kParamLooperCellBase + 8,
    // hidden, internal: the UI pulses it (1, then 0 again) when the looper's page opens, so the DSP reserves the
    // loop's memory (23 MB) in this run only. Owner, 2026-11-01: not in instances that never use the looper, and
    // the looper is ephemeral: nothing about it survives a save, so a reopened project starts without the memory.
    kParamLooperWake = kParamLooperCellEnd,
    // saving the loop to a file (owner, 2026-11-01): the UI bumps Looper Export (input, hidden, a counter); the DSP
    // writes the loop to looperExportFile() and answers in Looper Exported (output, hidden: counter * 2 + 1 if it
    // wrote it, counter * 2 if there was nothing to write)
    kParamLooperExport,
    kParamLooperExported,
    // the looper page's edits (owner, 2026-11-01), hidden, each a request the DSP acts on when it changes (a
    // counter, so the same request twice still is one): Looper Delete deletes a pass (counter * 8 + its lane, from 0);
    // Looper Start sets where Play starts the loop (counter * 1001 + where, 0..1000 of the way round)
    kParamLooperDelete,
    kParamLooperStart,

    kParamCount
};

constexpr uint32_t slotBypassParam(int slot)
{
    return kSlotParamBase + static_cast<uint32_t>(slot * kSlotParamCount);
}

constexpr uint32_t slotParam(int slot, int param)
{
    return slotBypassParam(slot) + 1 + static_cast<uint32_t>(param);
}

constexpr uint32_t slotMeterParam(int slot)
{
    return kParamSlotMeterBase + static_cast<uint32_t>(slot);
}

constexpr uint32_t slotLatchParam(int slot)
{
    return kParamSlotLatchBase + static_cast<uint32_t>(slot);
}

constexpr uint32_t looperParam(int param)
{
    return kParamLooperBase + static_cast<uint32_t>(param);
}

constexpr bool isSlotParam(uint32_t index)
{
    return index >= kSlotParamBase && index < kFirstLaterParam;
}

// The value (as the host sees it) a CC value 0..127 sets a mapped parameter to
float midiCcToValue(uint32_t index, int ccValue);

// Parameters a MIDI controller can be mapped to: the user-facing inputs (globals, slots and the looper's)
constexpr bool isMidiMappable(uint32_t index)
{
    return index < kParamInPeakL || isSlotParam(index) || (index >= kParamLooperBase && index < kParamLooperEnd);
}

enum ParamFlags : uint32_t {
    kParamFlagNone    = 0,
    kParamFlagOutput  = 1 << 0,
    kParamFlagToggle  = 1 << 1,
    kParamFlagInteger = 1 << 2,
    kParamFlagHidden  = 1 << 3,
};

struct GlobalParamSpec {
    const char* name;
    const char* symbol;
    const char* unit;
    float min, max, def;
    uint32_t flags;
};

// Spec for any parameter that isn't a slot parameter (globals and later parameters)
const GlobalParamSpec& globalParamSpec(uint32_t index);

} // namespace xythos
