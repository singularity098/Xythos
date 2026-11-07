// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// The pitch blocks share a delay-line pitch shifter (dsp/PitchShifter.hpp): polyphonic, zero reported latency,
// with a slight warble; a longer Window warbles less on low notes but trails more. Config: "hq" (Pitch Shifter,
// Whammy, Poly Octave, Harmonizer, Detune, Capo): spectral shifting instead (dsp/SpectralShifter.hpp), much cleaner,
// with about 150 ms of reported latency; the Window is then unused.

// Pitch Shifter / harmonizer: two voices at fixed intervals (whole semitones) plus a shared detune, and the dry
// signal. One voice a few cents off is a detune chorus; +4 and +7 a major-chord harmony.
extern const BlockDescriptor kPitchShifterBlockDescriptor;

enum PitchShifterParam {
    kPitchParamShift1, // semitones, rounded
    kPitchParamLevel1, // %
    kPitchParamShift2, // semitones, rounded
    kPitchParamLevel2, // %
    kPitchParamDetune, // cents: voice 1 up, voice 2 down
    kPitchParamDry,    // %
    kPitchParamWindow, // ms
    kPitchParamTone,   // Hz, low-pass on the voices; off at the top of the range
    kPitchParamCount
};

// Whammy: Position (an expression pedal through MIDI learn) bends from no shift to the chosen interval. Whammy mode is
// the shifted signal only; Harmony adds the dry signal.
extern const BlockDescriptor kWhammyBlockDescriptor;

enum WhammyParam {
    kWhammyParamPosition, // %
    kWhammyParamInterval, // choice: whammyIntervalSemitones()
    kWhammyParamMode,     // choice: Whammy, Harmony
    kWhammyParamWindow,   // ms
    kWhammyParamCount
};

int whammyIntervalCount();
double whammyIntervalSemitones(int index);

// Poly Octave (POG style): octaves down and up, for chords too
extern const BlockDescriptor kPolyOctaveBlockDescriptor;

enum PolyOctaveParam {
    kPolyOctParamSub2, // %, two octaves down
    kPolyOctParamSub1, // %
    kPolyOctParamUp1,  // %
    kPolyOctParamUp2,  // %, two octaves up
    kPolyOctParamDry,  // %
    kPolyOctParamTone, // Hz, low-pass on the octaves; off at the top of the range
    kPolyOctParamWindow, // ms
    kPolyOctParamCount
};

// Harmonizer: one or two voices a scale interval away from the played note, in a key and scale, so a "third above"
// is major or minor as the scale needs. It follows single notes (dsp/PitchTracker.hpp) to know the note; the
// shifting itself works on anything. Notes outside the scale use their nearest scale note's interval.
extern const BlockDescriptor kHarmonizerBlockDescriptor;

enum HarmonizerParam {
    kHarmParamKey,    // choice: C .. B
    kHarmParamScale,  // choice: harmonizerScale()
    kHarmParamVoice1, // choice: scale steps, -Oct .. +Oct
    kHarmParamLevel1, // %
    kHarmParamVoice2, // choice
    kHarmParamLevel2, // %
    kHarmParamDry,    // %
    kHarmParamWindow, // ms
    kHarmParamTrack,  // choice: Block In, Rig In: what the played note is found from
    kHarmParamCount
};

// The semitones from the played note (MIDI note number `note`) to the note `steps` scale steps away, for a key
// (0 = C) and scale (index into the Scale choice)
int harmonySemitones(int note, int key, int scale, int steps);

// Detune: one or two voices a few cents off the dry signal (Up, Down or Both), spread across the stereo field: a
// thickening micro-pitch doubler without a chorus's sweep.
extern const BlockDescriptor kDetuneBlockDescriptor;

enum DetuneParam {
    kDetuneParamDetune, // cents
    kDetuneParamVoices, // choice: Both, Up, Down
    kDetuneParamWidth,  // %
    kDetuneParamMix,    // %
    kDetuneParamWindow, // ms
    kDetuneParamCount
};

// Capo: shifts everything up or down by whole semitones (plus a fine offset), for drop tunings and capo positions
// without retuning. Chords too. The dry signal is off by default.
extern const BlockDescriptor kCapoBlockDescriptor;

enum CapoParam {
    kCapoParamShift,  // semitones, rounded
    kCapoParamFine,   // cents
    kCapoParamDry,    // %
    kCapoParamWindow, // ms
    kCapoParamCount
};

// Organ: the guitar turned into stacked organ pipes, with drawbar-like levels for the sub octave, the fundamental,
// the octave, the twelfth and two octaves up; Attack swells each new note in like an organ key instead of a pick.
extern const BlockDescriptor kOrganBlockDescriptor;

enum OrganParam {
    kOrganParamSub,     // %, an octave down
    kOrganParamFund,    // %
    kOrganParamOctave,  // %
    kOrganParamTwelfth, // %, an octave and a fifth up
    kOrganParamTwoOct,  // %
    kOrganParamAttack,  // ms
    kOrganParamTone,    // Hz, low-pass on the organ; off at the top of the range
    kOrganParamDry,     // %
    kOrganParamWindow,  // ms
    kOrganParamCount
};

// Arpeggiator: steps the held note or chord through a chord's intervals in a pattern, in time (tempo sync locks the
// steps to the beat). Gate shortens each step for a staccato pattern; Glide slides between steps.
extern const BlockDescriptor kArpeggiatorBlockDescriptor;

enum ArpeggiatorParam {
    kArpParamChord,   // choice: arpChordIntervals()
    kArpParamPattern, // choice: Up, Down, Up-Down, Random
    kArpParamRange,   // choice: 1 Oct, 2 Oct
    kArpParamRate,    // Hz, steps per second; when synced, a note value (rateNoteIndex)
    kArpParamSync,    // toggle
    kArpParamGate,    // %
    kArpParamGlide,   // ms
    kArpParamMix,     // %
    kArpParamWindow,  // ms
    kArpParamCount
};

// The arpeggiator's notes for a chord (Chord choice) over `octaves` (1 or 2), in semitones from the played note,
// lowest first; and which of the `notes` a fixed pattern (Up, Down, Up-Down) plays at step `step`
int arpNoteCount(int chord, int octaves);
int arpNote(int chord, int octaves, int index);
int arpPatternIndex(int pattern, long long step, int notes);

// Sustainer (E-Bow, infinite sustain): holds each note at the level you picked it, and over Bloom it grows into a
// harmonic (an octave, a twelfth or two octaves up) like an amp feeding back. A new pick starts again; the note
// lets go when it fades too far to hold.
extern const BlockDescriptor kSustainerBlockDescriptor;

enum SustainerParam {
    kSustainParamSustain,  // %
    kSustainParamBloom,    // ms
    kSustainParamHarmonic, // choice: Octave, Oct+5th, 2 Oct, 5th
    kSustainParamFeedback, // %, how much of the harmonic the note blooms into
    kSustainParamTone,     // Hz, low-pass; off at the top of the range
    kSustainParamMix,      // %
    kSustainParamCount
};

} // namespace xythos
