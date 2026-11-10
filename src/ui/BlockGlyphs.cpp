// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "BlockGlyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <string>

namespace xythos {

bool Glyph::operator==(const Glyph& other) const
{
    if (strokes != other.strokes || dots.size() != other.dots.size())
        return false;
    for (size_t k = 0; k < dots.size(); ++k)
    {
        if (dots[k].x != other.dots[k].x || dots[k].y != other.dots[k].y || dots[k].r != other.dots[k].r)
            return false;
    }
    return true;
}

namespace {

constexpr float kPi = 3.14159265f;

float rad(float degrees) { return degrees * kPi / 180.0f; }

// Builds a glyph from pen moves, lines, curves and shapes, in the pedal frame
class Builder {
public:
    Glyph glyph;

    void move(float x, float y) { glyph.strokes.push_back({ { x, y } }); }
    void line(float x, float y) { glyph.strokes.back().push_back({ x, y }); }

    // A cubic Bézier from the pen
    void curve(float x1, float y1, float x2, float y2, float x3, float y3)
    {
        const auto [x0, y0] = glyph.strokes.back().back();
        constexpr int steps = 12;
        for (int k = 1; k <= steps; ++k)
        {
            const float t = static_cast<float>(k) / steps, u = 1.0f - t;
            line(u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                 u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
        }
    }

    void polyline(std::initializer_list<Glyph::Point> points, bool closed = false)
    {
        glyph.strokes.emplace_back(points);
        if (closed)
            glyph.strokes.back().push_back(*points.begin());
    }

    void rect(float x0, float y0, float x1, float y1) { polyline({ { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } }, true); }

    // An arc from angle a0 to a1 in degrees (0 = right, 90 = down)
    void arc(float cx, float cy, float r, float a0, float a1)
    {
        const int steps = std::max(6, static_cast<int>(std::fabs(a1 - a0) / 10.0f));
        for (int k = 0; k <= steps; ++k)
        {
            const float a = rad(a0 + (a1 - a0) * k / steps);
            const float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
            if (k == 0)
                move(x, y);
            else
                line(x, y);
        }
    }

    void circle(float cx, float cy, float r) { arc(cx, cy, r, 0.0f, 360.0f); }

    // y = f(u) for x from x0 to x1, u running 0..1
    template <class F>
    void plot(float x0, float x1, F f, int steps = 48)
    {
        for (int k = 0; k <= steps; ++k)
        {
            const float u = static_cast<float>(k) / steps;
            if (k == 0)
                move(x0, f(u));
            else
                line(x0 + (x1 - x0) * u, f(u));
        }
    }

    // An arrowhead at the tip, pointing along (dx, dy)
    void arrowhead(float tipX, float tipY, float dx, float dy, float size = 9.0f)
    {
        const float len = std::hypot(dx, dy);
        dx /= len;
        dy /= len;
        const float c = std::cos(rad(150.0f)), s = std::sin(rad(150.0f));
        polyline({ { tipX + size * (dx * c - dy * s), tipY + size * (dx * s + dy * c) },
                   { tipX, tipY },
                   { tipX + size * (dx * c + dy * s), tipY + size * (-dx * s + dy * c) } });
    }

    void dot(float x, float y, float r) { glyph.dots.push_back({ x, y, r }); }
};

float sine(float u, float cycles, float phase = 0.0f) { return std::sin(2.0f * kPi * cycles * u + phase); }

// ---------------------------------------------------------------------------------------------------------------------
// The category symbols (blocks without a symbol of their own, and the first block of each category)

void delayRepeats(Builder& b) // repeats dying away
{
    static constexpr std::pair<float, float> repeats[] = { { 34, 18 }, { 50, 12 }, { 64, 7 }, { 76, 4 }, { 86, 2 } };
    for (const auto& [x, half] : repeats)
        b.polyline({ { x, 98.0f - half }, { x, 98.0f + half } });
}

void reverbArcs(Builder& b) // spreading arcs
{
    for (const float r : { 8.0f, 18.0f, 28.0f })
        b.arc(62.0f, 112.0f, r, 180.0f, 360.0f);
}

void compressorKnee(Builder& b) { b.polyline({ { 30, 114 }, { 52, 88 }, { 94, 84 } }); }

void clippedWave(Builder& b)
{
    b.polyline({ { 28, 108 }, { 38, 84 }, { 50, 84 }, { 68, 112 }, { 80, 112 }, { 94, 88 } });
}

void sineWave(Builder& b)
{
    b.move(28, 98);
    b.curve(37, 78, 50, 78, 62, 98);
    b.curve(74, 118, 86, 118, 96, 98);
}

void bumpAndDip(Builder& b)
{
    b.move(28, 100);
    b.curve(38, 100, 42, 82, 52, 82);
    b.curve(62, 82, 64, 112, 74, 112);
    b.curve(84, 112, 86, 100, 96, 100);
}

void upArrow(Builder& b)
{
    b.polyline({ { 62, 116 }, { 62, 82 } });
    b.polyline({ { 48, 96 }, { 62, 82 }, { 76, 96 } });
}

void sawWave(Builder& b)
{
    b.polyline({ { 28, 112 }, { 50, 84 }, { 50, 112 }, { 74, 84 }, { 74, 112 }, { 96, 84 } });
}

void diBox(Builder& b) // a DI box: the jack in, the XLR out
{
    b.rect(30, 82, 94, 114);
    b.circle(44, 98, 6);
    b.circle(78, 98, 9);
    b.dot(74, 95, 1.6f);
    b.dot(82, 95, 1.6f);
    b.dot(78, 102, 1.6f);
}

void lowPass(Builder& b) // a resonant low-pass
{
    b.move(28, 96);
    b.line(54, 96);
    b.curve(62, 96, 66, 84, 72, 90);
    b.curve(76, 94, 82, 108, 94, 116);
}

// ---------------------------------------------------------------------------------------------------------------------
// One symbol per block type

using Draw = void (*)(Builder&);

const std::map<std::string, Draw>& drawings()
{
    static const std::map<std::string, Draw> map = {
        // DI
        { "studiodi", [](Builder& b) { // a transformer: two windings on an iron core
             for (int k = 0; k < 4; ++k)
             {
                 b.arc(46, 84.0f + 9.0f * k, 4.5f, 270, 450);
                 b.arc(78, 84.0f + 9.0f * k, 4.5f, 90, 270);
             }
             b.polyline({ { 58, 78 }, { 58, 118 } });
             b.polyline({ { 66, 78 }, { 66, 118 } });
         } },
        { "valvedi", [](Builder& b) { // a valve: its glass, the plate, the grid and the cathode
             b.arc(62, 92, 18, 180, 360);
             b.polyline({ { 44, 92 }, { 44, 116 }, { 80, 116 }, { 80, 92 } });
             b.polyline({ { 52, 86 }, { 72, 86 } });
             b.polyline({ { 50, 98 }, { 54, 98 } });
             b.polyline({ { 58, 98 }, { 62, 98 } });
             b.polyline({ { 66, 98 }, { 70, 98 } });
             b.polyline({ { 52, 108 }, { 56, 104 }, { 68, 104 }, { 72, 108 } });
         } },
        { "tubepreamp", [](Builder& b) { // two preamp valves side by side
             for (const float cx : { 46.0f, 78.0f })
             {
                 b.arc(cx, 92, 11, 180, 360);
                 b.polyline({ { cx - 11, 92 }, { cx - 11, 114 }, { cx + 11, 114 }, { cx + 11, 92 } });
                 b.polyline({ { cx - 5, 104 }, { cx, 96 }, { cx + 5, 104 } });
             }
         } },
        { "optodi", [](Builder& b) { // a lamp shining on a light-dependent resistor
             b.circle(44, 98, 9);
             b.polyline({ { 40, 98 }, { 42, 94 }, { 46, 102 }, { 48, 98 } });
             b.polyline({ { 58, 92 }, { 66, 88 } });
             b.polyline({ { 58, 104 }, { 66, 108 } });
             b.polyline({ { 60, 98 }, { 68, 98 } });
             b.polyline({ { 76, 82 }, { 76, 86 }, { 82, 88 }, { 72, 92 }, { 82, 96 }, { 72, 100 }, { 82, 104 },
                          { 72, 108 }, { 76, 110 }, { 76, 114 } });
         } },
        { "hifipreamp", [](Builder& b) { // a mid bell that sweeps
             b.plot(28, 96, [](float u) { return 108.0f - 22.0f * std::exp(-std::pow((u - 0.5f) / 0.14f, 2.0f)); });
             b.polyline({ { 46, 120 }, { 78, 120 } });
             b.arrowhead(46, 120, -1, 0, 6);
             b.arrowhead(78, 120, 1, 0, 6);
         } },
        { "britishpreamp", [](Builder& b) { // a five-band graphic set to a smile
             static constexpr float kKnob[] = { 86, 98, 108, 98, 86 };
             for (int k = 0; k < 5; ++k)
             {
                 const float x = 34.0f + 14.0f * k;
                 b.polyline({ { x, 78 }, { x, 118 } });
                 b.rect(x - 4, kKnob[k] - 2, x + 4, kKnob[k] + 2);
             }
         } },
        { "driverdi", [](Builder& b) { // a speaker cone, driven
             b.polyline({ { 34, 90 }, { 44, 90 }, { 58, 78 }, { 58, 118 }, { 44, 106 }, { 34, 106 } }, true);
             b.arc(62, 98, 10, -50, 50);
             b.arc(62, 98, 20, -45, 45);
             b.plot(78, 96, [](float u) { return 98.0f - 8.0f * std::tanh(3.0f * sine(u, 1.0f)) / std::tanh(3.0f); });
         } },
        { "dualdrive", [](Builder& b) { // two channels side by side: clean above, distorted below
             b.plot(28, 96, [](float u) { return 88.0f - 7.0f * sine(u, 1.5f); });
             b.polyline({ { 28, 110 }, { 36, 110 }, { 36, 102 }, { 50, 102 }, { 50, 116 }, { 64, 116 }, { 64, 102 },
                          { 78, 102 }, { 78, 116 }, { 92, 116 }, { 92, 110 }, { 96, 110 } });
         } },
        { "moderndrive", [](Builder& b) { // a jagged high-gain edge riding on a deep, clean wave
             b.plot(28, 96, [](float u) { return 104.0f - 12.0f * sine(u, 1.0f); });
             b.move(28, 86);
             for (int k = 1; k <= 12; ++k)
                 b.line(28.0f + 5.67f * k, 86.0f + (k % 2 ? -6.0f : 6.0f) - 12.0f * sine(k / 12.0f, 1.0f));
         } },
        { "crossoverdrive", [](Builder& b) { // a low-pass and a high-pass crossing over
             b.move(28, 86);
             b.line(56, 86);
             b.curve(66, 86, 72, 110, 96, 118);
             b.move(28, 118);
             b.curve(52, 110, 58, 86, 68, 86);
             b.line(96, 86);
             b.dot(62, 92, 2.5f);
         } },

        // Distortion
        { "boost", [](Builder& b) { // an arrow lifting the level
             upArrow(b);
             b.polyline({ { 40, 120 }, { 84, 120 } });
         } },
        { "overdrive", [](Builder& b) { // a softly rounded-off wave
             b.plot(28, 96, [](float u) { return 98.0f - 17.0f * std::tanh(2.4f * sine(u, 1.5f)) / std::tanh(2.4f); });
         } },
        { "distortion", clippedWave },
        { "fuzz", [](Builder& b) { // squared right off
             b.polyline({ { 28, 110 }, { 40, 110 }, { 40, 86 }, { 56, 86 }, { 56, 110 }, { 72, 110 }, { 72, 86 },
                          { 88, 86 }, { 88, 110 }, { 96, 110 } });
         } },
        { "gatedfuzz", [](Builder& b) { // a square burst that stops dead
             b.polyline({ { 28, 98 }, { 28, 84 }, { 38, 84 }, { 38, 112 }, { 48, 112 }, { 48, 84 }, { 58, 84 },
                          { 58, 112 }, { 68, 112 }, { 68, 98 }, { 96, 98 } });
         } },
        { "envdrive", [](Builder& b) { // grit growing as you dig in
             b.move(28, 98);
             for (int k = 1; k <= 8; ++k)
             {
                 const float half = 2.0f + 2.2f * k;
                 b.line(28.0f + 8.5f * k, 98.0f + (k % 2 ? -half : half));
             }
         } },
        { "bitcrusher", [](Builder& b) { // a sine in coarse steps
             b.move(28, 98);
             for (int k = 0; k < 8; ++k)
             {
                 const float x = 28.0f + 8.5f * k;
                 const float y = 98.0f - 8.0f * std::round(2.0f * sine((k + 0.5f) / 8.0f, 1.0f));
                 b.line(x, y);
                 b.line(x + 8.5f, y);
             }
             b.line(96, 98);
         } },

        // Dynamics
        { "compressor", compressorKnee },
        { "diodebridge", [](Builder& b) { // a diode bridge: four diodes in a diamond
             const Glyph::Point corners[] = { { 62, 72 }, { 88, 98 }, { 62, 124 }, { 36, 98 } };
             b.polyline({ corners[0], corners[1], corners[2], corners[3] }, true);
             for (int k = 0; k < 4; ++k)
             {
                 // each side's diode, pointing down the diamond: a triangle into a bar
                 const auto [ax, ay] = corners[k];
                 const auto [bx, by] = corners[(k + 1) % 4];
                 const bool down = by > ay;
                 const float dx = (down ? bx - ax : ax - bx) / 36.77f, dy = (down ? by - ay : ay - by) / 36.77f;
                 const float mx = (ax + bx) / 2.0f, my = (ay + by) / 2.0f;
                 b.polyline({ { mx - 4 * dx - 5 * dy, my - 4 * dy + 5 * dx }, { mx + 4 * dx, my + 4 * dy },
                              { mx - 4 * dx + 5 * dy, my - 4 * dy - 5 * dx } }, true);
                 b.polyline({ { mx + 4 * dx - 5 * dy, my + 4 * dy + 5 * dx }, { mx + 4 * dx + 5 * dy, my + 4 * dy - 5 * dx } });
             }
         } },
        { "consolecomp", [](Builder& b) { // a console channel's fader in its slot, with its scale
             b.polyline({ { 62, 70 }, { 62, 122 } });
             b.rect(50, 90, 74, 102);
             b.polyline({ { 50, 96 }, { 74, 96 } });
             for (int k = 0; k < 6; ++k)
             {
                 const float y = 72.0f + 9.6f * k;
                 b.polyline({ { 36, y }, { k % 2 == 0 ? 44.0f : 41.0f, y } });
                 b.polyline({ { 80, y }, { k % 2 == 0 ? 88.0f : 85.0f, y } });
             }
         } },
        { "optoleveler", [](Builder& b) { // the levelling amp's big VU meter, its needle swinging back
             b.rect(26, 72, 98, 122);
             b.arc(62, 124, 38, 235, 305);
             for (int k = 0; k <= 6; ++k)
             {
                 const float a = rad(235.0f + 70.0f * k / 6.0f);
                 b.polyline({ { 62 + 38 * std::cos(a), 124 + 38 * std::sin(a) }, { 62 + 32 * std::cos(a), 124 + 32 * std::sin(a) } });
             }
             b.polyline({ { 62, 120 }, { 62 + 34 * std::cos(rad(258.0f)), 120 + 34 * std::sin(rad(258.0f)) } });
         } },
        { "optostomp", [](Builder& b) { // a photocell, its winding track, and light falling on it
             b.circle(70, 104, 17);
             b.polyline({ { 58, 96 }, { 82, 96 }, { 82, 101 }, { 58, 101 }, { 58, 107 }, { 82, 107 }, { 82, 112 }, { 58, 112 } });
             b.polyline({ { 30, 68 }, { 48, 86 } });
             b.arrowhead(48, 86, 1, 1, 7);
             b.polyline({ { 42, 66 }, { 58, 82 } });
             b.arrowhead(58, 82, 1, 1, 7);
         } },
        { "fetlimiter", [](Builder& b) { // a field-effect transistor: the channel, drain, source and the gate's arrow
             b.polyline({ { 62, 82 }, { 62, 114 } });
             b.polyline({ { 62, 86 }, { 84, 86 }, { 84, 68 } });
             b.polyline({ { 62, 110 }, { 84, 110 }, { 84, 124 } });
             b.polyline({ { 32, 104 }, { 62, 104 } });
             b.arrowhead(61, 104, 1, 0, 8);
         } },
        { "squashcomp", [](Builder& b) { // a ball squashed flat between two plates
             b.polyline({ { 28, 84 }, { 96, 84 } });
             b.polyline({ { 28, 114 }, { 96, 114 } });
             b.plot(34, 90, [](float u) { return 99.0f - 14.0f * std::sin(kPi * u); });
             b.plot(34, 90, [](float u) { return 99.0f + 14.0f * std::sin(kPi * u); });
             b.polyline({ { 62, 64 }, { 62, 78 } });
             b.arrowhead(62, 79, 0, 1, 7);
         } },
        { "gate", [](Builder& b) { // a ringing note shut off by the gate
             b.plot(28, 72, [](float u) { return 98.0f - 16.0f * (1.0f - 0.6f * u) * sine(u, 2.0f); });
             b.line(96, 98);
             b.polyline({ { 72, 80 }, { 72, 116 } });
         } },
        { "volume", [](Builder& b) { b.polyline({ { 28, 116 }, { 96, 116 }, { 96, 80 } }, true); } },
        { "swell", [](Builder& b) { b.polyline({ { 96, 80 }, { 28, 98 }, { 96, 116 } }); } }, // a crescendo
        { "killswitch", [](Builder& b) { // a power symbol
             b.arc(62, 100, 19, 305, 595);
             b.polyline({ { 62, 76 }, { 62, 98 } });
         } },

        // EQ
        { "eq.graphic", [](Builder& b) { // a row of bands
             static constexpr float tops[] = { 100, 86, 94, 80, 104 };
             for (int k = 0; k < 5; ++k)
                 b.polyline({ { 32.0f + 15.0f * k, 118 }, { 32.0f + 15.0f * k, tops[k] } });
         } },
        { "eq.parametric", bumpAndDip },

        // Modulation
        { "chorus", [](Builder& b) { // two voices drifting across each other
             b.plot(28, 96, [](float u) { return 98.0f - 13.0f * sine(u, 1.5f); });
             b.plot(28, 96, [](float u) { return 98.0f - 13.0f * sine(u, 1.5f, 1.8f); });
         } },
        { "flanger", [](Builder& b) { // a jet sweep
             b.plot(28, 96, [](float u) { return 98.0f - 14.0f * std::sin(2.0f * kPi * (0.6f * u + 1.4f * u * u)); },
                    64);
         } },
        { "phaser", [](Builder& b) { // the notches it sweeps
             b.polyline({ { 28, 88 }, { 38, 88 }, { 45, 112 }, { 52, 88 }, { 72, 88 }, { 79, 112 }, { 86, 88 },
                          { 96, 88 } });
         } },
        { "barberpole", [](Builder& b) { // a barber's pole
             b.rect(50, 76, 74, 120);
             b.polyline({ { 50, 92 }, { 74, 80 } });
             b.polyline({ { 50, 106 }, { 74, 94 } });
             b.polyline({ { 50, 120 }, { 74, 108 } });
         } },
        { "modsweeper", [](Builder& b) { // one wave turning into another: sine, triangle, square
             b.plot(28, 50, [](float u) { return 98.0f - 12.0f * sine(u, 1.0f); }, 16);
             b.line(56, 86);
             b.line(68, 110);
             b.line(74, 98);
             b.line(74, 86);
             b.line(85, 86);
             b.line(85, 110);
             b.line(96, 110);
             b.line(96, 98);
         } },
        { "tremolo", [](Builder& b) { // a wave pulsing in level
             b.plot(28, 96, [](float u) {
                 const float level = 0.25f + 0.75f * std::fabs(sine(u, 1.0f, 0.0f));
                 return 98.0f - 18.0f * level * sine(u, 4.0f);
             }, 96);
         } },
        { "rotary", [](Builder& b) { // a spinning speaker
             b.arc(62, 98, 19, 200, 520);
             const float a = rad(520);
             b.arrowhead(62.0f + 19.0f * std::cos(a), 98.0f + 19.0f * std::sin(a), -std::sin(a), std::cos(a));
             b.dot(62, 98, 4);
         } },
        { "doubler", [](Builder& b) { // the same part played twice
             b.plot(28, 96, [](float u) { return 89.0f - 8.0f * sine(u, 1.5f); });
             b.plot(28, 96, [](float u) { return 109.0f - 8.0f * sine(u, 1.5f); });
         } },
        { "ringmod", [](Builder& b) { // a ring and a multiply sign
             b.circle(62, 98, 19);
             b.polyline({ { 53, 89 }, { 71, 107 } });
             b.polyline({ { 71, 89 }, { 53, 107 } });
         } },
        { "warble", [](Builder& b) { // a worn cassette
             b.rect(28, 80, 96, 116);
             b.circle(46, 95, 7);
             b.circle(78, 95, 7);
             b.polyline({ { 40, 116 }, { 46, 108 }, { 78, 108 }, { 84, 116 } });
         } },
        { "slicer", [](Builder& b) { // a wave chopped into slices
             for (int k = 0; k < 3; ++k)
             {
                 const float x0 = 28.0f + 26.0f * k;
                 b.plot(x0, x0 + 16.0f, [](float u) { return 98.0f - 14.0f * sine(u, 1.0f); }, 16);
             }
         } },

        // Reverb
        { "reverb", reverbArcs },
        { "reverb.spring", [](Builder& b) { // a spring
             b.polyline({ { 28, 98 }, { 36, 98 }, { 40, 84 }, { 48, 112 }, { 56, 84 }, { 64, 112 }, { 72, 84 },
                          { 80, 112 }, { 84, 98 }, { 96, 98 } });
             b.dot(28, 98, 4);
             b.dot(96, 98, 4);
         } },
        { "shimmer", [](Builder& b) { // sparkles
             const auto star = [&b](float cx, float cy, float r) {
                 const float k = 0.12f * r;
                 b.move(cx, cy - r);
                 b.curve(cx + k, cy - k, cx + k, cy - k, cx + r, cy);
                 b.curve(cx + k, cy + k, cx + k, cy + k, cx, cy + r);
                 b.curve(cx - k, cy + k, cx - k, cy + k, cx - r, cy);
                 b.curve(cx - k, cy - k, cx - k, cy - k, cx, cy - r);
             };
             star(56, 102, 21);
             star(85, 81, 9);
         } },
        { "freeze", [](Builder& b) { // a snowflake
             for (int k = 0; k < 3; ++k)
             {
                 const float a = rad(90.0f + 60.0f * k), c = std::cos(a), s = std::sin(a);
                 b.polyline({ { 62.0f - 23.0f * c, 98.0f - 23.0f * s }, { 62.0f + 23.0f * c, 98.0f + 23.0f * s } });
             }
             for (int k = 0; k < 6; ++k)
             {
                 const float a = rad(90.0f + 60.0f * k);
                 const float bx = 62.0f + 14.0f * std::cos(a), by = 98.0f + 14.0f * std::sin(a);
                 const float l = rad(90.0f + 60.0f * k - 40.0f), r = rad(90.0f + 60.0f * k + 40.0f);
                 b.polyline({ { bx + 8.0f * std::cos(l), by + 8.0f * std::sin(l) },
                              { bx, by },
                              { bx + 8.0f * std::cos(r), by + 8.0f * std::sin(r) } });
             }
         } },
        { "bloomreverb", [](Builder& b) { // a flower opening
             for (int k = 0; k <= 120; ++k)
             {
                 const float t = kPi * k / 120.0f;
                 const float r = 23.0f * std::cos(5.0f * t);
                 const float x = 62.0f + r * std::cos(t - kPi / 2), y = 98.0f + r * std::sin(t - kPi / 2);
                 if (k == 0)
                     b.move(x, y);
                 else
                     b.line(x, y);
             }
         } },
        { "gatedreverb", [](Builder& b) { // reverb cut short
             for (const float r : { 10.0f, 20.0f, 30.0f })
                 b.arc(70, 116, r, 180, 270);
             b.polyline({ { 76, 78 }, { 76, 120 } });
         } },

        // Delay
        { "delay", delayRepeats },
        { "doppelganger", [](Builder& b) { // a twin
             b.circle(52, 98, 17);
             b.circle(72, 98, 17);
         } },
        { "tapeecho", [](Builder& b) { // tape reels
             b.circle(42, 92, 13);
             b.circle(82, 92, 13);
             b.polyline({ { 42, 105 }, { 82, 105 } });
             b.dot(42, 92, 3);
             b.dot(82, 92, 3);
         } },
        { "duckdelay", [](Builder& b) { // a duck
             b.circle(74, 84, 9);
             b.polyline({ { 82, 81 }, { 94, 86 }, { 82, 89 } });
             b.move(68, 92);
             b.curve(84, 98, 86, 116, 64, 116);
             b.line(46, 116);
             b.curve(34, 116, 28, 106, 30, 94);
             b.curve(40, 100, 52, 98, 60, 98);
             b.dot(76, 82, 2);
         } },
        { "reversedelay", [](Builder& b) { // repeats that grow instead of fading
             static constexpr std::pair<float, float> repeats[] = { { 38, 2 }, { 48, 4 }, { 60, 7 }, { 74, 12 },
                                                                    { 90, 18 } };
             for (const auto& [x, half] : repeats)
                 b.polyline({ { x, 98.0f - half }, { x, 98.0f + half } });
         } },
        { "pitchdelay", [](Builder& b) { // repeats climbing in pitch
             for (int k = 0; k < 5; ++k)
             {
                 const float x = 32.0f + 15.0f * k, y = 112.0f - 7.0f * k;
                 b.polyline({ { x, y - 7.0f }, { x, y + 7.0f } });
             }
         } },
        { "beatrepeat", [](Builder& b) { // repeat signs
             for (const float x : { 32.0f, 92.0f })
                 b.polyline({ { x, 80 }, { x, 116 } });
             b.polyline({ { 40, 80 }, { 40, 116 } });
             b.polyline({ { 84, 80 }, { 84, 116 } });
             for (const float x : { 50.0f, 74.0f })
             {
                 b.dot(x, 91, 3.5f);
                 b.dot(x, 105, 3.5f);
             }
         } },
        { "looper", [](Builder& b) { // round and round: an infinity sign
             for (int k = 0; k <= 72; ++k)
             {
                 const float t = 2.0f * kPi * k / 72.0f, d = 1.0f + std::sin(t) * std::sin(t);
                 const float x = 62.0f + 31.0f * std::cos(t) / d, y = 98.0f + 31.0f * std::sin(t) * std::cos(t) / d;
                 if (k == 0)
                     b.move(x, y);
                 else
                     b.line(x, y);
             }
         } },

        // Pitch/Synth
        { "octave", [](Builder& b) { // two notes an octave apart
             b.circle(46, 112, 6);
             b.circle(74, 96, 6);
             b.polyline({ { 52, 112 }, { 52, 84 }, { 80, 72 }, { 80, 96 } });
         } },
        { "polyoctave", [](Builder& b) { // a stacked chord
             b.circle(52, 116, 5);
             b.circle(70, 99, 5);
             b.circle(52, 82, 5);
             b.polyline({ { 75, 99 }, { 75, 70 } });
         } },
        { "pitch", [](Builder& b) { // up or down
             b.polyline({ { 50, 118 }, { 50, 80 } });
             b.arrowhead(50, 78, 0, -1, 12);
             b.polyline({ { 74, 78 }, { 74, 116 } });
             b.arrowhead(74, 118, 0, 1, 12);
         } },
        { "whammy", [](Builder& b) { // a bend
             b.move(28, 114);
             b.curve(62, 114, 84, 106, 90, 80);
             b.arrowhead(90, 78, 0.2f, -1, 12);
         } },
        { "harmonizer", [](Builder& b) { // a second voice above the first
             b.plot(28, 96, [](float u) { return 86.0f - 7.0f * sine(u, 3.0f); }, 64);
             b.plot(28, 96, [](float u) { return 110.0f - 7.0f * sine(u, 2.0f); });
         } },
        { "detune", [](Builder& b) { // a tuning fork
             b.move(52, 76);
             b.line(52, 96);
             b.curve(52, 108, 72, 108, 72, 96);
             b.line(72, 76);
             b.polyline({ { 62, 105 }, { 62, 122 } });
         } },
        { "capo", [](Builder& b) { // a capo on the neck
             b.polyline({ { 28, 91 }, { 96, 91 } });
             b.polyline({ { 28, 105 }, { 96, 105 } });
             b.rect(55, 76, 69, 120);
         } },
        { "arpeggiator", [](Builder& b) { // a rising run of notes
             for (int k = 0; k < 4; ++k)
             {
                 const float x = 34.0f + 16.0f * k, y = 114.0f - 10.0f * k;
                 b.dot(x, y, 5);
                 b.polyline({ { x + 5.0f, y }, { x + 5.0f, y - 18.0f } });
             }
         } },
        { "organ", [](Builder& b) { // organ pipes
             b.rect(36, 90, 48, 120);
             b.rect(56, 76, 68, 120);
             b.rect(76, 86, 88, 120);
         } },
        { "sustainer", [](Builder& b) { // a fermata: hold the note
             b.arc(62, 108, 25, 180, 360);
             b.dot(62, 101, 4.5f);
         } },
        { "synth", [](Builder& b) { // a keyboard
             b.rect(28, 80, 96, 116);
             for (const float x : { 45.0f, 62.0f, 79.0f })
                 b.polyline({ { x, 100 }, { x, 116 } });
             b.rect(40, 80, 50, 100);
             b.rect(74, 80, 84, 100);
         } },
        { "supersaw", [](Builder& b) { // a stack of saws
             for (const float y : { 88.0f, 110.0f })
             {
                 b.move(28, y + 7);
                 for (int k = 0; k < 3; ++k)
                 {
                     const float x = 28.0f + 22.0f * (k + 1);
                     b.line(x, y - 7);
                     b.line(x, y + 7);
                 }
             }
         } },
        { "fmsynth", [](Builder& b) { // a bell
             b.move(40, 112);
             b.curve(48, 106, 44, 80, 62, 79);
             b.curve(80, 80, 76, 106, 84, 112);
             b.line(40, 112);
             b.dot(62, 118, 4);
         } },
        { "foldsynth", [](Builder& b) { // a wave folded over on itself
             b.plot(28, 96, [](float u) { return 98.0f - 17.0f * std::sin(2.6f * sine(u, 1.0f)); }, 64);
         } },
        { "acidsynth", [](Builder& b) { // the acid smiley
             b.circle(62, 98, 23);
             b.dot(54, 91, 3.5f);
             b.dot(70, 91, 3.5f);
             b.arc(62, 98, 13, 25, 155);
         } },
        { "stringsynth", [](Builder& b) { // strings and a bow
             for (const float x : { 44.0f, 56.0f, 68.0f, 80.0f })
                 b.polyline({ { x, 76 }, { x, 120 } });
             b.polyline({ { 28, 116 }, { 96, 86 } });
         } },
        { "chipsynth", [](Builder& b) { // a game pad's cross
             b.polyline({ { 54, 76 }, { 70, 76 }, { 70, 90 }, { 84, 90 }, { 84, 106 }, { 70, 106 }, { 70, 120 },
                          { 54, 120 }, { 54, 106 }, { 40, 106 }, { 40, 90 }, { 54, 90 } },
                        true);
         } },
        { "theremin", [](Builder& b) { // the cabinet, its pitch rod and its volume loop
             b.rect(30, 108, 94, 122);
             b.polyline({ { 84, 108 }, { 84, 66 } });
             b.polyline({ { 42, 108 }, { 42, 100 } });
             b.circle(42, 90, 10);
         } },
        { "synclead", [](Builder& b) { // a saw restarted by each cycle of the note
             for (const float x0 : { 26.0f, 62.0f })
             {
                 b.polyline({ { x0, 116 }, { x0 + 14, 82 }, { x0 + 14, 116 }, { x0 + 28, 82 }, { x0 + 28, 116 },
                              { x0 + 36, 100 }, { x0 + 36, 116 } });
             }
         } },
        { "pdsynth", [](Builder& b) { // a cosine bent into a saw
             b.plot(28, 96, [](float u) {
                 constexpr float m = 0.18f;
                 const float q = u < m ? 0.5f * u / m : 0.5f + 0.5f * (u - m) / (1.0f - m);
                 return 98.0f + 18.0f * std::cos(2.0f * kPi * q);
             }, 60);
         } },
        { "wavetable", [](Builder& b) { // waves one behind the other, changing shape
             b.plot(44, 100, [](float u) { return 82.0f - 7.0f * sine(u, 2.0f) - 3.0f * sine(u, 6.0f); });
             b.plot(36, 92, [](float u) { return 98.0f - 7.0f * sine(u, 2.0f) - 2.0f * sine(u, 4.0f); });
             b.plot(28, 84, [](float u) { return 114.0f - 7.0f * sine(u, 2.0f); });
         } },
        { "subbass", [](Builder& b) { // a deep, slow wave under a faster one
             b.plot(26, 98, [](float u) { return 104.0f - 18.0f * sine(u, 1.0f); });
             b.plot(26, 98, [](float u) { return 76.0f - 5.0f * sine(u, 4.0f); }, 64);
         } },
        { "laser", [](Builder& b) { // a burst and a beam diving into a slower wave
             for (int k = 0; k < 6; ++k)
             {
                 const float a = rad(60.0f * k);
                 b.polyline({ { 32 + 4 * std::cos(a), 98 + 4 * std::sin(a) }, { 32 + 10 * std::cos(a), 98 + 10 * std::sin(a) } });
             }
             b.plot(44, 100, [](float u) { return 98.0f - 12.0f * std::sin(2.0f * kPi * 6.0f * (1.0f - (1.0f - u) * (1.0f - u))); }, 96);
         } },
        { "drone", [](Builder& b) { // three held notes: root, fifth, octave
             for (const auto& [y, x1] : { std::pair { 112.0f, 96.0f }, std::pair { 98.0f, 88.0f }, std::pair { 84.0f, 80.0f } })
             {
                 b.polyline({ { 36, y }, { x1, y } });
                 b.dot(32, y, 3);
             }
         } },
        { "harmonicpainter", [](Builder& b) { // harmonics as bars, under a brush stroke
             for (int k = 0; k < 8; ++k)
             {
                 const float x = 32.0f + 8.0f * k;
                 const float h = 34.0f / (1.0f + 0.45f * k);
                 b.polyline({ { x, 120 }, { x, 120 - h } });
             }
             b.move(30, 78);
             b.curve(48, 70, 70, 92, 94, 76);
         } },

        // Filter
        { "filter", lowPass },
        { "robotfilter", [](Builder& b) { // a robot
             b.rect(40, 84, 84, 120);
             b.dot(52, 98, 4);
             b.dot(72, 98, 4);
             b.polyline({ { 52, 110 }, { 72, 110 } });
             b.polyline({ { 62, 84 }, { 62, 77 } });
             b.dot(62, 76, 3);
         } },

        // Wah
        { "wah", [](Builder& b) { // a rocking treadle
             b.polyline({ { 30, 104 }, { 92, 82 }, { 94, 89 }, { 32, 111 } }, true);
             b.polyline({ { 66, 99 }, { 66, 118 } });
             b.polyline({ { 30, 118 }, { 94, 118 } });
         } },
        { "autowah", [](Builder& b) { // a resonant peak moving on its own
             b.move(28, 112);
             b.line(46, 110);
             b.curve(56, 106, 58, 78, 62, 78);
             b.curve(66, 78, 68, 106, 78, 110);
             b.line(96, 112);
             b.polyline({ { 40, 121 }, { 84, 121 } });
             b.arrowhead(38, 121, -1, 0, 7);
             b.arrowhead(86, 121, 1, 0, 7);
         } },
        { "talkbox", [](Builder& b) { // a speech bubble
             b.polyline({ { 30, 78 }, { 94, 78 }, { 94, 108 }, { 56, 108 }, { 42, 120 }, { 46, 108 }, { 30, 108 } },
                        true);
             b.plot(40, 84, [](float u) { return 93.0f - 6.0f * sine(u, 2.0f); }, 32);
         } },

        // Added in the third set
        { "jawari", [](Builder& b) { // a string buzzing on a curved bridge
             b.arc(62, 128, 26, 220, 320);
             b.move(28, 104);
             b.line(50, 104);
             for (int k = 0; k < 6; ++k)
                 b.line(52.0f + 4.0f * k, k % 2 ? 104.0f : 98.0f);
             b.line(96, 104);
         } },
        { "acoustic", [](Builder& b) { // an acoustic guitar's body and sound hole
             b.circle(62, 108, 13);
             b.circle(62, 86, 9);
             b.circle(62, 104, 4);
         } },
        { "swarm", [](Builder& b) { // a bee
             b.circle(62, 102, 10);
             b.polyline({ { 56, 94 }, { 56, 110 } });
             b.polyline({ { 64, 92 }, { 64, 112 } });
             b.circle(54, 86, 6);
             b.circle(70, 86, 6);
         } },
        { "chaosmod", [](Builder& b) { // the Lorenz butterfly
             for (int k = 0; k <= 64; ++k)
             {
                 // a lemniscate: two lobes around the attractor's two eyes
                 const float t = 2.0f * kPi * k / 64.0f;
                 const float d = 1.0f + std::cos(t) * std::cos(t);
                 const float x = 62.0f + 30.0f * std::sin(t) / d, y = 98.0f + 16.0f * std::sin(t) * std::cos(t) / d;
                 if (k == 0)
                     b.move(x, y);
                 else
                     b.line(x, y);
             }
             b.dot(48, 98, 2.5f);
             b.dot(76, 98, 2.5f);
         } },
        { "sympathetic", [](Builder& b) { // a harp: strings under a curved neck
             b.move(32, 80);
             b.curve(50, 72, 70, 92, 92, 84);
             for (int k = 0; k < 7; ++k)
             {
                 const float x = 36.0f + 9.0f * k;
                 const float u = (x - 32.0f) / 60.0f;
                 const float top = 80.0f + 4.0f * u + 4.0f * std::sin(kPi * u);
                 b.polyline({ { x, top }, { x, 120 } });
             }
         } },
        { "graincloud", [](Builder& b) { // a cloud of grains
             static constexpr float grains[][3] = { { 36, 92, 2.5f }, { 46, 84, 3.5f }, { 50, 102, 2.0f },
                                                    { 60, 92, 4.5f }, { 66, 108, 2.5f }, { 72, 82, 2.0f },
                                                    { 78, 98, 3.5f }, { 88, 90, 2.0f }, { 86, 110, 3.0f },
                                                    { 40, 112, 3.0f } };
             for (const auto& g : grains)
                 b.dot(g[0], g[1], g[2]);
             b.arc(62, 140, 40, 235, 305); // the sound they rise from
         } },
        { "tapestop", [](Builder& b) { // a reel winding down
             b.circle(52, 96, 16);
             for (const float a : { 0.0f, 120.0f, 240.0f })
                 b.polyline({ { 52, 96 }, { 52.0f + 13.0f * std::cos(rad(a)), 96.0f + 13.0f * std::sin(rad(a)) } });
             b.move(72, 82);
             b.curve(84, 82, 90, 100, 94, 116);
             b.arrowhead(94, 116, 0.3f, 1.0f, 7);
         } },
        { "bouncedelay", [](Builder& b) { // a ball bouncing lower each time
             b.dot(32, 82, 4);
             b.move(32, 86);
             float x = 32.0f, w = 26.0f, h = 30.0f;
             for (int k = 0; k < 4; ++k)
             {
                 b.curve(x + w * 0.25f, 120, x + w * 0.5f, 120, x + w * 0.5f, 118);
                 b.curve(x + w * 0.5f, 118 - h, x + w, 118 - h, x + w, 118);
                 x += w;
                 w *= 0.6f;
                 h *= 0.55f;
             }
         } },
        { "patterndelay", [](Builder& b) { // a step pattern: some steps sound, some don't
             static constexpr bool steps[8] = { true, false, true, true, false, true, false, true };
             for (int k = 0; k < 8; ++k)
             {
                 const float x = 32.0f + 8.5f * k;
                 if (steps[k])
                     b.dot(x, 98, 3.5f);
                 else
                     b.polyline({ { x - 2.5f, 98 }, { x + 2.5f, 98 } });
             }
             b.polyline({ { 28, 112 }, { 96, 112 } });
         } },
        { "strum", [](Builder& b) { // a strum across the strings
             for (int k = 0; k < 5; ++k)
                 b.polyline({ { 30, 82.0f + 8.0f * k }, { 94, 82.0f + 8.0f * k } });
             b.polyline({ { 50, 76 }, { 74, 120 } });
             b.arrowhead(74, 120, 24, 44);
         } },
        { "hardtune", [](Builder& b) { // a gliding note snapped into steps
             b.plot(28, 96, [](float u) { return 98.0f - 14.0f * sine(u, 1.0f); }, 24);
             b.move(28, 98);
             for (int k = 0; k < 8; ++k)
             {
                 const float x = 28.0f + 8.5f * k;
                 const float y = 98.0f - 7.0f * std::round(2.0f * sine((k + 0.5f) / 8.0f, 1.0f));
                 b.line(x, y + 14.0f);
                 b.line(x + 8.5f, y + 14.0f);
             }
         } },
        { "trumpet", [](Builder& b) { // a trumpet
             b.polyline({ { 28, 100 }, { 70, 100 } });
             b.polyline({ { 28, 106 }, { 70, 106 } });
             b.polyline({ { 70, 100 }, { 94, 86 }, { 94, 120 }, { 70, 106 } });
             for (const float x : { 42.0f, 50.0f, 58.0f })
                 b.polyline({ { x, 100 }, { x, 88 } });
         } },
        { "violin", [](Builder& b) { // a violin's f-hole and its bow
             b.move(58, 80);
             b.curve(70, 78, 70, 92, 62, 98);
             b.curve(54, 104, 54, 118, 66, 116);
             b.dot(58, 80, 2.5f);
             b.dot(66, 116, 2.5f);
             b.polyline({ { 30, 118 }, { 94, 80 } });
         } },
        { "piano", [](Builder& b) { // piano keys
             b.rect(30, 82, 94, 118);
             for (int k = 1; k < 6; ++k)
                 b.polyline({ { 30.0f + 64.0f * k / 6.0f, 82 }, { 30.0f + 64.0f * k / 6.0f, 118 } });
             for (const int k : { 1, 2, 4, 5 })
                 b.rect(30.0f + 64.0f * k / 6.0f - 3.0f, 82, 30.0f + 64.0f * k / 6.0f + 3.0f, 102);
         } },
        { "drumtrigger", [](Builder& b) { // a drum and its sticks
             // the head (an ellipse), the shell and the bottom rim
             for (int k = 0; k <= 32; ++k)
             {
                 const float t = 2.0f * kPi * k / 32.0f;
                 const float x = 62.0f + 20.0f * std::cos(t), y = 98.0f + 6.0f * std::sin(t);
                 if (k == 0)
                     b.move(x, y);
                 else
                     b.line(x, y);
             }
             b.polyline({ { 42, 98 }, { 42, 114 } });
             b.polyline({ { 82, 98 }, { 82, 114 } });
             b.plot(42, 82, [](float u) { return 114.0f + 6.0f * std::sin(kPi * u); }, 16);
             b.polyline({ { 44, 76 }, { 64, 94 } });
             b.polyline({ { 80, 76 }, { 60, 94 } });
         } },
        { "midiout", [](Builder& b) { // a MIDI socket: five pins in a ring, and the key
             b.circle(62, 100, 20);
             for (const float a : { 180.0f, 225.0f, 270.0f, 315.0f, 360.0f })
                 b.dot(62.0f + 12.0f * std::cos(rad(a)), 103.0f + 12.0f * std::sin(rad(a)), 2.5f);
             b.rect(58, 116, 66, 120);
         } },
        { "fart", [](Builder& b) { // a puff of wind
             b.arc(52, 104, 10, 90, 300);
             b.arc(66, 96, 10, 200, 380);
             b.arc(74, 110, 9, 260, 450);
             b.polyline({ { 30, 82 }, { 40, 86 } });
             b.polyline({ { 28, 96 }, { 38, 97 } });
             b.polyline({ { 30, 112 }, { 40, 109 } });
         } },
    };
    return map;
}

} // namespace

const Glyph* blockGlyph(const char* typeId)
{
    static std::map<std::string, Glyph> cache;
    if (typeId == nullptr)
        return nullptr;
    auto it = cache.find(typeId);
    if (it == cache.end())
    {
        const auto drawing = drawings().find(typeId);
        if (drawing == drawings().end())
            return nullptr;
        Builder b;
        drawing->second(b);
        it = cache.emplace(typeId, std::move(b.glyph)).first;
    }
    return &it->second;
}

const Glyph& categoryGlyph(Category category)
{
    static std::map<Category, Glyph> cache;
    auto it = cache.find(category);
    if (it == cache.end())
    {
        Builder b;
        switch (category)
        {
        case Category::Di:         diBox(b); break;
        case Category::Delay:      delayRepeats(b); break;
        case Category::Reverb:     reverbArcs(b); break;
        case Category::Dynamics:   compressorKnee(b); break;
        case Category::Distortion: clippedWave(b); break;
        case Category::Modulation: sineWave(b); break;
        case Category::Eq:         bumpAndDip(b); break;
        case Category::Pitch:      upArrow(b); break;
        case Category::Synth:      sawWave(b); break;
        case Category::Filter:
        case Category::Wah:        lowPass(b); break;
        default:                   b.circle(62, 98, 14); break; // a plain ring
        }
        it = cache.emplace(category, std::move(b.glyph)).first;
    }
    return it->second;
}

} // namespace xythos
