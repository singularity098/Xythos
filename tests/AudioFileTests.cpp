// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Saving the looper's loop (owner, 2026-11-01): WAV and FLAC

#include <doctest/doctest.h>

#include "TestSupport.hpp"
#include "engine/Engine.hpp"
#include "state/AudioFile.hpp"
#include "state/Paths.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <vector>

using namespace xythos;

namespace {

struct Signal {
    std::vector<float> left, right;
};

// A tone with noise on the left; a tone, clicks, a silent stretch and both extremes on the right
Signal testSignal(size_t frames)
{
    Signal s { std::vector<float>(frames), std::vector<float>(frames) };
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    for (size_t i = 0; i < frames; ++i)
    {
        s.left[i] = 0.5f * std::sin(static_cast<float>(i) * 0.01f) + noise(rng);
        s.right[i] = i > 5000 && i < 9000 ? 0.0f : 0.4f * std::sin(static_cast<float>(i) * 0.013f) + (i % 7 == 0 ? 0.5f : 0.0f);
    }
    s.right[100] = 1.0f;
    s.right[101] = -1.0f;
    s.left[102] = 1.5f; // clamped
    return s;
}

bool haveFfmpeg()
{
#ifdef _WIN32
    return false;
#else
    return std::system("ffmpeg -version > /dev/null 2>&1") == 0;
#endif
}

} // namespace

TEST_CASE("the loop's WAV goes there and back")
{
    const Signal s = testSignal(10000);
    for (const bool float32 : { true, false })
    {
        CAPTURE(float32);
        const std::string wav = encodeWav(s.left.data(), s.right.data(), s.left.size(), 44100, float32);
        CHECK(wav.size() == 44 + 10000 * 2 * (float32 ? 4 : 3));
        const std::optional<StereoAudio> back = decodeWav(wav);
        REQUIRE(back);
        CHECK(back->sampleRate == 44100);
        REQUIRE(back->left.size() == 10000);
        for (size_t i = 0; i < 10000; i += 17)
        {
            if (float32) // exact, unclamped (the DSP's hand-over)
            {
                CHECK(back->left[i] == s.left[i]);
                CHECK(back->right[i] == s.right[i]);
            }
            else // 24-bit: within half a step, clamped
            {
                CHECK(std::fabs(back->left[i] - std::clamp(s.left[i], -1.0f, 1.0f)) <= 1.0f / 8388607.0f);
                CHECK(std::fabs(back->right[i] - s.right[i]) <= 1.0f / 8388607.0f);
            }
        }
    }
    CHECK_FALSE(decodeWav("not a wav"));
}

TEST_CASE("the loop's FLAC is a valid, lossless 24-bit FLAC")
{
    const size_t frames = 4096 * 3 + 1234; // a short last block
    const Signal s = testSignal(frames);
    const std::string flac = encodeFlac(s.left.data(), s.right.data(), frames, 48000);
    REQUIRE(flac.size() > 42);
    CHECK(flac.substr(0, 4) == "fLaC");
    // STREAMINFO: the last metadata block, 34 bytes; 48 kHz, 2 channels, 24 bits, the sample count
    const auto byte = [&](size_t i) { return static_cast<uint8_t>(flac[i]); };
    CHECK(byte(4) == 0x80);
    CHECK((byte(5) << 16 | byte(6) << 8 | byte(7)) == 34);
    const uint64_t info = uint64_t { byte(18) } << 56 | uint64_t { byte(19) } << 48 | uint64_t { byte(20) } << 40
                        | uint64_t { byte(21) } << 32 | uint64_t { byte(22) } << 24 | uint64_t { byte(23) } << 16
                        | uint64_t { byte(24) } << 8 | byte(25);
    CHECK((info >> 44) == 48000);
    CHECK(((info >> 41) & 7) == 1);
    CHECK(((info >> 36) & 31) == 23);
    CHECK((info & 0xfffffffffull) == frames);
    // a tone (no noise to code) takes far less than the 24-bit WAV
    {
        std::vector<float> tone(48000);
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = 0.5f * std::sin(static_cast<float>(i) * 2.0f * 3.14159265f * 220.0f / 48000.0f);
        CHECK(encodeFlac(tone.data(), tone.data(), tone.size(), 48000).size() < tone.size() * 6 / 2);
    }

    // decoded by ffmpeg, it's the 24-bit WAV exactly (where ffmpeg is installed)
    if (!haveFfmpeg())
        return;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "xythos-flac-test";
    std::filesystem::create_directories(dir);
    REQUIRE(writeFileAtomic(dir / "loop.flac", flac));
    const std::string command = "ffmpeg -v error -y -i \"" + (dir / "loop.flac").string() + "\" -c:a pcm_s24le \""
                              + (dir / "decoded.wav").string() + "\" > /dev/null 2>&1";
    REQUIRE(std::system(command.c_str()) == 0);
    const std::optional<std::string> decoded = readFile(dir / "decoded.wav");
    REQUIRE(decoded);
    const std::optional<StereoAudio> back = decodeWav(*decoded);
    REQUIRE(back);
    REQUIRE(back->left.size() == frames);
    int mismatches = 0;
    for (size_t i = 0; i < frames; ++i)
        mismatches += toPcm24(back->left[i]) != toPcm24(s.left[i]) || toPcm24(back->right[i]) != toPcm24(s.right[i]);
    CHECK(mismatches == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("the engine hands over the loop to save only while it plays or is stopped")
{
    Engine engine { testing::testRegistry() };
    engine.prepare(48000.0, 256);
    CHECK(engine.looperWav().empty()); // no memory yet
    engine.setParameter(looperParam(kLooperParamMode), 1.0f); // Free
    engine.setParameter(looperParam(kLooperParamLatency), 0.0f);
    engine.setParameter(kParamLooperWake, 1.0f);
    std::vector<float> l(256, 0.25f), r(256, -0.25f), ol(256), orr(256);
    const float* in[2] = { l.data(), r.data() };
    float* out[2] = { ol.data(), orr.data() };
    engine.process(in, out, 256);
    REQUIRE(engine.serviceLooper());
    CHECK(engine.looperWav().empty()); // empty

    engine.setParameter(looperParam(kLooperParamRecDub), 1.0f);
    for (int b = 0; b < 40; ++b)
        engine.process(in, out, 256);
    CHECK(engine.looperWav().empty()); // being recorded
    engine.setParameter(looperParam(kLooperParamRecDub), 0.0f);
    engine.process(in, out, 256);
    engine.setParameter(looperParam(kLooperParamRecDub), 1.0f); // close the loop
    engine.process(in, out, 256);

    const std::optional<StereoAudio> loop = decodeWav(engine.looperWav());
    REQUIRE(loop);
    CHECK(loop->sampleRate == 48000);
    CHECK(loop->left.size() == 41 * 256);
    // Mono mode: the left input on both sides
    CHECK(loop->left[2000] == doctest::Approx(0.25f));
    CHECK(loop->right[2000] == doctest::Approx(0.25f));
}
