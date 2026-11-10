// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// More guitar synths. Like the Synth block, each follows the played note (single notes, dsp/NoteTracker.hpp) and
// takes its loudness from your playing, with its own Attack and Release. Each has its own
// sound engine, then a Pick Filter and Track (from the block's input or the rig's) as its last two controls. Config:
// "lookahead" (ms, 0 by default; blocks/TrackedInput.hpp), which adds that much latency, and "poly" (Tracking: Mono,
// the default, or Poly, where every note of a chord gets its own voice; blocks/TrackedSynth.hpp).

// Super Saw: seven detuned saws spread across the stereo field (the trance lead and big pad sound), through a
// low-pass that opens with your playing.
extern const BlockDescriptor kSuperSawBlockDescriptor;

enum SuperSawParam {
    kSuperSawParamDetune,    // cents, the outer saws
    kSuperSawParamWidth,     // %
    kSuperSawParamCutoff,    // Hz
    kSuperSawParamResonance, // Q
    kSuperSawParamEnv,       // %, how far the playing opens the filter
    kSuperSawParamAttack,    // ms
    kSuperSawParamRelease,   // ms
    kSuperSawParamOctave,    // choice: -2, -1, 0, +1
    kSuperSawParamGlide,     // ms
    kSuperSawParamMix,       // %
    kSuperSawParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kSuperSawParamTrack, // choice: Block In, Rig In (the clean guitar, before the chain)
    kSuperSawParamCount
};

// FM Synth: two-operator FM (a sine modulating a sine). The ratio picks the family of sounds (electric piano, bass,
// bells, metallic tones); Index is the brightness, which falls from its peak to Sustain over Decay on each pick.
extern const BlockDescriptor kFmSynthBlockDescriptor;

enum FmSynthParam {
    kFmParamRatio,    // choice: fmRatio()
    kFmParamIndex,    // modulation index at the pick
    kFmParamDecay,    // ms
    kFmParamSustain,  // %, of the index
    kFmParamFeedback, // %, the modulator modulating itself (grit)
    kFmParamAttack,   // ms
    kFmParamRelease,  // ms
    kFmParamOctave,   // choice: -2, -1, 0, +1
    kFmParamGlide,    // ms
    kFmParamMix,      // %
    kFmParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kFmParamTrack, // choice: Block In, Rig In (the clean guitar, before the chain)
    kFmParamCount
};

double fmRatio(int choice);

// Fold Synth (west coast): a sine through a wavefolder, folded harder as you dig in, then a low-pass gate: Follow
// tracks your playing, Pluck pings on each pick like a vactrol (a woody, bongo-like decay).
extern const BlockDescriptor kFoldSynthBlockDescriptor;

enum FoldSynthParam {
    kFoldParamFold,     // %
    kFoldParamEnv,      // %, how much your playing adds to the fold
    kFoldParamSymmetry, // %, -100..100
    kFoldParamGate,     // choice: Follow, Pluck
    kFoldParamDecay,    // ms, Pluck
    kFoldParamTone,     // Hz, the gate's cutoff when fully open
    kFoldParamOctave,   // choice: -2, -1, 0, +1
    kFoldParamGlide,    // ms
    kFoldParamMix,      // %
    kFoldParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kFoldParamTrack, // choice: Block In, Rig In (the clean guitar, before the chain)
    kFoldParamCount
};

// Acid Synth (303 style): a saw or square through a resonant, overdriven 24 dB low-pass, with a filter envelope that
// snaps open on each pick and decays; Accent makes harder picks squelch more; Glide slides between notes.
extern const BlockDescriptor kAcidSynthBlockDescriptor;

enum AcidSynthParam {
    kAcidParamWave,      // choice: Saw, Square
    kAcidParamCutoff,    // Hz
    kAcidParamResonance, // %
    kAcidParamEnvMod,    // %
    kAcidParamDecay,     // ms
    kAcidParamAccent,    // %
    kAcidParamDrive,     // %
    kAcidParamOctave,    // choice: -2, -1, 0, +1
    kAcidParamGlide,     // ms
    kAcidParamMix,       // %
    kAcidParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kAcidParamTrack, // choice: Block In, Rig In (the clean guitar, before the chain)
    kAcidParamCount
};

// String Machine (Solina style): saws an octave down, at the note and an octave up, through a three-voice ensemble
// chorus, with slow swells: lush 70s strings.
extern const BlockDescriptor kStringSynthBlockDescriptor;

enum StringSynthParam {
    kStringParamLow,      // %, an octave down
    kStringParamMid,      // %
    kStringParamHigh,     // %, an octave up
    kStringParamEnsemble, // %
    kStringParamTone,     // Hz, low-pass
    kStringParamAttack,   // ms
    kStringParamRelease,  // ms
    kStringParamOctave,   // choice: -2, -1, 0, +1
    kStringParamMix,      // %
    kStringParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kStringParamTrack, // choice: Block In, Rig In (the clean guitar, before the chain)
    kStringParamCount
};

// Chip Synth (8-bit game console): pulse waves at three widths or a stepped triangle, notes snapped to semitones,
// a fast arpeggio for chords, a delayed vibrato, and Crush for the console's coarse volume and samples.
extern const BlockDescriptor kChipSynthBlockDescriptor;

enum ChipSynthParam {
    kChipParamWave,     // choice: Pulse 12%, Pulse 25%, Pulse 50%, Triangle
    kChipParamArp,      // choice: Off, Octave, Major, Minor, Fifth
    kChipParamArpRate,  // Hz, notes per second
    kChipParamQuantize, // toggle: snap to semitones
    kChipParamVibrato,  // %
    kChipParamCrush,    // %
    kChipParamOctave,   // choice: -2, -1, 0, +1
    kChipParamMix,      // %
    kChipParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kChipParamTrack, // choice: Block In, Rig In (the clean guitar, before the chain)
    kChipParamCount
};

} // namespace xythos
