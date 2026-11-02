// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "state/Paths.hpp"
#include "state/UserImages.hpp"

#include <filesystem>

using namespace xythos;
namespace fs = std::filesystem;

namespace {

// The start of a PNG with the given bit depth (IHDR's byte 24)
std::string pngBytes(char depth, const std::string& rest = "pixels")
{
    return std::string("\x89PNG\r\n\x1a\n", 8) + std::string(16, '\0') + depth + rest;
}

struct ImageFixture {
    fs::path root = fs::temp_directory_path() / "xythos-test-images";

    ImageFixture()
    {
        fs::remove_all(root);
        testing::setEnv("XYTHOS_CONFIG_DIR", pathToUtf8(root / "config"));
    }
};

} // namespace

TEST_CASE("NAM blocks use the amp images, IR blocks the cab images, every other block the pedal images")
{
    CHECK(imageKindFor("nam") == ImageKind::Amp);
    CHECK(imageKindFor("ir") == ImageKind::Cab);
    CHECK(imageKindFor("delay") == ImageKind::Pedal);
    ImageFixture fixture;
    CHECK(userImagesDir(ImageKind::Amp) == fixture.root / "config" / "images" / "amps");
    CHECK(userImagesDir(ImageKind::Cab) == fixture.root / "config" / "images" / "cabs");
    CHECK(userImagesDir(ImageKind::Pedal) == fixture.root / "config" / "images" / "pedals");
}

TEST_CASE("only PNG (8 or 16-bit), JPEG and WebP images are taken")
{
    CHECK(isUsableImage(pngBytes('\x08')));
    CHECK(isUsableImage(pngBytes('\x10')));
    CHECK(isUsableImage("\xff\xd8\xff\xe0 jpeg"));
    CHECK(isUsableImage(std::string("RIFF\0\0\0\0WEBPVP8 ", 16)));
    std::string error;
    CHECK_FALSE(isUsableImage("GIF89a", &error));
    CHECK(!error.empty());
    CHECK_FALSE(isUsableImage(""));
}

TEST_CASE("images are added to their own library, listed, found and deleted")
{
    ImageFixture fixture;
    CHECK(listUserImages(ImageKind::Pedal).empty());

    const std::string a = pngBytes('\x08', "a");
    CHECK(addUserImage(ImageKind::Pedal, "/somewhere/Blue Fuzz.PNG", a) == "Blue Fuzz.png");
    // the same content again is the same image; other content with the name gets a number
    CHECK(addUserImage(ImageKind::Pedal, "Copy.png", a) == "Blue Fuzz.png");
    CHECK(addUserImage(ImageKind::Pedal, "Blue Fuzz.png", pngBytes('\x08', "b")) == "Blue Fuzz 2.png");
    CHECK(addUserImage(ImageKind::Pedal, "alpha.jpg", "\xff\xd8\xff alpha") == "alpha.jpg");
    std::string error;
    CHECK_FALSE(addUserImage(ImageKind::Pedal, "notes.txt", a, &error));
    CHECK_FALSE(addUserImage(ImageKind::Pedal, "Gif.png", "GIF89a", &error));

    const std::vector<UserImage> pedals = listUserImages(ImageKind::Pedal);
    REQUIRE(pedals.size() == 3);
    CHECK(pedals[0].name == "alpha");
    CHECK(pedals[1].id == "Blue Fuzz.png");
    CHECK(pedals[2].id == "Blue Fuzz 2.png");
    CHECK(listUserImages(ImageKind::Amp).empty()); // the libraries are separate
    CHECK(userImagePath(ImageKind::Amp, "Blue Fuzz.png").empty());

    const std::string path = userImagePath(ImageKind::Pedal, "Blue Fuzz.png");
    CHECK(readFile(pathFromUtf8(path)) == a);
    // ids are file names in the library, nothing else
    CHECK(userImagePath(ImageKind::Pedal, "../pedals/Blue Fuzz.png").empty());
    CHECK(userImagePath(ImageKind::Pedal, "").empty());

    CHECK(deleteUserImage(ImageKind::Pedal, "Blue Fuzz.png"));
    CHECK(userImagePath(ImageKind::Pedal, "Blue Fuzz.png").empty());
    CHECK_FALSE(deleteUserImage(ImageKind::Pedal, "Blue Fuzz.png"));
    CHECK(listUserImages(ImageKind::Pedal).size() == 2);
}

TEST_CASE("an image file is imported by copying it into the library")
{
    ImageFixture fixture;
    const fs::path source = fixture.root / "Pics" / "Cab.jpg";
    REQUIRE(writeFileAtomic(source, "\xff\xd8\xff cab"));
    CHECK(importUserImage(ImageKind::Cab, pathToUtf8(source)) == "Cab.jpg");
    fs::remove(source);
    CHECK(readFile(pathFromUtf8(userImagePath(ImageKind::Cab, "Cab.jpg"))) == "\xff\xd8\xff cab");
    CHECK_FALSE(importUserImage(ImageKind::Cab, pathToUtf8(source)));
}
