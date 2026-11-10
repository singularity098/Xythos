// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/SynthBlocks.hpp"

#include "engine/blocks/TrackedSynth.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/Lfo.hpp"
#include "engine/dsp/Oscillator.hpp"
#include "engine/dsp/Oversampler.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <cmath>

namespace xythos {

namespace {

constexpr const char* kOctaveNames[] = { "-2", "-1", "0", "+1" };
constexpr float kTwoPi = 6.2831853f;

// The filter cutoff for a base cutoff, opened by the playing level over up to five octaves
double openedCutoff(double cutoff, float amount, float envelope, double sampleRate)
{
    return std::min(cutoff * std::exp2(amount * 5.0f * std::min(1.0f, envelope * 6.0f)), 0.45 * sampleRate);
}

// ---------------------------------------------------------------------------------------------------------------------

constexpr int kSaws = 7;
// detune of each saw relative to the outer ones, slightly uneven so the beating never lines up
constexpr double kSawSpread[kSaws] = { -1.0, -0.62, -0.31, 0.0, 0.29, 0.6, 0.97 };

class SuperSawBlock : public TrackedSynth {
public:
    SuperSawBlock()
    {
        setAttack(10.0f);
        setRelease(300.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kSuperSawParamDetune:    fDetune = value; break;
        case kSuperSawParamWidth:     fWidth = value / 100.0f; break;
        case kSuperSawParamCutoff:    fCutoff = value; break;
        case kSuperSawParamResonance: fQ = value; break;
        case kSuperSawParamEnv:       fEnvAmount = value / 100.0f; break;
        case kSuperSawParamAttack:    setAttack(value); break;
        case kSuperSawParamRelease:   setRelease(value); break;
        case kSuperSawParamOctave:    setOctave(value); break;
        case kSuperSawParamGlide:     setGlide(value); break;
        case kSuperSawParamMix:       setMix(value); break;
        case kSuperSawParamPickFilter: setPickFilter(value); break;
        case kSuperSawParamTrack: setTrack(value); break;
        }
    }

protected:
    void resetVoice() override
    {
        // free-running saws start at scattered phases, like analog oscillators
        for (int k = 0; k < kSaws; ++k)
            fPhase[k] = std::fmod(0.137 + 0.618 * k, 1.0);
        fFilter[0].reset();
        fFilter[1].reset();
    }

    void render(const Note& note, float& left, float& right) override
    {
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < kSaws; ++k)
        {
            const double dt = note.dt * std::exp2(kSawSpread[k] * fDetune / 1200.0);
            advancePhase(fPhase[k], dt);
            const float v = blepSaw(fPhase[k], dt);
            const auto pan = static_cast<float>(fWidth * kSawSpread[k]);
            l += v * (1.0f - pan);
            r += v * (1.0f + pan);
        }
        const double cutoff = openedCutoff(fCutoff, fEnvAmount, note.envelope, fSampleRate);
        fFilter[0].set(fSampleRate, cutoff, fQ);
        fFilter[1].set(fSampleRate, cutoff, fQ);
        // seven saws at unrelated phases add up like noise: about the square root of seven
        const float gain = note.amp * 0.38f * 0.35f;
        left = fFilter[0].process(l).low * gain;
        right = fFilter[1].process(r).low * gain;
    }

private:
    float fDetune = 25.0f;
    float fWidth = 0.7f;
    float fCutoff = 3000.0f;
    float fQ = 1.0f;
    float fEnvAmount = 0.3f;
    double fPhase[kSaws] {};
    Svf fFilter[2];
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kFmRatioNames[] = { "1:0.5", "1:1", "1:2", "1:3", "1:3.5", "1:4", "1:7" };
constexpr double kFmRatios[] = { 0.5, 1.0, 2.0, 3.0, 3.5, 4.0, 7.0 };
constexpr int kFmRatioCount = sizeof(kFmRatios) / sizeof(kFmRatios[0]);

class FmSynthBlock : public TrackedSynth {
public:
    FmSynthBlock()
    {
        setAttack(2.0f);
        setRelease(500.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kFmParamRatio:
            fRatio = kFmRatios[std::clamp(static_cast<int>(value), 0, kFmRatioCount - 1)];
            break;
        case kFmParamIndex: fIndex = value; break;
        case kFmParamDecay:
            fDecayMs = value;
            updateDecay();
            break;
        case kFmParamSustain:  fSustain = value / 100.0f; break;
        case kFmParamFeedback: fFeedback = value / 100.0f; break;
        case kFmParamAttack:   setAttack(value); break;
        case kFmParamRelease:  setRelease(value); break;
        case kFmParamOctave:   setOctave(value); break;
        case kFmParamGlide:    setGlide(value); break;
        case kFmParamMix:      setMix(value); break;
        case kFmParamPickFilter: setPickFilter(value); break;
        case kFmParamTrack: setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override { updateDecay(); }

    void resetVoice() override
    {
        fCarrier = fModulator = 0.0;
        fLastMod = 0.0f;
        fBrightness = 0.0f;
    }

    void render(const Note& note, float& left, float& right) override
    {
        // each pick: full brightness, falling to Sustain
        if (note.onset)
            fBrightness = 1.0f;
        fBrightness *= fDecayCoef;
        float index = fIndex * (fSustain + (1.0f - fSustain) * fBrightness);
        // FM spreads sidebands about (index + 1) x the modulator's frequency each way: keep them under Nyquist
        const double modHz = note.hz * fRatio;
        index = std::min(index, static_cast<float>(std::max(0.0, 0.4 * fSampleRate / modHz - 1.0)));

        advancePhase(fCarrier, note.dt);
        advancePhase(fModulator, note.dt * fRatio);
        const float mod = std::sin(kTwoPi * static_cast<float>(fModulator) + fFeedback * 1.5f * fLastMod);
        fLastMod = mod;
        const float y = std::sin(kTwoPi * static_cast<float>(fCarrier) + index * mod) * note.amp * 0.4f;
        left = right = y;
    }

private:
    void updateDecay() { fDecayCoef = EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fDecayMs)); }

    double fRatio = 1.0;
    float fIndex = 3.0f;
    float fDecayMs = 400.0f;
    float fSustain = 0.2f;
    float fFeedback = 0.0f;
    float fDecayCoef = 0.0f;
    double fCarrier = 0.0, fModulator = 0.0;
    float fLastMod = 0.0f;
    float fBrightness = 0.0f;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kFoldGateNames[] = { "Follow", "Pluck" };

class FoldSynthBlock : public TrackedSynth {
public:
    FoldSynthBlock()
    {
        setAttack(3.0f);
        setRelease(80.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kFoldParamFold:     fFold = value / 100.0f; break;
        case kFoldParamEnv:      fEnvAmount = value / 100.0f; break;
        case kFoldParamSymmetry: fSymmetry = value / 100.0f; break;
        case kFoldParamGate:     fPluck = value >= 0.5f; break;
        case kFoldParamDecay:
            fDecayMs = value;
            updateDecay();
            break;
        case kFoldParamTone:   fTone = value; break;
        case kFoldParamOctave: setOctave(value); break;
        case kFoldParamGlide:  setGlide(value); break;
        case kFoldParamMix:    setMix(value); break;
        case kFoldParamPickFilter: setPickFilter(value); break;
        case kFoldParamTrack: setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fOversampler.prepare();
        fDcBlock.setLowPass(fSampleRate, 10.0);
        updateDecay();
    }

    void resetVoice() override
    {
        fPhase = 0.0;
        fOversampler.reset();
        fDcBlock.reset();
        fGateFilter.reset();
        fPing = fPeak = 0.0f;
    }

    void render(const Note& note, float& left, float& right) override
    {
        advancePhase(fPhase, note.dt);
        const float osc = std::sin(kTwoPi * static_cast<float>(fPhase));
        // the fold: up to ten times past the folder's edge, more as you dig in
        const float drive = std::min(1.0f, fFold + fEnvAmount * std::min(1.0f, note.envelope * 6.0f));
        const float gain = 1.0f + 9.0f * drive;
        const float offset = fSymmetry * 0.8f;
        // a sine folder: past the top the wave folds back down, again and again (run at 4x against aliasing)
        float folded = fOversampler.process(osc, [gain, offset](float v) {
            return std::sin(1.5707963f * (gain * v + offset));
        });
        folded = fDcBlock.highPass(folded);

        // the low-pass gate: one control for both loudness and brightness, like a vactrol
        float open;
        if (fPluck)
        {
            if (note.onset)
            {
                fPing = 1.0f;
                fPeak = 0.0f;
            }
            fPeak = std::max(fPeak, std::min(1.0f, note.envelope * 4.0f));
            fPing *= fDecayCoef;
            open = fPing * fPeak;
        }
        else
        {
            open = note.amp;
        }
        const double cutoff = std::min(80.0 * std::pow(fTone / 80.0, static_cast<double>(open)), 0.45 * fSampleRate);
        fGateFilter.set(fSampleRate, cutoff, 0.6);
        left = right = fGateFilter.process(folded).low * open * 0.35f;
    }

private:
    void updateDecay() { fDecayCoef = EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fDecayMs)); }

    float fFold = 0.3f;
    float fEnvAmount = 0.5f;
    float fSymmetry = 0.0f;
    bool fPluck = false;
    float fDecayMs = 300.0f;
    float fTone = 8000.0f;
    float fDecayCoef = 0.0f;
    double fPhase = 0.0;
    Oversampler4x fOversampler;
    OnePole fDcBlock;
    Svf fGateFilter;
    float fPing = 0.0f, fPeak = 0.0f;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kAcidWaveNames[] = { "Saw", "Square" };

class AcidSynthBlock : public TrackedSynth {
public:
    AcidSynthBlock()
    {
        setAttack(2.0f);
        setRelease(40.0f);
        setGlide(60.0f);
        setOctave(1.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kAcidParamWave:      fSquare = value >= 0.5f; break;
        case kAcidParamCutoff:    fCutoff = value; break;
        case kAcidParamResonance: fResonance = value / 100.0f; break;
        case kAcidParamEnvMod:    fEnvMod = value / 100.0f; break;
        case kAcidParamDecay:
            fDecayMs = value;
            updateDecay();
            break;
        case kAcidParamAccent: fAccent = value / 100.0f; break;
        case kAcidParamDrive:  fDrive = value / 100.0f; break;
        case kAcidParamOctave: setOctave(value); break;
        case kAcidParamGlide:  setGlide(value); break;
        case kAcidParamMix:    setMix(value); break;
        case kAcidParamPickFilter: setPickFilter(value); break;
        case kAcidParamTrack: setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override { updateDecay(); }

    void resetVoice() override
    {
        fPhase = 0.0;
        fFilter[0].reset();
        fFilter[1].reset();
        fEnv = 0.0f;
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
            fEnv = 1.0f;
        fEnv *= fDecayCoef;
        // harder playing: more accent, so the envelope opens further and the filter rings more
        const float accent = fAccent * std::min(1.0f, note.envelope * 8.0f);
        const double cutoff = std::min<double>(fCutoff * std::exp2(fEnvMod * 5.0f * fEnv * (1.0f + accent)), 0.45 * fSampleRate);
        const double q = std::min(0.7 + 14.0 * fResonance * (1.0 + 0.3 * accent), 20.0);

        advancePhase(fPhase, note.dt);
        float osc = fSquare ? blepPulse(fPhase, note.dt, 0.5) : blepSaw(fPhase, note.dt);
        const float driveGain = 1.0f + 4.0f * fDrive;
        osc = std::tanh(osc * driveGain) / std::tanh(driveGain);
        // two two-pole stages: a 24 dB slope, the resonance on the second
        fFilter[0].set(fSampleRate, cutoff, 0.7);
        fFilter[1].set(fSampleRate, cutoff, q);
        const float filtered = fFilter[1].process(fFilter[0].process(osc).low).low;
        // the resonant peak overdrives the output a little, which is half the squelch
        const float y = std::tanh(1.5f * filtered) * note.amp * 0.3f * (1.0f + 0.5f * accent);
        left = right = y;
    }

private:
    void updateDecay() { fDecayCoef = EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fDecayMs)); }

    bool fSquare = false;
    float fCutoff = 300.0f;
    float fResonance = 0.7f;
    float fEnvMod = 0.6f;
    float fDecayMs = 250.0f;
    float fAccent = 0.5f;
    float fDrive = 0.3f;
    float fDecayCoef = 0.0f;
    double fPhase = 0.0;
    Svf fFilter[2];
    float fEnv = 0.0f;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr double kEnsembleSlowHz = 0.63;
constexpr double kEnsembleFastHz = 5.7;
constexpr double kEnsembleMs = 4.0;   // the taps' middle delay
constexpr double kEnsembleSwingMs = 2.0;

class StringSynthBlock : public TrackedSynth {
public:
    StringSynthBlock()
    {
        setAttack(300.0f);
        setRelease(1200.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kStringParamLow:      fLevels[0] = value / 100.0f; break;
        case kStringParamMid:      fLevels[1] = value / 100.0f; break;
        case kStringParamHigh:     fLevels[2] = value / 100.0f; break;
        case kStringParamEnsemble: fEnsemble = value / 100.0f; break;
        case kStringParamTone:
            fTone = value;
            updateTone();
            break;
        case kStringParamAttack:  setAttack(value); break;
        case kStringParamRelease: setRelease(value); break;
        case kStringParamOctave:  setOctave(value); break;
        case kStringParamMix:     setMix(value); break;
        case kStringParamPickFilter: setPickFilter(value); break;
        case kStringParamTrack: setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        for (DelayLine& line : fLines)
            line.prepare(static_cast<size_t>(std::ceil((kEnsembleMs + 1.3 * kEnsembleSwingMs + 1.0) / 1000.0 * fSampleRate)));
        fSlow.prepare(fSampleRate);
        fSlow.setRate(kEnsembleSlowHz);
        fFast.prepare(fSampleRate);
        fFast.setRate(kEnsembleFastHz);
        updateTone();
    }

    void resetVoice() override
    {
        for (double& p : fPhase)
            p = 0.0;
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].reset();
            fToneFilter[ch].reset();
        }
        fSlow.reset();
        fFast.reset();
    }

    void render(const Note& note, float& left, float& right) override
    {
        // a divide-down organ's saws: an octave down, the note, an octave up
        static constexpr double kMultiples[3] = { 0.5, 1.0, 2.0 };
        float s = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const double dt = note.dt * kMultiples[k];
            advancePhase(fPhase[k], dt);
            s += blepSaw(fPhase[k], dt) * fLevels[k];
        }
        s *= note.amp * 0.45f;

        // the ensemble: three delays swept by a slow and a fast sine, a third of a cycle apart (left and right a
        // sixth apart), averaged
        const double slow = fSlow.advance(), fast = fFast.advance();
        const auto msToSamples = static_cast<float>(fSampleRate / 1000.0);
        float out[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].write(s);
            float wet = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const double offset = k / 3.0 + (ch == 1 ? 1.0 / 6.0 : 0.0);
                const float sweep = 0.8f * Lfo::shape(LfoShape::Sine, slow + offset)
                                  + 0.2f * Lfo::shape(LfoShape::Sine, fast + offset);
                const float delay = static_cast<float>(kEnsembleMs + kEnsembleSwingMs * fEnsemble * sweep) * msToSamples;
                wet += fLines[ch].readFractional(std::max(1.0f, delay));
            }
            wet /= 3.0f;
            out[ch] = fToneFilter[ch].process(s + (wet - s) * fEnsemble).low;
        }
        left = out[0];
        right = out[1];
    }

private:
    void updateTone()
    {
        for (Svf& filter : fToneFilter)
            filter.set(fSampleRate, std::min<double>(fTone, 0.45 * fSampleRate), 0.6);
    }

    float fLevels[3] = { 0.5f, 0.8f, 0.4f };
    float fEnsemble = 0.8f;
    float fTone = 4000.0f;
    double fPhase[3] {};
    DelayLine fLines[2];
    Lfo fSlow, fFast;
    Svf fToneFilter[2];
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kChipWaveNames[] = { "Pulse 12%", "Pulse 25%", "Pulse 50%", "Triangle" };
constexpr double kChipWidths[] = { 0.125, 0.25, 0.5 };
constexpr const char* kChipArpNames[] = { "Off", "Octave", "Major", "Minor", "Fifth" };
constexpr int kChipArps[][3] = { { 0, 0, 0 }, { 0, 12, 12 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 7, 12 } };
constexpr int kChipArpSizes[] = { 1, 2, 3, 3, 3 };
constexpr double kChipVibratoHz = 6.0;
constexpr double kChipVibratoCents = 40.0;
constexpr double kChipVibratoDelay = 0.15; // s after the pick, then fading in over the same again

class ChipSynthBlock : public TrackedSynth {
public:
    ChipSynthBlock()
    {
        setAttack(1.0f);
        setRelease(30.0f);
        setGlide(0.1f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kChipParamWave:     fWave = std::clamp(static_cast<int>(value), 0, 3); break;
        case kChipParamArp:      fArp = std::clamp(static_cast<int>(value), 0, 4); break;
        case kChipParamArpRate:  fArpHz = value; break;
        case kChipParamQuantize: fQuantize = value >= 0.5f; break;
        case kChipParamVibrato:  fVibrato = value / 100.0f; break;
        case kChipParamCrush:    fCrush = value / 100.0f; break;
        case kChipParamOctave:   setOctave(value); break;
        case kChipParamMix:      setMix(value); break;
        case kChipParamPickFilter: setPickFilter(value); break;
        case kChipParamTrack: setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fVibratoLfo.prepare(fSampleRate);
        fVibratoLfo.setRate(kChipVibratoHz);
    }

    void resetVoice() override
    {
        fPhase = fArpPhase = 0.0;
        fArpStep = 0;
        fSinceOnset = 0.0;
        fVibratoLfo.reset();
    }

    double targetHz(double tracked) override
    {
        if (!fQuantize || tracked <= 0.0)
            return tracked;
        return 440.0 * std::exp2(std::round(12.0 * std::log2(tracked / 440.0)) / 12.0);
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
        {
            fArpStep = 0;
            fArpPhase = 0.0;
            fSinceOnset = 0.0;
        }
        fSinceOnset += 1.0 / fSampleRate;
        if (fArp > 0 && advancePhase(fArpPhase, fArpHz / fSampleRate))
            fArpStep = (fArpStep + 1) % kChipArpSizes[fArp];

        double semitones = kChipArps[fArp][fArpStep];
        const double fade = std::clamp((fSinceOnset - kChipVibratoDelay) / kChipVibratoDelay, 0.0, 1.0);
        const float lfo = Lfo::shape(LfoShape::Sine, fVibratoLfo.advance());
        semitones += fVibrato * fade * kChipVibratoCents / 100.0 * lfo;
        const double dt = std::min(note.dt * std::exp2(semitones / 12.0), 0.45);
        advancePhase(fPhase, dt);

        float osc;
        if (fWave == 3)
        {
            // the console's triangle: 16 steps
            const double tri = fPhase < 0.5 ? 4.0 * fPhase - 1.0 : 3.0 - 4.0 * fPhase;
            osc = static_cast<float>(std::round(tri * 7.5) / 7.5);
        }
        else
        {
            osc = blepPulse(fPhase, dt, kChipWidths[fWave]);
        }

        float amp = note.amp;
        float y = osc * 0.3f;
        if (fCrush > 0.0f)
        {
            // the console's 16 volume steps, then fewer bits for the samples as Crush goes up
            amp = std::round(amp * 15.0f) / 15.0f;
            const float step = std::exp2(-(15.0f - 11.0f * fCrush));
            y = std::round(y * amp / step) * step;
        }
        else
        {
            y *= amp;
        }
        left = right = y;
    }

private:
    int fWave = 1;
    int fArp = 0;
    float fArpHz = 30.0f;
    bool fQuantize = false;
    float fVibrato = 0.3f;
    float fCrush = 0.3f;
    double fPhase = 0.0, fArpPhase = 0.0;
    int fArpStep = 0;
    double fSinceOnset = 0.0;
    Lfo fVibratoLfo;
};

} // namespace

double fmRatio(int choice)
{
    return kFmRatios[std::clamp(choice, 0, kFmRatioCount - 1)];
}

const BlockDescriptor kSuperSawBlockDescriptor = {
    "supersaw",
    "Super Saw",
    Category::Synth,
    kSuperSawParamCount,
    {
        ParamSpec { "Detune", "ct", 0.0f, 100.0f, 25.0f, ParamScale::Linear },
        ParamSpec { "Width", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Cutoff", "Hz", 100.0f, 12000.0f, 3000.0f, ParamScale::Log },
        ParamSpec { "Resonance", "", 0.5f, 10.0f, 1.0f, ParamScale::Log },
        ParamSpec { "Env", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<SuperSawBlock>()); },
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

const BlockDescriptor kFmSynthBlockDescriptor = {
    "fmsynth",
    "FM Synth",
    Category::Synth,
    kFmParamCount,
    {
        ParamSpec { "Ratio", "", 0.0f, static_cast<float>(kFmRatioCount - 1), 1.0f, ParamScale::Choice, kFmRatioNames },
        ParamSpec { "Index", "", 0.0f, 10.0f, 3.0f, ParamScale::Linear },
        ParamSpec { "Decay", "ms", 20.0f, 3000.0f, 400.0f, ParamScale::Log },
        ParamSpec { "Sustain", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Feedback", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 2.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 500.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<FmSynthBlock>()); },
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

const BlockDescriptor kFoldSynthBlockDescriptor = {
    "foldsynth",
    "Fold Synth",
    Category::Synth,
    kFoldParamCount,
    {
        ParamSpec { "Fold", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Env", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Symmetry", "%", -100.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Gate", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kFoldGateNames },
        ParamSpec { "Decay", "ms", 30.0f, 3000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Tone", "Hz", 500.0f, 16000.0f, 8000.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<FoldSynthBlock>()); },
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

const BlockDescriptor kAcidSynthBlockDescriptor = {
    "acidsynth",
    "Acid Synth",
    Category::Synth,
    kAcidParamCount,
    {
        ParamSpec { "Wave", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kAcidWaveNames },
        ParamSpec { "Cutoff", "Hz", 60.0f, 5000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Resonance", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Env Mod", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Decay", "ms", 30.0f, 2000.0f, 250.0f, ParamScale::Log },
        ParamSpec { "Accent", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Drive", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 60.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<AcidSynthBlock>()); },
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

const BlockDescriptor kStringSynthBlockDescriptor = {
    "stringsynth",
    "String Machine",
    Category::Synth,
    kStringParamCount,
    {
        ParamSpec { "Low", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Mid", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "High", "%", 0.0f, 100.0f, 40.0f, ParamScale::Linear },
        ParamSpec { "Ensemble", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 500.0f, 12000.0f, 4000.0f, ParamScale::Log },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 1200.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<StringSynthBlock>()); },
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

const BlockDescriptor kChipSynthBlockDescriptor = {
    "chipsynth",
    "Chip Synth",
    Category::Synth,
    kChipParamCount,
    {
        ParamSpec { "Wave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kChipWaveNames },
        ParamSpec { "Arp", "", 0.0f, 4.0f, 0.0f, ParamScale::Choice, kChipArpNames },
        ParamSpec { "Arp Rate", "Hz", 5.0f, 60.0f, 30.0f, ParamScale::Log },
        ParamSpec { "Quantize", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Vibrato", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Crush", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<ChipSynthBlock>()); },
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
