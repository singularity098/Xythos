// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xythos {

// Stereo audio as floats (-1..1), for saving the looper's loop (owner, 2026-11-01: WAV and FLAC; MP3 and Opus would
// need encoder libraries, LAME's licence doesn't suit an MIT plugin, so they were left out)
struct StereoAudio {
    std::vector<float> left, right;
    uint32_t sampleRate = 48000;
};

// A WAV file of `audio`: 24-bit PCM, or (float32) 32-bit float, the format the DSP hands the UI the loop in
std::string encodeWav(const float* left, const float* right, size_t frames, uint32_t sampleRate, bool float32);

// Reads a stereo or mono WAV in the formats encodeWav() writes (and 16-bit PCM); nullopt for anything else
std::optional<StereoAudio> decodeWav(std::string_view bytes);

// A FLAC file of `audio`, 24-bit, lossless: blocks of 4096 samples, each channel predicted by the best of FLAC's fixed
// predictors (orders 0-4) with Rice-coded residuals, and the best of FLAC's four stereo modes per block. The MD5 in
// its header is left unset (all zero), which FLAC allows.
std::string encodeFlac(const float* left, const float* right, size_t frames, uint32_t sampleRate);

// The file the DSP writes the loop to (a float WAV in the cache dir) when the UI asks to save it: per instance
std::filesystem::path looperExportFile(uint32_t instanceId);

// Floats as 24-bit samples (clamped, rounded): what both encoders store
int32_t toPcm24(float sample);

} // namespace xythos
