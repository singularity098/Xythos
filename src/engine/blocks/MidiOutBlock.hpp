// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// MIDI Out: turns your playing into MIDI notes for a synth plugin after Xythos (in Reaper, the next plugin in the
// FX chain). Mono follows single notes (dsp/NoteTracker.hpp): a note on at each note's start, at a velocity from
// how hard it was picked (Dynamics), a note off when it ends, and pitch bend for bends and vibrato (Bend: the range
// the synth is set to; Off sends plain notes). Poly finds the notes of chords (dsp/PolyNoteFinder.hpp: Focus and
// the Pick Filter as in the Poly Chip, at least 15 ms), without bend. Channel, Octave and Transpose shift what is
// sent; Audio passes the guitar on or mutes it. Track and Lookahead as in the synths: with lookahead, the notes are
// sent from their picks, in time with the (delayed) audio. The engine ends a bypassed or removed block's notes.
extern const BlockDescriptor kMidiOutBlockDescriptor;

enum MidiOutParam {
    kMidiOutParamMode,      // choice: Mono, Poly
    kMidiOutParamChannel,   // choice: 1 .. 16
    kMidiOutParamOctave,    // choice: -2 .. +2
    kMidiOutParamTranspose, // choice: -12 .. +12 semitones
    kMidiOutParamBend,      // choice: Off, 2, 12, 24 semitones (the synth's bend range)
    kMidiOutParamDynamics,  // %, how much the velocity follows the pick
    kMidiOutParamFocus,     // % (Poly)
    kMidiOutParamAudio,     // choice: Through, Mute
    kMidiOutParamPickFilter, // ms
    kMidiOutParamTrack,     // choice: Block In, Rig In
    kMidiOutParamCount
};

// The MIDI velocity for a level (the input's peak at the pick), with Dynamics 0..1: 0 sends 100 for every note
int midiVelocity(float level, float dynamics);

} // namespace xythos
