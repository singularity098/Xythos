// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/AmbientBlocks.hpp"

#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/Fdn.hpp"
#include "engine/dsp/OnsetDetector.hpp"
#include "engine/dsp/PitchShifter.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/SpectralShifter.hpp"
#include "engine/dsp/Waveshapers.hpp"

#include <algorithm>
#include <cmath>

namespace xythos {

namespace {

constexpr const char* kIntervalNames[] = { "+1 Oct", "+5th", "+Oct+5th", "+2 Oct", "-1 Oct" };
constexpr double kIntervalRatios[] = { 2.0, 1.4983, 2.9966, 4.0, 0.5 };
constexpr double kShimmerWindowMs = 80.0;
constexpr double kMaxSize = 1.5;

class ShimmerBlock : public Block {
public:
    ShimmerBlock()
    {
        fShimmer.snap(0.5f);
        fMix.snap(0.35f);
    }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fTank.prepare(sampleRate, kMaxSize);
        fShiftLine.prepare(static_cast<size_t>(std::ceil(kShimmerWindowMs / 1000.0 * sampleRate)) + 8);
        fShimmer.prepare(sampleRate, 0.05);
        fMix.prepare(sampleRate);
        fFeedbackHighPass.setLowPass(sampleRate, 150.0);
        if (fHq)
            fHqShifter.prepare(sampleRate);
        applyAll();
        reset();
    }

    // "hq": the tail is shifted by a spectral shifter (cleaner, no splice warble in the rising tail). Its latency only
    // delays the shimmer's regeneration a little, so the block reports none.
    bool configure(const nlohmann::json& config) override
    {
        fHq = hqFromConfig(config);
        if (fHq)
            fHqShifter.prepare(fSampleRate);
        reset();
        return true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kShimmerParamDecay:    fDecay = value; break;
        case kShimmerParamShimmer:  fShimmer.setTarget(value / 100.0f); break;
        case kShimmerParamInterval: fRatio = kIntervalRatios[std::clamp(static_cast<int>(value), 0, 4)]; break;
        case kShimmerParamTone:     fTone = value; break;
        case kShimmerParamSize:     fSize = value / 100.0f; break;
        case kShimmerParamLowCut:   fLowCut = value; break;
        case kShimmerParamMix:      fMix.setTarget(value / 100.0f); break;
        }
        applyAll();
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fShimmer.snap(fShimmer.target());
            fMix.snap(fMix.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (fHq && fHqPos == kHqChunk)
            {
                // HQ: the last chunk of the tail through the spectral shifter, played back over the next chunk
                const float* in[1] = { fHqIn };
                float* out[1] = { fHqOut };
                fHqShifter.setRatio(fRatio);
                fHqShifter.process(in, out, 1, kHqChunk);
                fHqPos = 0;
            }
            const float inL = io[0][i];
            const float inR = numChannels > 1 ? io[1][i] : inL;
            const float shimmer = fShimmer.next();
            const float mix = fMix.next();

            // the tail, pitch shifted and fed back in; soft-limited, and scaled so the loop's gain stays under one
            // however long the decay
            const float tail = fFeedbackHighPass.highPass(0.5f * (fLastL + fLastR));
            float shifted;
            if (fHq)
            {
                fHqIn[fHqPos] = tail;
                shifted = fHqOut[fHqPos++];
            }
            else
            {
                fShiftLine.write(tail);
                shifted = fVoice.read(fShiftLine, fRatio, shimmerWindow());
            }
            const float feed = shaper::soft(shifted * shimmer * fFeedScale);

            float wetL, wetR;
            fTank.process(inL + feed, inR + feed, wetL, wetR);
            fLastL = wetL;
            fLastR = wetR;
            wetL = fLowCutFilter[0].process(wetL);
            wetR = fLowCutFilter[1].process(wetR);

            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix);
            if (numChannels == 1)
            {
                io[0][i] = inL * dryGain + 0.5f * (wetL + wetR) * wetGain;
            }
            else
            {
                io[0][i] = inL * dryGain + wetL * wetGain;
                io[1][i] = inR * dryGain + wetR * wetGain;
            }
        }
    }

    void reset() override
    {
        fTank.reset();
        fShiftLine.reset();
        fVoice.reset(shimmerWindow());
        fFeedbackHighPass.reset();
        fLowCutFilter[0].reset();
        fLowCutFilter[1].reset();
        fLastL = fLastR = 0.0f;
        fHqShifter.reset();
        std::fill(std::begin(fHqIn), std::end(fHqIn), 0.0f);
        std::fill(std::begin(fHqOut), std::end(fHqOut), 0.0f);
        fHqPos = 0;
        fSnap = true;
    }

private:
    static constexpr uint32_t kHqChunk = 32;

    float shimmerWindow() const { return static_cast<float>(kShimmerWindowMs / 1000.0 * fSampleRate); }

    void applyAll()
    {
        fTank.setSize(0.5 + fSize);
        fTank.setDecay(fDecay);
        fTank.setDamping(fTone);
        for (Biquad& filter : fLowCutFilter)
            filter.setHighPass(fSampleRate, fLowCut);
        // the longer the decay, the more the tank sustains what's fed in: feed back less
        fFeedScale = static_cast<float>(0.9 / std::sqrt(1.0 + fDecay));
    }

    double fSampleRate = 48000.0;
    Fdn fTank;
    DelayLine fShiftLine;
    PitchVoice fVoice;
    OnePole fFeedbackHighPass;
    Biquad fLowCutFilter[2];
    Smoother fShimmer, fMix;
    float fDecay = 6.0f;
    double fRatio = 2.0;
    float fTone = 6000.0f;
    float fSize = 0.6f;
    float fLowCut = 150.0f;
    float fFeedScale = 0.3f;
    float fLastL = 0.0f, fLastR = 0.0f;
    bool fSnap = true;
    bool fHq = false;
    SpectralShifter fHqShifter;
    float fHqIn[kHqChunk] {};
    float fHqOut[kHqChunk] {};
    uint32_t fHqPos = 0;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr double kFreezeCaptureDecay = 1.5;  // while not frozen, the tank keeps about this much of the recent sound
constexpr double kLayerDecay = 60.0;         // layering: new playing adds in, and the oldest slowly fades

class FreezeBlock : public Block {
public:
    FreezeBlock() { fLevel.snap(0.7f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fTank.prepare(sampleRate, 1.2);
        fTank.setSize(1.2);
        fTank.setDamping(12000.0);
        fLevel.prepare(sampleRate);
        updateFades();
        updateTone();
        updateTank();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kFreezeParamFreeze:
            fFreeze = value >= 0.5f;
            updateTank();
            break;
        case kFreezeParamLayer:
            fLayer = value >= 0.5f;
            updateTank();
            break;
        case kFreezeParamFadeIn:
            fFadeInMs = value;
            updateFades();
            break;
        case kFreezeParamFadeOut:
            fFadeOutMs = value;
            updateFades();
            break;
        case kFreezeParamTone:
            fTone = value;
            updateTone();
            break;
        case kFreezeParamLevel:
            fLevel.setTarget(value / 100.0f);
            break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fLevel.snap(fLevel.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float inL = io[0][i];
            const float inR = numChannels > 1 ? io[1][i] : inL;
            // layering feeds new playing in while holding; otherwise the tank only listens while not frozen
            const float feed = fFreeze && !fLayer ? 0.0f : 1.0f;
            float wetL, wetR;
            fTank.process(inL * feed, inR * feed, wetL, wetR);

            if (fFreeze)
                fFade = std::min(1.0f, fFade + fFadeInStep);
            else
                fFade = std::max(0.0f, fFade - fFadeOutStep);
            const float gain = fFade * fFade * fLevel.next() * 2.0f;

            if (fToneOn)
            {
                wetL = fToneFilter[0].process(wetL);
                wetR = fToneFilter[1].process(wetR);
            }
            if (numChannels == 1)
            {
                io[0][i] = inL + 0.5f * (wetL + wetR) * gain;
            }
            else
            {
                io[0][i] = inL + wetL * gain;
                io[1][i] = inR + wetR * gain;
            }
        }
    }

    void reset() override
    {
        fTank.reset();
        fToneFilter[0].reset();
        fToneFilter[1].reset();
        fFade = 0.0f;
        fSnap = true;
    }

private:
    void updateTank()
    {
        if (fFreeze && fLayer)
        {
            fTank.setFrozen(false);
            fTank.setDecay(kLayerDecay);
        }
        else
        {
            fTank.setFrozen(fFreeze);
            fTank.setDecay(kFreezeCaptureDecay);
        }
    }

    void updateFades()
    {
        fFadeInStep = static_cast<float>(1.0 / (std::max(1.0f, fFadeInMs) / 1000.0 * fSampleRate));
        fFadeOutStep = static_cast<float>(1.0 / (std::max(1.0f, fFadeOutMs) / 1000.0 * fSampleRate));
    }

    void updateTone()
    {
        fToneOn = fTone < 19900.0f;
        for (Biquad& filter : fToneFilter)
            filter.setLowPass(fSampleRate, std::min<double>(fTone, 0.45 * fSampleRate));
    }

    double fSampleRate = 48000.0;
    Fdn fTank;
    Biquad fToneFilter[2];
    Smoother fLevel;
    bool fFreeze = false;
    bool fLayer = false;
    float fFadeInMs = 300.0f, fFadeOutMs = 1500.0f;
    float fFadeInStep = 0.0f, fFadeOutStep = 0.0f;
    float fTone = 8000.0f;
    bool fToneOn = true;
    float fFade = 0.0f;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr double kBloomMaxSize = 1.5;
constexpr double kBloomDuckMs = 30.0;

class BloomReverbBlock : public Block {
public:
    BloomReverbBlock() { fMix.snap(0.4f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fTank.prepare(sampleRate, kBloomMaxSize);
        fOnset.prepare(sampleRate);
        fMix.prepare(sampleRate);
        fDuckStep = static_cast<float>(1.0 / (kBloomDuckMs / 1000.0 * sampleRate));
        applyAll();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kBloomParamDecay:  fDecay = value; break;
        case kBloomParamBloom:  fBloomMs = value; break;
        case kBloomParamSize:   fSize = value / 100.0f; break;
        case kBloomParamTone:   fTone = value; break;
        case kBloomParamLowCut: fLowCut = value; break;
        case kBloomParamMix:    fMix.setTarget(value / 100.0f); break;
        }
        applyAll();
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float inL = io[0][i];
            const float inR = numChannels > 1 ? io[1][i] : inL;
            // a new note: take the feed down quickly (what's already in the tank rings on), then swell it back up
            if (fOnset.process(std::max(std::fabs(inL), std::fabs(inR))))
                fDucking = true;
            if (fDucking)
            {
                fFeed = std::max(0.0f, fFeed - fDuckStep);
                if (fFeed <= 0.0f)
                    fDucking = false;
            }
            else
            {
                fFeed = std::min(1.0f, fFeed + fBloomStep);
            }
            const float feed = fFeed * fFeed * (3.0f - 2.0f * fFeed);

            float wetL, wetR;
            fTank.process(inL * feed, inR * feed, wetL, wetR);
            wetL = fLowCutFilter[0].process(wetL);
            wetR = fLowCutFilter[1].process(wetR);

            const float mix = fMix.next();
            // the swell takes some level away; make it up so the bloom is as big as a plain reverb's
            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix) * 1.6f;
            if (numChannels == 1)
            {
                io[0][i] = inL * dryGain + 0.5f * (wetL + wetR) * wetGain;
            }
            else
            {
                io[0][i] = inL * dryGain + wetL * wetGain;
                io[1][i] = inR * dryGain + wetR * wetGain;
            }
        }
    }

    void reset() override
    {
        fTank.reset();
        fOnset.reset();
        fLowCutFilter[0].reset();
        fLowCutFilter[1].reset();
        fFeed = 0.0f;
        fDucking = false;
        fSnap = true;
    }

private:
    void applyAll()
    {
        fTank.setSize(0.5 + fSize);
        fTank.setDecay(fDecay);
        fTank.setDamping(fTone);
        for (Biquad& filter : fLowCutFilter)
            filter.setHighPass(fSampleRate, fLowCut);
        fBloomStep = static_cast<float>(1.0 / (std::max(1.0f, fBloomMs) / 1000.0 * fSampleRate));
    }

    double fSampleRate = 48000.0;
    Fdn fTank;
    OnsetDetector fOnset;
    Biquad fLowCutFilter[2];
    Smoother fMix;
    float fDecay = 5.0f;
    float fBloomMs = 1200.0f;
    float fSize = 0.7f;
    float fTone = 7000.0f;
    float fLowCut = 120.0f;
    float fFeed = 0.0f, fBloomStep = 0.0f, fDuckStep = 0.0f;
    bool fDucking = false;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

class GatedReverbBlock : public Block {
public:
    GatedReverbBlock() { fMix.snap(0.4f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fTank.prepare(sampleRate, 1.2);
        fEnvelope.prepare(sampleRate, 1.0, 30.0);
        fMix.prepare(sampleRate);
        fOpenCoef = EnvelopeFollower::coef(sampleRate, 1.0);
        applyAll();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kGatedRevParamSize:      fSize = value / 100.0f; break;
        case kGatedRevParamDecay:     fDecay = value; break;
        case kGatedRevParamThreshold: fThreshold = dbToGain(value); break;
        case kGatedRevParamHold:      fHoldMs = value; break;
        case kGatedRevParamRelease:   fReleaseMs = value; break;
        case kGatedRevParamTone:      fTone = value; break;
        case kGatedRevParamMix:       fMix.setTarget(value / 100.0f); break;
        }
        applyAll();
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float inL = io[0][i];
            const float inR = numChannels > 1 ? io[1][i] : inL;
            const float env = fEnvelope.process(std::max(std::fabs(inL), std::fabs(inR)));
            if (env > fThreshold)
                fHoldLeft = fHoldSamples;
            else if (fHoldLeft > 0)
                --fHoldLeft;
            // opens at once, closes over Release (an S-curve, so the cut is quick but click-free)
            if (fHoldLeft > 0)
                fGate = 1.0f + (fGate - 1.0f) * fOpenCoef;
            else
                fGate = std::max(0.0f, fGate - fCloseStep);
            const float gate = fGate * fGate * (3.0f - 2.0f * fGate);

            float wetL, wetR;
            fTank.process(inL, inR, wetL, wetR);
            const float mix = fMix.next();
            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix) * gate;
            if (numChannels == 1)
            {
                io[0][i] = inL * dryGain + 0.5f * (wetL + wetR) * wetGain;
            }
            else
            {
                io[0][i] = inL * dryGain + wetL * wetGain;
                io[1][i] = inR * dryGain + wetR * wetGain;
            }
        }
    }

    void reset() override
    {
        fTank.reset();
        fEnvelope.reset();
        fHoldLeft = 0;
        fGate = 0.0f;
        fSnap = true;
    }

private:
    void applyAll()
    {
        fTank.setSize(0.3 + 0.9 * fSize);
        fTank.setDecay(fDecay);
        fTank.setDamping(fTone);
        fHoldSamples = static_cast<int>(fHoldMs / 1000.0f * fSampleRate);
        fCloseStep = static_cast<float>(1.0 / (std::max(1.0f, fReleaseMs) / 1000.0 * fSampleRate));
    }

    double fSampleRate = 48000.0;
    Fdn fTank;
    EnvelopeFollower fEnvelope;
    Smoother fMix;
    float fSize = 0.5f;
    float fDecay = 2.0f;
    float fThreshold = 0.01f;
    float fHoldMs = 250.0f;
    float fReleaseMs = 40.0f;
    float fTone = 7000.0f;
    int fHoldSamples = 12000;
    int fHoldLeft = 0;
    float fGate = 0.0f, fOpenCoef = 0.0f, fCloseStep = 0.0f;
    bool fSnap = true;
};

} // namespace

const BlockDescriptor kShimmerBlockDescriptor = {
    "shimmer",
    "Shimmer",
    Category::Reverb,
    kShimmerParamCount,
    {
        ParamSpec { "Decay", "s", 1.0f, 20.0f, 6.0f, ParamScale::Log },
        ParamSpec { "Shimmer", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Interval", "", 0.0f, 4.0f, 0.0f, ParamScale::Choice, kIntervalNames },
        ParamSpec { "Tone", "Hz", 1000.0f, 16000.0f, 6000.0f, ParamScale::Log },
        ParamSpec { "Size", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Low Cut", "Hz", 20.0f, 1000.0f, 150.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 35.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new ShimmerBlock()); },
    -1,
    -1,
    false,
    nullptr,
    24.0f,
    MeterStyle::Reduction,
    false,
    false,
    true, // hq
};

const BlockDescriptor kFreezeBlockDescriptor = {
    "freeze",
    "Freeze",
    Category::Reverb,
    kFreezeParamCount,
    {
        ParamSpec { "Freeze", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Layer", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Fade In", "ms", 10.0f, 3000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Fade Out", "ms", 50.0f, 10000.0f, 1500.0f, ParamScale::Log },
        ParamSpec { "Tone", "Hz", 1000.0f, 20000.0f, 8000.0f, ParamScale::Log },
        ParamSpec { "Level", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new FreezeBlock()); },
};

const BlockDescriptor kBloomReverbBlockDescriptor = {
    "bloomreverb",
    "Bloom Reverb",
    Category::Reverb,
    kBloomParamCount,
    {
        ParamSpec { "Decay", "s", 1.0f, 20.0f, 5.0f, ParamScale::Log },
        ParamSpec { "Bloom", "ms", 50.0f, 5000.0f, 1200.0f, ParamScale::Log },
        ParamSpec { "Size", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 1000.0f, 16000.0f, 7000.0f, ParamScale::Log },
        ParamSpec { "Low Cut", "Hz", 20.0f, 1000.0f, 120.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 40.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new BloomReverbBlock()); },
};

const BlockDescriptor kGatedReverbBlockDescriptor = {
    "gatedreverb",
    "Gated Reverb",
    Category::Reverb,
    kGatedRevParamCount,
    {
        ParamSpec { "Size", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Decay", "s", 0.5f, 6.0f, 2.0f, ParamScale::Log },
        ParamSpec { "Threshold", "dB", -70.0f, -10.0f, -40.0f, ParamScale::Linear },
        ParamSpec { "Hold", "ms", 10.0f, 1000.0f, 250.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 5.0f, 500.0f, 40.0f, ParamScale::Log },
        ParamSpec { "Tone", "Hz", 1000.0f, 16000.0f, 7000.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 40.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new GatedReverbBlock()); },
};

} // namespace xythos
