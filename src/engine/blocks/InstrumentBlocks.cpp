// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/InstrumentBlocks.hpp"

#include "engine/blocks/TrackedSynth.hpp"
#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/Oscillator.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace xythos {

namespace {

constexpr const char* kOctaveNames[] = { "-2", "-1", "0", "+1" };
constexpr float kTwoPi = 6.2831853f;

// xorshift32 noise, -1..1
class Noise {
public:
    explicit Noise(uint32_t seed) : fState(seed ? seed : 1u) {}

    float next()
    {
        fState ^= fState << 13;
        fState ^= fState >> 17;
        fState ^= fState << 5;
        return static_cast<float>(fState) / 2147483648.0f - 1.0f;
    }

    // 0..1
    float uniform() { return 0.5f * (next() + 1.0f); }

private:
    uint32_t fState;
};

// ---------------------------------------------------------------------------------------------------------------------
// Fart

enum class Bend { Steady, Rise, Fall, Wobble, Quack };

struct FartKind {
    const char* name;
    float seconds;   // natural length
    Bend bend;
    float amount;    // how far the pitch bends (a ratio at the end, or the wobble's depth)
    float flutterHz; // the level's flutter
    float flutter;   // its depth
    float jitter;    // the period's randomness
    float noise;     // airy noise
    float formantHz;
    float sputterHz; // gurgling gaps (0: none)
    float bright;    // the buzz's width: brighter is a narrower pulse
    float level;
};

constexpr FartKind kFarts[kFartTypes] = {
    { "Squeaker", 0.35f, Bend::Rise, 1.5f, 40.0f, 0.3f, 0.05f, 0.05f, 900.0f, 0.0f, 0.8f, 0.8f },
    { "Tuba", 0.9f, Bend::Fall, 0.9f, 18.0f, 0.5f, 0.1f, 0.1f, 250.0f, 0.0f, 0.4f, 1.0f },
    { "Rumble", 1.4f, Bend::Fall, 0.8f, 12.0f, 0.7f, 0.2f, 0.25f, 180.0f, 0.0f, 0.2f, 1.0f },
    { "Wet", 0.6f, Bend::Steady, 1.0f, 25.0f, 0.6f, 0.3f, 0.7f, 400.0f, 8.0f, 0.5f, 0.9f },
    { "Stutter", 0.8f, Bend::Steady, 1.0f, 20.0f, 0.5f, 0.15f, 0.15f, 350.0f, 6.0f, 0.5f, 0.9f },
    { "Long Haul", 2.5f, Bend::Wobble, 0.12f, 22.0f, 0.5f, 0.1f, 0.1f, 300.0f, 0.0f, 0.45f, 0.9f },
    { "Whoopee", 1.2f, Bend::Fall, 0.6f, 30.0f, 0.4f, 0.05f, 0.2f, 500.0f, 0.0f, 0.6f, 0.9f },
    { "Machine Gun", 1.0f, Bend::Steady, 1.0f, 15.0f, 0.8f, 0.1f, 0.1f, 300.0f, 12.0f, 0.5f, 1.0f },
    { "Silent", 1.5f, Bend::Steady, 1.0f, 8.0f, 0.2f, 0.5f, 0.95f, 1500.0f, 0.0f, 0.3f, 0.5f },
    { "Duck", 0.5f, Bend::Quack, 1.25f, 50.0f, 0.3f, 0.05f, 0.1f, 1200.0f, 0.0f, 1.0f, 0.7f },
    { "Balloon", 1.8f, Bend::Rise, 2.0f, 35.0f, 0.25f, 0.02f, 0.05f, 1000.0f, 0.0f, 0.9f, 0.7f },
    { "Motorboat", 1.6f, Bend::Steady, 1.0f, 9.0f, 1.0f, 0.1f, 0.2f, 220.0f, 0.0f, 0.35f, 1.0f },
    { "Bubbles", 0.7f, Bend::Rise, 1.3f, 28.0f, 0.5f, 0.4f, 0.5f, 650.0f, 10.0f, 0.6f, 0.9f },
};

constexpr const char* kFartTypeNames[kFartTypes + 1] = {
    "Random", "Squeaker", "Tuba", "Rumble", "Wet", "Stutter", "Long Haul", "Whoopee", "Machine Gun", "Silent", "Duck",
    "Balloon", "Motorboat", "Bubbles",
};

constexpr int kFartVoices = 2;

class FartBlock : public TrackedSynth {
public:
    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kFartParamType:       fType = std::clamp(static_cast<int>(value), 0, kFartTypes); break;
        case kFartParamLength:     fLength = value / 100.0f; break;
        case kFartParamWetness:    fWetness = value / 100.0f; break;
        case kFartParamTone:       fTone = std::exp2((value - 50.0f) / 50.0f); break;
        case kFartParamOctave:     setOctave(value); break;
        case kFartParamMix:        setMix(value); break;
        case kFartParamPickFilter: setPickFilter(value); break;
        case kFartParamTrack:      setTrack(value); break;
        }
    }

protected:
    void resetVoice() override
    {
        for (Voice& voice : fVoices)
            voice.active = false;
        fLast = -1;
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
            trigger(note);
        float sum = 0.0f;
        for (Voice& voice : fVoices)
            if (voice.active)
                sum += renderVoice(voice, note);
        left = right = sum;
    }

private:
    struct Voice {
        bool active = false;
        const FartKind* kind = &kFarts[0];
        double hz = 100.0;
        double phase = 0.0, flutterPhase = 0.0, sputterPhase = 0.0;
        float time = 0.0f, length = 1.0f;
        float velocity = 0.0f;
        float jitter = 0.0f, jitterTarget = 0.0f;
        float wet = 0.0f;
        Svf formant, body, air;
    };

    void trigger(const Note& note)
    {
        // a new one takes a free voice, or the older one
        Voice* voice = &fVoices[0];
        for (Voice& v : fVoices)
            if (!v.active || v.time > voice->time)
                voice = &v;
        int kind = fType - 1;
        if (fType == 0)
        {
            // Random: any kind but the last one
            kind = static_cast<int>(fRandom.uniform() * (kFartTypes - 1));
            if (kind >= fLast && fLast >= 0)
                ++kind;
            kind = std::clamp(kind, 0, kFartTypes - 1);
        }
        fLast = kind;
        voice->active = true;
        voice->kind = &kFarts[kind];
        voice->hz = note.target;
        voice->phase = 0.0;
        voice->flutterPhase = fRandom.uniform();
        voice->sputterPhase = 0.25;
        voice->time = 0.0f;
        voice->length = std::max(0.08f, voice->kind->seconds * (0.25f + 1.75f * fLength));
        voice->velocity = 0.0f;
        voice->jitter = voice->jitterTarget = 0.0f;
        voice->wet = std::min(1.0f, voice->kind->noise * (0.5f + fWetness));
        const double formant = std::min(0.4 * fSampleRate, static_cast<double>(voice->kind->formantHz * fTone));
        voice->formant.set(fSampleRate, formant, 2.0);
        voice->body.set(fSampleRate, std::min(0.4 * fSampleRate, 2.0 * formant), 0.7);
        voice->air.set(fSampleRate, std::min(0.4 * fSampleRate, 3.0 * formant), 0.8);
        voice->formant.reset();
        voice->body.reset();
        voice->air.reset();
    }

    float renderVoice(Voice& v, const Note& note)
    {
        const FartKind& k = *v.kind;
        const auto dt = static_cast<float>(1.0 / fSampleRate);
        v.time += dt;
        const float u = v.time / v.length;
        if (u >= 1.0f)
        {
            v.active = false;
            return 0.0f;
        }
        // as loud as the pick (its level over the first few ms)
        if (v.time < 0.015f)
            v.velocity = std::max(v.velocity, std::min(1.0f, note.envelope * 5.0f));
        const float env = std::min(1.0f, v.time / 0.015f) * (u < 0.75f ? 1.0f : (1.0f - u) / 0.25f);

        float bend = 1.0f;
        switch (k.bend)
        {
        case Bend::Rise:
        case Bend::Fall:   bend = 1.0f + (k.amount - 1.0f) * u; break;
        case Bend::Wobble: bend = 1.0f + k.amount * std::sin(kTwoPi * 3.0f * v.time); break;
        case Bend::Quack:  bend = u < 0.3f ? 1.0f + (k.amount - 1.0f) * u / 0.3f : k.amount - 0.35f * (u - 0.3f); break;
        case Bend::Steady: break;
        }
        // the period wanders: a new random target each cycle
        v.jitter += (v.jitterTarget - v.jitter) * 0.01f;
        const double hz = std::clamp(v.hz * bend * (1.0 + k.jitter * v.jitter), 20.0, 0.3 * fSampleRate);
        const double step = hz / fSampleRate;
        if (advancePhase(v.phase, step))
            v.jitterTarget = fRandom.next();
        const float width = 0.5f - 0.4f * k.bright;
        const float buzz = blepPulse(v.phase, step, width);

        v.flutterPhase += k.flutterHz / fSampleRate;
        v.flutterPhase -= std::floor(v.flutterPhase);
        const float flutter = 1.0f - k.flutter * 0.5f * (1.0f + std::sin(kTwoPi * static_cast<float>(v.flutterPhase)));
        float sputter = 1.0f;
        if (k.sputterHz > 0.0f)
        {
            v.sputterPhase += k.sputterHz * (0.6 + 0.8 * fWetness) / fSampleRate;
            v.sputterPhase -= std::floor(v.sputterPhase);
            const float s = std::sin(kTwoPi * static_cast<float>(v.sputterPhase));
            sputter = std::clamp(2.0f * (s + 0.3f), 0.0f, 1.0f);
        }
        const float air = v.air.process(fRandom.next()).band * v.wet * 3.0f;
        const float source = buzz * (1.0f - v.wet) + air;
        const float shaped = 1.4f * v.formant.process(source).band + 0.5f * v.body.process(source).low;
        return std::tanh(shaped * 1.5f) * 0.45f * env * flutter * sputter * v.velocity * k.level;
    }

    int fType = 0;
    float fLength = 0.43f, fWetness = 0.5f, fTone = 1.0f;
    std::array<Voice, kFartVoices> fVoices;
    Noise fRandom { 0xfa47u };
    int fLast = -1;
};

// ---------------------------------------------------------------------------------------------------------------------
// Trumpet

constexpr const char* kMuteNames[] = { "Open", "Harmon", "Cup" };

class TrumpetBlock : public TrackedSynth {
public:
    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kTrumpetParamBright:  fBright = value / 100.0f; break;
        case kTrumpetParamGrowl:   fGrowl = value / 100.0f; break;
        case kTrumpetParamScoop:   fScoop = value; break;
        case kTrumpetParamVibrato: fVibrato = value / 100.0f; break;
        case kTrumpetParamMute:
            fMute = std::clamp(static_cast<int>(value), 0, 2);
            updateMute();
            break;
        case kTrumpetParamAttack:  setAttack(value); break;
        case kTrumpetParamRelease: setRelease(value); break;
        case kTrumpetParamOctave:  setOctave(value); break;
        case kTrumpetParamGlide:   setGlide(value); break;
        case kTrumpetParamMix:     setMix(value); break;
        case kTrumpetParamPickFilter: setPickFilter(value); break;
        case kTrumpetParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fBell[0].setPeak(fSampleRate, 1300.0, 5.0, 1.4);
        fBell[1].setPeak(fSampleRate, std::min(2900.0, 0.45 * fSampleRate), 3.0, 1.8);
        fScoopRelease = static_cast<float>(std::exp(-1.0 / (0.045 * fSampleRate)));
        fGrowlRelease = static_cast<float>(std::exp(-1.0 / (0.08 * fSampleRate)));
        updateMute();
    }

    void resetVoice() override
    {
        fPhase = 0.0;
        fFilter.reset();
        for (Biquad& b : fBell)
            b.reset();
        for (Biquad& b : fMuteFilters)
            b.reset();
        fScoopNow = 0.0f;
        fGrowlNow = 0.0f;
        fSinceOnset = 1.0f;
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
        {
            fScoopNow = -fScoop;
            fGrowlNow = 1.0f;
            fSinceOnset = 0.0f;
        }
        fScoopNow *= fScoopRelease;
        fGrowlNow *= fGrowlRelease;
        fSinceOnset += static_cast<float>(1.0 / fSampleRate);
        // vibrato from a quarter second into the note
        const float vibratoIn = std::clamp((fSinceOnset - 0.25f) / 0.3f, 0.0f, 1.0f);
        fVibratoPhase += 5.5 / fSampleRate;
        fVibratoPhase -= std::floor(fVibratoPhase);
        const float cents = fScoopNow + 25.0f * fVibrato * vibratoIn * std::sin(kTwoPi * static_cast<float>(fVibratoPhase));
        const double hz = note.hz * std::exp2(cents / 1200.0);
        const double dt = std::min(0.45, hz / fSampleRate);
        advancePhase(fPhase, dt);
        const float saw = blepSaw(fPhase, dt);
        // brass gets brighter the louder it plays
        const float amp = note.amp;
        const double cutoff = hz * (1.5 + 18.0 * fBright * std::pow(amp, 1.5f));
        fFilter.set(fSampleRate, std::min(cutoff, 0.45 * fSampleRate), 0.9);
        float y = fFilter.process(saw).low;
        y = std::tanh(y * (1.2f + 3.0f * fGrowl * fGrowlNow)) + 0.02f * fBreath.next() * amp;
        for (Biquad& b : fBell)
            y = b.process(y);
        for (Biquad& b : fMuteFilters)
            y = b.process(y);
        left = right = y * amp * 0.5f;
    }

private:
    void updateMute()
    {
        const double nyquist = 0.45 * fSampleRate;
        switch (fMute)
        {
        case 1: // Harmon: thin and buzzy
            fMuteFilters[0].setHighPass(fSampleRate, 700.0, 0.7);
            fMuteFilters[1].setPeak(fSampleRate, std::min(1800.0, nyquist), 9.0, 2.0);
            break;
        case 2: // Cup: soft and round
            fMuteFilters[0].setLowPass(fSampleRate, std::min(1600.0, nyquist), 0.8);
            fMuteFilters[1].setPeak(fSampleRate, 800.0, 4.0, 1.2);
            break;
        default:
            fMuteFilters[0].setPeak(fSampleRate, 1000.0, 0.0, 1.0);
            fMuteFilters[1].setPeak(fSampleRate, 1000.0, 0.0, 1.0);
            break;
        }
    }

    float fBright = 0.6f, fGrowl = 0.3f, fScoop = 40.0f, fVibrato = 0.4f;
    int fMute = 0;
    double fPhase = 0.0, fVibratoPhase = 0.0;
    Svf fFilter;
    Biquad fBell[2], fMuteFilters[2];
    float fScoopNow = 0.0f, fGrowlNow = 0.0f, fSinceOnset = 1.0f;
    float fScoopRelease = 0.999f, fGrowlRelease = 0.999f;
    Noise fBreath { 0x7a3u };
};

// ---------------------------------------------------------------------------------------------------------------------
// Violin

constexpr const char* kViolinBodyNames[] = { "Violin", "Viola", "Cello" };
constexpr double kBodyScale[3] = { 1.0, 0.78, 0.45 };
struct BodyPeak {
    double hz, db, q;
};
// the violin's body: the air (A0) and wood (B1) resonances, a dip, the bridge hill
constexpr BodyPeak kViolinPeaks[4] = { { 280, 6.0, 3.0 }, { 460, 4.0, 3.0 }, { 700, -3.0, 2.0 }, { 2600, 6.0, 1.4 } };
constexpr int kPlayers = 3;
constexpr float kPlayerDetune[kPlayers] = { 0.0f, -7.0f, 8.0f };     // cents
constexpr float kPlayerRate[kPlayers] = { 1.0f, 0.93f, 1.08f };      // vibrato rates

class ViolinBlock : public TrackedSynth {
public:
    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kViolinParamBody:
            fBody = std::clamp(static_cast<int>(value), 0, 2);
            updateBody();
            break;
        case kViolinParamVibrato:  fVibrato = value / 100.0f; break;
        case kViolinParamRate:     fRate = value; break;
        case kViolinParamBow:      fBow = value / 100.0f; break;
        case kViolinParamEnsemble: fEnsemble = value >= 0.5f; break;
        case kViolinParamAttack:   setAttack(value); break;
        case kViolinParamRelease:  setRelease(value); break;
        case kViolinParamOctave:   setOctave(value); break;
        case kViolinParamGlide:    setGlide(value); break;
        case kViolinParamMix:      setMix(value); break;
        case kViolinParamPickFilter: setPickFilter(value); break;
        case kViolinParamTrack:    setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fBowFilter.set(fSampleRate, std::min(3000.0, 0.45 * fSampleRate), 0.7);
        updateBody();
    }

    void resetVoice() override
    {
        for (int p = 0; p < kPlayers; ++p)
        {
            fPhase[p] = 0.3 * p;
            fVibratoPhase[p] = 0.37 * p;
        }
        for (int ch = 0; ch < 2; ++ch)
            for (Biquad& b : fBodyFilters[ch])
                b.reset();
        fBowFilter.reset();
        fSinceOnset = 1.0f;
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
            fSinceOnset = 0.0f;
        fSinceOnset += static_cast<float>(1.0 / fSampleRate);
        const float vibratoIn = std::clamp((fSinceOnset - 0.2f) / 0.4f, 0.0f, 1.0f);
        const int players = fEnsemble ? kPlayers : 1;
        float out[2] = { 0.0f, 0.0f };
        for (int p = 0; p < players; ++p)
        {
            fVibratoPhase[p] += fRate * kPlayerRate[p] / fSampleRate;
            fVibratoPhase[p] -= std::floor(fVibratoPhase[p]);
            const float cents = (fEnsemble ? kPlayerDetune[p] : 0.0f)
                                + 30.0f * fVibrato * vibratoIn * std::sin(kTwoPi * static_cast<float>(fVibratoPhase[p]));
            const double dt = std::min(0.45, note.hz * std::exp2(cents / 1200.0) / fSampleRate);
            advancePhase(fPhase[p], dt);
            const float saw = blepSaw(fPhase[p], dt);
            // the section: one player in the middle, the others either side
            const float pan = p == 0 ? 0.0f : (p == 1 ? -0.6f : 0.6f);
            out[0] += saw * (0.5f - 0.5f * pan);
            out[1] += saw * (0.5f + 0.5f * pan);
        }
        const float bow = fBowFilter.process(fNoise.next()).high * fBow * 0.3f;
        const float norm = 1.0f / std::sqrt(static_cast<float>(players));
        for (int ch = 0; ch < 2; ++ch)
        {
            float y = out[ch] * norm + bow;
            for (Biquad& b : fBodyFilters[ch])
                y = b.process(y);
            out[ch] = y * note.amp * 0.35f;
        }
        left = out[0];
        right = out[1];
    }

private:
    void updateBody()
    {
        const double nyquist = 0.45 * fSampleRate;
        for (int ch = 0; ch < 2; ++ch)
        {
            for (int k = 0; k < 4; ++k)
            {
                const BodyPeak& peak = kViolinPeaks[k];
                fBodyFilters[ch][k].setPeak(fSampleRate, std::min(peak.hz * kBodyScale[fBody], nyquist), peak.db,
                                            peak.q);
            }
            // the strings' highs roll off above the bridge hill
            fBodyFilters[ch][4].setLowPass(fSampleRate, std::min(6000.0 * kBodyScale[fBody] + 1500.0, nyquist), 0.7);
        }
    }

    int fBody = 0;
    float fVibrato = 0.5f, fRate = 5.5f, fBow = 0.3f;
    bool fEnsemble = false;
    double fPhase[kPlayers] = {}, fVibratoPhase[kPlayers] = {};
    std::array<std::array<Biquad, 5>, 2> fBodyFilters;
    Svf fBowFilter;
    Noise fNoise { 0xb0e5u };
    float fSinceOnset = 1.0f;
};

// ---------------------------------------------------------------------------------------------------------------------
// Piano

constexpr int kPianoVoices = 8;
constexpr int kPartials = 12;
constexpr double kInharmonicity = 0.0004;

class PianoBlock : public TrackedSynth {
public:
    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kPianoParamBright:  fBright = value / 100.0f; break;
        case kPianoParamDecay:   fDecay = value; break;
        case kPianoParamSustain: fSustain = value >= 0.5f; break;
        case kPianoParamDetune:  fDetune = value; break;
        case kPianoParamHammer:  fHammer = value / 100.0f; break;
        case kPianoParamTone:
            fToneHz = value;
            updateTone();
            break;
        case kPianoParamOctave:  setOctave(value); break;
        case kPianoParamMix:     setMix(value); break;
        case kPianoParamPickFilter: setPickFilter(value); break;
        case kPianoParamTrack:   setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override
    {
        fDamp = static_cast<float>(std::exp(-1.0 / (0.08 * fSampleRate)));
        fThumpFilter.setLowPass(fSampleRate, 400.0);
        updateTone();
    }

    void resetVoice() override
    {
        for (Voice& voice : fVoices)
            voice.active = false;
        fHeld = -1;
        fHeldMidi = -1;
        fWasOn = false;
        fThump = 0.0f;
        fTone.reset();
        fThumpFilter.reset();
    }

    void render(const Note& note, float& left, float& right) override
    {
        // a new note: a pick, or a new pitch without one (a hammer-on); letting go damps the held note
        const int midi = note.target > 0.0 ? static_cast<int>(std::lround(69.0 + 12.0 * std::log2(note.target / 440.0))) : -1;
        if (note.on && (note.onset || !fWasOn || midi != fHeldMidi))
            strike(note, midi);
        if (!note.on && fWasOn)
            release();
        fWasOn = note.on;
        // velocity: the pick's level over its first few ms
        if (fHeld >= 0 && fVoices[fHeld].age < 0.012f * fSampleRate)
            fVoices[fHeld].velocity = std::max(fVoices[fHeld].velocity, std::min(1.0f, note.envelope * 5.0f));

        float sum = 0.0f;
        for (Voice& voice : fVoices)
            if (voice.active)
                sum += renderVoice(voice);
        sum += fThumpFilter.lowPass(fNoise.next()) * fThump;
        fThump *= fThumpDecay;
        const float y = fTone.lowPass(sum) * 0.4f;
        left = right = y;
    }

private:
    struct Partial {
        float c = 1.0f, s = 0.0f; // the rotation per sample
        float x = 1.0f, y = 0.0f; // the oscillator
        float amp = 0.0f, decay = 1.0f;
    };
    struct Voice {
        bool active = false, held = false;
        std::array<Partial, 2 * kPartials> partials; // two strings per partial
        int count = 0;
        float velocity = 0.0f;
        float damping = 1.0f;
        float age = 0.0f;
    };

    void strike(const Note& note, int midi)
    {
        if (!fSustain && fHeld >= 0)
            fVoices[fHeld].damping = fDamp;
        // a free voice, else the quietest
        int pick = 0;
        float quietest = 1e9f;
        for (int v = 0; v < kPianoVoices; ++v)
        {
            const Voice& voice = fVoices[v];
            const float level = voice.active ? voice.partials[0].amp + voice.partials[2].amp : -1.0f;
            if (level < quietest)
            {
                quietest = level;
                pick = v;
            }
        }
        Voice& voice = fVoices[pick];
        voice.active = true;
        voice.held = true;
        voice.velocity = std::min(1.0f, std::max(0.2f, note.envelope * 5.0f));
        voice.damping = 1.0f;
        voice.age = 0.0f;
        voice.count = 0;
        const double f0 = note.target;
        const float vel = voice.velocity;
        double total = 0.0;
        for (int n = 1; n <= kPartials; ++n)
        {
            const double hz = n * f0 * std::sqrt(1.0 + kInharmonicity * n * n);
            if (hz > 0.45 * fSampleRate)
                break;
            // the hammer: brighter when harder, and a notch where it strikes (an eighth of the string)
            const double tilt = 1.3 - 0.8 * fBright * vel;
            const double amp = std::pow(static_cast<double>(n), -tilt) * (std::fabs(std::sin(M_PI * n / 8.0)) + 0.4);
            // higher partials die sooner; low notes ring longer
            const double seconds = fDecay * std::sqrt(220.0 / std::max(55.0, f0)) * (0.3 + 0.7 / n);
            for (int string = 0; string < 2; ++string)
            {
                const double cents = (string == 0 ? -0.5 : 0.5) * fDetune;
                const double w = 2.0 * M_PI * hz * std::exp2(cents / 1200.0) / fSampleRate;
                Partial& p = voice.partials[voice.count++];
                p.c = static_cast<float>(std::cos(w));
                p.s = static_cast<float>(std::sin(w));
                p.x = 1.0f;
                p.y = 0.0f;
                p.amp = static_cast<float>(0.5 * amp);
                p.decay = static_cast<float>(std::exp(-1.0 / (std::max(0.05, seconds) * fSampleRate)));
                total += 0.5 * amp;
            }
        }
        // a note's partials together peak at 1 at most
        for (int k = 0; k < voice.count; ++k)
            voice.partials[k].amp /= static_cast<float>(std::max(1.0, total));
        fHeld = pick;
        fHeldMidi = midi;
        fThump = 0.3f * fHammer * vel;
        fThumpDecay = static_cast<float>(std::exp(-1.0 / (0.012 * fSampleRate)));
    }

    void release()
    {
        if (fHeld >= 0 && !fSustain)
            fVoices[fHeld].damping = fDamp;
        fHeld = -1;
        fHeldMidi = -1;
    }

    float renderVoice(Voice& voice)
    {
        float sum = 0.0f, loudest = 0.0f;
        for (int k = 0; k < voice.count; ++k)
        {
            Partial& p = voice.partials[k];
            const float x = p.x * p.c - p.y * p.s;
            const float y = p.x * p.s + p.y * p.c;
            // keep the oscillator on its circle
            const float norm = 1.5f - 0.5f * (x * x + y * y);
            p.x = x * norm;
            p.y = y * norm;
            p.amp *= p.decay * voice.damping;
            sum += p.y * p.amp;
            loudest = std::max(loudest, p.amp);
        }
        voice.age += 1.0f;
        if (loudest < 1e-5f)
            voice.active = false;
        return sum * voice.velocity;
    }

    void updateTone()
    {
        fTone.setLowPass(fSampleRate, std::min<double>(fToneHz, 0.45 * fSampleRate));
    }

    float fBright = 0.5f, fDecay = 6.0f, fDetune = 1.5f, fHammer = 0.5f, fToneHz = 9000.0f;
    bool fSustain = false;
    std::array<Voice, kPianoVoices> fVoices;
    int fHeld = -1, fHeldMidi = -1;
    bool fWasOn = false;
    float fDamp = 0.999f;
    float fThump = 0.0f, fThumpDecay = 0.99f;
    OnePole fThumpFilter;
    OnePole fTone;
    Noise fNoise { 0x91a0u };
};

} // namespace

const char* fartTypeName(int type)
{
    return kFarts[std::clamp(type, 0, kFartTypes - 1)].name;
}

const BlockDescriptor kFartBlockDescriptor = {
    "fart",
    "Fart",
    Category::Synth,
    kFartParamCount,
    {
        ParamSpec { "Type", "", 0.0f, static_cast<float>(kFartTypes), 0.0f, ParamScale::Choice, kFartTypeNames },
        ParamSpec { "Length", "%", 0.0f, 100.0f, 43.0f, ParamScale::Linear },
        ParamSpec { "Wetness", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Tone", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<FartBlock>()); },
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

const BlockDescriptor kTrumpetBlockDescriptor = {
    "trumpet",
    "Trumpet",
    Category::Synth,
    kTrumpetParamCount,
    {
        ParamSpec { "Bright", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Growl", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Scoop", "ct", 0.0f, 150.0f, 40.0f, ParamScale::Linear },
        ParamSpec { "Vibrato", "%", 0.0f, 100.0f, 40.0f, ParamScale::Linear },
        ParamSpec { "Mute", "", 0.0f, 2.0f, 0.0f, ParamScale::Choice, kMuteNames },
        ParamSpec { "Attack", "ms", 1.0f, 500.0f, 25.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 2000.0f, 120.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 25.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<TrumpetBlock>()); },
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

const BlockDescriptor kViolinBlockDescriptor = {
    "violin",
    "Violin",
    Category::Synth,
    kViolinParamCount,
    {
        ParamSpec { "Body", "", 0.0f, 2.0f, 0.0f, ParamScale::Choice, kViolinBodyNames },
        ParamSpec { "Vibrato", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Rate", "Hz", 3.0f, 8.0f, 5.5f, ParamScale::Linear },
        ParamSpec { "Bow", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Ensemble", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Attack", "ms", 1.0f, 2000.0f, 120.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 3000.0f, 250.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Glide", "ms", 0.1f, 500.0f, 60.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<ViolinBlock>()); },
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

const BlockDescriptor kPianoBlockDescriptor = {
    "piano",
    "Piano",
    Category::Synth,
    kPianoParamCount,
    {
        ParamSpec { "Bright", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Decay", "s", 1.0f, 20.0f, 6.0f, ParamScale::Log },
        ParamSpec { "Sustain", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Detune", "ct", 0.0f, 6.0f, 1.5f, ParamScale::Linear },
        ParamSpec { "Hammer", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 1000.0f, 16000.0f, 9000.0f, ParamScale::Log },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new Voiced<PianoBlock>()); },
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
