// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/IrBlock.hpp"

#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/Smoother.hpp"
#include "state/Library.hpp"
#include "state/Paths.hpp"

#include "NAM/wav.h"
#include "TwoStageFFTConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace xythos {

namespace {

constexpr size_t kHeadBlockSize = 64;
constexpr size_t kTailBlockSize = 2048;

// Linear-interpolation resampling of one IR channel
std::vector<float> resample(const std::vector<float>& in, double fromRate, double toRate)
{
    if (in.empty() || fromRate == toRate)
        return in;
    const double ratio = fromRate / toRate;
    const size_t outLen = static_cast<size_t>(std::floor((in.size() - 1) / ratio)) + 1;
    std::vector<float> out(outLen);
    for (size_t i = 0; i < outLen; ++i)
    {
        const double pos = i * ratio;
        const size_t j = static_cast<size_t>(pos);
        const double frac = pos - j;
        const float next = j + 1 < in.size() ? in[j + 1] : 0.0f;
        out[i] = static_cast<float>(in[j] + (next - in[j]) * frac);
    }
    return out;
}

class IrBlock : public Block {
public:
    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fSampleRate = sampleRate;
        for (int ch = 0; ch < 2; ++ch)
        {
            fDry[ch].assign(maxFrames, 0.0f);
            fWet[ch].assign(maxFrames, 0.0f);
        }
        fGain.prepare(sampleRate);
        fMix.prepare(sampleRate);
        fGain.snap(targetGain());
        fMix.snap(fMixValue);
        updateFilters();
        buildConvolvers();
    }

    bool configure(const nlohmann::json& config) override
    {
        const std::string path = config.value("path", std::string());
        if (path.empty())
            return false;
        // a file inside a zip is extracted to the cache first
        const std::optional<std::string> file = materializeLibraryFile(path);
        if (!file)
        {
            std::fprintf(stderr, "Xythos: can't open IR %s\n", path.c_str());
            return false;
        }

        try
        {
            int channels = 0;
            const std::vector<float> data = nam::detail::load_wav_ir(pathFromUtf8(*file), fFileRate, channels);
            if (channels < 1 || data.empty())
                return false;

            // NAM's loader stores each channel contiguously; use up to two
            const size_t frames = data.size() / static_cast<size_t>(channels);
            const size_t used = std::min(frames, static_cast<size_t>(kIrMaxSeconds * fFileRate));
            fFileChannels.clear();
            for (int ch = 0; ch < std::min(channels, 2); ++ch)
                fFileChannels.emplace_back(data.begin() + ch * frames, data.begin() + ch * frames + used);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "Xythos: can't load IR %s: %s\n", path.c_str(), e.what());
            return false;
        }

        // Energy normalization, so IRs of different loudness land near the same level
        double energy = 0.0;
        for (float v : fFileChannels[0])
            energy += static_cast<double>(v) * v;
        // A float WAV can hold anything, NaN, infinity or 1e38 included (a damaged or hostile file): the convolution
        // would only ever output NaN or infinity. Real IRs peak near 1; past +60 dB the file is taken as damaged.
        for (const std::vector<float>& channel : fFileChannels)
        {
            for (float v : channel)
            {
                if (!(std::fabs(v) <= kIrMaxPeak))
                {
                    std::fprintf(stderr, "Xythos: can't load IR %s: its samples are damaged\n", path.c_str());
                    fFileChannels.clear();
                    return false;
                }
            }
        }
        fNormGain = energy > 0.0 ? static_cast<float>(1.0 / std::sqrt(energy)) : 1.0f;

        buildConvolvers();
        fGain.snap(targetGain());
        return fConvolvers[0] != nullptr;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kIrParamLevel:
            fLevelDb = value;
            break;
        case kIrParamMix:
            fMixValue = value / 100.0f;
            fMix.setTarget(fMixValue);
            break;
        case kIrParamLowCut:
            fLowCut = value;
            updateFilters();
            break;
        case kIrParamHighCut:
            fHighCut = value;
            updateFilters();
            break;
        case kIrParamNormalize:
            fNormalize = value >= 0.5f;
            break;
        }
        fGain.setTarget(targetGain());
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (!fConvolvers[0])
            return;

        const Smoother gainStart = fGain;
        const Smoother mixStart = fMix;
        for (uint32_t ch = 0; ch < numChannels; ++ch)
        {
            fGain = gainStart;
            fMix = mixStart;

            float* x = io[ch];
            float* dry = fDry[ch].data();
            float* wet = fWet[ch].data();
            std::copy(x, x + frames, dry);

            fConvolvers[ch]->process(dry, wet, frames);

            for (uint32_t i = 0; i < frames; ++i)
            {
                float y = wet[i];
                if (fLowCutOn)
                    y = fHighPass[ch].process(y);
                if (fHighCutOn)
                    y = fLowPass[ch].process(y);
                y *= fGain.next();
                const float mix = fMix.next();
                x[i] = dry[i] + (y - dry[i]) * mix;
            }
        }
    }

    // Leaves the convolvers alone: FFTConvolver's reset() unloads the IR (the engine calls this after prepare() when
    // the host reactivates the plugin, which silenced the block), and it can't clear only their history without
    // allocating. prepare() has just built fresh ones anyway.
    void reset() override
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            fHighPass[ch].reset();
            fLowPass[ch].reset();
        }
    }

private:
    float targetGain() const { return dbToGain(fLevelDb) * (fNormalize ? fNormGain : 1.0f); }

    // At the ends of their ranges the cut filters are off
    void updateFilters()
    {
        fLowCutOn = fLowCut > 20.5f;
        fHighCutOn = fHighCut < 19900.0f;
        const double nyquistSafe = 0.45 * fSampleRate;
        for (int ch = 0; ch < 2; ++ch)
        {
            fHighPass[ch].setHighPass(fSampleRate, std::min<double>(fLowCut, nyquistSafe));
            fLowPass[ch].setLowPass(fSampleRate, std::min<double>(fHighCut, nyquistSafe));
        }
    }

    // Non-RT: resamples the IR to the host rate and sets up one convolver per channel
    void buildConvolvers()
    {
        if (fFileChannels.empty())
            return;
        for (int ch = 0; ch < 2; ++ch)
        {
            const std::vector<float>& source = fFileChannels[std::min<size_t>(ch, fFileChannels.size() - 1)];
            const std::vector<float> ir = resample(source, fFileRate, fSampleRate);
            auto convolver = std::make_unique<fftconvolver::TwoStageFFTConvolver>();
            if (!convolver->init(kHeadBlockSize, kTailBlockSize, ir.data(), ir.size()))
            {
                fConvolvers[0].reset();
                return;
            }
            fConvolvers[ch] = std::move(convolver);
        }
    }

    std::vector<std::vector<float>> fFileChannels;
    double fFileRate = 48000.0;
    double fSampleRate = 48000.0;
    std::unique_ptr<fftconvolver::TwoStageFFTConvolver> fConvolvers[2];
    std::vector<float> fDry[2], fWet[2];
    Biquad fHighPass[2], fLowPass[2];

    float fNormGain = 1.0f;
    float fLevelDb = 0.0f;
    float fMixValue = 1.0f;
    float fLowCut = 20.0f;
    float fHighCut = 20000.0f;
    bool fNormalize = true;
    bool fLowCutOn = false;
    bool fHighCutOn = false;
    Smoother fGain, fMix;
};

} // namespace

const BlockDescriptor kIrBlockDescriptor = {
    "ir",
    "IR",
    Category::Ir,
    kIrParamCount,
    {
        ParamSpec { "Level", "dB", -24.0f, 12.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Low Cut", "Hz", 20.0f, 500.0f, 20.0f, ParamScale::Log },
        ParamSpec { "High Cut", "Hz", 2000.0f, 20000.0f, 20000.0f, ParamScale::Log },
        ParamSpec { "Normalize", "", 0.0f, 1.0f, 1.0f, ParamScale::Toggle },
    },
    [] { return std::unique_ptr<Block>(new IrBlock()); },
};

} // namespace xythos
