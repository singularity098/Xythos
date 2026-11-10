// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// Guitar synth: a monophonic oscillator that follows the played note, through a resonant low-pass whose cutoff
// rises with the playing (Env), with the guitar's own dynamics on the amplitude. The notes come from
// dsp/NoteTracker.hpp: a new note is found about 12 ms after the pick (low notes later); play single notes (chords
// confuse it, as with any guitar synth). The Pick Filter keeps the pick attack's unsteady pitch from sounding; Track
// picks the block's input or the rig's. Waves: square, saw, pulse, and sub (saw plus a square an octave down).
// Config: "lookahead" (ms, 0 by default; blocks/TrackedInput.hpp), which adds that much latency, and "poly"
// (Tracking: Mono, the default, or Poly, every note of a chord with its own voice; blocks/TrackedSynth.hpp).
extern const BlockDescriptor kSynthBlockDescriptor;

enum SynthParam {
    kSynthParamWave,      // choice: SynthWave
    kSynthParamOctave,    // choice: -2, -1, 0, +1
    kSynthParamCutoff,    // Hz
    kSynthParamResonance, // Q
    kSynthParamEnv,       // %, how far the playing opens the filter (up to 5 octaves)
    kSynthParamAttack,    // ms
    kSynthParamGlide,     // ms
    kSynthParamMix,       // %
    kSynthParamPickFilter, // ms a new note must hold steady before it sounds (0 = off)
    kSynthParamTrack,      // choice: Block In, Rig In (the clean guitar, before the chain)
    kSynthParamCount
};

enum class SynthWave { Square, Saw, Pulse, Sub };

} // namespace xythos
