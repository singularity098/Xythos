// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <signalsmith-stretch.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

namespace xythos {

// The pitch blocks' HQ mode: spectral (STFT) pitch shifting with Signalsmith Stretch (MIT; vendored with our pitch
// accuracy fixes, deps/signalsmith-stretch/README.md). No splices, so no warble: much cleaner on single notes, fifths
// and octaves up, about as clean as the delay-line PitchVoice on octave-down chords, and no better two octaves down.
// The cost is latency (latency(): about 1.25 x kBlockMs, 150 ms) and CPU (about 1% of a core per voice and channel).
// One shifter is one voice (one ratio) for one or two channels; mono and stereo each have their own stretcher (a
// stereo one keeps the channels phase-locked), so a mono chain doesn't pay for two channels. Both are made in
// prepare(), so process() never allocates.
//
// The block size is a trade: a longer window resolves low notes better, a shorter one has less latency (about 150 ms
// at 120 ms). Tested 2026-11-07: on a sustained open E chord an octave down, 120 ms kept all the energy at the shifted
// partials and 80 ms 76% (the delay-line shifter 90%); on strummed chords 40 and 60 ms smeared the low strings.
// Intervals shorter than a quarter block did worse and cost more. 120/30 ms is also the library's default preset. The computation is spread evenly over each interval
// (splitComputation), so a buffer never carries a whole FFT block's work at once; that adds one interval of latency.
class SpectralShifter {
public:
    static constexpr double kBlockMs = 120.0;
    static constexpr double kIntervalMs = 30.0;

    // Non-RT. Allocates.
    void prepare(double sampleRate, double blockMs = kBlockMs, double intervalMs = kIntervalMs)
    {
        const int block = std::max(256, static_cast<int>(blockMs / 1000.0 * sampleRate));
        const int interval = std::max(64, static_cast<int>(intervalMs / 1000.0 * sampleRate));
        for (int k = 0; k < 2; ++k)
        {
            fStretch[k] = std::make_unique<Stretch>(0x5eed + k); // fixed seeds: the same output every run
            fStretch[k]->configure(k + 1, block, interval, true);
        }
        reset();
    }

    bool prepared() const { return fStretch[0] != nullptr; }

    // Samples the output trails the input by (the same for mono and stereo)
    uint32_t latency() const
    {
        return fStretch[0] ? static_cast<uint32_t>(fStretch[0]->inputLatency() + fStretch[0]->outputLatency()) : 0;
    }

    // Samples the shifter's processing point (where a new ratio takes effect) trails the input by
    uint32_t inputLatency() const { return fStretch[0] ? static_cast<uint32_t>(fStretch[0]->inputLatency()) : 0; }

    // RT
    void reset()
    {
        for (auto& stretch : fStretch)
            if (stretch)
                stretch->reset();
        fChannels = 0;
    }

    // RT. Takes effect at the shifter's processing point, inputLatency() behind the input it is given.
    void setRatio(double ratio) { fRatio = static_cast<float>(ratio); }

    // RT. n samples of 1 or 2 channels in, the shifted sound out (`in` and `out` must not overlap).
    void process(const float* const* in, float* const* out, uint32_t numChannels, uint32_t n)
    {
        if (!fStretch[0])
        {
            for (uint32_t ch = 0; ch < numChannels; ++ch)
                std::fill(out[ch], out[ch] + n, 0.0f);
            return;
        }
        const int k = numChannels > 1 ? 1 : 0;
        if (static_cast<int>(numChannels) != fChannels)
        {
            // mono <-> stereo: the other stretcher starts afresh
            fStretch[k]->reset();
            fChannels = static_cast<int>(numChannels);
        }
        fStretch[k]->setTransposeFactor(fRatio);
        fStretch[k]->process(in, static_cast<int>(n), out, static_cast<int>(n));
    }

private:
    using Stretch = signalsmith::stretch::SignalsmithStretch<float>;
    std::array<std::unique_ptr<Stretch>, 2> fStretch;
    float fRatio = 1.0f;
    int fChannels = 0;
};

} // namespace xythos
