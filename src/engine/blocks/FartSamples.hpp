// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <cstddef>
#include <cstdint>

namespace xythos {

struct EmbeddedFile {
    const unsigned char* data;
    size_t size;
};

// The Fart pedal's samples: the WAVs in resources/farts/ (tools/prepare_fart_samples.py), embedded at build time by
// cmake/EmbedFartSamples.cmake. {nullptr, 0} for an id that isn't there.
EmbeddedFile builtinFartSample(const char* id);

// A sample's 16-bit mono PCM, read from its WAV (little-endian; not aligned, so read it with pcmSample())
struct PcmView {
    const unsigned char* data = nullptr;
    uint32_t frames = 0;
    uint32_t sampleRate = 0;
};

// The PCM of a 16-bit mono WAV in memory, or an empty view if it isn't one
PcmView pcmFromWav(const unsigned char* data, size_t size);

inline float pcmSample(const PcmView& pcm, uint32_t i)
{
    const unsigned char* p = pcm.data + 2 * static_cast<size_t>(i);
    return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(p[0] | (p[1] << 8)))) * (1.0f / 32768.0f);
}

} // namespace xythos
