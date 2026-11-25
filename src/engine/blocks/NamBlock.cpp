// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/NamBlock.hpp"

#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/ToneStack.hpp"
#include "state/Library.hpp"
#include "state/Paths.hpp"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace xythos {

namespace {

class NamBlock : public Block {
public:
    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fSampleRate = sampleRate;
        fMaxFrames = maxFrames;
        for (std::vector<float>& buffer : fOut)
            buffer.assign(maxFrames, 0.0f);
        fInputGain.prepare(sampleRate);
        fOutputGain.prepare(sampleRate);
        // DC blocker: one-pole high-pass at ~5 Hz, as models can output a small DC offset
        fDcCoef = static_cast<float>(std::exp(-2.0 * M_PI * 5.0 / sampleRate));
        fInputGain.snap(dbToGain(fInputDb));
        fOutputGain.snap(targetOutputGain());
        fTone.prepare(sampleRate);
        resetModels();
    }

    bool configure(const nlohmann::json& config) override
    {
        const std::string path = config.value("path", std::string());
        const double a2Size = config.value("a2Size", 1.0);
        fHasTone = config.value(kNamToneKey, false);
        updateTone();
        if (path.empty())
            return false;
        // a file inside a zip is extracted to the cache first
        const std::optional<std::string> file = materializeLibraryFile(path);
        if (!file)
        {
            std::fprintf(stderr, "Xythos: can't open %s\n", path.c_str());
            return false;
        }

        try
        {
            // One model per channel, because models keep state. In mono mode only the first runs.
            for (auto& model : fModels)
            {
                model = nam::get_dsp(pathFromUtf8(*file));
                if (!model || model->NumInputChannels() != 1 || model->NumOutputChannels() != 1)
                    return false;
                if (auto* slimmable = dynamic_cast<nam::SlimmableModel*>(model.get()))
                    slimmable->SetSlimmableSize(a2Size);
            }
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "Xythos: can't load %s: %s\n", path.c_str(), e.what());
            return false;
        }

        fLoudnessDb = fModels[0]->HasLoudness() ? fModels[0]->GetLoudness() : 0.0;
        fHasLoudness = fModels[0]->HasLoudness();
        resetModels();
        fOutputGain.snap(targetOutputGain());
        return true;
    }

    void setNormalizeLevels(const NormalizeLevels& levels) override
    {
        if (levels.namDb == fTargetLoudnessDb)
            return;
        fTargetLoudnessDb = levels.namDb;
        fOutputGain.setTarget(targetOutputGain());
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kNamParamInput:
            fInputDb = value;
            fInputGain.setTarget(dbToGain(value));
            break;
        case kNamParamOutput:
            fOutputDb = value;
            fOutputGain.setTarget(targetOutputGain());
            break;
        case kNamParamNormalize:
            fNormalize = value >= 0.5f;
            fOutputGain.setTarget(targetOutputGain());
            break;
        case kNamParamBass:
        case kNamParamMiddle:
        case kNamParamTreble:
            fToneDb[index - kNamParamBass] = value;
            updateTone();
            break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (!fModels[0])
            return;

        // Gains are smoothed once per buffer for both channels, so they stay identical in stereo
        const Smoother inputStart = fInputGain;
        const Smoother outputStart = fOutputGain;
        for (uint32_t ch = 0; ch < numChannels; ++ch)
        {
            fInputGain = inputStart;
            fOutputGain = outputStart;

            float* x = io[ch];
            for (uint32_t i = 0; i < frames; ++i)
                x[i] *= fInputGain.next();

            NAM_SAMPLE* in[1] = { x };
            NAM_SAMPLE* out[1] = { fOut[ch].data() };
            fModels[ch]->process(in, out, static_cast<int>(frames));

            const float* y = fOut[ch].data();
            float dcIn = fDcIn[ch], dcOut = fDcOut[ch];
            for (uint32_t i = 0; i < frames; ++i)
            {
                dcOut = y[i] - dcIn + fDcCoef * dcOut;
                dcIn = y[i];
                x[i] = dcOut * fOutputGain.next();
            }
            fDcIn[ch] = dcIn;
            fDcOut[ch] = dcOut;
        }
        fTone.process(io, numChannels, frames);
    }

    void reset() override
    {
        resetModels();
        fTone.reset();
        fDcIn[0] = fDcIn[1] = fDcOut[0] = fDcOut[1] = 0.0f;
    }

private:
    float targetOutputGain() const
    {
        float db = fOutputDb;
        if (fNormalize && fHasLoudness)
            db += static_cast<float>(fTargetLoudnessDb - fLoudnessDb);
        return dbToGain(db);
    }

    // flat (bypassed exactly) for a block from before the tone controls
    void updateTone()
    {
        if (fHasTone)
            fTone.setGains(fToneDb[0], fToneDb[1], fToneDb[2]);
        else
            fTone.setGains(0.0f, 0.0f, 0.0f);
    }

    void resetModels()
    {
        for (auto& model : fModels)
        {
            if (model && fMaxFrames > 0)
                model->Reset(fSampleRate, static_cast<int>(fMaxFrames));
        }
    }

    std::unique_ptr<nam::DSP> fModels[2];
    std::vector<float> fOut[2];
    double fSampleRate = 48000.0;
    uint32_t fMaxFrames = 0;
    double fLoudnessDb = 0.0;
    float fTargetLoudnessDb = NormalizeLevels {}.namDb;
    bool fHasLoudness = false;
    float fInputDb = 0.0f;
    float fOutputDb = 0.0f;
    bool fNormalize = true;
    Smoother fInputGain, fOutputGain;
    ToneStack fTone;
    float fToneDb[3] {};
    bool fHasTone = false;
    float fDcCoef = 0.9993f;
    float fDcIn[2] {}, fDcOut[2] {};
};

} // namespace

const BlockDescriptor kNamBlockDescriptor = {
    "nam",
    "NAM",
    Category::Nam,
    kNamParamCount,
    {
        ParamSpec { "Input", "dB", -24.0f, 24.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Output", "dB", -24.0f, 24.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Normalize", "", 0.0f, 1.0f, 1.0f, ParamScale::Toggle },
        ParamSpec { "Bass", "dB", -12.0f, 12.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Middle", "dB", -12.0f, 12.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Treble", "dB", -12.0f, 12.0f, 0.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new NamBlock()); },
};

} // namespace xythos
