// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "state/Ink.hpp"
#include "state/Settings.hpp"


using namespace xythos;
using namespace xythos::ink;

TEST_CASE("every theme's paint, accent and text are light enough to read on the window")
{
    for (const Theme& theme : themes())
    {
        CAPTURE(theme.name);
        CHECK(theme.colors.size() >= 2);
        CHECK(theme.colors.size() <= 3);
        // the paint's colours are lifted when drawn; what the text uses must be light as it is
        for (const Rgb color : theme.colors)
            CHECK(isLightEnough(lighten(color)));
        CHECK(isLightEnough(theme.accent));
        CHECK(isLightEnough(theme.text));
    }
}

TEST_CASE("the themes offered, in order: Iris first, then Kaia's")
{
    const auto& list = themes();
    const std::vector<std::string_view> names = { "Iris", "Ember", "Abyss", "Phosphor", "Synthwave", "Gold Leaf",
                                                  "Aurora", "Blood Moon", "Glacier", "Graphite", "Oxide" };
    REQUIRE(list.size() == names.size());
    for (size_t i = 0; i < names.size(); ++i)
        CHECK(list[i].name == names[i]);
    // Iris is the look Xythos always had: Splotchy in the owner's colours, white accent
    const Theme& iris = list[0];
    CHECK(iris.mode == Mode::Splotch);
    CHECK(toHex(iris.colors[0]) == "#7278fe");
    CHECK(toHex(iris.colors[1]) == "#c688d6");
    CHECK(toHex(iris.colors[2]) == "#a85eea");
    CHECK(toHex(iris.accent) == "#ffffff");
    CHECK(toHex(iris.text) == "#e6e6ea");
    CHECK(toHex(iris.textDim) == "#8a8a94");
    CHECK(iris.glowA == 0);
    CHECK(!iris.scanlines);
    CHECK(themeNamed("Phosphor").scanlines);
    CHECK(themeNamed("Gold Leaf").name == "Gold Leaf");
    CHECK(themeNamed("Default").name == "Iris"); // unknown names get the default
    CHECK(themeNamed("").name == "Iris");
}

TEST_CASE("dark colours are lifted toward white, light ones are kept")
{
    CHECK(isLightEnough(Rgb { 0xff, 0xff, 0xff }));
    CHECK_FALSE(isLightEnough(Rgb { 0x00, 0x00, 0x00 }));
    CHECK_FALSE(isLightEnough(Rgb { 0x20, 0x10, 0x60 }));

    const Rgb light { 0x7b, 0x5c, 0xff };
    CHECK(lighten(light) == light);

    for (const Rgb dark : { Rgb { 0, 0, 0 }, Rgb { 0x20, 0x10, 0x60 }, Rgb { 0, 0, 0x30 }, Rgb { 0x10, 0x40, 0 } })
    {
        const Rgb lifted = lighten(dark);
        CHECK(isLightEnough(lifted));
        // Lifted toward white: no channel goes down
        CHECK(lifted.r >= dark.r);
        CHECK(lifted.g >= dark.g);
        CHECK(lifted.b >= dark.b);
    }
}

TEST_CASE("hex colours parse and format")
{
    CHECK(toHex(Rgb { 0xff, 0x6a, 0x3d }) == "#ff6a3d");
    CHECK(parseHex("#ff6a3d") == Rgb { 0xff, 0x6a, 0x3d });
    CHECK(parseHex("FF6A3D") == Rgb { 0xff, 0x6a, 0x3d });
    CHECK_FALSE(parseHex("#ff6a3").has_value());
    CHECK_FALSE(parseHex("#ff6a3g").has_value());
    CHECK_FALSE(parseHex("").has_value());
}

TEST_CASE("sampling interpolates evenly spaced colours")
{
    const std::vector<Rgb> colors { { 0, 0, 0 }, { 255, 0, 0 }, { 255, 255, 255 } };
    CHECK(sample(colors, 0.0) == Rgb { 0, 0, 0 });
    CHECK(sample(colors, 0.5) == Rgb { 255, 0, 0 });
    CHECK(sample(colors, 1.0) == Rgb { 255, 255, 255 });
    CHECK(sample(colors, 0.25).r == 128);
    CHECK(sample(colors, 2.0) == Rgb { 255, 255, 255 });
    CHECK(sample({ { 9, 8, 7 } }, 0.7) == Rgb { 9, 8, 7 });
}

TEST_CASE("the smooth texture runs from the first colour to the last")
{
    const std::vector<Rgb> colors { { 0xff, 0x6a, 0x3d }, { 0xff, 0x2e, 0x88 }, { 0x7b, 0x5c, 0xff } };
    const auto pixels = makeTexture(Mode::Smooth, colors);
    REQUIRE(pixels.size() == static_cast<size_t>(kTextureWidth * kTextureHeight * 4));
    // Top-left is the first colour, the bottom-right corner the last (the tilt puts the top-right at about 0.9)
    const auto left = pixels.begin();
    const auto right = pixels.begin() + ((kTextureHeight - 1) * kTextureWidth + kTextureWidth - 1) * 4;
    CHECK(left[0] == 0xff);
    CHECK(left[1] < 0x90);
    CHECK(right[2] >= 0xf0); // t is about 0.996 at the very corner
    for (size_t i = 3; i < pixels.size(); i += 4)
        CHECK(pixels[i] == 255);
}

TEST_CASE("the splotch texture uses every colour and is the same every time")
{
    const std::vector<Rgb> colors { { 0xff, 0x6a, 0x3d }, { 0x00, 0xd2, 0xff }, { 0x6f, 0x5c, 0xff } };
    const auto first = makeTexture(Mode::Splotch, colors);
    const auto second = makeTexture(Mode::Splotch, colors);
    CHECK(first == second);

    bool hasFirst = false, hasLast = false;
    for (size_t i = 0; i < first.size(); i += 4)
    {
        const Rgb pixel { first[i], first[i + 1], first[i + 2] };
        hasFirst = hasFirst || pixel == colors.front();
        hasLast = hasLast || pixel == colors.back();
    }
    CHECK(hasFirst);
    CHECK(hasLast);
}

TEST_CASE("the theme setting round-trips and defaults to Iris")
{
    Settings defaults;
    CHECK(defaults.theme == "Iris");

    Settings settings;
    settings.theme = "Synthwave";
    CHECK(Settings::fromJson(settings.toJson()).theme == "Synthwave");

    // the accent and highlight keys it replaced are ignored
    const Settings old = Settings::fromJson(
        R"({"accent": 2, "accentColor": "#b8bcff", "highlightStyle": 0, "highlightColors": ["#ff6a3d", "#ff2e88"]})");
    CHECK(old == defaults);
    CHECK(old.toJson().find("highlight") == std::string::npos);
}

TEST_CASE("HSV converts both ways")
{
    CHECK(fromHsv({ 0.0, 1.0, 1.0 }) == Rgb { 255, 0, 0 });
    CHECK(fromHsv({ 120.0, 1.0, 1.0 }) == Rgb { 0, 255, 0 });
    CHECK(fromHsv({ 240.0, 1.0, 1.0 }) == Rgb { 0, 0, 255 });
    CHECK(fromHsv({ 360.0, 1.0, 1.0 }) == Rgb { 255, 0, 0 });
    CHECK(fromHsv({ 77.0, 0.0, 0.5 }) == Rgb { 128, 128, 128 });
    const Hsv red = toHsv(Rgb { 255, 0, 0 });
    CHECK(red.h == doctest::Approx(0.0));
    CHECK(red.s == doctest::Approx(1.0));
    CHECK(red.v == doctest::Approx(1.0));
    CHECK(toHsv(Rgb { 0, 0, 0 }).s == 0.0);
    // every colour survives the round trip
    for (int r = 0; r < 256; r += 15)
        for (int g = 0; g < 256; g += 15)
            for (int b = 0; b < 256; b += 15)
            {
                const Rgb color { static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b) };
                CHECK(fromHsv(toHsv(color)) == color);
            }
}

TEST_CASE("colours get plain-English names")
{
    CHECK(colorName(Rgb { 0xb8, 0xbc, 0xff }) == "pale indigo");
    CHECK(colorName(Rgb { 0x72, 0x78, 0xfe }) == "light indigo");
    CHECK(colorName(Rgb { 0xff, 0x00, 0x00 }) == "vivid red");
    CHECK(colorName(Rgb { 0xff, 0xff, 0xff }) == "white");
    CHECK(colorName(Rgb { 0x00, 0x00, 0x00 }) == "black");
    CHECK(colorName(Rgb { 0x80, 0x80, 0x80 }) == "grey");
    CHECK(colorName(Rgb { 0x8b, 0x45, 0x13 }) == "brown");
    CHECK(colorName(Rgb { 0x80, 0x80, 0x00 }) == "olive");
    CHECK(colorName(Rgb { 0xff, 0xc8, 0xa8 }) == "pale peach");
    CHECK(colorName(Rgb { 0x00, 0x60, 0x50 }) == "deep teal");
    CHECK(colorName(Rgb { 0x08, 0x10, 0x40 }) == "dark blue");
    // every colour gets a name
    for (int h = 0; h < 360; h += 5)
        for (const double s : { 0.05, 0.3, 0.5, 0.9 })
            for (const double v : { 0.1, 0.5, 0.9 })
                CHECK_FALSE(colorName(fromHsv({ static_cast<double>(h), s, v })).empty());
}
