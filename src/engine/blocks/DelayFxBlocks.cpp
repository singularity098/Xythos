// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/DelayFxBlocks.hpp"

#include "engine/TempoSync.hpp"
#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/PitchShifter.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/SpectralShifter.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace xythos {

namespace {

// Keeps runaway feedback bounded (the same knee as the other delays)
float limitFeedback(float x)
{
    const float a = std::fabs(x);
    if (a <= 0.5f)
        return x;
    return std::copysign(0.5f + 0.5f * std::tanh(2.0f * (a - 0.5f)), x);
}

// A time knob in ms, or (synced) the note value the same knob picks, in ms at `bpm`
double syncedMs(const ParamSpec& spec, float ms, bool sync, double bpm)
{
    return sync ? noteBeats(timeNoteIndex(spec.toNormalized(ms))) * 60000.0 / bpm : ms;
}

// ---------------------------------------------------------------------------------------------------------------------

class ReverseDelayBlock : public Block {
public:
    ReverseDelayBlock()
    {
        fFeedback.snap(0.3f);
        fMix.snap(0.5f);
    }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        // a reversed slice of Time needs twice Time of history: it starts at the newest sample and plays back
        for (DelayLine& line : fLines)
            line.prepare(static_cast<size_t>(std::ceil(2.0 * kRevDelayMaxMs / 1000.0 * sampleRate)) + 16);
        fFeedback.prepare(sampleRate);
        fMix.prepare(sampleRate);
        updateTime();
        updateTone();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kRevDelayParamTime:
            fTimeMs = value;
            updateTime();
            break;
        case kRevDelayParamSync:
            fSync = value >= 0.5f;
            updateTime();
            break;
        case kRevDelayParamFeedback: fFeedback.setTarget(value / 100.0f); break;
        case kRevDelayParamMix:      fMix.setTarget(value / 100.0f); break;
        case kRevDelayParamTone:
            fTone = value;
            updateTone();
            break;
        }
    }

    void setTempo(double bpm) override
    {
        if (bpm != fBpm)
        {
            fBpm = bpm;
            updateTime();
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fFeedback.snap(fFeedback.target());
            fMix.snap(fMix.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float feedback = fFeedback.next();
            const float mix = fMix.next();
            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix);
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                fLines[ch].write(x + limitFeedback(fLast[ch] * feedback));
                // a pitch voice at -1x is a reverse player: each tap sweeps back through a slice, Hann-faded, and the
                // two taps overlap by half
                float wet = fVoices[ch].read(fLines[ch], -1.0, fWindow);
                if (fToneOn)
                    wet = fToneFilter[ch].process(wet);
                fLast[ch] = wet;
                io[ch][i] = x * dryGain + wet * wetGain;
            }
        }
    }

    void reset() override
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].reset();
            fVoices[ch].reset(fWindow);
            fToneFilter[ch].reset();
            fLast[ch] = 0.0f;
        }
        fSnap = true;
    }

private:
    void updateTime()
    {
        const double ms = std::min(syncedMs(kReverseDelayBlockDescriptor.params[kRevDelayParamTime], fTimeMs, fSync, fBpm),
                                   kRevDelayMaxMs);
        // each tap's window covers twice the slice (it moves two samples back per sample)
        fWindow = static_cast<float>(2.0 * ms / 1000.0 * fSampleRate);
    }

    void updateTone()
    {
        fToneOn = fTone < 19900.0f;
        for (Biquad& filter : fToneFilter)
            filter.setLowPass(fSampleRate, std::min<double>(fTone, 0.45 * fSampleRate));
    }

    double fSampleRate = 48000.0;
    double fBpm = 120.0;
    float fTimeMs = 500.0f;
    bool fSync = false;
    float fTone = 8000.0f;
    bool fToneOn = true;
    float fWindow = 48000.0f;
    DelayLine fLines[2];
    PitchVoice fVoices[2];
    Biquad fToneFilter[2];
    float fLast[2] {};
    Smoother fFeedback, fMix;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr double kPitchDelayWindowMs = 40.0;

class PitchDelayBlock : public Block {
public:
    PitchDelayBlock()
    {
        fFeedback.snap(0.5f);
        fMix.snap(0.4f);
    }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].prepare(static_cast<size_t>(std::ceil(kPitchDelayMaxMs / 1000.0 * sampleRate)) + 8);
            fShiftLines[ch].prepare(static_cast<size_t>(std::ceil(kPitchDelayWindowMs / 1000.0 * sampleRate)) + 8);
            fLowCut[ch].setLowPass(sampleRate, 60.0);
        }
        fTime.prepare(sampleRate, 0.1);
        fFeedback.prepare(sampleRate);
        fMix.prepare(sampleRate);
        if (fHq)
            fHqShifter.prepare(sampleRate);
        updateTime();
        updateTone();
        reset();
    }

    // "hq": the repeats are shifted by a spectral shifter (cleaner, no splice warble). The repeat is read that much
    // earlier from the delay line, so the echoes still land on time, down to the shifter's latency (about 150 ms;
    // shorter times play at that). The dry signal isn't delayed, so the block reports no latency.
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
        case kPitchDelayParamTime:
            fTimeMs = value;
            updateTime();
            break;
        case kPitchDelayParamSync:
            fSync = value >= 0.5f;
            updateTime();
            break;
        case kPitchDelayParamFeedback: fFeedback.setTarget(value / 100.0f); break;
        case kPitchDelayParamMix:      fMix.setTarget(value / 100.0f); break;
        case kPitchDelayParamShift:    fShift = std::round(value); break;
        case kPitchDelayParamFine:     fFine = value; break;
        case kPitchDelayParamTone:
            fTone = value;
            updateTone();
            break;
        }
    }

    void setTempo(double bpm) override
    {
        if (bpm != fBpm)
        {
            fBpm = bpm;
            updateTime();
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fTime.snap(fTime.target());
            fFeedback.snap(fFeedback.target());
            fMix.snap(fMix.target());
        }
        const double ratio = semitonesToRatio(fShift + fFine / 100.0);
        const float window = static_cast<float>(kPitchDelayWindowMs / 1000.0 * fSampleRate);
        const float maxDelay = static_cast<float>(fLines[0].capacity() - 4);
        if (fHq)
        {
            processHq(io, numChannels, frames, ratio, maxDelay);
            return;
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float time = std::clamp(fTime.next(), 1.0f, maxDelay);
            const float feedback = fFeedback.next();
            const float mix = fMix.next();
            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix);
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                // the repeat, shifted; it goes round again, so each pass is shifted once more
                fShiftLines[ch].write(fLines[ch].readFractional(time));
                float wet = fVoices[ch].read(fShiftLines[ch], ratio, window);
                if (fToneOn)
                    wet = fToneFilter[ch].process(wet);
                fLines[ch].write(x + limitFeedback(fLowCut[ch].highPass(wet) * feedback));
                io[ch][i] = x * dryGain + wet * wetGain;
            }
        }
    }

    void reset() override
    {
        const float window = static_cast<float>(kPitchDelayWindowMs / 1000.0 * fSampleRate);
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].reset();
            fShiftLines[ch].reset();
            fVoices[ch].reset(window);
            fToneFilter[ch].reset();
            fLowCut[ch].reset();
        }
        fHqShifter.reset();
        fSnap = true;
    }

private:
    static constexpr uint32_t kHqChunk = 32;

    void processHq(float* const* io, uint32_t numChannels, uint32_t frames, double ratio, float maxDelay)
    {
        const auto latency = static_cast<float>(fHqShifter.latency());
        fHqShifter.setRatio(ratio);
        for (uint32_t start = 0; start < frames; start += kHqChunk)
        {
            const uint32_t n = std::min(kHqChunk, frames - start);
            // the chunk's repeats, read `latency` early (all from before the chunk: at least kHqChunk + 1 back)
            for (uint32_t i = 0; i < n; ++i)
            {
                const float time = std::clamp(fTime.next(), 1.0f, maxDelay);
                const float read = std::max(time - latency, static_cast<float>(kHqChunk + 1)) - static_cast<float>(i);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    fHqIn[ch][i] = fLines[ch].readFractional(read);
            }
            const float* in[2] = { fHqIn[0], fHqIn[1] };
            float* out[2] = { fHqOut[0], fHqOut[1] };
            fHqShifter.process(in, out, numChannels, n);
            for (uint32_t i = 0; i < n; ++i)
            {
                const float feedback = fFeedback.next();
                const float mix = fMix.next();
                const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = io[ch][start + i];
                    float wet = fHqOut[ch][i];
                    if (fToneOn)
                        wet = fToneFilter[ch].process(wet);
                    fLines[ch].write(x + limitFeedback(fLowCut[ch].highPass(wet) * feedback));
                    io[ch][start + i] = x * dryGain + wet * wetGain;
                }
            }
        }
    }

    void updateTime()
    {
        const double ms = std::min(syncedMs(kPitchDelayBlockDescriptor.params[kPitchDelayParamTime], fTimeMs, fSync, fBpm),
                                   kPitchDelayMaxMs);
        fTime.setTarget(static_cast<float>(ms / 1000.0 * fSampleRate));
    }

    void updateTone()
    {
        fToneOn = fTone < 19900.0f;
        for (Biquad& filter : fToneFilter)
            filter.setLowPass(fSampleRate, std::min<double>(fTone, 0.45 * fSampleRate));
    }

    double fSampleRate = 48000.0;
    double fBpm = 120.0;
    float fTimeMs = 400.0f;
    bool fSync = false;
    float fShift = 12.0f;
    float fFine = 0.0f;
    float fTone = 6000.0f;
    bool fToneOn = true;
    DelayLine fLines[2], fShiftLines[2];
    PitchVoice fVoices[2];
    Biquad fToneFilter[2];
    OnePole fLowCut[2];
    Smoother fTime, fFeedback, fMix;
    bool fSnap = true;
    bool fHq = false;
    SpectralShifter fHqShifter;
    float fHqIn[2][kHqChunk] {};
    float fHqOut[2][kHqChunk] {};
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr double kRepeatEdgeMs = 2.0;   // each repeat fades in and out over this, so it doesn't click
constexpr double kRepeatBlendMs = 3.0;  // the crossfade between your playing and the repeats
constexpr double kRepeatMaxPitch = 36.0; // semitones, however many repeats

class BeatRepeatBlock : public Block {
public:
    BeatRepeatBlock() { fMix.snap(1.0f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        // the longest slice, plus room so the slice isn't overwritten while the repeats fade out after a release
        const size_t size = static_cast<size_t>(std::ceil((kRepeatMaxMs + 20.0) / 1000.0 * sampleRate));
        for (std::vector<float>& history : fHistory)
            history.assign(size, 0.0f);
        fMix.prepare(sampleRate);
        fBlend.prepare(sampleRate, kRepeatBlendMs / 1000.0);
        updateSlice();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kRepeatParamRepeat: {
            const bool held = value >= 0.5f;
            if (held && !fHeld && fLatch)
                fLatched = !fLatched;
            fHeld = held;
            break;
        }
        case kRepeatParamLatch:
            if (fLatch && value < 0.5f)
                fLatched = false;
            fLatch = value >= 0.5f;
            break;
        case kRepeatParamSlice:
            fSliceMs = value;
            updateSlice();
            break;
        case kRepeatParamSync:
            fSync = value >= 0.5f;
            updateSlice();
            break;
        case kRepeatParamGate:    fGate = value / 100.0f; break;
        case kRepeatParamDecay:   fDecay = value / 100.0f; break;
        case kRepeatParamPitch:   fPitch = std::round(value); break;
        case kRepeatParamReverse: fReverse = value >= 0.5f; break;
        case kRepeatParamMix:     fMix.setTarget(value / 100.0f); break;
        }
    }

    void setTempo(double bpm) override
    {
        if (bpm != fBpm)
        {
            fBpm = bpm;
            updateSlice();
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        const bool wanted = fLatch ? fLatched : fHeld;
        if (wanted && !fRepeating)
            start();
        else if (!wanted)
            fRepeating = false;
        fBlend.setTarget(fRepeating ? 1.0f : 0.0f);

        const size_t size = fHistory[0].size();
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float blend = fBlend.next();
            const float mix = fMix.next();
            if (!fRepeating && blend < 1e-4f)
            {
                // not repeating: just remember what you play
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    fHistory[ch][fWrite] = io[ch][i];
                fWrite = (fWrite + 1) % size;
                continue;
            }
            // repeating (or fading back to your playing): the history stands still, so the slice stays intact

            // where in the slice, and the repeat's fades and gate
            const double sounding = fGate * fLength;
            const double edge = std::min(kRepeatEdgeMs / 1000.0 * fSampleRate, 0.25 * sounding);
            double env = std::min({ 1.0, fPos / edge, (sounding - fPos) / edge });
            env = std::max(0.0, env);
            const double at = fReverse ? fLength - 1.0 - fPos : fPos;
            const double clamped = std::clamp(at, 0.0, fLength - 1.0);
            const size_t whole = static_cast<size_t>(clamped);
            const auto t = static_cast<float>(clamped - static_cast<double>(whole));
            const size_t a = (fStart + whole) % size, b = (fStart + whole + 1) % size;
            const float gain = static_cast<float>(env) * fLevel * blend * mix;
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const std::vector<float>& history = fHistory[ch];
                const float repeat = history[a] + (history[b] - history[a]) * t;
                io[ch][i] = io[ch][i] * (1.0f - blend * mix) + repeat * gain;
            }

            fPos += fSpeed;
            if (fPos >= fLength)
            {
                fPos = 0.0;
                ++fCount;
                fLevel *= 1.0f - fDecay;
                fSpeed = semitonesToRatio(std::clamp(fPitch * fCount, -kRepeatMaxPitch, kRepeatMaxPitch));
            }
        }
    }

    uint32_t latched() const override { return fLatched ? 1u << kRepeatParamRepeat : 0u; }

    void setLatched(uint32_t bits) override { fLatched = (bits >> kRepeatParamRepeat) & 1u; }

    void reset() override
    {
        for (std::vector<float>& history : fHistory)
            std::fill(history.begin(), history.end(), 0.0f);
        fWrite = 0;
        fRepeating = false;
        fBlend.snap(0.0f);
        fSnap = true;
    }

private:
    // Takes the last Slice of the history and starts repeating it
    void start()
    {
        const size_t size = fHistory[0].size();
        fLength = std::clamp(fSliceSamples, 16.0, static_cast<double>(size) - 1.0);
        fStart = (fWrite + size - static_cast<size_t>(fLength)) % size;
        fPos = 0.0;
        fCount = 0;
        fLevel = 1.0f;
        fSpeed = 1.0;
        fRepeating = true;
    }

    void updateSlice()
    {
        const double ms = std::min(syncedMs(kBeatRepeatBlockDescriptor.params[kRepeatParamSlice], fSliceMs, fSync, fBpm),
                                   kRepeatMaxMs);
        fSliceSamples = ms / 1000.0 * fSampleRate;
    }

    double fSampleRate = 48000.0;
    double fBpm = 120.0;
    bool fHeld = false, fLatch = false, fLatched = false;
    float fSliceMs = 125.0f;
    bool fSync = false;
    float fGate = 1.0f;
    float fDecay = 0.0f;
    double fPitch = 0.0;
    bool fReverse = false;
    double fSliceSamples = 6000.0;

    std::vector<float> fHistory[2];
    size_t fWrite = 0;
    bool fRepeating = false;
    size_t fStart = 0;
    double fLength = 6000.0;
    double fPos = 0.0;
    int fCount = 0;
    float fLevel = 1.0f;
    double fSpeed = 1.0;
    Smoother fMix, fBlend;
    bool fSnap = true;
};

} // namespace

const BlockDescriptor kReverseDelayBlockDescriptor = {
    "reversedelay",
    "Reverse Delay",
    Category::Delay,
    kRevDelayParamCount,
    {
        ParamSpec { "Time", "ms", 100.0f, static_cast<float>(kRevDelayMaxMs), 500.0f, ParamScale::Log },
        ParamSpec { "Sync", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Feedback", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 1000.0f, 20000.0f, 8000.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new ReverseDelayBlock()); },
    kRevDelayParamSync,
    kRevDelayParamTime,
    false,
};

const BlockDescriptor kPitchDelayBlockDescriptor = {
    "pitchdelay",
    "Pitch Delay",
    Category::Delay,
    kPitchDelayParamCount,
    {
        ParamSpec { "Time", "ms", 20.0f, static_cast<float>(kPitchDelayMaxMs), 400.0f, ParamScale::Log },
        ParamSpec { "Sync", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Feedback", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 40.0f, ParamScale::Linear },
        ParamSpec { "Shift", "st", -12.0f, 12.0f, 12.0f, ParamScale::Linear },
        ParamSpec { "Fine", "ct", -50.0f, 50.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 1000.0f, 20000.0f, 6000.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new PitchDelayBlock()); },
    kPitchDelayParamSync,
    kPitchDelayParamTime,
    false,
    nullptr,
    24.0f,
    MeterStyle::Reduction,
    false,
    false,
    true, // hq
};

const BlockDescriptor kBeatRepeatBlockDescriptor = {
    "beatrepeat",
    "Beat Repeat",
    Category::Delay,
    kRepeatParamCount,
    {
        ParamSpec { "Repeat", "", 0.0f, 1.0f, 0.0f, ParamScale::Momentary },
        ParamSpec { "Latch", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Slice", "ms", 10.0f, static_cast<float>(kRepeatMaxMs), 125.0f, ParamScale::Log },
        ParamSpec { "Sync", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Gate", "%", 10.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Decay", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Pitch", "st", -12.0f, 12.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Reverse", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new BeatRepeatBlock()); },
    kRepeatParamSync,
    kRepeatParamSlice,
    false,
};

} // namespace xythos
