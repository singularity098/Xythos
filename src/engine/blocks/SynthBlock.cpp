// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/SynthBlock.hpp"

#include "engine/blocks/TrackedSynth.hpp"
#include "engine/dsp/Oscillator.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <cmath>

namespace xythos {

namespace {

constexpr const char* kWaveNames[] = { "Square", "Saw", "Pulse", "Sub" };
constexpr const char* kOctaveNames[] = { "-2", "-1", "0", "+1" };

constexpr float kEnvOctaves = 5.0f;

class SynthBlock : public TrackedSynth {
public:
    SynthBlock()
    {
        setAttack(5.0f);
        setRelease(30.0f);
        setGlide(10.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kSynthParamWave:
            fWave = static_cast<SynthWave>(std::clamp(static_cast<int>(value), 0, 3));
            break;
        case kSynthParamOctave:    setOctave(value); break;
        case kSynthParamCutoff:    fCutoff = value; break;
        case kSynthParamResonance: fQ = value; break;
        case kSynthParamEnv:       fEnvAmount = value / 100.0f; break;
        case kSynthParamAttack:    setAttack(value); break;
        case kSynthParamGlide:     setGlide(value); break;
        case kSynthParamMix:       setMix(value); break;
        case kSynthParamPickFilter: setPickFilter(value); break;
        case kSynthParamTrack:     setTrack(value); break;
        }
    }

protected:
    void resetVoice() override
    {
        fFilter.reset();
        fPhase = fSubPhase = 0.0;
    }

    void render(const Note& note, float& left, float& right) override
    {
        const double dt = note.dt;
        fPhase += dt;
        if (fPhase >= 1.0)
            fPhase -= 1.0;
        fSubPhase += 0.5 * dt;
        if (fSubPhase >= 1.0)
            fSubPhase -= 1.0;
        const float osc = oscillator(dt);
        // the filter opens with the guitar's envelope
        const double cutoff = fCutoff * std::exp2(fEnvAmount * kEnvOctaves * std::min(1.0f, note.envelope * 6.0f));
        fFilter.set(fSampleRate, std::min(cutoff, 0.45 * fSampleRate), fQ);
        left = right = fFilter.process(osc).low * note.amp * 0.5f;
    }

private:
    float oscillator(double dt) const
    {
        switch (fWave)
        {
        case SynthWave::Saw:
            return blepSaw(fPhase, dt);
        case SynthWave::Pulse:
            return blepPulse(fPhase, dt, 0.25);
        case SynthWave::Sub:
            return 0.6f * blepSaw(fPhase, dt) + 0.6f * blepPulse(fSubPhase, 0.5 * dt, 0.5);
        default:
            return blepPulse(fPhase, dt, 0.5);
        }
    }

    SynthWave fWave = SynthWave::Square;
    float fCutoff = 400.0f;
    float fQ = 4.0f;
    float fEnvAmount = 0.6f;
    Svf fFilter;
    double fPhase = 0.0, fSubPhase = 0.0;
};

} // namespace

const BlockDescriptor kSynthBlockDescriptor = {
    "synth",
    "Synth",
    Category::Synth,
    kSynthParamCount,
    {
        ParamSpec { "Wave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kWaveNames },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Cutoff", "Hz", 100.0f, 8000.0f, 400.0f, ParamScale::Log },
        ParamSpec { "Resonance", "", 0.5f, 15.0f, 4.0f, ParamScale::Log },
        ParamSpec { "Env", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 2000.0f, 5.0f, ParamScale::Log },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<SynthBlock>()); },
    -1,
    -1,
    false,
    nullptr,
    24.0f,
    MeterStyle::Reduction,
    true,
    false,
    false,
    true, // poly
};

} // namespace xythos
