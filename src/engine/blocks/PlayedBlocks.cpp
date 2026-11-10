// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/PlayedBlocks.hpp"

#include "engine/blocks/PitchBlocks.hpp"
#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/OnsetDetector.hpp"
#include "engine/dsp/PitchShifter.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace xythos {

namespace {

constexpr const char* kKeyNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr double kMaxWindowMs = 100.0;

double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

constexpr const char* kStrumChordNames[] = { "Diatonic", "Major", "Minor", "Power", "Sus4", "Dom7", "Maj7", "Min7",
                                             "Octaves" };
constexpr int kStrumChordCount = 9;
// The notes over the played one, for the fixed shapes (Diatonic: scale steps 2, 4, 7 and 9 over it)
constexpr int kStrumChords[kStrumChordCount][4] = {
    { 0, 0, 0, 0 },     // Diatonic (from the key)
    { 4, 7, 12, 16 },   // Major
    { 3, 7, 12, 15 },   // Minor
    { 7, 12, 19, 24 },  // Power
    { 5, 7, 12, 17 },   // Sus4
    { 4, 7, 10, 12 },   // Dom7
    { 4, 7, 11, 12 },   // Maj7
    { 3, 7, 10, 12 },   // Min7
    { 12, 24, -12, 19 }, // Octaves
};
constexpr int kDiatonicSteps[4] = { 2, 4, 7, 9 };
constexpr const char* kStrumScaleNames[] = { "Major", "Minor" };
constexpr const char* kStrumNoteNames[] = { "2", "3", "4" };
constexpr const char* kStrumDirectionNames[] = { "Down", "Up", "Alternate" };
constexpr int kMaxStrumVoices = 4;

class StrumBlock : public Block {
public:
    StrumBlock()
    {
        fLevel.snap(0.8f);
        fDry.snap(1.0f);
    }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fInput.prepare(sampleRate);
        fLevel.prepare(sampleRate);
        fDry.prepare(sampleRate);
        for (Smoother& s : fShift)
            s.prepare(sampleRate, 0.01);
        for (DelayLine& line : fLines)
            line.prepare(static_cast<size_t>(std::ceil(kMaxWindowMs / 1000.0 * sampleRate)) + 8);
        fMaxWindow = static_cast<float>(kMaxWindowMs / 1000.0 * sampleRate);
        fAttack = 1.0f - EnvelopeFollower::coef(sampleRate, 3.0);
        fRelease = 1.0f - EnvelopeFollower::coef(sampleRate, 80.0);
        reset();
    }

    bool configure(const nlohmann::json& config) override
    {
        fInput.configure(config);
        return true;
    }

    uint32_t latency() const override { return fInput.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override { fInput.setRig(input, numChannels); }

    void setParam(int index, float value) override
    {
        const int choice = static_cast<int>(value);
        switch (index)
        {
        case kStrumParamChord:     fChord = std::clamp(choice, 0, kStrumChordCount - 1); fRetune = true; break;
        case kStrumParamKey:       fKey = std::clamp(choice, 0, 11); fRetune = true; break;
        case kStrumParamScale:     fScale = std::clamp(choice, 0, 1); fRetune = true; break;
        case kStrumParamNotes:     fNotes = std::clamp(choice, 0, 2) + 2; break;
        case kStrumParamStrum:     fStrumMs = value; break;
        case kStrumParamDirection: fDirection = std::clamp(choice, 0, 2); break;
        case kStrumParamLevel:     fLevel.setTarget(value / 100.0f); break;
        case kStrumParamDry:       fDry.setTarget(value / 100.0f); break;
        case kStrumParamWindow:    fWindowMs = std::min<float>(value, kMaxWindowMs); break;
        case kStrumParamTrack:     fInput.setTrackRig(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fLevel.snap(fLevel.target());
            fDry.snap(fDry.target());
        }
        const float baseWindow = std::min(fMaxWindow, static_cast<float>(fWindowMs / 1000.0 * fSampleRate));
        for (uint32_t i = 0; i < frames; ++i)
        {
            // (io[..][i] is delayed by the lookahead from here on)
            const NoteTracker::Note& note = fInput.process(io, numChannels, i);
            if (note.on && (fRetune || std::lround(hzToMidi(note.hz)) != fPlayed))
            {
                fRetune = false;
                fPlayed = static_cast<int>(std::lround(hzToMidi(note.hz)));
                for (int v = 0; v < kMaxStrumVoices; ++v)
                    fShift[v].setTarget(static_cast<float>(strumChordNote(fChord, fKey, fScale, fPlayed, v)));
            }
            if (note.onset)
                strum();
            if (!note.on)
                fStrumming = false;
            else
                ++fSinceStrum;

            const float window = pitchSyncWindow(baseWindow, note.hz > 0.0 ? fSampleRate / note.hz : 0.0, fMaxWindow);
            const float level = fLevel.next(), dry = fDry.next();
            double ratios[kMaxStrumVoices];
            float gains[kMaxStrumVoices];
            for (int v = 0; v < kMaxStrumVoices; ++v)
            {
                ratios[v] = semitonesToRatio(fShift[v].next());
                // each string comes in at its turn in the strum, and they all stop with the note
                const bool open = fStrumming && v < fNotes && fSinceStrum >= fStartAt[v];
                const float target = open ? 1.0f : 0.0f;
                fGain[v] += (target - fGain[v]) * (target > fGain[v] ? fAttack : fRelease);
                gains[v] = fGain[v] * level;
            }
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                fLines[ch].write(x);
                float wet = 0.0f;
                for (int v = 0; v < kMaxStrumVoices; ++v)
                    if (gains[v] > 1e-5f)
                        wet += fVoices[ch][v].read(fLines[ch], ratios[v], window) * gains[v];
                    else
                        fVoices[ch][v].read(fLines[ch], ratios[v], window);
                io[ch][i] = x * dry + wet * (1.0f / std::sqrt(static_cast<float>(fNotes)));
            }
        }
    }

    void reset() override
    {
        fInput.reset();
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].reset();
            for (int v = 0; v < kMaxStrumVoices; ++v)
                fVoices[ch][v].reset(fWindowMs / 1000.0f * static_cast<float>(fSampleRate), 0.25 * v);
        }
        for (float& g : fGain)
            g = 0.0f;
        fStrumming = false;
        fUp = false;
        fPlayed = -1;
        fRetune = true;
        fSnap = true;
    }

private:
    // A new pick: the strings come in one after another, in order of pitch
    void strum()
    {
        const bool up = fDirection == 1 || (fDirection == 2 && fUp);
        fUp = !fUp;
        std::array<int, kMaxStrumVoices> order {};
        for (int v = 0; v < fNotes; ++v)
        {
            // (an insertion sort: at most four)
            int k = v;
            const float pitch = fShift[v].target();
            while (k > 0 && (up ? fShift[order[k - 1]].target() < pitch : fShift[order[k - 1]].target() > pitch))
            {
                order[k] = order[k - 1];
                --k;
            }
            order[k] = v;
        }
        const auto gap = static_cast<int64_t>(fStrumMs / 1000.0f * fSampleRate);
        // Down: the played note is the lowest string, so the strum starts one gap after it; Up starts at the top
        for (int k = 0; k < fNotes; ++k)
            fStartAt[order[k]] = (up ? k : k + 1) * gap;
        fSinceStrum = 0;
        fStrumming = true;
    }

    double fSampleRate = 48000.0;
    TrackedInput fInput;
    int fChord = 0, fKey = 0, fScale = 0, fNotes = 3, fDirection = 0;
    float fStrumMs = 25.0f, fWindowMs = 40.0f;
    Smoother fLevel, fDry;
    std::array<Smoother, kMaxStrumVoices> fShift;
    DelayLine fLines[2];
    PitchVoice fVoices[2][kMaxStrumVoices];
    float fMaxWindow = 4800.0f;
    std::array<float, kMaxStrumVoices> fGain {};
    std::array<int64_t, kMaxStrumVoices> fStartAt {};
    int64_t fSinceStrum = 0;
    bool fStrumming = false, fUp = false, fRetune = true;
    int fPlayed = -1;
    float fAttack = 0.01f, fRelease = 0.001f;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kTuneScaleNames[] = { "Chromatic", "Major", "Minor", "Major Pent.", "Minor Pent.", "Blues" };
constexpr int kTuneScaleCount = 6;
// Each scale's notes as a mask of the 12 pitch classes from the key
constexpr uint16_t kTuneScales[kTuneScaleCount] = {
    0xfff,                                                                        // chromatic
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 5) | (1 << 7) | (1 << 9) | (1 << 11), // major
    (1 << 0) | (1 << 2) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 8) | (1 << 10), // natural minor
    (1 << 0) | (1 << 2) | (1 << 4) | (1 << 7) | (1 << 9),                        // major pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 7) | (1 << 10),                       // minor pentatonic
    (1 << 0) | (1 << 3) | (1 << 5) | (1 << 6) | (1 << 7) | (1 << 10),            // blues
};

class HardTuneBlock : public Block {
public:
    HardTuneBlock() { fMix.snap(1.0f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fInput.prepare(sampleRate);
        fMix.prepare(sampleRate);
        for (DelayLine& line : fLines)
            line.prepare(static_cast<size_t>(std::ceil(kMaxWindowMs / 1000.0 * sampleRate)) + 8);
        fMaxWindow = static_cast<float>(kMaxWindowMs / 1000.0 * sampleRate);
        fReturn = 1.0 - EnvelopeFollower::coef(sampleRate, 50.0);
        updateSpeed();
        reset();
    }

    bool configure(const nlohmann::json& config) override
    {
        fInput.configure(config);
        return true;
    }

    uint32_t latency() const override { return fInput.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override { fInput.setRig(input, numChannels); }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kHardTuneParamKey:    fKey = std::clamp(static_cast<int>(value), 0, 11); break;
        case kHardTuneParamScale:  fScale = std::clamp(static_cast<int>(value), 0, kTuneScaleCount - 1); break;
        case kHardTuneParamSpeed:
            fSpeedMs = value;
            updateSpeed();
            break;
        case kHardTuneParamAmount: fAmount = value / 100.0f; break;
        case kHardTuneParamMix:    fMix.setTarget(value / 100.0f); break;
        case kHardTuneParamWindow: fWindowMs = std::min<float>(value, kMaxWindowMs); break;
        case kHardTuneParamTrack:  fInput.setTrackRig(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        const float baseWindow = std::min(fMaxWindow, static_cast<float>(fWindowMs / 1000.0 * fSampleRate));
        for (uint32_t i = 0; i < frames; ++i)
        {
            const NoteTracker::Note& note = fInput.process(io, numChannels, i);
            if (fInput.poly() && fInput.frameReady())
                fChordError = chordError(fInput.frame());
            if (note.on)
            {
                double error;
                if (fInput.poly())
                {
                    error = fChordError;
                }
                else
                {
                    const double played = hzToMidi(note.hz);
                    error = nearestNote(played) - played;
                }
                fCorrection += (error * fAmount - fCorrection) * fSpeedCoef;
            }
            else
            {
                fCorrection += (0.0 - fCorrection) * fReturn;
            }
            const double ratio = semitonesToRatio(fCorrection);
            const float window = pitchSyncWindow(baseWindow, note.hz > 0.0 ? fSampleRate / note.hz : 0.0, fMaxWindow);
            const float mix = fMix.next();
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                fLines[ch].write(x);
                const float tuned = fVoices[ch].read(fLines[ch], ratio, window);
                io[ch][i] = x + (tuned - x) * mix;
            }
        }
    }

    void reset() override
    {
        fInput.reset();
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].reset();
            fVoices[ch].reset(fWindowMs / 1000.0f * static_cast<float>(fSampleRate));
        }
        fCorrection = 0.0;
        fChordError = 0.0;
        fSnap = true;
    }

    // Poly: one shift for the whole chord (a pitch shifter moves every note together): how far its notes are from
    // the nearest notes of the key and scale, on average, the louder notes counting more
    double chordError(const TrackedInput::Frame& f) const
    {
        double sum = 0.0, weights = 0.0;
        for (int b = 0; b < TrackedInput::kBands; ++b)
        {
            const auto k = static_cast<size_t>(b);
            if (!f.sounding[k])
                continue;
            const double hz = fInput.bandHz(b);
            if (hz <= 0.0)
                continue;
            const double played = hzToMidi(hz);
            const double weight = f.level[k];
            sum += (nearestNote(played) - played) * weight;
            weights += weight;
        }
        return weights > 0.0 ? sum / weights : 0.0;
    }

    // The nearest note of the key and scale to a played pitch (MIDI, fractional)
    double nearestNote(double midi) const
    {
        const uint16_t mask = kTuneScales[fScale];
        const auto base = static_cast<int>(std::floor(midi));
        double best = midi;
        double distance = 1e9;
        for (int n = base - 6; n <= base + 7; ++n)
        {
            const int pc = ((n - fKey) % 12 + 12) % 12;
            if (!(mask & (1u << pc)))
                continue;
            const double d = std::fabs(n - midi);
            if (d < distance)
            {
                distance = d;
                best = n;
            }
        }
        return best;
    }

private:
    void updateSpeed() { fSpeedCoef = fSpeedMs < 0.5f ? 1.0 : 1.0 - EnvelopeFollower::coef(fSampleRate, fSpeedMs); }

    double fSampleRate = 48000.0;
    TrackedInput fInput;
    int fKey = 0, fScale = 0;
    float fSpeedMs = 10.0f, fAmount = 1.0f, fWindowMs = 25.0f;
    double fSpeedCoef = 1.0, fReturn = 0.001;
    double fCorrection = 0.0;
    double fChordError = 0.0;
    Smoother fMix;
    DelayLine fLines[2];
    PitchVoice fVoices[2];
    float fMaxWindow = 4800.0f;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kDrumModeNames[] = { "Auto", "Kick", "Snare", "Hi-Hat", "Alternate" };
constexpr double kListenMs = 6.0;

class DrumTriggerBlock : public Block {
public:
    DrumTriggerBlock() { fDry.snap(0.5f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fDry.prepare(sampleRate);
        fOnset.prepare(sampleRate);
        fListen = static_cast<int>(kListenMs / 1000.0 * sampleRate);
        fLow.setLowPass(sampleRate, 300.0);
        fHigh.setLowPass(sampleRate, 2000.0);
        updateSensitivity();
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kDrumParamMode:        fMode = std::clamp(static_cast<int>(value), 0, 4); break;
        case kDrumParamSplit:       fSplit = value / 100.0f; break;
        case kDrumParamSensitivity:
            fSensitivity = value / 100.0f;
            updateSensitivity();
            break;
        case kDrumParamKick:        fLevels[0] = value / 100.0f; break;
        case kDrumParamSnare:       fLevels[1] = value / 100.0f; break;
        case kDrumParamHat:         fLevels[2] = value / 100.0f; break;
        case kDrumParamTune:        fTune = std::exp2((value - 50.0f) / 50.0f); break;
        case kDrumParamDecay:       fDecay = std::exp2((value - 50.0f) / 25.0f); break;
        case kDrumParamDynamics:    fDynamics = value / 100.0f; break;
        case kDrumParamDry:         fDry.setTarget(value / 100.0f); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fDry.snap(fDry.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float x = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
            const float low = fLow.lowPass(x);
            const float high = x - fHigh.lowPass(x);
            if (fOnset.process(std::fabs(x)) && fListening == 0)
            {
                fListening = fListen;
                fLowEnergy = fHighEnergy = fPeak = 0.0f;
            }
            if (fListening > 0)
            {
                // how bright the hit is: its energy over 2 kHz against under 300 Hz, in its first few ms
                fLowEnergy += low * low;
                fHighEnergy += high * high;
                fPeak = std::max(fPeak, std::fabs(x));
                if (--fListening == 0)
                    hit();
            }
            const float drums = renderKick() * fLevels[0] + renderSnare() * fLevels[1] + renderHat() * fLevels[2];
            const float dry = fDry.next();
            for (uint32_t ch = 0; ch < numChannels; ++ch)
                io[ch][i] = io[ch][i] * dry + drums;
        }
    }

    void reset() override
    {
        fOnset.reset();
        fLow.reset();
        fHigh.reset();
        fListening = 0;
        fKick = Kick {};
        fSnare = Snare {};
        fHat = Hat {};
        fAlternate = false;
        fSnap = true;
    }

private:
    struct Kick {
        double phase = 0.0;
        double hz = 0.0, low = 48.0, sweep = 0.0, sweepDecay = 0.0;
        float amp = 0.0f, decay = 0.0f, click = 0.0f, clickDecay = 0.0f;
    };
    struct Snare {
        double phase1 = 0.0, phase2 = 0.0, hz1 = 185.0, hz2 = 330.0;
        float tone = 0.0f, toneDecay = 0.0f, noise = 0.0f, noiseDecay = 0.0f;
        Svf band;
    };
    struct Hat {
        float amp = 0.0f, decay = 0.0f;
        Svf high;
    };

    void updateSensitivity() { fOnset.setThreshold(std::pow(10.0f, (-60.0f + 40.0f * (1.0f - fSensitivity)) / 20.0f)); }

    float noise()
    {
        fNoise ^= fNoise << 13;
        fNoise ^= fNoise >> 17;
        fNoise ^= fNoise << 5;
        return static_cast<float>(fNoise) / 2147483648.0f - 1.0f;
    }

    float decayPerSample(double seconds) const
    {
        return static_cast<float>(std::exp(-1.0 / (std::max(0.002, seconds) * fSampleRate)));
    }

    void hit()
    {
        const float brightness = fHighEnergy / std::max(1e-12f, fLowEnergy + fHighEnergy);
        const float threshold = 0.05f + 0.6f * fSplit;
        int drum = 0;
        switch (fMode)
        {
        case 1: drum = 0; break;
        case 2: drum = 1; break;
        case 3: drum = 2; break;
        case 4:
            drum = fAlternate ? 1 : 0;
            fAlternate = !fAlternate;
            break;
        default:
            drum = brightness < threshold ? 0 : (brightness < std::min(0.95f, threshold + 0.3f) ? 1 : 2);
            break;
        }
        // louder hits, louder drums (as much as Dynamics asks)
        const float strength = std::clamp(fPeak * 3.0f, 0.05f, 1.0f);
        const float velocity = 1.0f - fDynamics + fDynamics * strength;
        if (drum == 0)
        {
            fKick.phase = 0.0;
            fKick.low = 48.0 * fTune;
            fKick.sweep = 102.0 * fTune;
            fKick.sweepDecay = decayPerSample(0.03);
            fKick.amp = velocity;
            fKick.decay = decayPerSample(0.25 * fDecay);
            fKick.click = 0.35f * velocity;
            fKick.clickDecay = decayPerSample(0.002);
        }
        else if (drum == 1)
        {
            fSnare.hz1 = 185.0 * fTune;
            fSnare.hz2 = 330.0 * fTune;
            fSnare.tone = 0.5f * velocity;
            fSnare.toneDecay = decayPerSample(0.05 * fDecay);
            fSnare.noise = 0.8f * velocity;
            fSnare.noiseDecay = decayPerSample(0.15 * fDecay);
            fSnare.band.set(fSampleRate, std::min(3500.0 * fTune, 0.45 * fSampleRate), 0.8);
        }
        else
        {
            fHat.amp = 0.5f * velocity;
            fHat.decay = decayPerSample(0.04 * fDecay);
            fHat.high.set(fSampleRate, std::min(7000.0 * fTune, 0.45 * fSampleRate), 0.7);
        }
    }

    float renderKick()
    {
        if (fKick.amp < 1e-5f && fKick.click < 1e-5f)
            return 0.0f;
        fKick.sweep *= fKick.sweepDecay;
        advance(fKick.phase, (fKick.low + fKick.sweep) / fSampleRate);
        const float y = static_cast<float>(std::sin(2.0 * M_PI * fKick.phase)) * fKick.amp + noise() * fKick.click;
        fKick.amp *= fKick.decay;
        fKick.click *= fKick.clickDecay;
        return y;
    }

    float renderSnare()
    {
        if (fSnare.tone < 1e-5f && fSnare.noise < 1e-5f)
            return 0.0f;
        advance(fSnare.phase1, fSnare.hz1 / fSampleRate);
        advance(fSnare.phase2, fSnare.hz2 / fSampleRate);
        const float tones = static_cast<float>(std::sin(2.0 * M_PI * fSnare.phase1) + 0.6 * std::sin(2.0 * M_PI * fSnare.phase2));
        const float y = tones * fSnare.tone + fSnare.band.process(noise()).band * 2.0f * fSnare.noise;
        fSnare.tone *= fSnare.toneDecay;
        fSnare.noise *= fSnare.noiseDecay;
        return y;
    }

    float renderHat()
    {
        if (fHat.amp < 1e-5f)
            return 0.0f;
        const float y = fHat.high.process(noise()).high * fHat.amp;
        fHat.amp *= fHat.decay;
        return y;
    }

    static void advance(double& phase, double dt)
    {
        phase += dt;
        phase -= std::floor(phase);
    }

    double fSampleRate = 48000.0;
    int fMode = 0;
    float fSplit = 0.5f, fSensitivity = 0.6f;
    std::array<float, 3> fLevels { 0.8f, 0.8f, 0.6f };
    double fTune = 1.0, fDecay = 1.0;
    float fDynamics = 0.6f;
    Smoother fDry;
    OnsetDetector fOnset;
    OnePole fLow, fHigh;
    int fListen = 288, fListening = 0;
    float fLowEnergy = 0.0f, fHighEnergy = 0.0f, fPeak = 0.0f;
    bool fAlternate = false;
    Kick fKick;
    Snare fSnare;
    Hat fHat;
    uint32_t fNoise = 0x2545f491u;
    bool fSnap = true;
};

} // namespace

int strumChordNote(int chord, int key, int scale, int playedMidi, int index)
{
    const int c = std::clamp(chord, 0, kStrumChordCount - 1);
    const int v = std::clamp(index, 0, kMaxStrumVoices - 1);
    if (c == 0)
        return harmonySemitones(playedMidi, key, scale, kDiatonicSteps[v]);
    return kStrumChords[c][v];
}

const BlockDescriptor kStrumBlockDescriptor = {
    "strum",
    "Strum Machine",
    Category::Pitch,
    kStrumParamCount,
    {
        ParamSpec { "Chord", "", 0.0f, static_cast<float>(kStrumChordCount - 1), 0.0f, ParamScale::Choice, kStrumChordNames },
        ParamSpec { "Key", "", 0.0f, 11.0f, 0.0f, ParamScale::Choice, kKeyNames },
        ParamSpec { "Scale", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kStrumScaleNames },
        ParamSpec { "Notes", "", 0.0f, 2.0f, 1.0f, ParamScale::Choice, kStrumNoteNames },
        ParamSpec { "Strum", "ms", 0.0f, 80.0f, 25.0f, ParamScale::Linear },
        ParamSpec { "Direction", "", 0.0f, 2.0f, 0.0f, ParamScale::Choice, kStrumDirectionNames },
        ParamSpec { "Level", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 40.0f, ParamScale::Log },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new StrumBlock()); },
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

const BlockDescriptor kHardTuneBlockDescriptor = {
    "hardtune",
    "Hard Tune",
    Category::Pitch,
    kHardTuneParamCount,
    {
        ParamSpec { "Key", "", 0.0f, 11.0f, 0.0f, ParamScale::Choice, kKeyNames },
        ParamSpec { "Scale", "", 0.0f, static_cast<float>(kTuneScaleCount - 1), 1.0f, ParamScale::Choice, kTuneScaleNames },
        ParamSpec { "Speed", "ms", 0.0f, 300.0f, 10.0f, ParamScale::Linear },
        ParamSpec { "Amount", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 10.0f, 60.0f, 25.0f, ParamScale::Log },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new HardTuneBlock()); },
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

const BlockDescriptor kDrumTriggerBlockDescriptor = {
    "drumtrigger",
    "Drum Trigger",
    Category::Synth,
    kDrumParamCount,
    {
        ParamSpec { "Mode", "", 0.0f, 4.0f, 0.0f, ParamScale::Choice, kDrumModeNames },
        ParamSpec { "Split", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Sensitivity", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Kick", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Snare", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Hi-Hat", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Tune", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Decay", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Dynamics", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new DrumTriggerBlock()); },
};

} // namespace xythos
