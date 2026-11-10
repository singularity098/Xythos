// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"

namespace xythos {

// Analog-style octaver (like an OC-2): flip-flops divide the played note by two and four, switching the signal's
// polarity at its zero crossings, so the octaves down keep the guitar's dynamics with zero latency. The detector's
// low-pass follows the played note (dsp/PitchTracker.hpp), which keeps harmonics from upsetting the division. Octave up is a
// full-wave rectifier. Single notes only; chords confuse it, as they do the original. Config: "lookahead", and "poly"
// (Poly tracking: the low-pass follows the chord's lowest note; blocks/TrackedInput.hpp).
extern const BlockDescriptor kOctaveBlockDescriptor;

enum OctaveParam {
    kOctParamSub1,     // %, one octave down
    kOctParamSub2,     // %, two octaves down
    kOctParamUp,       // %, one octave up
    kOctParamDry,      // %
    kOctParamTone,     // Hz, low-pass on the octaves
    kOctParamTracking, // Hz, the detector's low-pass until the note is known (then it follows the note)
    kOctParamTrack,    // choice: Block In, Rig In: what the note is found from (the octaves always come from the input)
    kOctParamCount
};

} // namespace xythos
