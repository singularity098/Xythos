// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/PitchBlocks.hpp"

#include "engine/TempoSync.hpp"
#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/DelayLine.hpp"
#include "engine/dsp/Lfo.hpp"
#include "engine/dsp/OnsetDetector.hpp"
#include "engine/dsp/PeriodFinder.hpp"
#include "engine/dsp/PitchShifter.hpp"
#include "engine/dsp/PitchTracker.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/SpectralShifter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace xythos {

namespace {

constexpr double kMaxWindowMs = 120.0;

// What the pitch blocks share: a delay line per channel and N voices per channel reading it, a period finder and a
// pitch tracker on the mono input (so windows can be pitch-synchronous), a tone filter per channel, the window.
//
// HQ mode (block config "hq": true; BlockDescriptor::hq shows the panel's Quality button): each voice is a spectral
// shifter (dsp/SpectralShifter.hpp) instead, and the dry signal is delayed to match; the block reports that latency.
// Blocks process in chunks of kChunk samples: beginChunk() runs the HQ voices over the chunk, then per sample dryIn()
// gives the dry sample (delayed in HQ; in the standard mode it also feeds the delay line) and voice() a voice's
// output. In HQ a voice's ratio takes effect from the next chunk (0.7 ms later). Voices a block marks inactive
// (setVoiceActive(): level at zero) are skipped in HQ, which saves their CPU; they start afresh when turned up.
template <int N>
class PitchBase : public Block {
public:
    static constexpr uint32_t kChunk = 32;

    PitchBase()
    {
        std::fill(std::begin(fHqActive), std::end(fHqActive), true);
        std::fill(std::begin(fHqRatio), std::end(fHqRatio), 1.0);
    }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        for (DelayLine& line : fLines)
            line.prepare(static_cast<size_t>(std::ceil(kMaxWindowMs / 1000.0 * sampleRate)) + 8);
        fMaxWindow = static_cast<float>(kMaxWindowMs / 1000.0 * sampleRate);
        fTracker.prepare(sampleRate);
        fFinder.prepare(sampleRate);
        if (fHq)
            prepareHq();
        updateWindow();
        updateTone();
        reset();
    }

    bool configure(const nlohmann::json& config) override
    {
        configureHq(config);
        return true;
    }

    uint32_t latency() const override { return hqLatency(); }

    void reset() override
    {
        fTracker.reset();
        fFinder.reset();
        for (int ch = 0; ch < 2; ++ch)
        {
            fLines[ch].reset();
            fTone[ch].reset();
            fHqDry[ch].reset();
            for (int v = 0; v < N; ++v)
                fVoices[ch][v].reset(fWindow, 0.25 * v); // voices start at different points, so they don't jump together
        }
        for (int v = 0; v < N; ++v)
        {
            fHqVoices[v].reset();
            fHqIdle[v] = false;
        }
    }

protected:
    // Non-RT: reads "hq" from a block config (prepare() has already run)
    void configureHq(const nlohmann::json& config)
    {
        fHq = hqFromConfig(config);
        if (fHq)
            prepareHq();
        reset();
    }

    uint32_t hqLatency() const { return fHq ? fHqVoices[0].latency() : 0; }
    // where a ratio set now takes effect, in whole chunks behind the input
    uint32_t hqRatioDelayChunks() const { return fHq ? (fHqVoices[0].inputLatency() + kChunk / 2) / kChunk : 0; }
    bool hq() const { return fHq; }

    void setVoiceActive(int v, bool active) { fHqActive[v] = active; }

    // RT: call at the start of each chunk, io[..][start, start + n), n <= kChunk
    void beginChunk(float* const* io, uint32_t numChannels, uint32_t start, uint32_t n)
    {
        fChunkStart = start;
        if (!fHq)
            return;
        const uint32_t delay = hqLatency() + 1; // read(1) is the sample just written
        const float* in[2];
        for (uint32_t ch = 0; ch < numChannels; ++ch)
        {
            for (uint32_t i = 0; i < n; ++i)
            {
                fHqIn[ch][i] = io[ch][start + i];
                fHqDry[ch].write(fHqIn[ch][i]);
                fHqDryOut[ch][i] = fHqDry[ch].read(delay);
            }
            in[ch] = fHqIn[ch];
        }
        for (int v = 0; v < N; ++v)
        {
            float* out[2] = { fHqWet[v][0], fHqWet[v][1] };
            if (!fHqActive[v])
            {
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    std::fill(out[ch], out[ch] + n, 0.0f);
                fHqIdle[v] = true;
                continue;
            }
            if (fHqIdle[v])
            {
                fHqVoices[v].reset();
                fHqIdle[v] = false;
            }
            fHqVoices[v].setRatio(fHqRatio[v]);
            fHqVoices[v].process(in, out, numChannels, n);
        }
    }

    // RT: the dry sample at buffer position i (delayed by the latency in HQ); call once per channel and sample
    float dryIn(float* const* io, uint32_t ch, uint32_t i)
    {
        if (fHq)
            return fHqDryOut[ch][i - fChunkStart];
        const float x = io[ch][i];
        fLines[ch].write(x);
        return x;
    }

    // RT: voice v's output at buffer position i, at `ratio`; after dryIn() for this channel and sample
    float voice(uint32_t ch, int v, uint32_t i, double ratio, float window)
    {
        if (fHq)
        {
            fHqRatio[v] = ratio;
            return fHqWet[v][ch][i - fChunkStart];
        }
        return fVoices[ch][v].read(fLines[ch], ratio, window);
    }

    void setWindowMs(float ms)
    {
        fWindowMs = std::min<float>(ms, kMaxWindowMs);
        updateWindow();
    }

    void setToneHz(float hz)
    {
        fToneHz = hz;
        updateTone();
    }

    // Follows the input (mono) and returns the window for this sample: a whole number of the signal's repetition
    // period when it has a clear one (a note, or a chord's common period), else of the tracked note, else the Window
    // setting
    float track(float in)
    {
        if (fHq)
            return fWindow; // no splices to line up
        fTracker.process(in);
        fFinder.process(in);
        double period = fFinder.period();
        if (period <= 0.0 && fTracker.sounding() && fTracker.hz() > 0.0)
            period = fSampleRate / fTracker.hz();
        return pitchSyncWindow(fWindow, period, fMaxWindow);
    }

    float tone(uint32_t ch, float x) { return fToneOn ? fTone[ch].process(x) : x; }

    double fSampleRate = 48000.0;
    DelayLine fLines[2];
    PitchVoice fVoices[2][N];

private:
    void prepareHq()
    {
        for (SpectralShifter& shifter : fHqVoices)
            shifter.prepare(fSampleRate);
        for (DelayLine& line : fHqDry)
            line.prepare(fHqVoices[0].latency() + 8);
    }

    void updateWindow() { fWindow = std::min(static_cast<float>(fWindowMs / 1000.0 * fSampleRate), fMaxWindow); }

    void updateTone()
    {
        fToneOn = fToneHz < 19900.0f;
        for (Biquad& filter : fTone)
            filter.setLowPass(fSampleRate, std::min<double>(fToneHz, 0.45 * fSampleRate));
    }

    float fWindowMs = 45.0f;
    float fWindow = 2160.0f;
    float fMaxWindow = 5760.0f;
    float fToneHz = 20000.0f;
    bool fToneOn = false;
    Biquad fTone[2];
    PitchTracker fTracker;
    PeriodFinder fFinder;

    bool fHq = false;
    SpectralShifter fHqVoices[N];
    DelayLine fHqDry[2];
    float fHqIn[2][kChunk] {};
    float fHqDryOut[2][kChunk] {};
    float fHqWet[N][2][kChunk] {};
    double fHqRatio[N] {};
    bool fHqActive[N] {};
    bool fHqIdle[N] {};
    uint32_t fChunkStart = 0;
};

// ---------------------------------------------------------------------------------------------------------------------

class PitchShifterBlock : public PitchBase<2> {
public:
    PitchShifterBlock()
    {
        fLevel[0].snap(1.0f);
        fLevel[1].snap(0.0f);
        fDry.snap(1.0f);
    }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        for (Smoother* s : { &fLevel[0], &fLevel[1], &fDry })
            s->prepare(sampleRate);
        PitchBase::prepare(sampleRate, maxFrames);
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kPitchParamShift1: fShift[0] = std::round(value); break;
        case kPitchParamLevel1: fLevel[0].setTarget(value / 100.0f); break;
        case kPitchParamShift2: fShift[1] = std::round(value); break;
        case kPitchParamLevel2: fLevel[1].setTarget(value / 100.0f); break;
        case kPitchParamDetune: fDetune = value; break;
        case kPitchParamDry:    fDry.setTarget(value / 100.0f); break;
        case kPitchParamWindow: setWindowMs(value); break;
        case kPitchParamTone:   setToneHz(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            for (Smoother* s : { &fLevel[0], &fLevel[1], &fDry })
                s->snap(s->target());
        }
        const double ratios[2] = { semitonesToRatio(fShift[0] + fDetune / 100.0),
                                   semitonesToRatio(fShift[1] - fDetune / 100.0) };
        for (uint32_t start = 0; start < frames; start += kChunk)
        {
            const uint32_t n = std::min(kChunk, frames - start);
            for (int v = 0; v < 2; ++v)
                setVoiceActive(v, fLevel[v].target() > 0.0f || fLevel[v].value() > 1e-4f);
            beginChunk(io, numChannels, start, n);
            for (uint32_t i = start; i < start + n; ++i)
            {
                const float levels[2] = { fLevel[0].next(), fLevel[1].next() };
                const float dry = fDry.next();
                const float window = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = dryIn(io, ch, i);
                    float wet = 0.0f;
                    for (int v = 0; v < 2; ++v)
                    {
                        const float y = voice(ch, v, i, ratios[v], window); // keep each voice moving
                        wet += y * levels[v];
                    }
                    io[ch][i] = x * dry + tone(ch, wet);
                }
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fSnap = true;
    }

private:
    float fShift[2] = { 12.0f, 7.0f };
    float fDetune = 0.0f;
    Smoother fLevel[2], fDry;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kIntervalNames[] = { "+2 Oct", "+1 Oct", "+5th", "+4th", "+2nd", "-2nd",
                                           "-4th",   "-5th",   "-1 Oct", "-2 Oct", "Dive" };
constexpr double kIntervals[] = { 24.0, 12.0, 7.0, 5.0, 2.0, -2.0, -5.0, -7.0, -12.0, -24.0, -36.0 };
constexpr int kIntervalCount = sizeof(kIntervals) / sizeof(kIntervals[0]);
constexpr const char* kWhammyModeNames[] = { "Whammy", "Harmony" };

class WhammyBlock : public PitchBase<1> {
public:
    WhammyBlock() { fPosition.snap(0.0f); }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fPosition.prepare(sampleRate, 0.02);
        PitchBase::prepare(sampleRate, maxFrames);
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kWhammyParamPosition: fPosition.setTarget(value / 100.0f); break;
        case kWhammyParamInterval:
            fInterval = kIntervals[std::clamp(static_cast<int>(value), 0, kIntervalCount - 1)];
            break;
        case kWhammyParamMode:   fHarmony = value >= 0.5f; break;
        case kWhammyParamWindow: setWindowMs(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fPosition.snap(fPosition.target());
        }
        for (uint32_t start = 0; start < frames; start += kChunk)
        {
            const uint32_t n = std::min(kChunk, frames - start);
            beginChunk(io, numChannels, start, n);
            for (uint32_t i = start; i < start + n; ++i)
            {
                const double ratio = semitonesToRatio(fInterval * fPosition.next());
                const float window = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = dryIn(io, ch, i);
                    const float wet = voice(ch, 0, i, ratio, window);
                    io[ch][i] = fHarmony ? 0.7071f * (x + wet) : wet;
                }
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fSnap = true;
    }

private:
    double fInterval = 12.0;
    bool fHarmony = false;
    Smoother fPosition;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr double kPolyRatios[4] = { 0.25, 0.5, 2.0, 4.0 };

class PolyOctaveBlock : public PitchBase<4> {
public:
    PolyOctaveBlock()
    {
        fLevel[0].snap(0.0f);
        fLevel[1].snap(0.7f);
        fLevel[2].snap(0.0f);
        fLevel[3].snap(0.0f);
        fDry.snap(1.0f);
    }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        for (Smoother& s : fLevel)
            s.prepare(sampleRate);
        fDry.prepare(sampleRate);
        PitchBase::prepare(sampleRate, maxFrames);
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kPolyOctParamSub2:   fLevel[0].setTarget(value / 100.0f); break;
        case kPolyOctParamSub1:   fLevel[1].setTarget(value / 100.0f); break;
        case kPolyOctParamUp1:    fLevel[2].setTarget(value / 100.0f); break;
        case kPolyOctParamUp2:    fLevel[3].setTarget(value / 100.0f); break;
        case kPolyOctParamDry:    fDry.setTarget(value / 100.0f); break;
        case kPolyOctParamTone:   setToneHz(value); break;
        case kPolyOctParamWindow: setWindowMs(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            for (Smoother& s : fLevel)
                s.snap(s.target());
            fDry.snap(fDry.target());
        }
        for (uint32_t start = 0; start < frames; start += kChunk)
        {
            const uint32_t n = std::min(kChunk, frames - start);
            for (int v = 0; v < 4; ++v)
                setVoiceActive(v, fLevel[v].target() > 0.0f || fLevel[v].value() > 1e-4f);
            beginChunk(io, numChannels, start, n);
            for (uint32_t i = start; i < start + n; ++i)
            {
                float levels[4];
                for (int v = 0; v < 4; ++v)
                    levels[v] = fLevel[v].next();
                const float dry = fDry.next();
                const float window = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = dryIn(io, ch, i);
                    float wet = 0.0f;
                    for (int v = 0; v < 4; ++v)
                    {
                        const float y = voice(ch, v, i, kPolyRatios[v], window);
                        wet += y * levels[v];
                    }
                    io[ch][i] = x * dry + tone(ch, wet);
                }
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fSnap = true;
    }

private:
    Smoother fLevel[4], fDry;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kKeyNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
constexpr const char* kScaleNames[] = { "Major", "Minor", "Dorian", "Mixolydian", "Lydian", "Phrygian", "Harm. Minor" };
constexpr int kScales[][7] = {
    { 0, 2, 4, 5, 7, 9, 11 }, // major
    { 0, 2, 3, 5, 7, 8, 10 }, // natural minor
    { 0, 2, 3, 5, 7, 9, 10 }, // dorian
    { 0, 2, 4, 5, 7, 9, 10 }, // mixolydian
    { 0, 2, 4, 6, 7, 9, 11 }, // lydian
    { 0, 1, 3, 5, 7, 8, 10 }, // phrygian
    { 0, 2, 3, 5, 7, 8, 11 }, // harmonic minor
};
constexpr int kScaleCount = sizeof(kScales) / sizeof(kScales[0]);
constexpr const char* kVoiceNames[] = { "-Oct", "-7th", "-6th", "-5th", "-4th", "-3rd", "-2nd", "Unison",
                                        "+2nd", "+3rd", "+4th", "+5th", "+6th", "+7th", "+Oct" };
constexpr int kVoiceCount = 15; // steps -7 .. +7

class HarmonizerBlock : public PitchBase<2> {
public:
    HarmonizerBlock()
    {
        fLevel[0].snap(1.0f);
        fLevel[1].snap(0.0f);
        fDry.snap(1.0f);
    }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        for (Smoother* s : { &fLevel[0], &fLevel[1], &fDry })
            s->prepare(sampleRate);
        for (Smoother& s : fShift)
            s.prepare(sampleRate, 0.015);
        fInput.prepare(sampleRate);
        PitchBase::prepare(sampleRate, maxFrames);
        resizeHistory();
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        const int choice = static_cast<int>(value);
        switch (index)
        {
        case kHarmParamKey:    fKey = std::clamp(choice, 0, 11); break;
        case kHarmParamScale:  fScale = std::clamp(choice, 0, kScaleCount - 1); break;
        case kHarmParamVoice1: fSteps[0] = std::clamp(choice, 0, kVoiceCount - 1) - 7; break;
        case kHarmParamLevel1: fLevel[0].setTarget(value / 100.0f); break;
        case kHarmParamVoice2: fSteps[1] = std::clamp(choice, 0, kVoiceCount - 1) - 7; break;
        case kHarmParamLevel2: fLevel[1].setTarget(value / 100.0f); break;
        case kHarmParamDry:    fDry.setTarget(value / 100.0f); break;
        case kHarmParamWindow: setWindowMs(value); break;
        case kHarmParamTrack:  fInput.setTrackRig(value); break;
        }
    }

    bool configure(const nlohmann::json& config) override
    {
        fInput.configure(config);
        configureHq(config);
        resizeHistory();
        return true;
    }

    uint32_t latency() const override { return fInput.latency() + hqLatency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override { fInput.setRig(input, numChannels); }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            for (Smoother* s : { &fLevel[0], &fLevel[1], &fDry })
                s->snap(s->target());
        }
        for (uint32_t start = 0; start < frames; start += kChunk)
        {
            const uint32_t n = std::min(kChunk, frames - start);
            // first the played notes over the chunk (io[..][i] is delayed by the lookahead from here on), so the HQ
            // voices get the delayed input
            for (uint32_t i = start; i < start + n; ++i)
            {
                const NoteTracker::Note& played = fInput.process(io, numChannels, i);
                fWindows[i - start] = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
                if (played.on)
                {
                    // the played note, to the nearest semitone; the harmony is relative to it, so bends carry over
                    const int note = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(played.hz / 440.0)));
                    for (int v = 0; v < 2; ++v)
                        fShift[v].setTarget(static_cast<float>(harmonySemitones(note, fKey, fScale, fSteps[v])));
                }
                for (int v = 0; v < 2; ++v)
                    fShifts[v][i - start] = fShift[v].next();
            }
            const auto& delayed = advanceHistory(fShifts[0][n - 1], fShifts[1][n - 1]);
            for (int v = 0; v < 2; ++v)
                setVoiceActive(v, fLevel[v].target() > 0.0f || fLevel[v].value() > 1e-4f);
            beginChunk(io, numChannels, start, n);
            for (uint32_t i = start; i < start + n; ++i)
            {
                double ratios[2];
                for (int v = 0; v < 2; ++v)
                    ratios[v] = semitonesToRatio(hq() ? delayed[v] : fShifts[v][i - start]);
                const float levels[2] = { fLevel[0].next(), fLevel[1].next() };
                const float dry = fDry.next();
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = dryIn(io, ch, i);
                    float wet = 0.0f;
                    for (int v = 0; v < 2; ++v)
                        wet += voice(ch, v, i, ratios[v], fWindows[i - start]) * levels[v];
                    io[ch][i] = x * dry + wet;
                }
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fInput.reset();
        std::fill(fShiftHistory.begin(), fShiftHistory.end(), std::array<float, 2> { 0.0f, 0.0f });
        fHistoryPos = 0;
        fSnap = true;
    }

private:
    // Non-RT. HQ: the shifts wait until the shifter's processing point reaches the audio they were found in
    void resizeHistory()
    {
        fShiftHistory.assign(hqRatioDelayChunks() + 1, { 0.0f, 0.0f });
        fHistoryPos = 0;
    }

    // Pushes this chunk's shifts and returns those from hqRatioDelayChunks() chunks ago
    const std::array<float, 2>& advanceHistory(float shift0, float shift1)
    {
        fShiftHistory[fHistoryPos] = { shift0, shift1 };
        fHistoryPos = (fHistoryPos + 1) % fShiftHistory.size();
        return fShiftHistory[fHistoryPos]; // the oldest
    }

    TrackedInput fInput;
    float fWindows[kChunk] {};
    float fShifts[2][kChunk] {};
    std::vector<std::array<float, 2>> fShiftHistory = std::vector<std::array<float, 2>>(1);
    size_t fHistoryPos = 0;
    int fKey = 0;
    int fScale = 0;
    int fSteps[2] = { 2, 4 };
    Smoother fShift[2];
    Smoother fLevel[2], fDry;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kDetuneVoiceNames[] = { "Both", "Up", "Down" };

class DetuneBlock : public PitchBase<2> {
public:
    DetuneBlock() { fMix.snap(0.5f); }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fMix.prepare(sampleRate);
        PitchBase::prepare(sampleRate, maxFrames);
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kDetuneParamDetune: fCents = value; break;
        case kDetuneParamVoices: fVoiceMode = std::clamp(static_cast<int>(value), 0, 2); break;
        case kDetuneParamWidth:  fWidth = value / 100.0f; break;
        case kDetuneParamMix:    fMix.setTarget(value / 100.0f); break;
        case kDetuneParamWindow: setWindowMs(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        const double ratios[2] = { semitonesToRatio(fCents / 100.0), semitonesToRatio(-fCents / 100.0) };
        // stereo: the up voice leans left and the down voice right
        const float width = numChannels > 1 ? fWidth : 0.0f;
        const float near = 0.5f + 0.5f * width, far = 0.5f - 0.5f * width;
        setVoiceActive(0, fVoiceMode != 2);
        setVoiceActive(1, fVoiceMode != 1);
        for (uint32_t start = 0; start < frames; start += kChunk)
        {
            const uint32_t n = std::min(kChunk, frames - start);
            beginChunk(io, numChannels, start, n);
            for (uint32_t i = start; i < start + n; ++i)
            {
                const float window = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
                const float mix = fMix.next();
                const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix);
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = dryIn(io, ch, i);
                    const float up = voice(ch, 0, i, ratios[0], window);
                    const float down = voice(ch, 1, i, ratios[1], window);
                    float wet;
                    switch (fVoiceMode)
                    {
                    case 1:  wet = up; break;
                    case 2:  wet = down; break;
                    default: wet = ch == 0 ? up * near + down * far : down * near + up * far; break;
                    }
                    io[ch][i] = x * dryGain + wet * wetGain;
                }
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fSnap = true;
    }

private:
    float fCents = 10.0f;
    int fVoiceMode = 0;
    float fWidth = 0.8f;
    Smoother fMix;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

class CapoBlock : public PitchBase<1> {
public:
    CapoBlock() { fDry.snap(0.0f); }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fDry.prepare(sampleRate);
        PitchBase::prepare(sampleRate, maxFrames);
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kCapoParamShift:  fShift = std::round(value); break;
        case kCapoParamFine:   fFine = value; break;
        case kCapoParamDry:    fDry.setTarget(value / 100.0f); break;
        case kCapoParamWindow: setWindowMs(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fDry.snap(fDry.target());
        }
        const double ratio = semitonesToRatio(fShift + fFine / 100.0);
        for (uint32_t start = 0; start < frames; start += kChunk)
        {
            const uint32_t n = std::min(kChunk, frames - start);
            beginChunk(io, numChannels, start, n);
            for (uint32_t i = start; i < start + n; ++i)
            {
                const float window = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
                const float dry = fDry.next();
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    const float x = dryIn(io, ch, i);
                    io[ch][i] = x * dry + voice(ch, 0, i, ratio, window);
                }
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fSnap = true;
    }

private:
    float fShift = -1.0f;
    float fFine = 0.0f;
    Smoother fDry;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

// sub, octave, twelfth, two octaves (the fundamental is the input itself)
constexpr double kOrganRatios[4] = { 0.5, 2.0, 2.9966, 4.0 };
constexpr float kOrganScale = 0.6f; // the stops add up; keep the defaults near the dry level
constexpr double kOrganDuckMs = 4.0;

class OrganBlock : public PitchBase<4> {
public:
    // no HQ mode: its swell and steps follow the input as it arrives
    bool configure(const nlohmann::json& /*config*/) override { return true; }

    OrganBlock()
    {
        const float defaults[5] = { 0.5f, 0.8f, 0.6f, 0.0f, 0.3f };
        for (int k = 0; k < 5; ++k)
            fLevel[k].snap(defaults[k]);
        fDry.snap(0.0f);
    }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        for (Smoother& s : fLevel)
            s.prepare(sampleRate);
        fDry.prepare(sampleRate);
        fOnset.prepare(sampleRate);
        fDuckStep = static_cast<float>(1.0 / (kOrganDuckMs / 1000.0 * sampleRate));
        PitchBase::prepare(sampleRate, maxFrames);
        updateAttack();
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kOrganParamSub:     fLevel[0].setTarget(value / 100.0f); break;
        case kOrganParamFund:    fLevel[1].setTarget(value / 100.0f); break;
        case kOrganParamOctave:  fLevel[2].setTarget(value / 100.0f); break;
        case kOrganParamTwelfth: fLevel[3].setTarget(value / 100.0f); break;
        case kOrganParamTwoOct:  fLevel[4].setTarget(value / 100.0f); break;
        case kOrganParamAttack:
            fAttackMs = value;
            updateAttack();
            break;
        case kOrganParamTone:   setToneHz(value); break;
        case kOrganParamDry:    fDry.setTarget(value / 100.0f); break;
        case kOrganParamWindow: setWindowMs(value); break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            for (Smoother& s : fLevel)
                s.snap(s.target());
            fDry.snap(fDry.target());
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const float mono = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
            const float window = track(mono);
            // each new pick: duck out quickly, then swell in over Attack like a pressed key
            if (fOnset.process(std::fabs(mono)))
                fDucking = true;
            if (fDucking)
            {
                fEnv = std::max(0.0f, fEnv - fDuckStep);
                if (fEnv <= 0.0f)
                    fDucking = false;
            }
            else
            {
                fEnv = std::min(1.0f, fEnv + fAttackStep);
            }
            const float swell = fEnv * fEnv * (3.0f - 2.0f * fEnv);

            float levels[5];
            for (int k = 0; k < 5; ++k)
                levels[k] = fLevel[k].next();
            const float dry = fDry.next();
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                fLines[ch].write(x);
                float organ = x * levels[1];
                organ += fVoices[ch][0].read(fLines[ch], kOrganRatios[0], window) * levels[0];
                for (int v = 1; v < 4; ++v)
                    organ += fVoices[ch][v].read(fLines[ch], kOrganRatios[v], window) * levels[v + 1];
                io[ch][i] = x * dry + tone(ch, organ * kOrganScale) * swell;
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fOnset.reset();
        fEnv = 0.0f;
        fDucking = false;
        fSnap = true;
    }

private:
    void updateAttack() { fAttackStep = static_cast<float>(1.0 / (std::max(1.0f, fAttackMs) / 1000.0 * fSampleRate)); }

    Smoother fLevel[5], fDry;
    OnsetDetector fOnset;
    float fAttackMs = 40.0f;
    float fAttackStep = 0.0f, fDuckStep = 0.0f;
    float fEnv = 0.0f;
    bool fDucking = false;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kArpChordNames[] = { "Octaves", "Major", "Minor", "Power", "Sus4", "Major 7", "Minor 7", "Dom 7" };
constexpr int kArpChords[][4] = { { 0 },       { 0, 4, 7 },     { 0, 3, 7 },     { 0, 7 },
                                  { 0, 5, 7 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 }, { 0, 4, 7, 10 } };
constexpr int kArpChordSizes[] = { 1, 3, 3, 2, 3, 4, 4, 4 };
constexpr int kArpChordCount = sizeof(kArpChordSizes) / sizeof(kArpChordSizes[0]);
constexpr const char* kArpPatternNames[] = { "Up", "Down", "Up-Down", "Random" };
constexpr const char* kArpRangeNames[] = { "1 Oct", "2 Oct" };
constexpr double kArpGateMs = 3.0; // each step's edges, so they don't click

class ArpeggiatorBlock : public PitchBase<1> {
public:
    // no HQ mode: its swell and steps follow the input as it arrives
    bool configure(const nlohmann::json& /*config*/) override { return true; }

    ArpeggiatorBlock() { fMix.snap(1.0f); }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fMix.prepare(sampleRate);
        fGate.prepare(sampleRate, kArpGateMs / 1000.0);
        fLfo.prepare(sampleRate);
        PitchBase::prepare(sampleRate, maxFrames);
        updateRate();
        updateGlide();
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kArpParamChord:   fChord = std::clamp(static_cast<int>(value), 0, kArpChordCount - 1); break;
        case kArpParamPattern: fPattern = std::clamp(static_cast<int>(value), 0, 3); break;
        case kArpParamRange:   fOctaves = value >= 0.5f ? 2 : 1; break;
        case kArpParamRate:
            fRateHz = value;
            updateRate();
            break;
        case kArpParamSync:
            fSync = value >= 0.5f;
            updateRate();
            break;
        case kArpParamGate: fGateLength = value / 100.0f; break;
        case kArpParamGlide:
            fGlideMs = value;
            updateGlide();
            break;
        case kArpParamMix:    fMix.setTarget(value / 100.0f); break;
        case kArpParamWindow: setWindowMs(value); break;
        }
    }

    void setTempo(double bpm) override
    {
        if (bpm != fBpm)
        {
            fBpm = bpm;
            updateRate();
        }
    }

    void setSongPosition(double beats) override
    {
        // synced: each step starts on the beat, and the pattern counts from the song's start
        if (fSync)
        {
            const double cycle = rateNoteBeats(rateNoteIndex(kArpeggiatorBlockDescriptor.params[kArpParamRate].toNormalized(fRateHz)));
            const double phase = songPhase(beats, cycle);
            fLfo.syncTo(phase);
            // away from a step's edges, where the song and the running phase could disagree on which step it is
            if (phase > 0.05 && phase < 0.95)
                fStep = static_cast<long long>(std::floor(beats / cycle));
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
            fShift.snap(static_cast<float>(noteFor(fStep)));
        }
        for (uint32_t i = 0; i < frames; ++i)
        {
            const double previous = fLfo.phase();
            const double phase = fLfo.advance();
            if (phase < previous)
                ++fStep;
            fShift.setTarget(static_cast<float>(noteFor(fStep)));
            if (fGlideMs < 0.5f)
                fShift.snap(fShift.target());
            fGate.setTarget(phase < fGateLength ? 1.0f : 0.0f);

            const double ratio = semitonesToRatio(fShift.next());
            const float gate = fGate.next();
            const float mix = fMix.next();
            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix) * gate;
            const float window = track(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i]);
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                fLines[ch].write(x);
                io[ch][i] = x * dryGain + fVoices[ch][0].read(fLines[ch], ratio, window) * wetGain;
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fLfo.reset();
        fStep = 0;
        fRandomStep = -1;
        fGate.snap(0.0f);
        fSnap = true;
    }

private:
    // The interval for a step; Random picks a new note (never the same twice running) once per step
    int noteFor(long long step)
    {
        const int notes = arpNoteCount(fChord, fOctaves);
        int index;
        if (fPattern == 3)
        {
            if (step != fRandomStep)
            {
                fRandomStep = step;
                int pick = static_cast<int>(random() % static_cast<uint32_t>(notes));
                if (pick == fRandomIndex && notes > 1)
                    pick = (pick + 1) % notes;
                fRandomIndex = pick;
            }
            index = std::min(fRandomIndex, notes - 1);
        }
        else
        {
            index = arpPatternIndex(fPattern, step, notes);
        }
        return arpNote(fChord, fOctaves, index);
    }

    uint32_t random()
    {
        fRandom ^= fRandom << 13;
        fRandom ^= fRandom >> 17;
        fRandom ^= fRandom << 5;
        return fRandom;
    }

    void updateRate()
    {
        const ParamSpec& spec = kArpeggiatorBlockDescriptor.params[kArpParamRate];
        fLfo.setRate(fSync ? rateNoteHz(rateNoteIndex(spec.toNormalized(fRateHz)), fBpm) : fRateHz);
    }

    void updateGlide() { fShift.prepare(fSampleRate, std::max(0.5f, fGlideMs) / 3000.0); }

    double fBpm = 120.0;
    int fChord = 1;
    int fPattern = 0;
    int fOctaves = 1;
    float fRateHz = 8.0f;
    bool fSync = false;
    float fGateLength = 0.8f;
    float fGlideMs = 0.0f;
    Lfo fLfo;
    long long fStep = 0;
    long long fRandomStep = -1;
    int fRandomIndex = 0;
    uint32_t fRandom = 0x6c8e9cf5u;
    Smoother fShift, fGate, fMix;
    bool fSnap = true;
};

// ---------------------------------------------------------------------------------------------------------------------

constexpr const char* kSustainHarmonicNames[] = { "Octave", "Oct+5th", "2 Oct", "5th" };
constexpr double kSustainHarmonicRatios[] = { 2.0, 2.9966, 4.0, 1.4983 };
constexpr float kSustainMaxGainDb = 36.0f;
constexpr float kSustainFloor = 0.001f; // -60 dB: quieter than this, the note is let go

class SustainerBlock : public PitchBase<1> {
public:
    SustainerBlock() { fMix.snap(1.0f); }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fMix.prepare(sampleRate);
        fEnvelope.prepare(sampleRate, 2.0, 50.0);
        fOnset.prepare(sampleRate);
        fGainDown = EnvelopeFollower::coef(sampleRate, 1.0);
        fGainUp = EnvelopeFollower::coef(sampleRate, 30.0);
        fBloomSmooth = EnvelopeFollower::coef(sampleRate, 20.0);
        PitchBase::prepare(sampleRate, maxFrames);
        updateSustain();
        updateBloom();
        fSnap = true;
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kSustainParamSustain:
            fSustain = value / 100.0f;
            updateSustain();
            break;
        case kSustainParamBloom:
            fBloomMs = value;
            updateBloom();
            break;
        case kSustainParamHarmonic:
            fRatio = kSustainHarmonicRatios[std::clamp(static_cast<int>(value), 0, 3)];
            break;
        case kSustainParamFeedback: fFeedback = value / 100.0f; break;
        case kSustainParamTone:     setToneHz(value); break;
        case kSustainParamMix:      fMix.setTarget(value / 100.0f); break;
        }
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
            const float mono = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
            const float level = std::fabs(mono);
            const bool onset = fOnset.process(level);
            const float env = fEnvelope.process(level);

            // hold the level the note was picked at (it lets go slowly, over the Sustain time); a new pick starts again
            fTarget = std::max(env, onset ? 0.0f : fTarget * fTargetDecay);
            const float wanted = env > kSustainFloor ? std::min(fTarget / env, fMaxGain) : 1.0f;
            fGain = wanted + (fGain - wanted) * (wanted < fGain ? fGainDown : fGainUp);

            // the harmonic grows while the note is held
            if (onset)
                fBloom = 0.0f;
            else if (env > kSustainFloor)
                fBloom = std::min(1.0f, fBloom + fBloomStep);
            fBloomLevel = fBloom + (fBloomLevel - fBloom) * fBloomSmooth;
            const float harmonic = fBloomLevel * fFeedback;

            const float window = track(mono);
            const float mix = fMix.next();
            const float dryGain = dryGainForMix(mix), wetGain = wetGainForMix(mix);
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                const float x = io[ch][i];
                const float held = x * fGain;
                fLines[ch].write(held);
                const float up = fVoices[ch][0].read(fLines[ch], fRatio, window);
                const float y = tone(ch, held * (1.0f - 0.6f * harmonic) + up * harmonic);
                io[ch][i] = x * dryGain + y * wetGain;
            }
        }
    }

    void reset() override
    {
        PitchBase::reset();
        fEnvelope.reset();
        fOnset.reset();
        fTarget = 0.0f;
        fGain = 1.0f;
        fBloom = fBloomLevel = 0.0f;
        fSnap = true;
    }

private:
    void updateSustain()
    {
        fMaxGain = dbToGain(kSustainMaxGainDb * fSustain);
        // how long the held level takes to let go: half a second at 0%, about 20 s at 100%
        const double seconds = 0.5 + 20.0 * fSustain * fSustain;
        fTargetDecay = static_cast<float>(std::exp(-1.0 / (seconds * fSampleRate)));
    }

    void updateBloom() { fBloomStep = static_cast<float>(1.0 / (std::max(1.0f, fBloomMs) / 1000.0 * fSampleRate)); }

    float fSustain = 0.6f;
    float fBloomMs = 2000.0f;
    double fRatio = 2.0;
    float fFeedback = 0.7f;
    EnvelopeFollower fEnvelope;
    OnsetDetector fOnset;
    float fTarget = 0.0f, fTargetDecay = 1.0f, fMaxGain = 1.0f;
    float fGain = 1.0f, fGainDown = 0.0f, fGainUp = 0.0f;
    float fBloom = 0.0f, fBloomLevel = 0.0f, fBloomStep = 0.0f, fBloomSmooth = 0.0f;
    Smoother fMix;
    bool fSnap = true;
};

} // namespace

int harmonySemitones(int note, int key, int scale, int steps)
{
    const int* degrees = kScales[std::clamp(scale, 0, kScaleCount - 1)];
    const int pc = ((note - key) % 12 + 12) % 12;
    // the scale degree at or just below the note
    int degree = 0;
    for (int d = 0; d < 7; ++d)
        if (degrees[d] <= pc)
            degree = d;
    const int target = degree + steps;
    const int octaves = target >= 0 ? target / 7 : -((-target + 6) / 7);
    const int wrapped = target - octaves * 7;
    return degrees[wrapped] + 12 * octaves - degrees[degree];
}

int arpNoteCount(int chord, int octaves)
{
    return kArpChordSizes[std::clamp(chord, 0, kArpChordCount - 1)] * std::clamp(octaves, 1, 2) + 1;
}

int arpNote(int chord, int octaves, int index)
{
    const int c = std::clamp(chord, 0, kArpChordCount - 1);
    const int size = kArpChordSizes[c];
    const int i = std::clamp(index, 0, arpNoteCount(c, octaves) - 1);
    // the chord in each octave, then the top octave to finish
    return 12 * (i / size) + kArpChords[c][i % size];
}

int arpPatternIndex(int pattern, long long step, int notes)
{
    if (notes <= 1)
        return 0;
    const long long s = step < 0 ? 0 : step;
    switch (pattern)
    {
    case 1: return notes - 1 - static_cast<int>(s % notes);
    case 2: {
        const long long period = 2LL * notes - 2;
        const int k = static_cast<int>(s % period);
        return k < notes ? k : static_cast<int>(period) - k;
    }
    default: return static_cast<int>(s % notes);
    }
}

int whammyIntervalCount()
{
    return kIntervalCount;
}

double whammyIntervalSemitones(int index)
{
    return kIntervals[std::clamp(index, 0, kIntervalCount - 1)];
}

const BlockDescriptor kPitchShifterBlockDescriptor = {
    "pitch",
    "Pitch Shifter",
    Category::Pitch,
    kPitchParamCount,
    {
        ParamSpec { "Shift 1", "st", -24.0f, 24.0f, 12.0f, ParamScale::Linear },
        ParamSpec { "Level 1", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Shift 2", "st", -24.0f, 24.0f, 7.0f, ParamScale::Linear },
        ParamSpec { "Level 2", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Detune", "ct", 0.0f, 50.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 45.0f, ParamScale::Log },
        ParamSpec { "Tone", "Hz", 1000.0f, 20000.0f, 20000.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new PitchShifterBlock()); },
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

const BlockDescriptor kWhammyBlockDescriptor = {
    "whammy",
    "Whammy",
    Category::Pitch,
    kWhammyParamCount,
    {
        ParamSpec { "Position", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Interval", "", 0.0f, static_cast<float>(kIntervalCount - 1), 1.0f, ParamScale::Choice,
                    kIntervalNames },
        ParamSpec { "Mode", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kWhammyModeNames },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 40.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new WhammyBlock()); },
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

const BlockDescriptor kPolyOctaveBlockDescriptor = {
    "polyoctave",
    "Poly Octave",
    Category::Pitch,
    kPolyOctParamCount,
    {
        ParamSpec { "Sub 2", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Sub 1", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Up 1", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Up 2", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 500.0f, 20000.0f, 20000.0f, ParamScale::Log },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 50.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new PolyOctaveBlock()); },
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

} // namespace xythos

namespace xythos {

const BlockDescriptor kHarmonizerBlockDescriptor = {
    "harmonizer",
    "Harmonizer",
    Category::Pitch,
    kHarmParamCount,
    {
        ParamSpec { "Key", "", 0.0f, 11.0f, 0.0f, ParamScale::Choice, kKeyNames },
        ParamSpec { "Scale", "", 0.0f, static_cast<float>(kScaleCount - 1), 0.0f, ParamScale::Choice, kScaleNames },
        ParamSpec { "Voice 1", "", 0.0f, static_cast<float>(kVoiceCount - 1), 9.0f, ParamScale::Choice, kVoiceNames },
        ParamSpec { "Level 1", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Voice 2", "", 0.0f, static_cast<float>(kVoiceCount - 1), 11.0f, ParamScale::Choice, kVoiceNames },
        ParamSpec { "Level 2", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 40.0f, ParamScale::Log },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new HarmonizerBlock()); },
    -1,
    -1,
    false,
    nullptr,
    24.0f,
    MeterStyle::Reduction,
    true,
    false,
    true, // hq
};

} // namespace xythos

namespace xythos {

const BlockDescriptor kDetuneBlockDescriptor = {
    "detune",
    "Detune",
    Category::Pitch,
    kDetuneParamCount,
    {
        ParamSpec { "Detune", "ct", 1.0f, 50.0f, 10.0f, ParamScale::Linear },
        ParamSpec { "Voices", "", 0.0f, 2.0f, 0.0f, ParamScale::Choice, kDetuneVoiceNames },
        ParamSpec { "Width", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 30.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new DetuneBlock()); },
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

const BlockDescriptor kCapoBlockDescriptor = {
    "capo",
    "Capo",
    Category::Pitch,
    kCapoParamCount,
    {
        ParamSpec { "Shift", "st", -12.0f, 12.0f, -1.0f, ParamScale::Linear },
        ParamSpec { "Fine", "ct", -50.0f, 50.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 50.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new CapoBlock()); },
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

const BlockDescriptor kOrganBlockDescriptor = {
    "organ",
    "Organ",
    Category::Synth,
    kOrganParamCount,
    {
        ParamSpec { "Sub", "%", 0.0f, 100.0f, 50.0f, ParamScale::Linear },
        ParamSpec { "Fund", "%", 0.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Octave", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "12th", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "2 Oct", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Attack", "ms", 1.0f, 1000.0f, 40.0f, ParamScale::Log },
        ParamSpec { "Tone", "Hz", 500.0f, 20000.0f, 5000.0f, ParamScale::Log },
        ParamSpec { "Dry", "%", 0.0f, 100.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 45.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new OrganBlock()); },
};

const BlockDescriptor kArpeggiatorBlockDescriptor = {
    "arpeggiator",
    "Arpeggiator",
    Category::Synth,
    kArpParamCount,
    {
        ParamSpec { "Chord", "", 0.0f, static_cast<float>(kArpChordCount - 1), 1.0f, ParamScale::Choice, kArpChordNames },
        ParamSpec { "Pattern", "", 0.0f, 3.0f, 0.0f, ParamScale::Choice, kArpPatternNames },
        ParamSpec { "Range", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kArpRangeNames },
        ParamSpec { "Rate", "Hz", 0.5f, 20.0f, 8.0f, ParamScale::Log },
        ParamSpec { "Sync", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Gate", "%", 10.0f, 100.0f, 80.0f, ParamScale::Linear },
        ParamSpec { "Glide", "ms", 0.0f, 200.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Window", "ms", 15.0f, 100.0f, 30.0f, ParamScale::Log },
    },
    [] { return std::unique_ptr<Block>(new ArpeggiatorBlock()); },
    kArpParamSync,
    kArpParamRate,
    true,
};

const BlockDescriptor kSustainerBlockDescriptor = {
    "sustainer",
    "Sustainer",
    Category::Pitch,
    kSustainParamCount,
    {
        ParamSpec { "Sustain", "%", 0.0f, 100.0f, 60.0f, ParamScale::Linear },
        ParamSpec { "Bloom", "ms", 100.0f, 10000.0f, 2000.0f, ParamScale::Log },
        ParamSpec { "Harmonic", "", 0.0f, 3.0f, 0.0f, ParamScale::Choice, kSustainHarmonicNames },
        ParamSpec { "Feedback", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Tone", "Hz", 1000.0f, 20000.0f, 8000.0f, ParamScale::Log },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new SustainerBlock()); },
};

} // namespace xythos
