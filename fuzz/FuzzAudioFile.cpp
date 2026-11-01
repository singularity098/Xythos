// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// WAV reading (src/state/AudioFile.*) on any bytes, and both encoders on whatever it decodes: any float the file
// held, NaN and infinities included.

#include "FuzzTarget.hpp"

#include "state/AudioFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace xythos;

XYTHOS_FUZZ_TARGET(wav)
{
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    const std::optional<StereoAudio> audio = decodeWav(bytes);
    if (!audio)
        return 0;
    FUZZ_CHECK(audio->left.size() == audio->right.size());
    FUZZ_CHECK(audio->sampleRate > 0);

    const size_t frames = audio->left.size();
    // float WAV keeps every bit
    const std::string wav = encodeWav(audio->left.data(), audio->right.data(), frames, audio->sampleRate, true);
    const std::optional<StereoAudio> again = decodeWav(wav);
    FUZZ_CHECK(again.has_value());
    FUZZ_CHECK(again->sampleRate == audio->sampleRate);
    FUZZ_CHECK(again->left.size() == frames);
    FUZZ_CHECK(frames == 0 || std::memcmp(again->left.data(), audio->left.data(), frames * sizeof(float)) == 0);
    FUZZ_CHECK(frames == 0 || std::memcmp(again->right.data(), audio->right.data(), frames * sizeof(float)) == 0);

    // the 24-bit encoders take any float (clipped; NaN as silence)
    const std::string pcm = encodeWav(audio->left.data(), audio->right.data(), frames, audio->sampleRate, false);
    const std::optional<StereoAudio> pcmAgain = decodeWav(pcm);
    FUZZ_CHECK(pcmAgain.has_value() && pcmAgain->left.size() == frames);
    for (size_t i = 0; i < frames; ++i)
    {
        const float in[2] = { audio->left[i], audio->right[i] };
        const float out[2] = { pcmAgain->left[i], pcmAgain->right[i] };
        for (int ch = 0; ch < 2; ++ch)
        {
            const float expected = std::isnan(in[ch]) ? 0.0f : std::clamp(in[ch], -1.0f, 1.0f);
            FUZZ_CHECK(std::fabs(out[ch] - expected) <= 1.5f / 8388607.0f);
        }
    }
    const std::string flac = encodeFlac(audio->left.data(), audio->right.data(), frames, audio->sampleRate);
    FUZZ_CHECK(flac.size() >= 4 && flac.compare(0, 4, "fLaC") == 0);
    return 0;
}
