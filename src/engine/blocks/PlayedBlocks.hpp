// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// Strum Machine: turns each picked note into a strummed chord. The note is followed (NoteTracker), and on each pick
// up to four pitch-shifted copies of it come in one after another, Strum ms apart, like a hand sweeping across the
// strings: low to high (Down), high to low (Up), or alternating. Chord picks the notes over the played one:
// Diatonic builds them from Key and Scale, so every note gets the chord that belongs to it; the others are fixed
// shapes. Track and Lookahead as in the synths (blocks/TrackedInput.hpp); in Poly the shape goes on the chord's
// lowest note, strummed when any of its notes starts.
extern const BlockDescriptor kStrumBlockDescriptor;

enum StrumParam {
    kStrumParamChord,     // choice: Diatonic, Major, Minor, Power, Sus4, Dom7, Maj7, Min7, Octaves
    kStrumParamKey,       // choice: C .. B (Diatonic)
    kStrumParamScale,     // choice: Major, Minor (Diatonic)
    kStrumParamNotes,     // choice: 2, 3, 4 added notes
    kStrumParamStrum,     // ms between strings
    kStrumParamDirection, // choice: Down, Up, Alternate
    kStrumParamLevel,     // %, of the added notes
    kStrumParamDry,       // %
    kStrumParamWindow,    // ms, the pitch shifter's window
    kStrumParamTrack,     // choice: Block In, Rig In
    kStrumParamCount
};

int strumChordNote(int chord, int key, int scale, int playedMidi, int index); // semitones over the played note

// Hard Tune: pitch correction for guitar. The played note is followed and pulled to the nearest note of Key and
// Scale: Speed 0 snaps at once (the robotic sound), longer speeds glide like a gentle correction; Amount pulls part
// way. The correction is a pitch shift of the input (pitch-synchronous window). Track and Lookahead as in the
// synths. In Poly tracking the whole chord is shifted by its notes' average distance from the scale (louder notes
// count more), since one shift moves every note.
extern const BlockDescriptor kHardTuneBlockDescriptor;

enum HardTuneParam {
    kHardTuneParamKey,    // choice: C .. B
    kHardTuneParamScale,  // choice: Chromatic, Major, Minor, Major Pent., Minor Pent., Blues
    kHardTuneParamSpeed,  // ms
    kHardTuneParamAmount, // %
    kHardTuneParamMix,    // %
    kHardTuneParamWindow, // ms
    kHardTuneParamTrack,  // choice: Block In, Rig In
    kHardTuneParamCount
};

// Drum Trigger: hits on muted strings play drums made in code. Each pick is classed by how bright its first 6 ms
// are: low thumps play the kick, brighter hits the snare, the brightest the hi-hat (Mode Auto), or every hit plays
// one drum, or kick and snare alternate. The drums: a kick (a sine falling from 150 to 48 Hz, with a click), a snare
// (two tones and band-passed noise) and a hi-hat (high-passed noise). The drum starts 6 ms after the pick (the time
// it listens for). No config.
extern const BlockDescriptor kDrumTriggerBlockDescriptor;

enum DrumTriggerParam {
    kDrumParamMode,        // choice: Auto, Kick, Snare, Hi-Hat, Alternate
    kDrumParamSplit,       // %, the brightness between kick and snare
    kDrumParamSensitivity, // %
    kDrumParamKick,        // %
    kDrumParamSnare,       // %
    kDrumParamHat,         // %
    kDrumParamTune,        // %, 50 = as designed
    kDrumParamDecay,       // %, 50 = as designed
    kDrumParamDynamics,    // %, how much the drums follow how hard you hit
    kDrumParamDry,         // %
    kDrumParamCount
};

} // namespace xythos
