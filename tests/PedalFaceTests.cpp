// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include <doctest/doctest.h>

#include "engine/BlockRegistry.hpp"
#include "ui/PedalFaces.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace xythos;

namespace {

// New pedals whose photo the owner hasn't rendered yet (their prompts are in docs/PedalImagePrompts.md). Take a pedal
// off this list when its photo and face are added.
bool awaitingPhoto(const std::string& typeId)
{
    static const std::vector<std::string> kAwaiting = { "theremin", "synclead", "pdsynth",         "wavetable",
                                                        "subbass",  "laser",    "drone",           "harmonicpainter",
                                                        "humeliminator" };
    for (const std::string& id : kAwaiting)
        if (typeId == id)
            return true;
    return false;
}

} // namespace

TEST_CASE("pedal face knobs turn real knob parameters, inside their image, with short names")
{
    for (const PedalFace& face : pedalFaces())
    {
        CAPTURE(std::string(face.typeId));
        const BlockDescriptor* type = BlockRegistry::builtin().find(face.typeId);
        REQUIRE(type != nullptr);
        CHECK(pedalFace(face.typeId) == &face);
        REQUIRE(!face.knobs.empty());
        CHECK(face.knobs.size() <= 3);
        for (const FaceKnob& knob : face.knobs)
        {
            CAPTURE(std::string(knob.param));
            int found = -1;
            for (int p = 0; p < type->numParams; ++p)
                if (std::strcmp(type->params[p].name, knob.param) == 0)
                    found = p;
            REQUIRE(found >= 0);
            const ParamScale scale = type->params[found].scale;
            CHECK((scale == ParamScale::Linear || scale == ParamScale::Log));
            CHECK(knob.radius > 0.0f);
            CHECK(knob.x - knob.radius >= 0.0f);
            CHECK(knob.x + knob.radius <= 1.0f);
            CHECK(knob.y > 0.0f);
            CHECK(knob.y < 1.0f);
            CHECK(std::strlen(faceKnobLabel(knob.param)) <= 5);
        }
        for (size_t i = 0; i < face.knobs.size(); ++i)
            for (size_t j = i + 1; j < face.knobs.size(); ++j)
                CHECK(std::strcmp(faceKnobLabel(face.knobs[i].param), faceKnobLabel(face.knobs[j].param)) != 0);
    }
    CHECK(pedalFace("no-such-block") == nullptr);
    CHECK(std::strcmp(faceKnobLabel("Damping"), "Damp") == 0);
    CHECK(std::strcmp(faceKnobLabel("Mix"), "Mix") == 0);
}

TEST_CASE("pedal face LEDs sit inside their image")
{
    for (const PedalFace& face : pedalFaces())
    {
        CAPTURE(std::string(face.typeId));
        for (const FaceLed& led : face.leds)
        {
            CHECK(led.radius > 0.0f);
            CHECK(led.x - led.radius >= 0.0f);
            CHECK(led.x + led.radius <= 1.0f);
            CHECK(led.y > 0.0f);
            CHECK(led.y < 1.0f);
        }
    }
}

TEST_CASE("every pedal has an image with its knobs, power LED and footswitch")
{
    const std::filesystem::path images = XYTHOS_BLOCK_IMAGES;
    for (const BlockDescriptor* type : BlockRegistry::builtin().types())
    {
        const std::string typeId = type->typeId;
        CAPTURE(typeId);
        if (typeId == "nam" || typeId == "ir")
            continue; // these get a look per file (FileLooks.cpp)
        if (awaitingPhoto(typeId))
        {
            // drawn with the placeholder art until the owner sends its render (docs/PedalImagePrompts.md)
            CHECK_FALSE(std::filesystem::exists(images / (typeId + ".webp")));
            CHECK(pedalFace(type->typeId) == nullptr);
            continue;
        }
        CHECK(std::filesystem::exists(images / (typeId + ".webp")));
        CHECK(std::filesystem::exists(images / "thumbs" / (typeId + ".webp")));
        const PedalFace* face = pedalFace(type->typeId);
        REQUIRE(face != nullptr);
        CHECK(face->leds.size() == 1);
        const FaceSwitch& sw = face->footswitch;
        CHECK(sw.radius > 0.0f);
        CHECK(sw.x - sw.radius >= 0.0f);
        CHECK(sw.x + sw.radius <= 1.0f);
        CHECK(sw.y > 0.0f);
        CHECK(sw.y < 1.0f);

        // nothing on the face overlaps (the images are 1:2, so y distances count twice the x ones)
        const auto apart = [](float x1, float y1, float r1, float x2, float y2, float r2) {
            return std::hypot(x1 - x2, (y1 - y2) * 2.0f) >= r1 + r2;
        };
        for (size_t i = 0; i < face->knobs.size(); ++i)
        {
            const FaceKnob& a = face->knobs[i];
            CAPTURE(std::string(a.param));
            for (size_t j = i + 1; j < face->knobs.size(); ++j)
                CHECK(apart(a.x, a.y, a.radius, face->knobs[j].x, face->knobs[j].y, face->knobs[j].radius));
            CHECK(apart(a.x, a.y, a.radius, sw.x, sw.y, sw.radius));
            for (const FaceLed& led : face->leds)
                CHECK(apart(a.x, a.y, a.radius, led.x, led.y, led.radius));
        }
    }
}

TEST_CASE("every generic pedal photo has a footswitch and an LED, and no knobs")
{
    const std::filesystem::path images = XYTHOS_PEDAL_IMAGES;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(images))
    {
        if (entry.path().extension() != ".webp")
            continue;
        const std::string id = entry.path().stem().string();
        CAPTURE(id);
        ++count;
        const PedalFace* face = genericPedalFace(id.c_str());
        REQUIRE(face != nullptr);
        CHECK(face->knobs.empty()); // the painted knobs aren't any particular controls
        REQUIRE(face->leds.size() == 1);
        const FaceLed& led = face->leds[0];
        const FaceSwitch& sw = face->footswitch;
        CHECK(sw.radius > 0.0f);
        CHECK(sw.x - sw.radius >= 0.0f);
        CHECK(sw.x + sw.radius <= 1.0f);
        CHECK(led.radius > 0.0f);
        CHECK(led.x - led.radius * 1.42f >= 0.0f);
        CHECK(led.x + led.radius * 1.42f <= 1.0f);
        // above the footswitch, not on it (the photos are 823 x 1280)
        CHECK(led.y < sw.y);
        CHECK(std::hypot(led.x - sw.x, (led.y - sw.y) * 1280.0f / 823.0f) >= led.radius + sw.radius);
    }
    CHECK(count == static_cast<int>(genericPedalFaces().size()));
}

TEST_CASE("every amp head photo has five knobs for the NAM tone controls, a power LED and a switch")
{
    const std::filesystem::path images = XYTHOS_AMP_IMAGES;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(images))
    {
        if (entry.path().extension() != ".webp")
            continue;
        const std::string id = entry.path().stem().string();
        if (!id.starts_with("amp-"))
            continue; // the half stacks: below
        CAPTURE(id);
        ++count;
        const PedalFace* face = ampHeadFace(id.c_str());
        REQUIRE(face != nullptr);
        REQUIRE(face->knobs.size() == 5);
        const char* const names[] = { "Input", "Bass", "Middle", "Treble", "Output" };
        for (size_t i = 0; i < 5; ++i)
        {
            CHECK(std::string(face->knobs[i].param) == names[i]);
            if (i > 0)
                CHECK(face->knobs[i].x > face->knobs[i - 1].x);
        }
        REQUIRE(face->leds.size() == 1);
        const FaceLed& led = face->leds[0];
        const FaceSwitch& sw = face->footswitch;
        CHECK(sw.radius > 0.0f);
        CHECK(sw.x + sw.radius <= 1.0f);
        // nothing overlaps (the photos are 1920 x 1088: y in width units is y * 1088 / 1920)
        const auto apart = [](float ax, float ay, float ar, float bx, float by, float br) {
            return std::hypot(ax - bx, (ay - by) * 1088.0f / 1920.0f) >= ar + br;
        };
        CHECK(apart(led.x, led.y, led.radius * 1.42f, sw.x, sw.y, sw.radius * 0.8f));
        for (const FaceKnob& knob : face->knobs)
        {
            CHECK(apart(knob.x, knob.y, knob.radius, led.x, led.y, led.radius * 1.42f));
            CHECK(apart(knob.x, knob.y, knob.radius, sw.x, sw.y, sw.radius));
        }
        for (size_t i = 1; i < face->knobs.size(); ++i)
            CHECK(apart(face->knobs[i].x, face->knobs[i].y, face->knobs[i].radius, face->knobs[i - 1].x,
                        face->knobs[i - 1].y, face->knobs[i - 1].radius));
    }
    CHECK(count == static_cast<int>(ampHeadFaces().size()));
}

TEST_CASE("every half stack photo's head has five knobs for the NAM tone controls, a power LED and a switch")
{
    const std::filesystem::path images = XYTHOS_AMP_IMAGES;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(images))
    {
        if (entry.path().extension() != ".webp")
            continue;
        const std::string id = entry.path().stem().string();
        if (!id.starts_with("stack-"))
            continue;
        CAPTURE(id);
        ++count;
        const PedalFace* face = ampHeadFace(id.c_str());
        REQUIRE(face != nullptr);
        REQUIRE(face->knobs.size() == 5);
        const char* const names[] = { "Input", "Bass", "Middle", "Treble", "Output" };
        for (size_t i = 0; i < 5; ++i)
        {
            CHECK(std::string(face->knobs[i].param) == names[i]);
            if (i > 0)
                CHECK(face->knobs[i].x > face->knobs[i - 1].x);
        }
        REQUIRE(face->leds.size() == 1);
        const FaceLed& led = face->leds[0];
        const FaceSwitch& sw = face->footswitch;
        CHECK(sw.radius > 0.0f);
        CHECK(sw.x + sw.radius <= 1.0f);
        CHECK(led.x < sw.x); // left of the switch
        // nothing overlaps (the photos are 823 x 1280: y in width units is y * 1280 / 823)
        const auto apart = [](float ax, float ay, float ar, float bx, float by, float br) {
            return std::hypot(ax - bx, (ay - by) * 1280.0f / 823.0f) >= ar + br;
        };
        CHECK(apart(led.x, led.y, led.radius * 1.42f, sw.x, sw.y, sw.radius * 0.8f));
        for (const FaceKnob& knob : face->knobs)
        {
            CHECK(knob.x - knob.radius >= 0.0f);
            CHECK(apart(knob.x, knob.y, knob.radius, led.x, led.y, led.radius * 1.42f));
            CHECK(apart(knob.x, knob.y, knob.radius, sw.x, sw.y, sw.radius));
        }
        for (size_t i = 1; i < face->knobs.size(); ++i)
            CHECK(apart(face->knobs[i].x, face->knobs[i].y, face->knobs[i].radius, face->knobs[i - 1].x,
                        face->knobs[i - 1].y, face->knobs[i - 1].radius));
    }
    CHECK(count >= 34);
    CHECK(count == static_cast<int>(halfStackFaces().size()));
}
