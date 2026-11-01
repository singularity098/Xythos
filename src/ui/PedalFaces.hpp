// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <vector>

namespace xythos {

// Knobs drawn on a block's built-in image (resources/blocks/), each turning one of the block's parameters: the same
// host parameter as its knob among the panel's controls, so turning either turns both. Positions are fractions of the
// image (x and the radius of its width, y of its height). Three knobs sit in a shallow triangle, the middle one higher,
// so its name gets a row of its own. Plain data, no DPF, so the tests can check it.
struct FaceKnob {
    const char* param; // the ParamSpec name
    float x, y, radius;
};

// An LED on a block's image: drawn lit while the block is on and dark while it's bypassed, over the image's own
// (x and the radius are fractions of the image's width, y of its height; the colour is the lit one)
struct FaceLed {
    float x, y, radius;
    unsigned char r, g, b;
};

// The image's latching footswitch (for a treadle, its toe): clicking it turns the block on or off
struct FaceSwitch {
    float x, y, radius;
};

// The colour of the face knobs' glowing rim, tinted from the blue-rim knob (resources/knobs/rim.png); all zero:
// plain black knobs (knob.png)
struct FaceRim {
    unsigned char r, g, b;
    bool lit() const { return r != 0 || g != 0 || b != 0; }
};

struct PedalFace {
    const char* typeId;
    std::vector<FaceKnob> knobs;
    std::vector<FaceLed> leds;
    FaceSwitch footswitch;
    FaceRim rim;
};

// The name a face engraves under a knob turning `param`: at most five letters, so neighbouring knobs' names don't run
// into each other ("Damping" is "Damp"); the panel's knobs keep the full names
const char* faceKnobLabel(const char* param);

// A block type's face knobs and LEDs, or nullptr if its image has none
const PedalFace* pedalFace(const char* typeId);

const std::vector<PedalFace>& pedalFaces();

// The generic pedal photos' faces (resources/pedals/, by image id "pedal-<n>"): a working footswitch and a power LED,
// and no knobs (owner, 2026-10-30): their painted knobs aren't any particular controls. nullptr for an unknown id.
const PedalFace* genericPedalFace(const char* id);
const std::vector<PedalFace>& genericPedalFaces();

// The amp head photos' faces (resources/amps/, by image id "amp-<n>"; owner, 2026-10-31): five knobs on the head's blank
// panel turning the NAM block's Input, Bass, Middle, Treble and Output, a power LED beside the power switch, and the
// switch, which turns the block on or off. Coloured knobs only on heads that have that colour. nullptr for an unknown id.
// Also the half stacks' (resources/amps/, "stack-<n>"; owner, 2026-11-01), whose heads have the same.
const PedalFace* ampHeadFace(const char* id);
const std::vector<PedalFace>& ampHeadFaces();

// The half stack photos' faces (by image id "stack-<n>"): the head's five knobs, power LED and switch, as on the heads
const std::vector<PedalFace>& halfStackFaces();

} // namespace xythos
