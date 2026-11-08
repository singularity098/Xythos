// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// The fourth set of synths (owner, 2026-11-08): mono, like those in SynthBlocks.hpp. Each follows the played note
// (dsp/NoteTracker.hpp, through TrackedSynth), with a Pick Filter and Track as its last two controls and the
// "lookahead" config (ms, 0 by default; blocks/TrackedInput.hpp).

// Theremin: a sine that glides along your pitch without a new note on each pick, so bends, slides and vibrato become
// swoops; Wobble is the old instrument's drift in pitch and level, Timbre warms the sine towards a reedy voice.
extern const BlockDescriptor kThereminBlockDescriptor;

enum ThereminParam {
    kThereminParamGlide,   // ms
    kThereminParamVibrato, // %, up to half a semitone
    kThereminParamRate,    // Hz
    kThereminParamWobble,  // %
    kThereminParamTimbre,  // %
    kThereminParamAttack,  // ms
    kThereminParamRelease, // ms
    kThereminParamOctave,  // choice: -2, -1, 0, +1
    kThereminParamMix,     // %
    kThereminParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kThereminParamTrack,   // choice: Block In, Rig In
    kThereminParamCount
};

// Sync Lead: a saw hard-synced to the note, its own pitch Sync times higher; each pick sweeps the ratio up by Env and
// back over Decay (the tearing sync scream), with a square an octave down under it.
extern const BlockDescriptor kSyncLeadBlockDescriptor;

enum SyncLeadParam {
    kSyncParamSync,    // ratio of the synced saw to the note, 1..8
    kSyncParamEnv,     // %, how far each pick sweeps the ratio (up to three octaves)
    kSyncParamDecay,   // ms
    kSyncParamSub,     // %, a square an octave down
    kSyncParamTone,    // Hz, low-pass
    kSyncParamAttack,  // ms
    kSyncParamRelease, // ms
    kSyncParamOctave,  // choice: -2, -1, 0, +1
    kSyncParamGlide,   // ms
    kSyncParamMix,     // %
    kSyncParamPickFilter, // ms
    kSyncParamTrack,   // choice: Block In, Rig In
    kSyncParamCount
};

// Phase Distortion (Casio CZ style): a cosine read with a bent phase, which sharpens it into a saw, square or pulse,
// or a windowed resonant sine; Amount is how far (the "DCW"), which jumps up by Env on each pick and falls to Sustain
// over Decay. Detune adds a second line a few cents up, the other side of the stereo field.
extern const BlockDescriptor kPdSynthBlockDescriptor;

enum PdSynthParam {
    kPdParamWave,    // choice: Saw, Square, Pulse, Reso Saw, Reso Tri
    kPdParamAmount,  // %
    kPdParamEnv,     // %
    kPdParamDecay,   // ms
    kPdParamSustain, // % of Env
    kPdParamDetune,  // cents
    kPdParamAttack,  // ms
    kPdParamRelease, // ms
    kPdParamOctave,  // choice: -2, -1, 0, +1
    kPdParamGlide,   // ms
    kPdParamMix,     // %
    kPdParamPickFilter, // ms
    kPdParamTrack,   // choice: Block In, Rig In
    kPdParamCount
};

// Wavetable: morphs through a table of eight waves (Vocal, Metal, PWM, Digital); Position picks the wave, your
// playing level moves it by Env, and an LFO sweeps it. The waves are band-limited per octave, so high notes don't
// alias.
extern const BlockDescriptor kWavetableBlockDescriptor;

enum WavetableParam {
    kWavetableParamTable,    // choice: wavetableNames
    kWavetableParamPosition, // %
    kWavetableParamEnv,      // %, -100..100: how far your playing moves the position
    kWavetableParamLfoRate,  // Hz
    kWavetableParamLfoDepth, // %
    kWavetableParamAttack,   // ms
    kWavetableParamRelease,  // ms
    kWavetableParamOctave,   // choice: -2, -1, 0, +1
    kWavetableParamGlide,    // ms
    kWavetableParamMix,      // %
    kWavetableParamPickFilter, // ms
    kWavetableParamTrack,    // choice: Block In, Rig In
    kWavetableParamCount
};

// The number of waves in each wavetable, and a table's wave (one cycle, `size` samples, harmonics up to `harmonics`
// (a power of two, 1..256): the band-limited copy for a note). For the tests.
constexpr int kWavetableFrames = 8;
const float* wavetableWave(int table, int frame, int harmonics, int& size);

// Sub Bass: a Moog-style bass: a sine, triangle, square or saw (an octave down by default) and a sine an octave
// below that, through a four-pole ladder low-pass with Growl (drive into it) and a filter that opens with your
// playing.
extern const BlockDescriptor kSubBassBlockDescriptor;

enum SubBassParam {
    kSubBassParamWave,      // choice: Sine, Triangle, Square, Saw
    kSubBassParamSub,       // %
    kSubBassParamCutoff,    // Hz
    kSubBassParamResonance, // %
    kSubBassParamGrowl,     // %
    kSubBassParamEnv,       // %
    kSubBassParamAttack,    // ms
    kSubBassParamRelease,   // ms
    kSubBassParamOctave,    // choice: -2, -1, 0, +1 (-1 by default)
    kSubBassParamGlide,     // ms
    kSubBassParamMix,       // %
    kSubBassParamPickFilter, // ms
    kSubBassParamTrack,     // choice: Block In, Rig In
    kSubBassParamCount
};

// Laser: on each pick the pitch dives from Dive semitones above your note down to it over Speed (the sci-fi zap);
// Repeat fires it again and again while the note rings, and an echo with feedback trails it.
extern const BlockDescriptor kLaserBlockDescriptor;

enum LaserParam {
    kLaserParamDive,    // semitones
    kLaserParamSpeed,   // ms
    kLaserParamWave,    // choice: Sine, Square, Saw
    kLaserParamRepeat,  // Hz, 0 = once per pick
    kLaserParamEcho,    // %, feedback
    kLaserParamTime,    // ms
    kLaserParamRelease, // ms
    kLaserParamOctave,  // choice: -2, -1, 0, +1
    kLaserParamMix,     // %
    kLaserParamPickFilter, // ms
    kLaserParamTrack,   // choice: Block In, Rig In
    kLaserParamCount
};

// Drone Keeper: listens to your notes over the last Memory seconds, finds the key's root (the pitch class heard most,
// backed by its fifth), and holds a slowly moving drone on it, with its fifth and octave, under your playing. Your
// guitar always passes; Level is the drone's. The drone fades in while you play and out over Fade after you stop, or
// stays with Hold.
extern const BlockDescriptor kDroneBlockDescriptor;

enum DroneParam {
    kDroneParamMemory,   // s
    kDroneParamRegister, // choice: Low, Mid, High (the root's octave: 1, 2, 3)
    kDroneParamFifth,    // %
    kDroneParamOctave,   // %, an octave up
    kDroneParamMotion,   // %, the filter's slow drift
    kDroneParamTone,     // Hz
    kDroneParamFade,     // s
    kDroneParamHold,     // toggle
    kDroneParamLevel,    // %
    kDroneParamPickFilter, // ms
    kDroneParamTrack,    // choice: Block In, Rig In
    kDroneParamCount
};

// The root a set of pitch-class weights (C = 0) suggests: the class heard most, backed by its fifth and thirds; -1
// for none. For the tests.
int droneRoot(const float weights[12]);

// Harmonic Painter: an additive synth, your note's first 32 harmonics: Tilt sets the brightness, Odd/Even the balance
// (hollow or reedy), Stretch pulls them out of tune like a piano's or a bell's, Shimmer makes each drift in level
// (at Rate), Spread pans odd and even harmonics apart.
extern const BlockDescriptor kHarmonicPainterBlockDescriptor;

enum HarmonicPainterParam {
    kHarmonicParamTilt,    // dB per octave
    kHarmonicParamOddEven, // %, -100 (odd only) .. 100 (even only)
    kHarmonicParamStretch, // %
    kHarmonicParamShimmer, // %
    kHarmonicParamRate,    // Hz
    kHarmonicParamSpread,  // %
    kHarmonicParamAttack,  // ms
    kHarmonicParamRelease, // ms
    kHarmonicParamOctave,  // choice: -2, -1, 0, +1
    kHarmonicParamGlide,   // ms
    kHarmonicParamMix,     // %
    kHarmonicParamPickFilter, // ms
    kHarmonicParamTrack,   // choice: Block In, Rig In
    kHarmonicParamCount
};

constexpr int kHarmonicPartials = 32;

} // namespace xythos
