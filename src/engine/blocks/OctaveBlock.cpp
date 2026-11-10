// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/OctaveBlock.hpp"

#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <cmath>

namespace xythos {

namespace {

constexpr float kHysteresis = 0.25f; // of the signal's envelope: how far past zero it must swing to arm a toggle
constexpr float kGateLevel = 0.002f; // below this envelope the octaves fade out (no toggling on noise)

class OctaveBlock : public Block {
public:
    OctaveBlock()
    {
        fSub1.snap(0.7f);
        fSub2.snap(0.0f);
        fUp.snap(0.0f);
        fDry.snap(0.7f);
    }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        for (Smoother* s : { &fSub1, &fSub2, &fUp, &fDry })
            s->prepare(sampleRate);
        fGate.prepare(sampleRate, 0.01);
        fInput.prepare(sampleRate);
        updateFilters();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kOctParamSub1: fSub1.setTarget(value / 100.0f); break;
        case kOctParamSub2: fSub2.setTarget(value / 100.0f); break;
        case kOctParamUp:   fUp.setTarget(value / 100.0f); break;
        case kOctParamDry:  fDry.setTarget(value / 100.0f); break;
        case kOctParamTone:
            fTone = value;
            updateFilters();
            break;
        case kOctParamTracking:
            fTracking = value;
            updateFilters();
            break;
        case kOctParamTrack:
            fInput.setTrackRig(value);
            break;
        }
    }

    bool configure(const nlohmann::json& config) override
    {
        fInput.configure(config);
        return true;
    }

    uint32_t latency() const override { return fInput.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override { fInput.setRig(input, numChannels); }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            for (Smoother* s : { &fSub1, &fSub2, &fUp, &fDry })
                s->snap(s->target());
        }

        for (uint32_t i = 0; i < frames; ++i)
        {
            // once the note is known, the detector's low-pass sits just above it, so harmonics (often stronger than
            // the fundamental on a plucked string) can't add zero crossings. (io[..][i] is delayed by the lookahead
            // from here on, so with lookahead the low-pass is set when the note arrives.)
            const NoteTracker::Note& note = fInput.process(io, numChannels, i);
            if (note.on && note.hz != fTrackedHz)
            {
                fTrackedHz = note.hz;
                setTrackLowPass(1.3 * note.hz);
            }
            // one detector for both channels (it's a single-note effect)
            float in = io[0][i];
            if (numChannels > 1)
                in = 0.5f * (in + io[1][i]);
            const float tracked = fTrack[2].process(fTrack[1].process(fTrack[0].process(in)));
            const float env = fEnvelope.process(tracked);

            // arm on a swing past +hysteresis, toggle at the next downward zero crossing: the polarity switches
            // where the signal is near zero, so it doesn't click
            const float threshold = kHysteresis * env;
            if (tracked > threshold)
                fArmed = true;
            if (fArmed && fPrevious > 0.0f && tracked <= 0.0f)
            {
                fArmed = false;
                fDiv1 = -fDiv1;
                if (fDiv1 > 0.0f)
                    fDiv2 = -fDiv2;
            }
            fPrevious = tracked;

            fGate.setTarget(env > kGateLevel ? 1.0f : 0.0f);
            const float gate = fGate.next();
            const float sub1 = fSub1.next(), sub2 = fSub2.next(), up = fUp.next(), dry = fDry.next();

            // the octaves are made from the tracked (low-passed) signal, as in the original
            float octaves = (tracked * fDiv1 * sub1 + tracked * fDiv2 * sub2) * 1.6f;
            octaves += fUpHighPass.highPass(std::fabs(tracked)) * up * 2.5f;
            octaves = fToneFilter[1].process(fToneFilter[0].process(octaves)) * gate;

            for (uint32_t ch = 0; ch < numChannels; ++ch)
                io[ch][i] = io[ch][i] * dry + octaves;
        }
    }

    void reset() override
    {
        for (Biquad& filter : fTrack)
            filter.reset();
        for (Biquad& filter : fToneFilter)
            filter.reset();
        fInput.reset();
        fTrackedHz = 0.0;
        setTrackLowPass(fTracking);
        fUpHighPass.reset();
        fEnvelope.reset();
        fArmed = false;
        fPrevious = 0.0f;
        fDiv1 = fDiv2 = 1.0f;
        fGate.snap(0.0f);
        fSnap = true;
    }

private:
    void updateFilters()
    {
        const double nyquistSafe = 0.45 * fSampleRate;
        fTrack[0].setHighPass(fSampleRate, 50.0);
        setTrackLowPass(fTracking);
        fToneFilter[0].setLowPass(fSampleRate, std::min<double>(fTone, nyquistSafe));
        fToneFilter[1].setLowPass(fSampleRate, std::min<double>(fTone, nyquistSafe));
        fUpHighPass.setLowPass(fSampleRate, 40.0);
        // fast enough to follow a pick attack, slow enough to hold through a low note's cycle
        fEnvelope.prepare(fSampleRate, 1.0, 60.0);
    }

    // the detector's low-pass: two sections (fourth order)
    void setTrackLowPass(double hz)
    {
        const double f = std::clamp(hz, 60.0, 0.45 * fSampleRate);
        fTrack[1].setLowPass(fSampleRate, f);
        fTrack[2].setLowPass(fSampleRate, f);
    }

    double fSampleRate = 48000.0;
    float fTone = 1500.0f;
    float fTracking = 700.0f;
    Biquad fTrack[3], fToneFilter[2];
    TrackedInput fInput;
    double fTrackedHz = 0.0;
    OnePole fUpHighPass;
    EnvelopeFollower fEnvelope;
    Smoother fSub1, fSub2, fUp, fDry, fGate;
    bool fArmed = false;
    float fPrevious = 0.0f;
    float fDiv1 = 1.0f, fDiv2 = 1.0f;
    bool fSnap = true;
};

} // namespace

const BlockDescriptor kOctaveBlockDescriptor = {
    "octave",
    "Octave",
    Category::Pitch,
    kOctParamCount,
    {
        ParamSpec { "Sub 1", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Sub 2", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Up", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 200.0f, 8000.0f, 1500.0f, ParamScale::Log },
        ParamSpec { "Tracking", "Hz", 200.0f, 2000.0f, 700.0f, ParamScale::Log },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new OctaveBlock()); },
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
