// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/MoreSynthBlocks.hpp"

#include "engine/blocks/TrackedSynth.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/Lfo.hpp"
#include "engine/dsp/Oscillator.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace xythos {

namespace {

constexpr const char* kOctaveNames[] = { "-2", "-1", "0", "+1" };
constexpr double kTwoPi = 6.283185307179586;

// The filter cutoff for a base cutoff, opened by the playing level over up to five octaves
double openedCutoff(double cutoff, float amount, float envelope, double sampleRate, double ceiling = 0.45)
{
    return std::min(cutoff * std::exp2(amount * 5.0f * std::min(1.0f, envelope * 6.0f)), ceiling * sampleRate);
}

// A one-pole coefficient for a time constant (0 = instant)
float follow(double sampleRate, double ms) { return 1.0f - EnvelopeFollower::coef(sampleRate, ms); }

// A sine from a table, for the additive synth: phase 0..1
class SineTable {
public:
    static constexpr int kSize = 4096;

    static const SineTable& get()
    {
        static const SineTable table;
        return table;
    }

    float operator()(double phase) const
    {
        const double x = phase * kSize;
        const auto i = static_cast<int>(x);
        const auto frac = static_cast<float>(x - i);
        const int k = i & (kSize - 1);
        return fTable[static_cast<size_t>(k)] + (fTable[static_cast<size_t>(k + 1)] - fTable[static_cast<size_t>(k)]) * frac;
    }

private:
    SineTable()
    {
        for (int i = 0; i <= kSize; ++i)
            fTable[static_cast<size_t>(i)] = static_cast<float>(std::sin(kTwoPi * i / kSize));
    }

    std::array<float, kSize + 1> fTable {};
};

// Hash noise 0..1 for a whole number (fixed "random" choices in the tables)
float hash01(uint32_t n)
{
    n ^= n >> 16;
    n *= 0x7feb352dU;
    n ^= n >> 15;
    n *= 0x846ca68bU;
    n ^= n >> 16;
    return static_cast<float>(n & 0xffffff) / static_cast<float>(0x1000000);
}

// ---------------------------------------------------------------------------------------------------------------------
// Theremin

class ThereminBlock : public TrackedSynth {
public:
    ThereminBlock()
    {
        setGlide(120.0f);
        setAttack(60.0f);
        setRelease(400.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kThereminParamGlide:   setGlide(value); break;
        case kThereminParamVibrato: fVibrato = value / 100.0f; break;
        case kThereminParamRate:
            fRate = value;
            fVibratoLfo.setRate(fRate);
            break;
        case kThereminParamWobble:  fWobble = value / 100.0f; break;
        case kThereminParamTimbre:  fTimbre = value / 100.0f; break;
        case kThereminParamAttack:  setAttack(value); break;
        case kThereminParamRelease: setRelease(value); break;
        case kThereminParamOctave:  setOctave(value); break;
        case kThereminParamMix:     setMix(value); break;
        case kThereminParamPickFilter: setPickFilter(value); break;
        case kThereminParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fVibratoLfo.prepare(fSampleRate);
        fVibratoLfo.setRate(fRate);
        fDcBlock.setLowPass(fSampleRate, 15.0);
    }

    void resetVoice() override
    {
        fPhase = 0.0;
        fVibratoLfo.reset();
        fPitchDrift.prepare(fSampleRate, 0.7, 0x5eed1u);
        fLevelDrift.prepare(fSampleRate, 1.3, 0x5eed2u);
        fDcBlock.reset();
    }

    void render(const Note& note, float& left, float& right) override
    {
        // the vibrato on top of yours (up to half a semitone), and the old instrument's drift (up to 15 cents)
        const double cents = 50.0 * fVibrato * Lfo::shape(LfoShape::Sine, fVibratoLfo.advance())
                           + 15.0 * fWobble * fPitchDrift.next();
        const double dt = std::min(note.dt * std::exp2(cents / 1200.0), 0.45);
        advancePhase(fPhase, dt);
        const auto s = static_cast<float>(std::sin(kTwoPi * fPhase));

        // Timbre: from a pure sine to a reedy, slightly lopsided voice (odd and even harmonics)
        float y = s;
        if (fTimbre > 0.0f)
        {
            const float k = 1.0f + 5.0f * fTimbre, bias = 0.35f * fTimbre;
            const float shaped = (std::tanh(k * (s + bias)) - std::tanh(k * bias)) / std::tanh(k);
            y = s + (shaped - s) * fTimbre;
        }
        y = fDcBlock.highPass(y);
        const float level = std::max(0.0f, 1.0f + 0.25f * fWobble * fLevelDrift.next());
        left = right = y * note.amp * level * 0.35f;
    }

private:
    float fVibrato = 0.2f;
    float fRate = 5.5f;
    float fWobble = 0.3f;
    float fTimbre = 0.15f;
    double fPhase = 0.0;
    Lfo fVibratoLfo;
    RandomLfo fPitchDrift, fLevelDrift;
    OnePole fDcBlock;
};

// ---------------------------------------------------------------------------------------------------------------------
// Sync Lead

class SyncLeadBlock : public TrackedSynth {
public:
    SyncLeadBlock()
    {
        setAttack(3.0f);
        setRelease(150.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kSyncParamSync: fSync = value; break;
        case kSyncParamEnv:  fEnv = value / 100.0f; break;
        case kSyncParamDecay:
            fDecayMs = value;
            fDecay = EnvelopeFollower::coef(fSampleRate, fDecayMs);
            break;
        case kSyncParamSub: fSub = value / 100.0f; break;
        case kSyncParamTone:
            fTone = value;
            fToneFilter.set(fSampleRate, std::min<double>(fTone, 0.45 * fSampleRate), 0.7);
            break;
        case kSyncParamAttack:  setAttack(value); break;
        case kSyncParamRelease: setRelease(value); break;
        case kSyncParamOctave:  setOctave(value); break;
        case kSyncParamGlide:   setGlide(value); break;
        case kSyncParamMix:     setMix(value); break;
        case kSyncParamPickFilter: setPickFilter(value); break;
        case kSyncParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fDecay = EnvelopeFollower::coef(fSampleRate, fDecayMs);
        fToneFilter.set(fSampleRate, std::min<double>(fTone, 0.45 * fSampleRate), 0.7);
    }

    void resetVoice() override
    {
        fMaster = fSlave = fSubPhase = 0.0;
        fHeld = 0.0f;
        fSweep = 0.0f;
        fToneFilter.reset();
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
            fSweep = 1.0f;
        else
            fSweep *= fDecay;

        // the synced saw's pitch: Sync times the note, swept up to three octaves higher by each pick
        const double dtMaster = note.dt;
        const double ratio = fSync * std::exp2(3.0 * fEnv * fSweep);
        const double dtSlave = std::min(dtMaster * ratio, 0.45);

        // The saw, its steps (its own wrap and each reset by the note's cycle) smoothed by a two-sample polyBLEP,
        // played one sample late: a step at distance t (0..1 samples) before this sample adds h t^2/2 to the
        // previous sample and -h (1-t)^2/2 to this one.
        float now = 0.0f;
        auto step = [&](double h, double t) {
            t = std::clamp(t, 0.0, 1.0);
            fHeld += static_cast<float>(h * t * t * 0.5);
            now -= static_cast<float>(h * (1.0 - t) * (1.0 - t) * 0.5);
        };
        fMaster += dtMaster;
        if (fMaster >= 1.0)
        {
            fMaster -= std::floor(fMaster);
            const double since = fMaster / dtMaster; // samples since the note's cycle began
            double before = fSlave + dtSlave * (1.0 - since);
            if (before >= 1.0)
            {
                before -= 1.0;
                step(-2.0, since + before / dtSlave);
            }
            step(-2.0 * before, since);
            fSlave = dtSlave * since;
        }
        else
        {
            fSlave += dtSlave;
            if (fSlave >= 1.0)
            {
                fSlave -= std::floor(fSlave);
                step(-2.0, fSlave / dtSlave);
            }
        }
        const float out = fHeld;
        fHeld = static_cast<float>(2.0 * fSlave - 1.0) + now;

        advancePhase(fSubPhase, 0.5 * dtMaster);
        const float sub = blepPulse(fSubPhase, 0.5 * dtMaster, 0.5);
        const float y = fToneFilter.process(out + 0.8f * fSub * sub).low;
        left = right = y * note.amp * 0.3f;
    }

private:
    float fSync = 2.0f;
    float fEnv = 0.5f;
    float fDecayMs = 300.0f;
    float fDecay = 0.0f;
    float fSub = 0.2f;
    float fTone = 6000.0f;
    double fMaster = 0.0, fSlave = 0.0, fSubPhase = 0.0;
    float fHeld = 0.0f;
    float fSweep = 0.0f;
    Svf fToneFilter;
};

// ---------------------------------------------------------------------------------------------------------------------
// Phase Distortion

constexpr const char* kPdWaveNames[] = { "Saw", "Square", "Pulse", "Reso Saw", "Reso Tri" };

// One sample of a phase-distorted cosine: phase p (0..1), distortion d (0..1), dt the phase step (it keeps the
// bends from getting steeper than the note's rate allows, so high notes don't alias)
float pdWave(int wave, double p, double d, double dt)
{
    switch (wave)
    {
    case 1: // Square: each half races through its half-cycle, then holds
    {
        const double w = std::clamp(1.0 - 0.98 * d, std::min(1.0, 4.0 * dt), 1.0);
        const double half = p < 0.5 ? 0.0 : 1.0;
        const double x = 2.0 * p - half;
        return static_cast<float>(-std::cos(M_PI * (half + std::min(1.0, x / w))));
    }
    case 2: // Pulse: the whole cycle squeezed into the start, then held
    {
        const double m = std::clamp(1.0 - 0.95 * d, std::min(1.0, 3.0 * dt), 1.0);
        return p < m ? static_cast<float>(-std::cos(kTwoPi * p / m)) : -1.0f;
    }
    case 3: // Reso Saw: a sine Ratio times faster, restarting each cycle, under a falling ramp
    case 4: // Reso Tri: the same under a triangle
    {
        const double ratio = std::min(1.0 + 15.0 * d, 0.4 / std::max(dt, 1e-9));
        const double window = wave == 3 ? 1.0 - p : (p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p);
        return static_cast<float>(1.0 - (1.0 - std::cos(kTwoPi * ratio * p)) * window);
    }
    default: // Saw: the first half of the cosine sped up, the second slowed down
    {
        const double m = std::clamp(0.5 - 0.49 * d, std::min(0.5, 2.0 * dt), 0.5);
        const double q = p < m ? 0.5 * p / m : 0.5 + 0.5 * (p - m) / (1.0 - m);
        return static_cast<float>(-std::cos(kTwoPi * q));
    }
    }
}

class PdSynthBlock : public TrackedSynth {
public:
    PdSynthBlock()
    {
        setAttack(2.0f);
        setRelease(300.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kPdParamWave:   fWave = std::clamp(static_cast<int>(value), 0, 4); break;
        case kPdParamAmount: fAmount = value / 100.0f; break;
        case kPdParamEnv:    fEnv = value / 100.0f; break;
        case kPdParamDecay:
            fDecayMs = value;
            fDecay = EnvelopeFollower::coef(fSampleRate, fDecayMs);
            break;
        case kPdParamSustain: fSustain = value / 100.0f; break;
        case kPdParamDetune:  fDetune = value; break;
        case kPdParamAttack:  setAttack(value); break;
        case kPdParamRelease: setRelease(value); break;
        case kPdParamOctave:  setOctave(value); break;
        case kPdParamGlide:   setGlide(value); break;
        case kPdParamMix:     setMix(value); break;
        case kPdParamPickFilter: setPickFilter(value); break;
        case kPdParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fDecay = EnvelopeFollower::coef(fSampleRate, fDecayMs);
        for (OnePole& f : fDcBlock)
            f.setLowPass(fSampleRate, 15.0);
    }

    void resetVoice() override
    {
        fPhase[0] = fPhase[1] = 0.0;
        fPick = 0.0f;
        for (OnePole& f : fDcBlock)
            f.reset();
    }

    void render(const Note& note, float& left, float& right) override
    {
        // the DCW envelope: up by Env on each pick, falling to Sustain over Decay
        if (note.onset)
            fPick = 1.0f;
        else
            fPick = fSustain + (fPick - fSustain) * fDecay;
        const double d = std::clamp(fAmount + fEnv * fPick, 0.0f, 1.0f);

        const double dt0 = note.dt;
        const double dt1 = std::min(note.dt * std::exp2(fDetune / 1200.0), 0.45);
        advancePhase(fPhase[0], dt0);
        advancePhase(fPhase[1], dt1);
        const float a = fDcBlock[0].highPass(pdWave(fWave, fPhase[0], d, dt0));
        if (fDetune > 0.0f)
        {
            // the second line, a few cents up, on the right, a little of each in the other side
            const float b = fDcBlock[1].highPass(pdWave(fWave, fPhase[1], d, dt1));
            left = (a + 0.35f * b) * note.amp * 0.25f;
            right = (b + 0.35f * a) * note.amp * 0.25f;
        }
        else
        {
            left = right = a * note.amp * 0.3f;
        }
    }

private:
    int fWave = 0;
    float fAmount = 0.3f;
    float fEnv = 0.5f;
    float fDecayMs = 400.0f;
    float fDecay = 0.0f;
    float fSustain = 0.3f;
    float fDetune = 6.0f;
    double fPhase[2] {};
    float fPick = 0.0f;
    OnePole fDcBlock[2];
};

// ---------------------------------------------------------------------------------------------------------------------
// Wavetable

constexpr const char* kWavetableNames[] = { "Vocal", "Metal", "PWM", "Digital" };
constexpr int kWavetableCount = 4;
constexpr int kWaveSize = 2048;
constexpr int kWaveLevels = 9; // harmonics up to 1, 2, 4, ... 256

// The harmonic h's amplitude (and phase, as a share of a cycle) in a table's frame
void wavetableHarmonic(int table, int frame, int h, float& amplitude, float& phase)
{
    const float t = static_cast<float>(frame) / (kWavetableFrames - 1);
    phase = 0.0f;
    switch (table)
    {
    case 0: // Vocal: vowels U, O, A, E, I, their formants on a voice at 130 Hz
    {
        static constexpr float kVowels[5][3] = {
            { 325, 700, 2530 }, { 450, 800, 2830 }, { 800, 1150, 2900 }, { 400, 1700, 2600 }, { 290, 2200, 3000 },
        };
        static constexpr float kWidth[3] = { 90, 110, 160 }, kGain[3] = { 1.0f, 0.6f, 0.3f };
        const float pos = t * 4.0f;
        const int v = std::min(3, static_cast<int>(pos));
        const float frac = pos - v;
        const float hz = 130.0f * h;
        float a = 0.12f;
        for (int k = 0; k < 3; ++k)
        {
            const float formant = kVowels[v][k] + (kVowels[v + 1][k] - kVowels[v][k]) * frac;
            const float z = (hz - formant) / kWidth[k];
            a += kGain[k] * std::exp(-0.5f * z * z);
        }
        amplitude = a / std::pow(static_cast<float>(h), 0.7f); // the voice's own roll-off under the formants
        break;
    }
    case 1: // Metal: uneven, clangy harmonics, more of them further in
    {
        if (h == 1)
        {
            amplitude = 1.0f;
            break;
        }
        const float strong = hash01(static_cast<uint32_t>(h) * 7919u) < 0.25f + 0.6f * t ? 1.0f : 0.08f;
        amplitude = strong * (0.25f + 0.75f * hash01(static_cast<uint32_t>(h) * 104729u))
                  / std::pow(static_cast<float>(h), 1.4f - 0.8f * t);
        phase = hash01(static_cast<uint32_t>(h) * 15485863u);
        break;
    }
    case 2: // PWM: a pulse narrowing from square to 5%
    {
        const float width = 0.5f - 0.45f * t;
        amplitude = std::sin(static_cast<float>(M_PI) * h * width) / h;
        phase = 0.25f; // cosines: the pulse's own shape
        break;
    }
    default: // Digital: every n-th harmonic only, n growing, the slope flattening: saw, square, then thinner combs
    {
        const int every = frame + 1;
        amplitude = (h - 1) % every == 0 ? 1.0f / std::pow(static_cast<float>(h), 1.0f - 0.08f * frame) : 0.0f;
        break;
    }
    }
}

struct WaveBank {
    std::vector<float> data; // [table][frame][level][kWaveSize + 1] (the last sample repeats the first)

    WaveBank()
    {
        constexpr size_t stride = kWaveSize + 1;
        data.assign(static_cast<size_t>(kWavetableCount * kWavetableFrames * kWaveLevels) * stride, 0.0f);
        std::vector<float> sine(kWaveSize);
        for (int i = 0; i < kWaveSize; ++i)
            sine[static_cast<size_t>(i)] = static_cast<float>(std::sin(kTwoPi * i / kWaveSize));
        std::vector<float> wave(kWaveSize);
        for (int table = 0; table < kWavetableCount; ++table)
        {
            for (int frame = 0; frame < kWavetableFrames; ++frame)
            {
                std::fill(wave.begin(), wave.end(), 0.0f);
                int h = 1;
                for (int level = 0; level < kWaveLevels; ++level)
                {
                    // each level adds the harmonics up to 2^level to the one below
                    for (const int top = 1 << level; h <= top; ++h)
                    {
                        float amplitude, phase;
                        wavetableHarmonic(table, frame, h, amplitude, phase);
                        if (amplitude == 0.0f)
                            continue;
                        const auto offset = static_cast<int>(phase * kWaveSize);
                        for (int i = 0; i < kWaveSize; ++i)
                            wave[static_cast<size_t>(i)] += amplitude * sine[static_cast<size_t>((h * i + offset) & (kWaveSize - 1))];
                    }
                    std::copy(wave.begin(), wave.end(), at(table, frame, level));
                }
                // one gain for the frame, from its full copy, so the levels match
                float peak = 0.0f;
                for (const float v : wave)
                    peak = std::max(peak, std::fabs(v));
                const float gain = peak > 0.0f ? 1.0f / peak : 0.0f;
                for (int level = 0; level < kWaveLevels; ++level)
                {
                    float* w = at(table, frame, level);
                    for (int i = 0; i < kWaveSize; ++i)
                        w[i] *= gain;
                    w[kWaveSize] = w[0];
                }
            }
        }
    }

    float* at(int table, int frame, int level)
    {
        return data.data() + (static_cast<size_t>((table * kWavetableFrames + frame) * kWaveLevels + level)) * (kWaveSize + 1);
    }

    const float* at(int table, int frame, int level) const
    {
        return data.data() + (static_cast<size_t>((table * kWavetableFrames + frame) * kWaveLevels + level)) * (kWaveSize + 1);
    }

    static const WaveBank& get()
    {
        static const WaveBank bank;
        return bank;
    }
};

float readWave(const float* wave, double phase)
{
    const double x = phase * kWaveSize;
    const auto i = std::min(static_cast<int>(x), kWaveSize - 1);
    const auto frac = static_cast<float>(x - i);
    return wave[i] + (wave[i + 1] - wave[i]) * frac;
}

class WavetableBlock : public TrackedSynth {
public:
    WavetableBlock()
    {
        setAttack(5.0f);
        setRelease(250.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kWavetableParamTable:    fTable = std::clamp(static_cast<int>(value), 0, kWavetableCount - 1); break;
        case kWavetableParamPosition: fPosition = value / 100.0f; break;
        case kWavetableParamEnv:      fEnv = value / 100.0f; break;
        case kWavetableParamLfoRate:
            fLfoRate = value;
            fLfo.setRate(fLfoRate);
            break;
        case kWavetableParamLfoDepth: fLfoDepth = value / 100.0f; break;
        case kWavetableParamAttack:   setAttack(value); break;
        case kWavetableParamRelease:  setRelease(value); break;
        case kWavetableParamOctave:   setOctave(value); break;
        case kWavetableParamGlide:    setGlide(value); break;
        case kWavetableParamMix:      setMix(value); break;
        case kWavetableParamPickFilter: setPickFilter(value); break;
        case kWavetableParamTrack:    setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fBank = &WaveBank::get(); // built once, off the audio thread
        fLfo.prepare(fSampleRate);
        fLfo.setRate(fLfoRate);
        fPosCoef = follow(fSampleRate, 8.0);
    }

    void resetVoice() override
    {
        fPhase = 0.0;
        fLfo.reset();
        fPos = fPosition;
    }

    void render(const Note& note, float& left, float& right) override
    {
        const float playing = std::min(1.0f, note.envelope * 6.0f);
        const float lfo = Lfo::shape(LfoShape::Sine, fLfo.advance());
        const float target = std::clamp(fPosition + fEnv * playing + 0.5f * fLfoDepth * lfo, 0.0f, 1.0f);
        fPos += (target - fPos) * fPosCoef;

        advancePhase(fPhase, note.dt);
        // the copy with as many harmonics as fit below 0.45 of the rate
        const double room = 0.45 / std::max(note.dt, 1e-9);
        const int level = std::clamp(static_cast<int>(std::floor(std::log2(std::max(1.0, room)))), 0, kWaveLevels - 1);
        const float frame = fPos * (kWavetableFrames - 1);
        const int f0 = std::min(static_cast<int>(frame), kWavetableFrames - 2);
        const float frac = frame - f0;
        const float a = readWave(fBank->at(fTable, f0, level), fPhase);
        const float b = readWave(fBank->at(fTable, f0 + 1, level), fPhase);
        left = right = (a + (b - a) * frac) * note.amp * 0.3f;
    }

private:
    const WaveBank* fBank = nullptr;
    int fTable = 0;
    float fPosition = 0.2f;
    float fEnv = 0.5f;
    float fLfoRate = 0.3f;
    float fLfoDepth = 0.0f;
    double fPhase = 0.0;
    float fPos = 0.2f;
    float fPosCoef = 1.0f;
    Lfo fLfo;
};

// ---------------------------------------------------------------------------------------------------------------------
// Sub Bass

constexpr const char* kSubBassWaveNames[] = { "Sine", "Triangle", "Square", "Saw" };

class SubBassBlock : public TrackedSynth {
public:
    SubBassBlock()
    {
        setAttack(3.0f);
        setRelease(120.0f);
        setGlide(20.0f);
        setOctave(1.0f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kSubBassParamWave:      fWave = std::clamp(static_cast<int>(value), 0, 3); break;
        case kSubBassParamSub:       fSub = value / 100.0f; break;
        case kSubBassParamCutoff:    fCutoff = value; break;
        case kSubBassParamResonance: fResonance = value / 100.0f; break;
        case kSubBassParamGrowl:     fGrowl = value / 100.0f; break;
        case kSubBassParamEnv:       fEnv = value / 100.0f; break;
        case kSubBassParamAttack:    setAttack(value); break;
        case kSubBassParamRelease:   setRelease(value); break;
        case kSubBassParamOctave:    setOctave(value); break;
        case kSubBassParamGlide:     setGlide(value); break;
        case kSubBassParamMix:       setMix(value); break;
        case kSubBassParamPickFilter: setPickFilter(value); break;
        case kSubBassParamTrack:     setTrack(value); break;
        }
    }

protected:
    void resetVoice() override
    {
        fPhase = fSubPhase = 0.0;
        for (float& s : fStage)
            s = 0.0f;
    }

    void render(const Note& note, float& left, float& right) override
    {
        advancePhase(fPhase, note.dt);
        advancePhase(fSubPhase, 0.5 * note.dt);
        float osc;
        switch (fWave)
        {
        case 1: osc = static_cast<float>(fPhase < 0.5 ? 4.0 * fPhase - 1.0 : 3.0 - 4.0 * fPhase); break;
        case 2: osc = blepPulse(fPhase, note.dt, 0.5); break;
        case 3: osc = blepSaw(fPhase, note.dt); break;
        default: osc = static_cast<float>(std::sin(kTwoPi * fPhase)); break;
        }
        const auto sub = static_cast<float>(std::sin(kTwoPi * fSubPhase));
        const float x = (osc + fSub * sub) * (1.0f + 6.0f * fGrowl) * 0.3f;

        // the ladder: four saturating one-pole stages with the last fed back (resonance), the input lifted to make
        // up for the low end the feedback takes
        const double cutoff = openedCutoff(fCutoff, fEnv, note.envelope, fSampleRate, 0.2);
        const auto g = static_cast<float>(1.0 - std::exp(-kTwoPi * cutoff / fSampleRate));
        const float k = 3.6f * fResonance;
        const float u = std::tanh(x * (1.0f + 0.3f * k) - k * fStage[3]);
        fStage[0] += g * (u - std::tanh(fStage[0]));
        fStage[1] += g * (std::tanh(fStage[0]) - std::tanh(fStage[1]));
        fStage[2] += g * (std::tanh(fStage[1]) - std::tanh(fStage[2]));
        fStage[3] += g * (std::tanh(fStage[2]) - std::tanh(fStage[3]));
        left = right = fStage[3] * note.amp * 0.8f / (1.0f + 0.8f * fGrowl);
    }

private:
    int fWave = 3;
    float fSub = 0.5f;
    float fCutoff = 500.0f;
    float fResonance = 0.2f;
    float fGrowl = 0.2f;
    float fEnv = 0.3f;
    double fPhase = 0.0, fSubPhase = 0.0;
    float fStage[4] {};
};

// ---------------------------------------------------------------------------------------------------------------------
// Laser

constexpr const char* kLaserWaveNames[] = { "Sine", "Square", "Saw" };
constexpr float kLaserMaxEchoMs = 1000.0f;

class LaserBlock : public TrackedSynth {
public:
    LaserBlock()
    {
        setAttack(1.0f);
        setRelease(150.0f);
        setGlide(0.1f);
        setMix(100.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kLaserParamDive:    fDive = value; break;
        case kLaserParamSpeed:   fSpeedMs = value; break;
        case kLaserParamWave:    fWave = std::clamp(static_cast<int>(value), 0, 2); break;
        case kLaserParamRepeat:  fRepeat = value; break;
        case kLaserParamEcho:    fEcho = value / 100.0f; break;
        case kLaserParamTime:    fTimeMs = value; break;
        case kLaserParamRelease: setRelease(value); break;
        case kLaserParamOctave:  setOctave(value); break;
        case kLaserParamMix:     setMix(value); break;
        case kLaserParamPickFilter: setPickFilter(value); break;
        case kLaserParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fLine.prepare(static_cast<size_t>(std::ceil(kLaserMaxEchoMs / 1000.0 * fSampleRate)) + 4);
        fTimeSmoothing = follow(fSampleRate, 50.0);
    }

    void resetVoice() override
    {
        fPhase = 0.0;
        fSince = 1e9;
        fSinceRepeat = 0.0;
        fLine.reset();
        fDelay = fTimeMs;
    }

    void render(const Note& note, float& left, float& right) override
    {
        const double step = 1.0 / fSampleRate;
        if (note.onset)
            fSince = fSinceRepeat = 0.0;
        else if (fRepeat > 0.0f && note.on && fSinceRepeat >= 1.0 / fRepeat)
            fSince = fSinceRepeat = 0.0; // fire again while the note rings
        fSince += step;
        fSinceRepeat += step;

        // the dive: Dive semitones above the note, falling exponentially to it, nearly there after Speed
        const double tau = std::max(1.0, static_cast<double>(fSpeedMs)) / 1000.0 / 3.0;
        const double semitones = fDive * std::exp(-fSince / tau);
        const double dt = std::min(note.dt * std::exp2(semitones / 12.0), 0.45);
        advancePhase(fPhase, dt);
        float osc;
        switch (fWave)
        {
        case 1: osc = blepPulse(fPhase, dt, 0.5) * 0.7f; break;
        case 2: osc = blepSaw(fPhase, dt) * 0.7f; break;
        default: osc = static_cast<float>(std::sin(kTwoPi * fPhase)); break;
        }
        const float dry = osc * note.amp * 0.3f;

        // the echo: feedback up to 85%, the right side's taps a little earlier
        fDelay += (fTimeMs - fDelay) * fTimeSmoothing;
        const auto samples = static_cast<float>(fDelay / 1000.0 * fSampleRate);
        const float wetL = fLine.readFractional(std::max(1.0f, samples));
        const float wetR = fLine.readFractional(std::max(1.0f, samples * 0.75f));
        const float feedback = 0.85f * fEcho;
        fLine.write(std::tanh(dry + wetL * feedback));
        left = dry + 0.6f * fEcho * wetL;
        right = dry + 0.6f * fEcho * wetR;
    }

private:
    float fDive = 24.0f;
    float fSpeedMs = 150.0f;
    int fWave = 0;
    float fRepeat = 0.0f;
    float fEcho = 0.35f;
    float fTimeMs = 180.0f;
    double fPhase = 0.0;
    double fSince = 1e9, fSinceRepeat = 0.0;
    DelayLine fLine;
    float fDelay = 180.0f;
    float fTimeSmoothing = 1.0f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Drone Keeper

constexpr const char* kDroneRegisterNames[] = { "Low", "Mid", "High" };
constexpr int kDroneUpdate = 64; // samples between histogram decays
constexpr double kDroneDecideSeconds = 0.1;
constexpr double kDroneChangeOut = 0.3, kDroneChangeIn = 1.5; // s: the drone fades out, moves, fades back in
constexpr double kDroneRise = 1.5; // s
constexpr float kDroneSwitch = 1.25f; // a new root must score this much higher than the current one

float droneScore(const float weights[12], int root)
{
    return weights[root] + 0.6f * weights[(root + 7) % 12] + 0.25f * (weights[(root + 4) % 12] + weights[(root + 3) % 12]);
}

class DroneBlock : public TrackedSynth {
public:
    DroneBlock()
    {
        setAddsToDry(true);
        setAttack(1.0f);
        setRelease(30.0f);
        setGlide(0.1f);
        setMix(50.0f);
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kDroneParamMemory:
            fMemory = value;
            updateMemory();
            break;
        case kDroneParamRegister: fRegister = std::clamp(static_cast<int>(value), 0, 2); break;
        case kDroneParamFifth:    fFifth = value / 100.0f; break;
        case kDroneParamOctave:   fOctaveUp = value / 100.0f; break;
        case kDroneParamMotion:   fMotion = value / 100.0f; break;
        case kDroneParamTone:     fTone = value; break;
        case kDroneParamFade:
            fFade = value;
            fFall = EnvelopeFollower::coef(fSampleRate, fFade * 1000.0);
            break;
        case kDroneParamHold:  fHold = value >= 0.5f; break;
        case kDroneParamLevel: setMix(value); break;
        case kDroneParamPickFilter: setPickFilter(value); break;
        case kDroneParamTrack: setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        updateMemory();
        fRiseCoef = follow(fSampleRate, kDroneRise * 1000.0 / 3.0);
        fFall = EnvelopeFollower::coef(fSampleRate, fFade * 1000.0);
        fSlowLfo.prepare(fSampleRate);
        fSlowLfo.setRate(0.05);
    }

    void resetVoice() override
    {
        for (float& w : fWeights)
            w = 0.0f;
        fRoot = fPending = -1;
        fCount = 0;
        fSinceDecision = 0.0;
        fGain = 0.0f;
        fChange = 1.0f;
        fStarted = false;
        for (double& p : fPhase)
            p = 0.0;
        for (int k = 0; k < kVoices; ++k)
            fPhase[k] = std::fmod(0.31 + 0.618 * k, 1.0);
        for (Svf& f : fFilter)
            f.reset();
        fSlowLfo.reset();
        fDrift.prepare(fSampleRate, 0.13, 0xd70e1u);
        fFilterCount = 0;
    }

    void render(const Note& note, float& left, float& right) override
    {
        listen(note);

        // the drone's level: up while you play (or for good, with Hold), down over Fade
        const bool sounding = fRoot >= 0 && (note.on || (fHold && fStarted));
        if (sounding)
            fGain += (1.0f - fGain) * fRiseCoef;
        else
            fGain *= fFall;

        // a new root: fade out, move, fade back in (at once while silent)
        if (fPending != fRoot)
        {
            if (fRoot < 0 || fGain * fChange < 0.01f)
            {
                fRoot = fPending;
                fChange = fRoot >= 0 && fGain > 0.01f ? 0.0f : 1.0f;
            }
            else
            {
                fChange -= static_cast<float>(1.0 / (kDroneChangeOut * fSampleRate));
                if (fChange <= 0.0f)
                {
                    fChange = 0.0f;
                    fRoot = fPending;
                }
            }
        }
        else if (fChange < 1.0f)
        {
            fChange = std::min(1.0f, fChange + static_cast<float>(1.0 / (kDroneChangeIn * fSampleRate)));
        }

        const float gain = fGain * fChange;
        if (fRoot < 0 || gain < 1e-5f)
        {
            left = right = 0.0f;
            return;
        }
        voices(gain, left, right);
    }

private:
    static constexpr int kVoices = 8;

    void updateMemory() { fKeep = std::exp(-kDroneUpdate / (std::max(0.5f, fMemory) * fSampleRate)); }

    // The pitch-class weights: each sample of a sounding note adds its level to its class; all of them fade with
    // Memory. A decision every 100 ms.
    void listen(const Note& note)
    {
        if (note.on && note.target > 20.0)
        {
            const double midi = 69.0 + 12.0 * std::log2(note.target / 440.0);
            const int pitchClass = ((static_cast<int>(std::lround(midi)) % 12) + 12) % 12;
            fWeights[pitchClass] += std::min(1.0f, note.envelope * 4.0f);
            fStarted = fStarted || fRoot >= 0 || fPending >= 0;
        }
        if (++fCount >= kDroneUpdate)
        {
            fCount = 0;
            for (float& w : fWeights)
                w *= fKeep;
        }
        fSinceDecision += 1.0 / fSampleRate;
        if (fSinceDecision >= kDroneDecideSeconds)
        {
            fSinceDecision = 0.0;
            const int best = droneRoot(fWeights);
            if (best >= 0 && best != fPending)
            {
                const int current = fPending;
                if (current < 0 || droneScore(fWeights, best) > kDroneSwitch * droneScore(fWeights, current))
                    fPending = best;
            }
        }
    }

    void voices(float gain, float& left, float& right)
    {
        // C in the register's octave (1, 2 or 3), up to the root
        const double root = 16.351597831287414 * std::exp2(fRegister + 1) * std::exp2(fRoot / 12.0);
        // three saws on the root, two on the fifth, two an octave up (each pair detuned and spread), a sine on the
        // root for body
        static constexpr double kRatio[kVoices] = { 1.0, 1.0, 1.0, 1.5, 1.5, 2.0, 2.0, 1.0 };
        static constexpr double kCents[kVoices] = { -7.0, 0.0, 6.0, -5.0, 5.0, -4.0, 4.0, 0.0 };
        static constexpr float kPan[kVoices] = { -0.8f, 0.0f, 0.8f, -0.6f, 0.6f, 0.7f, -0.7f, 0.0f };
        const float level[kVoices] = { 1.0f, 1.0f, 1.0f, fFifth, fFifth, fOctaveUp, fOctaveUp, 1.2f };
        float l = 0.0f, r = 0.0f;
        for (int k = 0; k < kVoices; ++k)
        {
            if (level[k] <= 0.0f)
                continue;
            const double dt = std::min(root * kRatio[k] * std::exp2(kCents[k] / 1200.0) / fSampleRate, 0.45);
            advancePhase(fPhase[k], dt);
            const float v = (k == kVoices - 1 ? static_cast<float>(std::sin(kTwoPi * fPhase[k])) : blepSaw(fPhase[k], dt))
                          * level[k];
            l += v * (1.0f - kPan[k]);
            r += v * (1.0f + kPan[k]);
        }

        // the filter drifts slowly with Motion (an LFO over twenty seconds and a wandering random)
        const float lfo = Lfo::shape(LfoShape::Sine, fSlowLfo.advance());
        const float drift = fDrift.next();
        if (++fFilterCount >= 32)
        {
            fFilterCount = 0;
            const double cutoff = fTone * std::exp2(fMotion * (1.2f * lfo + 0.8f * drift));
            for (Svf& f : fFilter)
                f.set(fSampleRate, std::min(cutoff, 0.45 * fSampleRate), 0.9);
        }
        const float out = gain * 0.09f;
        left = fFilter[0].process(l).low * out;
        right = fFilter[1].process(r).low * out;
    }

    float fMemory = 8.0f;
    int fRegister = 1;
    float fFifth = 0.6f;
    float fOctaveUp = 0.3f;
    float fMotion = 0.5f;
    float fTone = 1200.0f;
    float fFade = 4.0f;
    bool fHold = false;

    float fWeights[12] {};
    float fKeep = 1.0f;
    int fCount = 0;
    double fSinceDecision = 0.0;
    int fRoot = -1, fPending = -1;
    bool fStarted = false;
    float fGain = 0.0f, fChange = 1.0f;
    float fRiseCoef = 1.0f, fFall = 0.0f;
    double fPhase[kVoices] {};
    Svf fFilter[2];
    Lfo fSlowLfo;
    RandomLfo fDrift;
    int fFilterCount = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Harmonic Painter

class HarmonicPainterBlock : public TrackedSynth {
public:
    HarmonicPainterBlock()
    {
        setAttack(5.0f);
        setRelease(300.0f);
        setMix(100.0f);
        updateShape();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kHarmonicParamTilt:
            fTilt = value;
            updateShape();
            break;
        case kHarmonicParamOddEven:
            fOddEven = value / 100.0f;
            updateShape();
            break;
        case kHarmonicParamStretch:
            fStretch = value / 100.0f;
            updateShape();
            break;
        case kHarmonicParamShimmer: fShimmer = value / 100.0f; break;
        case kHarmonicParamRate:
            fRate = value;
            updateRates();
            break;
        case kHarmonicParamSpread:  fSpread = value / 100.0f; break;
        case kHarmonicParamAttack:  setAttack(value); break;
        case kHarmonicParamRelease: setRelease(value); break;
        case kHarmonicParamOctave:  setOctave(value); break;
        case kHarmonicParamGlide:   setGlide(value); break;
        case kHarmonicParamMix:     setMix(value); break;
        case kHarmonicParamPickFilter: setPickFilter(value); break;
        case kHarmonicParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fSine = &SineTable::get();
        updateRates();
    }

    void resetVoice() override
    {
        for (int h = 0; h < kHarmonicPartials; ++h)
        {
            fPhase[h] = hash01(static_cast<uint32_t>(h + 1) * 2654435761u);
            fDrift[h].prepare(fSampleRate, fRate * (0.6 + 0.8 * hash01(static_cast<uint32_t>(h) * 40503u)),
                              0x9e3779b9u + static_cast<uint32_t>(h) * 7u);
        }
    }

    void render(const Note& note, float& left, float& right) override
    {
        float l = 0.0f, r = 0.0f;
        for (int h = 0; h < kHarmonicPartials; ++h)
        {
            const double dt = note.dt * fRatio[h];
            // fade out the harmonics nearing 0.45 of the rate, silence those past it
            if (dt >= 0.45)
                break;
            const float edge = dt > 0.4 ? static_cast<float>((0.45 - dt) / 0.05) : 1.0f;
            advancePhase(fPhase[h], dt);
            const float drift = fShimmer > 0.0f ? std::max(0.0f, 1.0f + 0.95f * fShimmer * fDrift[h].next()) : 1.0f;
            const float v = (*fSine)(fPhase[h]) * fAmp[h] * drift * edge;
            // odd harmonics to one side, even to the other; the fundamental in the middle
            const float pan = h == 0 ? 0.0f : ((h + 1) % 2 == 1 ? -fSpread : fSpread);
            l += v * (1.0f - pan);
            r += v * (1.0f + pan);
        }
        left = l * note.amp;
        right = r * note.amp;
    }

private:
    void updateShape()
    {
        // Tilt in dB per octave; Odd/Even fades the even (or odd) harmonics above the fundamental; normalized to an
        // even loudness; Stretch pulls the harmonics sharp, more the higher they are (a stiff string's or a bell's)
        const float exponent = fTilt / 6.0206f;
        const double b = 0.002 * fStretch;
        float sum = 0.0f;
        for (int h = 0; h < kHarmonicPartials; ++h)
        {
            const int n = h + 1;
            float a = std::pow(static_cast<float>(n), exponent);
            if (n > 1)
                a *= n % 2 == 0 ? std::min(1.0f, 1.0f + fOddEven) : std::min(1.0f, 1.0f - fOddEven);
            fAmp[h] = a;
            sum += a * a;
            fRatio[h] = n * std::sqrt(1.0 + b * n * n);
        }
        const float gain = sum > 0.0f ? 0.15f / std::sqrt(0.5f * sum) : 0.0f;
        for (float& a : fAmp)
            a *= gain;
    }

    void updateRates()
    {
        for (int h = 0; h < kHarmonicPartials; ++h)
            fDrift[h].setRate(fSampleRate, fRate * (0.6 + 0.8 * hash01(static_cast<uint32_t>(h) * 40503u)));
    }

    float fTilt = -6.0f;
    float fOddEven = 0.0f;
    float fStretch = 0.0f;
    float fShimmer = 0.2f;
    float fRate = 0.5f;
    float fSpread = 0.3f;
    const SineTable* fSine = nullptr;
    float fAmp[kHarmonicPartials] {};
    double fRatio[kHarmonicPartials] {};
    double fPhase[kHarmonicPartials] {};
    RandomLfo fDrift[kHarmonicPartials];
};

} // namespace

const float* wavetableWave(int table, int frame, int harmonics, int& size)
{
    size = kWaveSize;
    int level = 0;
    while (level < kWaveLevels - 1 && (1 << (level + 1)) <= harmonics)
        ++level;
    return WaveBank::get().at(std::clamp(table, 0, kWavetableCount - 1), std::clamp(frame, 0, kWavetableFrames - 1), level);
}

int droneRoot(const float weights[12])
{
    int best = -1;
    float bestScore = 0.0f;
    for (int root = 0; root < 12; ++root)
    {
        const float score = droneScore(weights, root);
        if (score > bestScore * 1.0001f && score > 1e-6f)
        {
            best = root;
            bestScore = score;
        }
    }
    return best;
}

#define TRACKED_SYNTH_TAIL                                                                                             \
    -1, -1, false, nullptr, 24.0f, MeterStyle::Reduction, true

#define PICK_FILTER_AND_TRACK                                                                                          \
    ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },                                          \
    ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames }

const BlockDescriptor kThereminBlockDescriptor = {
    "theremin",
    "Theremin",
    Category::Synth,
    kThereminParamCount,
    {
        ParamSpec { "Glide", "ms", 1.0f, 1000.0f, 120.0f, ParamScale::Log },
        ParamSpec { "Vibrato", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Rate", "Hz", 2.0f, 9.0f, 5.5f, ParamScale::Linear },
        ParamSpec { "Wobble", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Timbre", "%", 0.0f, 100.0f, 15.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 60.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 400.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new ThereminBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kSyncLeadBlockDescriptor = {
    "synclead",
    "Sync Lead",
    Category::Synth,
    kSyncParamCount,
    {
        ParamSpec { "Sync", "x", 1.0f, 8.0f, 2.0f, ParamScale::Linear },
        ParamSpec { "Env", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Decay", "ms", 20.0f, 3000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Sub", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 500.0f, 16000.0f, 6000.0f, ParamScale::Log },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 3.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 150.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new SyncLeadBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kPdSynthBlockDescriptor = {
    "pdsynth",
    "Phase Distortion",
    Category::Synth,
    kPdParamCount,
    {
        ParamSpec { "Wave", "", 0.0f, 4.0f, 0.0f, ParamScale::Choice, kPdWaveNames },
        ParamSpec { "Amount", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Env", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Decay", "ms", 20.0f, 3000.0f, 400.0f, ParamScale::Log },
        ParamSpec { "Sustain", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Detune", "ct", 0.0f, 50.0f, 6.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 2.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new PdSynthBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kWavetableBlockDescriptor = {
    "wavetable",
    "Wavetable",
    Category::Synth,
    kWavetableParamCount,
    {
        ParamSpec { "Table", "", 0.0f, static_cast<float>(kWavetableCount - 1), 0.0f, ParamScale::Choice, kWavetableNames },
        ParamSpec { "Position", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Env", "%", -100.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "LFO Rate", "Hz", 0.05f, 10.0f, 0.3f, ParamScale::Log },
        ParamSpec { "LFO Depth", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 5.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 250.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new WavetableBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kSubBassBlockDescriptor = {
    "subbass",
    "Sub Bass",
    Category::Synth,
    kSubBassParamCount,
    {
        ParamSpec { "Wave", "", 0.0f, 3.0f, 3.0f, ParamScale::Choice, kSubBassWaveNames },
        ParamSpec { "Sub", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Cutoff", "Hz", 40.0f, 5000.0f, 500.0f, ParamScale::Log },
        ParamSpec { "Resonance", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Growl", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Env", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 3.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 120.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 20.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new SubBassBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kLaserBlockDescriptor = {
    "laser",
    "Laser",
    Category::Synth,
    kLaserParamCount,
    {
        ParamSpec { "Dive", "st", 0.0f, 48.0f, 24.0f, ParamScale::Linear },
        ParamSpec { "Speed", "ms", 10.0f, 2000.0f, 150.0f, ParamScale::Log },
        ParamSpec { "Wave", "", 0.0f, 2.0f, 0.0f, ParamScale::Choice, kLaserWaveNames },
        ParamSpec { "Repeat", "Hz", 0.0f, 20.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Echo", "%", 0.0f, 100.0f, 35.0f, ParamScale::Linear },
        ParamSpec { "Time", "ms", 20.0f, kLaserMaxEchoMs, 180.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 150.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new LaserBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kDroneBlockDescriptor = {
    "drone",
    "Drone Keeper",
    Category::Synth,
    kDroneParamCount,
    {
        ParamSpec { "Memory", "s", 2.0f, 30.0f, 8.0f, ParamScale::Log },
        ParamSpec { "Register", "", 0.0f, 2.0f, 1.0f, ParamScale::Choice, kDroneRegisterNames },
        ParamSpec { "Fifth", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Octave Up", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Motion", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 200.0f, 8000.0f, 1200.0f, ParamScale::Log },
        ParamSpec { "Fade", "s", 0.5f, 20.0f, 4.0f, ParamScale::Log },
        ParamSpec { "Hold", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Level", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new DroneBlock()); },
    TRACKED_SYNTH_TAIL,
};

const BlockDescriptor kHarmonicPainterBlockDescriptor = {
    "harmonicpainter",
    "Harmonic Painter",
    Category::Synth,
    kHarmonicParamCount,
    {
        ParamSpec { "Tilt", "dB/oct", -18.0f, 6.0f, -6.0f, ParamScale::Linear },
        ParamSpec { "Odd/Even", "%", -100.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Stretch", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Shimmer", "%", 0.0f, 100.0f, 20.0f, ParamScale::Linear },
        ParamSpec { "Rate", "Hz", 0.05f, 8.0f, 0.5f, ParamScale::Log },
        ParamSpec { "Spread", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 3000.0f, 5.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 5000.0f, 300.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 10.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        PICK_FILTER_AND_TRACK,
    },
    [] { return std::unique_ptr<Block>(new HarmonicPainterBlock()); },
    TRACKED_SYNTH_TAIL,
};

} // namespace xythos
