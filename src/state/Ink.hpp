// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xythos::ink {

// How the highlight is painted: one colour, or 2-3 colours spread across the window (Smooth runs them left to right,
// Splotch in soft blobs)
enum class Mode : int { Solid = 0, Smooth = 1, Splotch = 2 };

struct Rgb
{
    uint8_t r = 0, g = 0, b = 0;

    bool operator==(const Rgb&) const = default;
};

// A theme (owner, 2026-11-08): a fixed look picked by name in Settings. Iris is Xythos' own and the default; the
// others came from Kaia (the owner's drum sampler), all but its Default.
struct Theme
{
    std::string_view name;
    Mode mode = Mode::Splotch;
    std::vector<Rgb> colors;  // the highlight paint: outlines, fills, knobs, the logo's circle (2 or 3)
    Rgb accent;               // the highlight where the paint can't go (text)
    Rgb text, textDim;
    uint32_t glowA = 0;       // faint glows over the background, top right and bottom left (0xAARRGGBB; 0 for none)
    uint32_t glowB = 0;
    bool scanlines = false;   // faint CRT lines over the whole window (Phosphor)
};

// The themes Settings offers, in the order they're shown; the first is the default. Their names are stored in
// the settings: never rename one.
const std::vector<Theme>& themes();
// The theme called `name`, or the default (Iris) for an unknown name
const Theme& themeNamed(std::string_view name);

// The smallest relative luminance a colour may have (WCAG-style, 0..1): about 3:1 against the window's darkest
// background, so every colour reads as light. Colours below it are lifted toward white (lighten()).
constexpr double kMinLuminance = 0.12;

double relativeLuminance(Rgb color);
bool isLightEnough(Rgb color);
// The colour, or the nearest one toward white that is light enough
Rgb lighten(Rgb color);

// "#rrggbb" (the leading # is optional in the input)
std::string toHex(Rgb color);
std::optional<Rgb> parseHex(std::string_view text);

// Hue in degrees (0..360, red at 0), saturation and value (brightness) 0..1: the colour wheel's coordinates
struct Hsv
{
    double h = 0.0, s = 0.0, v = 1.0;
};
Hsv toHsv(Rgb color);
Rgb fromHsv(Hsv hsv);

// A plain-English name for a colour, shown as the picker moves: "pale indigo", "deep teal", "light grey"
std::string colorName(Rgb color);

// The texture the paint is drawn from: kTextureWidth x kTextureHeight RGBA pixels, stretched over the window.
// Smooth runs the colours left to right, slightly tilted; Splotch fills the window with soft, almost random blobs
// of them (fixed noise, so it looks the same every time). Solid is one colour.
constexpr int kTextureWidth = 128;
constexpr int kTextureHeight = 80;
std::vector<uint8_t> makeTexture(Mode mode, const std::vector<Rgb>& colors);

// The colour at t (0..1) along the colours, evenly spaced
Rgb sample(const std::vector<Rgb>& colors, double t);

} // namespace xythos::ink
