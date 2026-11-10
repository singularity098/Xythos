// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/InstrumentBlocks.hpp"

#include "engine/blocks/FartSamples.hpp"
#include "engine/blocks/TrackedSynth.hpp"
#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/Oscillator.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

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

// The samples (resources/farts/; tools/prepare_fart_samples.py prints these rows), lowest to highest: the Type
// control's choices after Random, in this order (never add or remove one once released). hz: the sample's pitch
// (YIN's median period, or its strongest spectral peak for a hiss or a splat); gain: so they all play about as loud.
struct FartSample {
    const char* id;
    const char* name;
    float hz;
    float gain;
};

constexpr FartSample kFartSamples[kFartTypes] = {
    { "chop1-06", "Rumble", 62.7f, 0.846f },     // 0.86 s
    { "chop1-09", "Thunder", 64.3f, 0.904f },    // 1.57 s
    { "apebble-5", "Tuba", 83.9f, 0.762f },      // 0.87 s
    { "wet-6139", "Wet One", 88.9f, 0.597f },    // 0.92 s
    { "proud", "Proud", 108.9f, 0.528f },        // 0.92 s
    { "chop1-01", "Toot", 137.0f, 1.059f },      // 0.50 s
    { "wet-long", "Marathon", 138.1f, 1.059f },  // 4.66 s
    { "chop1-12", "Trombone", 140.0f, 0.854f },  // 1.04 s
    { "chop2-03", "Long Haul", 140.9f, 0.558f }, // 1.72 s
    { "chop2-02", "Groaner", 160.7f, 0.633f },   // 1.06 s
    { "furtz", "Pop", 163.9f, 0.462f },          // 0.22 s
    { "chop1-07", "Honk", 175.0f, 0.842f },      // 0.55 s
    { "short-83471", "Blip", 195.8f, 0.610f },   // 0.16 s
    { "apebble-4", "Squelch", 199.2f, 0.421f },  // 0.57 s, spectral peak
    { "chop1-02", "Puff", 199.2f, 1.059f },      // 0.25 s, spectral peak
    { "wet-short", "Squish", 252.7f, 0.809f },   // 0.52 s
    { "chop2-01", "Whoopee", 254.3f, 0.724f },   // 1.30 s
    { "apebble-8", "Brass", 257.8f, 0.682f },    // 0.95 s, spectral peak
    { "chop1-04", "Sputter", 269.5f, 1.059f },   // 0.28 s, spectral peak
    { "chop1-08", "Splat", 269.5f, 0.710f },     // 0.68 s, spectral peak
    { "chop1-10", "Raspberry", 269.5f, 1.059f }, // 0.62 s, spectral peak
    { "chop1-05", "Squeak", 294.5f, 0.643f },    // 0.34 s
    { "chop1-03", "Pfft", 316.4f, 0.977f },      // 0.36 s, spectral peak
    { "balloon", "Balloon", 385.5f, 0.689f },    // 7.43 s
    { "apebble-7", "Mouse", 595.4f, 0.597f },    // 0.38 s
};

constexpr std::array<const char*, kFartTypes + 1> fartTypeNames()
{
    std::array<const char*, kFartTypes + 1> names {};
    names[0] = "Random";
    for (int i = 0; i < kFartTypes; ++i)
        names[static_cast<size_t>(i + 1)] = kFartSamples[i].name;
    return names;
}

constexpr std::array<const char*, kFartTypes + 1> kFartTypeNames = fartTypeNames();
constexpr const char* kFartPitchNames[] = { "Follow", "Natural" };
constexpr const char* kFartHoldNames[] = { "One Shot", "Held" };

// The samples' PCM, found in their embedded WAVs once (the first Fart block made, never on the audio thread)
const std::array<PcmView, kFartTypes>& fartPcm()
{
    static const std::array<PcmView, kFartTypes> pcm = [] {
        std::array<PcmView, kFartTypes> views {};
        for (int i = 0; i < kFartTypes; ++i)
        {
            const EmbeddedFile file = builtinFartSample(kFartSamples[i].id);
            views[static_cast<size_t>(i)] = pcmFromWav(file.data, file.size);
        }
        return views;
    }();
    return pcm;
}

// up to this many farts at once (a new one fades out the oldest), and a slot or two for those fading out
constexpr int kFartSounding = 3;
constexpr int kFartVoices = 5;
// how far Random will shift a sample to reach your note, in semitones (if fewer than kFartNearest samples are that
// close, the nearest ones)
constexpr float kFartRandomRange = 6.0f;
constexpr int kFartNearest = 3;
// the furthest a sample is played from its own pitch: a ratio of 1/8 to 4
constexpr double kFartMinRatio = 0.125, kFartMaxRatio = 4.0;
// the samples' level (each about -16 dBFS RMS by its gain) to the block's: a little over a DI guitar's, with room for
// three at once
constexpr float kFartLevel = 0.6f;

class FartBlock : public TrackedSynth {
public:
    FartBlock() : fPcm(fartPcm()), fRandom(0xfa47u + 0x9e3779b9u * fInstances.fetch_add(1)) {}

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kFartParamType:       fType = std::clamp(static_cast<int>(value), 0, kFartTypes); break;
        case kFartParamLength:     fLength = std::clamp(value / 100.0f, 0.05f, 1.0f); break;
        case kFartParamTone:       fToneValue = value; updateTone(); break;
        case kFartParamPitch:      fNatural = value >= 0.5f; break;
        case kFartParamOctave:     setOctave(value); break;
        case kFartParamMix:        setMix(value); break;
        case kFartParamHold:       fHeld = value >= 0.5f; break;
        case kFartParamPickFilter: setPickFilter(value); break;
        case kFartParamTrack:      setTrack(value); break;
        }
    }

protected:
    void prepareVoice() override { updateTone(); }

    void resetVoice() override
    {
        for (Voice& voice : fVoices)
            voice.active = false;
        fLast = -1;
        fTone.reset();
        fWasOn = false;
    }

    void render(const Note& note, float& left, float& right) override
    {
        if (note.onset)
            trigger(note);
        else if (fHeld && fWasOn && !note.on)
            for (Voice& voice : fVoices)
                if (voice.active)
                    voice.fadeStep = std::max(voice.fadeStep, static_cast<float>(1.0 / (0.06 * fSampleRate)));
        fWasOn = note.on;
        float sum = 0.0f;
        for (Voice& voice : fVoices)
            if (voice.active)
                sum += renderVoice(voice, note);
        if (fToneMode != 0)
        {
            const Svf::Out out = fTone.process(sum);
            sum = fToneMode < 0 ? out.low : out.high;
        }
        left = right = sum;
    }

private:
    struct Voice {
        bool active = false;
        int sample = 0;
        double pos = 0.0, step = 1.0;
        double end = 0.0;       // where it stops (Length), in the sample's frames
        double fadeFrom = 0.0;  // where its fade-out to `end` starts
        float gain = 1.0f;
        float velocity = 0.0f;
        float time = 0.0f;      // seconds since it started
        float fade = 1.0f, fadeStep = 0.0f; // a fade-out (stolen, or let go of while Held)
    };

    // The sample to play for this note: the chosen one, or Random's pick (one near the note when following it, never
    // the last one if there's another)
    int pickSample(double hz)
    {
        if (fType > 0)
            return fType - 1;
        std::array<int, kFartTypes> candidates {};
        int count = 0;
        if (fNatural || !(hz > 0.0))
        {
            for (int i = 0; i < kFartTypes; ++i)
                candidates[static_cast<size_t>(count++)] = i;
        }
        else
        {
            std::array<float, kFartTypes> distance {};
            for (int i = 0; i < kFartTypes; ++i)
                distance[static_cast<size_t>(i)] =
                    std::abs(12.0f * std::log2(static_cast<float>(hz) / kFartSamples[i].hz));
            for (int i = 0; i < kFartTypes; ++i)
                if (distance[static_cast<size_t>(i)] <= kFartRandomRange)
                    candidates[static_cast<size_t>(count++)] = i;
            if (count < kFartNearest)
            {
                // the nearest few (a note far below or above them all)
                count = 0;
                std::array<bool, kFartTypes> taken {};
                for (int n = 0; n < kFartNearest; ++n)
                {
                    int best = -1;
                    for (int i = 0; i < kFartTypes; ++i)
                        if (!taken[static_cast<size_t>(i)] &&
                            (best < 0 || distance[static_cast<size_t>(i)] < distance[static_cast<size_t>(best)]))
                            best = i;
                    taken[static_cast<size_t>(best)] = true;
                    candidates[static_cast<size_t>(count++)] = best;
                }
            }
        }
        if (count > 1)
        {
            // drop the last one played
            for (int c = 0; c < count; ++c)
                if (candidates[static_cast<size_t>(c)] == fLast)
                {
                    candidates[static_cast<size_t>(c)] = candidates[static_cast<size_t>(count - 1)];
                    --count;
                    break;
                }
        }
        const int c = std::min(count - 1, static_cast<int>(fRandom.uniform() * static_cast<float>(count)));
        return candidates[static_cast<size_t>(c)];
    }

    void trigger(const Note& note)
    {
        // too many sounding: the oldest fades out quickly
        int sounding = 0;
        Voice* oldest = nullptr;
        for (Voice& v : fVoices)
            if (v.active && v.fadeStep == 0.0f)
            {
                ++sounding;
                if (!oldest || v.time > oldest->time)
                    oldest = &v;
            }
        if (sounding >= kFartSounding && oldest)
            oldest->fadeStep = static_cast<float>(1.0 / (0.005 * fSampleRate));
        Voice* voice = nullptr;
        for (Voice& v : fVoices)
            if (!v.active)
            {
                voice = &v;
                break;
            }
        if (!voice)
        {
            // every slot busy (picks faster than the fades): the one furthest into its fade
            voice = &fVoices[0];
            for (Voice& v : fVoices)
                if (v.fade < voice->fade)
                    voice = &v;
        }

        const int sample = pickSample(note.target);
        fLast = sample;
        const PcmView& pcm = fPcm[static_cast<size_t>(sample)];
        if (pcm.frames < 4 || pcm.sampleRate == 0)
        {
            voice->active = false;
            return;
        }
        const double ratio = fNatural || !(note.target > 0.0)
                                 ? 1.0
                                 : std::clamp(note.target / kFartSamples[sample].hz, kFartMinRatio, kFartMaxRatio);
        voice->active = true;
        voice->sample = sample;
        voice->pos = 0.0;
        voice->step = ratio * pcm.sampleRate / fSampleRate;
        voice->end = static_cast<double>(pcm.frames - 1) * fLength;
        // fade out over its last 30 ms (as heard) when Length cuts it short; the whole sample has its own fade
        const double fade = fLength < 1.0f ? std::min(0.5 * voice->end, 0.03 * fSampleRate * voice->step) : 0.0;
        voice->fadeFrom = voice->end - fade;
        voice->gain = kFartSamples[sample].gain * kFartLevel;
        voice->velocity = 0.0f;
        voice->time = 0.0f;
        voice->fade = 1.0f;
        voice->fadeStep = 0.0f;
    }

    float renderVoice(Voice& v, const Note& note)
    {
        const PcmView& pcm = fPcm[static_cast<size_t>(v.sample)];
        if (v.pos >= v.end || v.fade <= 0.0f)
        {
            v.active = false;
            return 0.0f;
        }
        // as loud as the pick (its level over the first few ms)
        v.time += static_cast<float>(1.0 / fSampleRate);
        if (v.time < 0.015f)
            v.velocity = std::max(v.velocity, std::min(1.0f, note.envelope * 5.0f));

        // cubic Hermite between the samples around pos
        const auto i = static_cast<int64_t>(v.pos);
        const auto t = static_cast<float>(v.pos - static_cast<double>(i));
        auto at = [&](int64_t k) {
            return k < 0 || k >= static_cast<int64_t>(pcm.frames) ? 0.0f : pcmSample(pcm, static_cast<uint32_t>(k));
        };
        const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        const float y = ((c3 * t + c2) * t + c1) * t + y1;

        float env = v.fade;
        if (v.pos > v.fadeFrom && v.end > v.fadeFrom)
            env *= static_cast<float>((v.end - v.pos) / (v.end - v.fadeFrom));
        if (v.fadeStep > 0.0f)
            v.fade -= v.fadeStep;
        v.pos += v.step;
        return y * v.gain * v.velocity * env;
    }

    // Tone: 50% leaves the samples as they are; below, a low-pass closes down to 250 Hz; above, a high-pass opens up
    // to 800 Hz
    void updateTone()
    {
        const float u = (fToneValue - 50.0f) / 50.0f;
        if (std::abs(u) < 0.01f)
        {
            fToneMode = 0;
            return;
        }
        const int mode = u < 0.0f ? -1 : 1;
        if (mode != fToneMode)
            fTone.reset();
        fToneMode = mode;
        const double hz = u < 0.0f ? 16000.0 * std::exp2(6.0 * u) : 20.0 * std::exp2(5.3 * u);
        fTone.set(fSampleRate, std::min(hz, 0.45 * fSampleRate), 0.707);
    }

    static inline std::atomic<uint32_t> fInstances { 0 };

    const std::array<PcmView, kFartTypes>& fPcm;
    int fType = 0;
    float fLength = 1.0f;
    float fToneValue = 50.0f;
    int fToneMode = 0; // -1: low-pass, 0: none, 1: high-pass
    Svf fTone;
    bool fNatural = false, fHeld = false, fWasOn = false;
    std::array<Voice, kFartVoices> fVoices;
    Noise fRandom;
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

PcmView pcmFromWav(const unsigned char* data, size_t size)
{
    auto u16 = [&](size_t at) { return static_cast<uint32_t>(data[at] | (data[at + 1] << 8)); };
    auto u32 = [&](size_t at) { return u16(at) | (u16(at + 2) << 16); };
    if (!data || size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        return {};
    bool pcm16Mono = false;
    uint32_t rate = 0;
    for (size_t at = 12; at + 8 <= size;)
    {
        const uint32_t chunk = u32(at + 4);
        const size_t body = at + 8;
        if (chunk > size - body)
            return {};
        if (std::memcmp(data + at, "fmt ", 4) == 0 && chunk >= 16)
        {
            pcm16Mono = u16(body) == 1 && u16(body + 2) == 1 && u16(body + 14) == 16;
            rate = u32(body + 4);
        }
        else if (std::memcmp(data + at, "data", 4) == 0)
        {
            if (!pcm16Mono || rate == 0)
                return {};
            return { data + body, chunk / 2, rate };
        }
        at = body + chunk + (chunk & 1);
    }
    return {};
}

const char* fartTypeName(int type)
{
    return kFartSamples[std::clamp(type, 0, kFartTypes - 1)].name;
}

const BlockDescriptor kFartBlockDescriptor = {
    "fart",
    "Fart",
    Category::Synth,
    kFartParamCount,
    {
        ParamSpec { "Type", "", 0.0f, static_cast<float>(kFartTypes), 0.0f, ParamScale::Choice, kFartTypeNames.data() },
        ParamSpec { "Length", "%", 5.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Tone", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Pitch", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kFartPitchNames },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Hold", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kFartHoldNames },
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
