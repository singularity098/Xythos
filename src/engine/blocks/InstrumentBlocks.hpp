// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// Instruments played from your notes (blocks/TrackedSynth.hpp: single notes, or chords in Poly; Track and Lookahead
// as in the synths).
// Everything is synthesized in code.

// Fart: every pick plays a fart at your note (an octave down by default). Thirteen kinds, from a squeaker to a
// motorboat, or Random to pick one each time (never the same twice running). Each is a buzzing pulse at the note,
// with the pitch bending its own way, a flutter in level, a jittery period, some airy noise and gurgling gaps,
// through a low formant. Length stretches them, Wetness adds the noise and the gurgles, Tone moves the formant.
extern const BlockDescriptor kFartBlockDescriptor;

enum FartParam {
    kFartParamType,    // choice: Random, then fartTypeName()
    kFartParamLength,  // %
    kFartParamWetness, // %
    kFartParamTone,    // %, 50 = as designed
    kFartParamOctave,  // choice: -2, -1, 0, +1
    kFartParamMix,     // %
    kFartParamPickFilter, // ms
    kFartParamTrack,   // choice: Block In, Rig In
    kFartParamCount
};

constexpr int kFartTypes = 13;
const char* fartTypeName(int type);

// Trumpet: a brass voice that brightens as you dig in (a saw through a low-pass that opens with the level, then the
// bell's formants), scoops up into each note, growls on hard attacks and adds a delayed vibrato. Mute: open, Harmon
// (thin and buzzy) or cup (soft and round).
extern const BlockDescriptor kTrumpetBlockDescriptor;

enum TrumpetParam {
    kTrumpetParamBright,  // %
    kTrumpetParamGrowl,   // %
    kTrumpetParamScoop,   // cents below the note each note starts
    kTrumpetParamVibrato, // %
    kTrumpetParamMute,    // choice: Open, Harmon, Cup
    kTrumpetParamAttack,  // ms
    kTrumpetParamRelease, // ms
    kTrumpetParamOctave,  // choice: -2, -1, 0, +1
    kTrumpetParamGlide,   // ms
    kTrumpetParamMix,     // %
    kTrumpetParamPickFilter, // ms
    kTrumpetParamTrack,   // choice: Block In, Rig In
    kTrumpetParamCount
};

// Violin: a bowed string: a saw (the bowed string's motion) through the body's resonances (violin, viola or cello),
// with bow noise, a slow bowed attack, a vibrato that fades in, and Ensemble for a small section (three players,
// slightly apart in pitch and vibrato).
extern const BlockDescriptor kViolinBlockDescriptor;

enum ViolinParam {
    kViolinParamBody,     // choice: Violin, Viola, Cello
    kViolinParamVibrato,  // %
    kViolinParamRate,     // Hz, the vibrato's
    kViolinParamBow,      // %, bow noise
    kViolinParamEnsemble, // toggle
    kViolinParamAttack,   // ms
    kViolinParamRelease,  // ms
    kViolinParamOctave,   // choice: -2, -1, 0, +1
    kViolinParamGlide,    // ms
    kViolinParamMix,      // %
    kViolinParamPickFilter, // ms
    kViolinParamTrack,    // choice: Block In, Rig In
    kViolinParamCount
};

// Piano: each note you pick strikes a piano note (up to eight ring at once): twelve partials, each a pair of
// strings a little out of tune with each other (Detune: the beating), stretched like a real piano's (inharmonicity),
// the higher ones dying sooner, a hammer's thump, and brighter the harder you pick. Letting go of a note damps it,
// unless Sustain is on (the pedal down).
extern const BlockDescriptor kPianoBlockDescriptor;

enum PianoParam {
    kPianoParamBright,  // %
    kPianoParamDecay,   // s
    kPianoParamSustain, // toggle
    kPianoParamDetune,  // cents
    kPianoParamHammer,  // %
    kPianoParamTone,    // Hz, low-pass
    kPianoParamOctave,  // choice: -2, -1, 0, +1
    kPianoParamMix,     // %
    kPianoParamPickFilter, // ms
    kPianoParamTrack,   // choice: Block In, Rig In
    kPianoParamCount
};

} // namespace xythos
