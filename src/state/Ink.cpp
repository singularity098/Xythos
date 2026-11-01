// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/Ink.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xythos::ink {

const std::vector<Palette>& presets()
{
    static const std::vector<Palette> palettes = {
        // the owner's colours (2026-10-30), Splotchy by default
        { "Iris", { { 0x72, 0x78, 0xfe }, { 0xc6, 0x88, 0xd6 }, { 0xa8, 0x5e, 0xea } } },
        // pale, which reads better than vivid colours on the black window (owner, 2026-10-30)
        { "Sunset", { { 0xff, 0xb4, 0x9c }, { 0xff, 0xa3, 0xc8 }, { 0xb9, 0xab, 0xff } } },
        { "Ocean", { { 0x9e, 0xe6, 0xff }, { 0xa3, 0xc2, 0xf0 }, { 0xb5, 0xac, 0xff } } },
        { "Mint", { { 0xa2, 0xf5, 0xd3 }, { 0xa0, 0xe9, 0xf5 }, { 0xc6, 0xae, 0xfb } } },
        { "Gold", { { 0xff, 0xe6, 0x8f }, { 0xff, 0xc9, 0x8f }, { 0xff, 0xa8, 0xbc } } },
    };
    return palettes;
}

namespace {

double linear(uint8_t channel)
{
    const double c = channel / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// A small hash of an integer lattice point, in 0..1
double lattice(int x, int y)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xffffffu) / 16777215.0;
}

// Smooth value noise over the plane, in 0..1
double valueNoise(double x, double y)
{
    const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
    double fx = x - x0, fy = y - y0;
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    const double top = lattice(x0, y0) + (lattice(x0 + 1, y0) - lattice(x0, y0)) * fx;
    const double bottom = lattice(x0, y0 + 1) + (lattice(x0 + 1, y0 + 1) - lattice(x0, y0 + 1)) * fx;
    return top + (bottom - top) * fy;
}

} // namespace

double relativeLuminance(Rgb color)
{
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
}

bool isLightEnough(Rgb color)
{
    return relativeLuminance(color) >= kMinLuminance;
}

Rgb lighten(Rgb color)
{
    if (isLightEnough(color))
        return color;
    // Mixing toward white only ever raises the luminance, so bisect the mix amount
    auto mix = [&color](double t) {
        auto channel = [t](uint8_t c) { return static_cast<uint8_t>(std::lround(c + (255.0 - c) * t)); };
        return Rgb { channel(color.r), channel(color.g), channel(color.b) };
    };
    double low = 0.0, high = 1.0;
    for (int i = 0; i < 24; ++i)
    {
        const double middle = (low + high) / 2.0;
        if (isLightEnough(mix(middle)))
            high = middle;
        else
            low = middle;
    }
    return mix(high);
}

std::string toHex(Rgb color)
{
    char text[8];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x", color.r, color.g, color.b);
    return text;
}

std::optional<Rgb> parseHex(std::string_view text)
{
    if (!text.empty() && text.front() == '#')
        text.remove_prefix(1);
    if (text.size() != 6)
        return std::nullopt;
    unsigned value = 0;
    for (const char c : text)
    {
        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value |= static_cast<unsigned>(c - 'A' + 10);
        else
            return std::nullopt;
    }
    return Rgb { static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
}

Rgb sample(const std::vector<Rgb>& colors, double t)
{
    if (colors.empty())
        return Rgb { 255, 255, 255 };
    if (colors.size() == 1)
        return colors.front();
    const double position = std::clamp(t, 0.0, 1.0) * static_cast<double>(colors.size() - 1);
    const size_t index = std::min(static_cast<size_t>(position), colors.size() - 2);
    const double f = position - static_cast<double>(index);
    const Rgb& a = colors[index];
    const Rgb& b = colors[index + 1];
    auto channel = [f](uint8_t x, uint8_t y) { return static_cast<uint8_t>(std::lround(x + (y - x) * f)); };
    return Rgb { channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b) };
}

Hsv toHsv(Rgb color)
{
    const double r = color.r / 255.0, g = color.g / 255.0, b = color.b / 255.0;
    const double high = std::max({ r, g, b }), low = std::min({ r, g, b }), span = high - low;
    Hsv hsv { 0.0, high > 0.0 ? span / high : 0.0, high };
    if (span > 0.0)
    {
        if (high == r)
            hsv.h = 60.0 * std::fmod((g - b) / span + 6.0, 6.0);
        else if (high == g)
            hsv.h = 60.0 * ((b - r) / span + 2.0);
        else
            hsv.h = 60.0 * ((r - g) / span + 4.0);
    }
    return hsv;
}

Rgb fromHsv(Hsv hsv)
{
    // a drag's maths never gives NaN or infinity, but they'd make the conversion to int below undefined
    if (!std::isfinite(hsv.h))
        hsv.h = 0.0;
    if (std::isnan(hsv.s))
        hsv.s = 0.0;
    if (std::isnan(hsv.v))
        hsv.v = 0.0;
    const double h = std::fmod(std::fmod(hsv.h, 360.0) + 360.0, 360.0) / 60.0;
    const double s = std::clamp(hsv.s, 0.0, 1.0), v = std::clamp(hsv.v, 0.0, 1.0);
    const double c = v * s, x = c * (1.0 - std::fabs(std::fmod(h, 2.0) - 1.0)), m = v - c;
    double r = 0.0, g = 0.0, b = 0.0;
    switch (static_cast<int>(h))
    {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    auto channel = [m](double value) { return static_cast<uint8_t>(std::lround((value + m) * 255.0)); };
    return Rgb { channel(r), channel(g), channel(b) };
}

std::string colorName(Rgb color)
{
    const Hsv hsv = toHsv(color);
    // greys first: too little colour to name a hue
    if (hsv.s < 0.1 || (hsv.s < 0.18 && hsv.v < 0.3))
    {
        if (hsv.v > 0.94)
            return "white";
        if (hsv.v > 0.75)
            return "light grey";
        if (hsv.v > 0.45)
            return "grey";
        if (hsv.v > 0.18)
            return "dark grey";
        return "black";
    }
    // hues by where they start, in degrees
    static constexpr std::pair<double, const char*> hues[] = {
        { 0, "red" },        { 10, "scarlet" },  { 20, "orange" },     { 38, "amber" },  { 48, "yellow" },
        { 64, "chartreuse" }, { 85, "green" },   { 140, "emerald" },   { 160, "teal" },  { 175, "cyan" },
        { 195, "azure" },    { 215, "blue" },    { 235, "indigo" },    { 255, "violet" }, { 275, "purple" },
        { 290, "magenta" },  { 315, "rose" },    { 345, "red" },
    };
    std::string hue = "red";
    for (const auto& [start, name] : hues)
        if (hsv.h >= start)
            hue = name;
    // dark oranges are browns, dark yellows olives: they take no "deep"
    const bool earthy = (hue == "orange" || hue == "amber" || hue == "scarlet") ? hsv.v < 0.6
                        : (hue == "yellow" || hue == "chartreuse")             ? hsv.v < 0.6
                                                                               : false;
    if (earthy)
        hue = hue == "yellow" || hue == "chartreuse" ? "olive" : "brown";
    // and pale oranges are peach
    if ((hue == "orange" || hue == "scarlet") && hsv.v > 0.8 && hsv.s < 0.6)
        return hsv.s < 0.35 ? "pale peach" : "peach";

    std::string shade;
    if (hsv.v < 0.35)
        shade = "dark";
    else if (hsv.v < 0.6 && !earthy)
        shade = hsv.s > 0.5 ? "deep" : "dusky";
    else if (hsv.v > 0.8 && hsv.s < 0.35)
        shade = "pale";
    else if (hsv.v > 0.8 && hsv.s < 0.6)
        shade = "light";
    else if (hsv.s < 0.35)
        shade = "muted";
    else if (hsv.s > 0.85 && hsv.v > 0.85)
        shade = "vivid";
    return shade.empty() ? hue : shade + " " + hue;
}

std::vector<uint8_t> makeTexture(Mode mode, const std::vector<Rgb>& colors)
{
    std::vector<double> field(static_cast<size_t>(kTextureWidth) * kTextureHeight, 0.0);
    for (int y = 0; y < kTextureHeight; ++y)
    {
        for (int x = 0; x < kTextureWidth; ++x)
        {
            const double u = (x + 0.5) / kTextureWidth, v = (y + 0.5) / kTextureHeight;
            double t = u * 0.9 + v * 0.1;
            if (mode == Mode::Splotch)
            {
                // Two octaves of value noise, fixed; the field is stretched to the full range below
                t = 0.65 * valueNoise(u * 3.0, v * 2.0) + 0.35 * valueNoise(u * 6.0 + 17.0, v * 4.0 + 5.0);
            }
            field[static_cast<size_t>(y) * kTextureWidth + x] = t;
        }
    }
    if (mode == Mode::Splotch)
    {
        const auto [low, high] = std::minmax_element(field.begin(), field.end());
        const double low_value = *low, span = std::max(*high - *low, 1e-9);
        for (double& t : field)
            t = (t - low_value) / span;
    }

    std::vector<uint8_t> pixels(field.size() * 4);
    for (size_t i = 0; i < field.size(); ++i)
    {
        const Rgb color = sample(colors, field[i]);
        pixels[i * 4 + 0] = color.r;
        pixels[i * 4 + 1] = color.g;
        pixels[i * 4 + 2] = color.b;
        pixels[i * 4 + 3] = 255;
    }
    return pixels;
}

} // namespace xythos::ink
