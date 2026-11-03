// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xythos {

// Each NAM model and cab IR gets a look of its own in the placeholder art, picked from its file's path, so two amps
// or two cabs side by side tell apart at a glance and a file looks the same wherever it appears (chain, library,
// presets). Plain data, no DPF, so the tests can check it.
//
// An empty key gives the generic look (the Add Block list, which has no file).

// The logos, shared by amps and cabs
enum Badge { BadgeNone, BadgeCircle, BadgeDiamond, BadgeBolt, BadgeStar, BadgePlate, BadgeTriangle, kBadges };

// Amps are always a wide head (owner, 2026-10-25; combos were dropped), in reds and oranges
struct AmpLook {
    enum Grille { Slats, Bars, Crosshatch, Perforated, Waves, Zigzag, kGrilles };

    static constexpr float kMinHue = -6.0f; // a crimson red
    static constexpr float kMaxHue = 32.0f; // an orange

    int knobs = 6;           // along the top panel, 3..7
    Grille grille = Slats;   // the pattern of the grille
    Badge badge = BadgeNone; // the logo on the grille
    float hue = 0.0f;        // the amp's colour, in degrees on the colour wheel (0 red, 30 orange); mostly red
    std::array<float, 7> turns { 0.55f, 0.4f, 0.65f, 0.5f, 0.35f, 0.6f, 0.45f }; // knob positions, 0..1
};

struct CabLook {
    enum Cone { Plain, Rim, Spokes, BigCap, kCones };
    enum Cloth { Bare, Diagonal, Vertical, kCloths };

    Cone cone = Plain;       // how each speaker is drawn
    Cloth cloth = Bare;      // lines of grille cloth behind the speakers
    Badge badge = BadgeNone; // the logo above the speakers
    bool corners = false;    // metal corner protectors
    float hue = 0.0f;        // shift of the cab colour, in degrees
};

AmpLook ampLook(std::string_view key);
CabLook cabLook(std::string_view key);

// The built-in speaker cabinet images (resources/cabs/, BlockImages.hpp) that IR blocks show. Their ids are
// "<size>-<n>" ("4x12-3"). The sizes, in the picker's order: bass cabs, then guitar cabs.
constexpr std::array<const char*, 6> kCabSizes { "1x10", "2x10", "4x10", "1x12", "2x12", "4x12" };
constexpr int kBassCabSizes = 3; // the first three are bass cabs

// The size part of a cab image id ("4x12-3" -> "4x12"); "" if it has none
std::string_view cabImageSize(std::string_view id);

// The ids of one size from `ids`, in their numbers' order ("4x12-2" before "4x12-10")
std::vector<std::string> cabImagesOfSize(const std::vector<std::string>& ids, std::string_view size);

// The cab size a file's name gives, if it's one of kCabSizes ("Mesa 4X12 V30" -> "4x12"); else ""
std::string_view cabSizeInName(std::string_view name);

// The cab image an IR shows until the user picks another: picked from `ids` by `key` (the IR's content hash), so
// the same file always gets the same cab; among the cabs of the size its file name gives, if it gives one and there
// are some, else among the 4x12s. An empty key (no file) gets the first 4x12. "" if `ids` is empty.
std::string defaultCabImage(std::string_view key, std::string_view fileName, const std::vector<std::string>& ids);

// The built-in generic pedal images (resources/pedals/, BlockImages.hpp), "pedal-<n>": NAM pedal captures show one
// until the user picks another, and so do pedals without a photo of their own yet (owner, 2026-10-30). Picked from
// `ids` by `key` (a NAM file's content hash, or the block's typeId), so the same file or pedal always gets the same
// one; an empty key gets the first. "" if `ids` is empty.
std::string defaultPedalImage(std::string_view key, const std::vector<std::string>& ids);

// The built-in amp head images (resources/amps/, BlockImages.hpp), "amp-<n>", that NAM amp models show (owner,
// 2026-10-31). Each photo has styles it suits (picked by eye from how tame or how strange it looks), and a file's name
// may say what it's for ("Clean", "Jazz", "Crunch", "Lead", "Djent", ...): a file gets a head among those that suit the
// style its name gives, the strangest-looking ones for the heaviest tones, picked by its content hash, so the same file
// always gets the same head. A name that says nothing gets any head.
enum AmpStyle : unsigned {
    kAmpClean = 1,    // clean, glassy, sparkly
    kAmpJazz = 2,     // warm, dark, jazz
    kAmpCrunch = 4,   // edge of breakup, blues, classic rock
    kAmpHighGain = 8, // lead, hard rock, metal
    kAmpExtreme = 16, // very heavy and aggressive: the strangest heads
};

// The styles an amp image suits (0 for an unknown id)
unsigned ampImageStyle(std::string_view id);

// The style a NAM file's name suggests, one of the AmpStyle bits, or 0 if it doesn't say (a fuzzy match on words like
// "clean", "jazz", "crunch", "lead", "metal", "djent", and gain numbers like "Gain 9" or "G10")
unsigned ampStyleInName(std::string_view name);

// The amp head a NAM amp model shows until the user picks another: from `ids`, by `key` (its content hash), among those
// suiting the style its file name suggests (all of them if it suggests none, or none suit it). An empty key gets the
// first. "" if `ids` is empty.
std::string defaultAmpImage(std::string_view key, std::string_view fileName, const std::vector<std::string>& ids);

// The half stack (a head on its cab; owner, 2026-11-01) a NAM model of an amp with its cab shows until the user picks
// another: from the "stack-<n>" ids among `ids`, by `key` (its content hash) alone. An empty key gets the first. "" if
// there are none.
std::string defaultStackImage(std::string_view key, const std::vector<std::string>& ids);

} // namespace xythos
