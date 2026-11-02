// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "ui/KnobImages.hpp"
#include "ui/LedImages.hpp"
#include "ui/RasterImage.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

using namespace xythos;

namespace {

// 3x2 RGBA PNG: red, half-transparent green, transparent blue; (10, 20, 30), (200, 100, 50), (1, 2, 3, 4)
const unsigned char kTinyPng[] = {
    137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82, 0, 0, 0, 3, 0, 0, 0, 2, 8, 6, 0, 0, 0, 157,
    116, 102, 26, 0, 0, 0, 35, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240, 159, 225, 63, 67, 3, 3, 195,
    127, 6, 6, 46, 17, 185, 255, 39, 82, 140, 254, 51, 50, 49, 179, 0, 0, 117, 118, 8, 31, 47, 18, 246, 101, 0,
    0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130
};

// 8x8 baseline JPEG, all (120, 60, 30)
const unsigned char kTinyJpeg[] = {
    255, 216, 255, 224, 0, 16, 74, 70, 73, 70, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 255, 219, 0, 67, 0, 2, 1, 1, 1, 1,
    1, 2, 1, 1, 1, 2, 2, 2, 2, 2, 4, 3, 2, 2, 2, 2, 5, 4, 4, 3, 4, 6, 5, 6, 6, 6, 5, 6, 6, 6, 7, 9, 8, 6, 7, 9,
    7, 6, 6, 8, 11, 8, 9, 10, 10, 10, 10, 10, 6, 8, 11, 12, 11, 10, 12, 9, 10, 10, 10, 255, 219, 0, 67, 1, 2, 2,
    2, 2, 2, 2, 5, 3, 3, 5, 10, 7, 6, 7, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 255, 192, 0, 17, 8, 0, 8, 0, 8, 3, 1, 34, 0, 2, 17, 1, 3, 17, 1, 255, 196, 0, 31, 0, 0,
    1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 255, 196, 0, 181, 16, 0,
    2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125, 1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7, 34,
    113, 20, 50, 129, 145, 161, 8, 35, 66, 177, 193, 21, 82, 209, 240, 36, 51, 98, 114, 130, 9, 10, 22, 23, 24,
    25, 26, 37, 38, 39, 40, 41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86,
    87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132,
    133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167,
    168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202,
    210, 211, 212, 213, 214, 215, 216, 217, 218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242,
    243, 244, 245, 246, 247, 248, 249, 250, 255, 196, 0, 31, 1, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 255, 196, 0, 181, 17, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2,
    119, 0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113, 19, 34, 50, 129, 8, 20, 66, 145, 161, 177,
    193, 9, 35, 51, 82, 240, 21, 98, 114, 209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38, 39, 40, 41, 42,
    53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102,
    103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135, 136, 137, 138,
    146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170, 178, 179, 180,
    181, 182, 183, 184, 185, 186, 194, 195, 196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215,
    216, 217, 218, 226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248, 249, 250,
    255, 218, 0, 12, 3, 1, 0, 2, 17, 3, 17, 0, 63, 0, 248, 174, 138, 40, 175, 198, 207, 232, 3, 255, 217
};

// A PNG whose header claims width x height (nothing after the header: only stbi_info gets that far)
std::vector<unsigned char> pngHeader(uint32_t width, uint32_t height)
{
    std::vector<unsigned char> b(kTinyPng, kTinyPng + 33); // signature + IHDR
    for (int i = 0; i < 4; ++i)
    {
        b[16 + i] = static_cast<unsigned char>(width >> (24 - 8 * i));
        b[20 + i] = static_cast<unsigned char>(height >> (24 - 8 * i));
    }
    return b;
}

} // namespace

TEST_CASE("PNG and JPEG decode to 8-bit RGBA")
{
    RgbaImage png;
    REQUIRE(decodeRaster(kTinyPng, sizeof(kTinyPng), png));
    CHECK(png.width == 3);
    CHECK(png.height == 2);
    const unsigned char expected[] = { 255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0,
                                       10, 20, 30, 255, 200, 100, 50, 255, 1, 2, 3, 4 };
    REQUIRE(png.pixels.size() == sizeof(expected));
    CHECK(std::memcmp(png.pixels.data(), expected, sizeof(expected)) == 0);

    RgbaImage jpeg;
    REQUIRE(decodeRaster(kTinyJpeg, sizeof(kTinyJpeg), jpeg));
    CHECK(jpeg.width == 8);
    CHECK(jpeg.height == 8);
    REQUIRE(jpeg.pixels.size() == 8 * 8 * 4);
    CHECK(std::abs(jpeg.pixels[0] - 120) <= 3);
    CHECK(std::abs(jpeg.pixels[1] - 60) <= 3);
    CHECK(std::abs(jpeg.pixels[2] - 30) <= 3);
    CHECK(jpeg.pixels[3] == 255);
}

TEST_CASE("damaged, unknown and oversized images don't decode")
{
    RgbaImage image;
    CHECK_FALSE(decodeRaster(nullptr, 0, image));
    CHECK_FALSE(decodeRaster(kTinyPng, 20, image));            // cut off in the header
    // a cut-off JPEG may still decode what it has (stb fills the rest); either way the buffer matches the size
    if (decodeRaster(kTinyJpeg, sizeof(kTinyJpeg) / 3, image))
        CHECK(image.pixels.size() == size_t(image.width) * image.height * 4);
    const std::string gif = "GIF89a\x01\x00\x01\x00\x00\x00\x00;";
    CHECK_FALSE(decodeRaster(reinterpret_cast<const unsigned char*>(gif.data()), gif.size(), image));

    // sizes past the limits are refused from the header, before anything is allocated
    for (const auto& [w, h] : { std::pair<uint32_t, uint32_t> { kMaxImageSide + 1, 1 }, { 1, kMaxImageSide + 1 },
                                { 10000, 10000 }, { 0xffffffffu, 0xffffffffu } })
    {
        const std::vector<unsigned char> header = pngHeader(w, h);
        CHECK_FALSE(decodeRaster(header.data(), header.size(), image));
    }
}

TEST_CASE("the built-in knob and LED images decode")
{
    for (const BuiltinImage builtin : { knobImage(), knobPointerImage(), knobSheenImage(), rimKnobImage(),
                                        rimKnobPointerImage(), rimKnobColourImage(), ledOffImage(),
                                        ledOffColourImage(), ledLensImage(), ledLensColourImage(), ledBloomImage(),
                                        ledBloomColourImage(), ledGlassImage() })
    {
        RgbaImage image;
        CHECK(decodeRaster(builtin.data, builtin.size, image));
        CHECK(image.width > 0);
    }
}
