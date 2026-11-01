// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/TapTempo.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/metronome/Metronome.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace xythos {

// The looper's controls, as LooperParam indices with their real values (the global looper's host parameters and the
// old Looper block's slots both map to these)
enum LooperParam {
    kLooperParamRecDub,   // momentary
    kLooperParamPlayStop, // momentary
    kLooperParamClear,    // momentary
    kLooperParamLevel,    // %
    kLooperParamFeedback, // %
    kLooperParamReverse,  // toggle
    kLooperParamHalf,     // toggle
    kLooperParamMode,     // choice: Count In, Free
    kLooperParamBpm,      // the count-in metronome's tempo
    kLooperParamBeats,    // choice: beats to the bar, 1..12
    kLooperParamCountIn,  // choice: clicks before recording, 0..16
    kLooperParamMeasures, // choice: bars to record, 1..16
    kLooperParamLoops,    // choice: passes recorded (the first, then overdubs), 1..8
    kLooperParamClick,    // %: the clicks' level (100% = the metronome's default volume; up to 200%)
    kLooperParamTap,      // momentary: tap tempo for BPM
    kLooperParamLatency,  // ms: the round trip compensated for (config "latency"; added 2026-10-31)
    kLooperParamCount
};

enum class LooperState { Empty, Recording, Playing, Overdubbing, Stopped, CountIn };

constexpr double kLooperMaxSeconds = 60.0;
constexpr uint32_t kLooperMaxSamples = 60 * 48000;

// The status the old Looper block reports through its meter: exact in a float (under 2^24)
struct LooperStatus {
    LooperState state = LooperState::Empty;
    int lengthTenths = 0; // loop length in tenths of a second (0..600)
    int progress = 0;     // position in the loop, percent (0..99); Count In mode's recording: how much is done,
                          // percent; counting in: the click being counted (1..16)

    float encode() const
    {
        return static_cast<float>(static_cast<int>(state) * 1000000 + lengthTenths * 100 + progress);
    }

    static LooperStatus decode(float value)
    {
        const int v = static_cast<int>(value + 0.5f);
        LooperStatus s;
        s.state = static_cast<LooperState>(v / 1000000 > 5 ? 0 : v / 1000000);
        s.lengthTenths = (v / 100) % 10000;
        s.progress = v % 100;
        return s;
    }
};

// The waveform the looper page draws (owner, 2026-11-01: mockup 3, a lane per pass): each pass's peaks in
// kLooperWaveColumns columns over the loop, 0..255 (the square root of the peak, so quiet playing still shows).
// Pass 1 is the first recording; each overdub cycle round the loop is the next, up to kLooperWaveLanes (later ones
// go in the last lane). A cell travels to the UI as one exact integer in a float (encodeLooperCell()): the
// generation (bumped by Clear, so the UI drops cells of a loop that's gone), the lane, the column and the peak.
constexpr int kLooperWaveColumns = 128;
constexpr int kLooperWaveLanes = 8;
constexpr int kLooperGenerations = 64;

constexpr float encodeLooperCell(int generation, int lane, int column, int peak)
{
    return static_cast<float>(((generation * 8 + lane) * 128 + column) * 256 + peak);
}

struct LooperCell {
    int generation, lane, column, peak;
};

inline LooperCell decodeLooperCell(float value)
{
    const int v = std::clamp(static_cast<int>(value + 0.5f), 0, 16777215);
    return { v >> 18, (v >> 15) & 7, (v >> 8) & 127, v & 255 };
}

// What the looper reports to the UI besides its waveform (the global looper's output parameters)
struct LooperReport {
    LooperState state = LooperState::Empty;
    int pass = 0;        // the pass being recorded, from 1 (0 when none is)
    int lanes = 0;       // lanes with something in them
    int generation = 0;
    float position = 0;  // where the loop is, 0..1; a timed recording: how much is done; else 0
    int count = 0;       // counting in: the click being counted (1..), else 0
    float seconds = 0;   // the loop's length (a timed recording: what it will be; a free one: so far)

    // state, pass, lanes and generation in one exact float
    float encodeState() const
    {
        return static_cast<float>(static_cast<int>(state) + 8 * pass + 128 * lanes + 2048 * generation);
    }

    void decodeState(float value)
    {
        const int v = std::max(0, static_cast<int>(value + 0.5f));
        state = static_cast<LooperState>(v % 8 > 5 ? 0 : v % 8);
        pass = (v / 8) % 16;
        lanes = (v / 128) % 16;
        generation = (v / 2048) % kLooperGenerations;
    }
};

namespace looper_detail {
constexpr uint32_t kFadeSamples = 256; // the loop's end crossfades this long into what came just before its start
constexpr float kClickGain = 0.8f;      // the clicks at Click 100%, as the metronome's default volume (up to 200%)
constexpr double kMaxLatencyMs = 200.0;
} // namespace looper_detail

// The looper (the global one, Engine; and the old Looper block, which wraps one): one loop of up to 60 s (at 48 kHz;
// the buffer is a fixed number of samples). See LooperBlock.hpp for how its controls behave.
class Looper {
public:
    Looper()
    {
        fLevel.snap(1.0f);
        fFeedback.snap(1.0f);
    }

    // Non-RT: allocates the loop (once). Prepared again at the same rate (the host restarting audio) it keeps the loop.
    void prepare(double sampleRate)
    {
        if (fPrepared && sampleRate == fSampleRate)
        {
            fClickData = nullptr;
            return;
        }
        fPrepared = true;
        fSampleRate = sampleRate;
        // the loop holds a fixed number of samples (no reallocation if prepared again at another rate)
        for (std::vector<float>& buffer : fBuffer)
            if (buffer.size() != kLooperMaxSamples)
                buffer.assign(kLooperMaxSamples, 0.0f);
        fLevel.prepare(sampleRate);
        fFeedback.prepare(sampleRate);
        fFade.prepare(sampleRate, 0.005);
        fLatency = latencySamples(fLatencyMs);
        fAccentClick = classicMetronomeClick(true, sampleRate);
        fBeatClick = classicMetronomeClick(false, sampleRate);
        fClickData = nullptr;
        clear();
    }

    // Loopers saved before Count In existed stay in Free mode (LooperBlock's config); the global looper has it
    void setHasCountIn(bool hasCountIn) { fHasCountIn = hasCountIn; }

    // RT. A LooperParam and its real value; the momentary ones act on the 0 -> 1 edge
    void setParam(int index, float value)
    {
        const bool on = value >= 0.5f;
        switch (index)
        {
        case kLooperParamRecDub:
            if (on && !fRecDubHeld)
                recDub();
            fRecDubHeld = on;
            break;
        case kLooperParamPlayStop:
            if (on && !fPlayStopHeld)
                playStop();
            fPlayStopHeld = on;
            break;
        case kLooperParamClear:
            if (on && !fClearHeld)
                clear();
            fClearHeld = on;
            break;
        case kLooperParamLevel:    fLevel.setTarget(value / 100.0f); break;
        case kLooperParamFeedback: fFeedback.setTarget(value / 100.0f); break;
        case kLooperParamReverse:  fReverse = on; break;
        case kLooperParamHalf:     fHalf = on; break;
        // the count-in settings are taken when a count-in starts
        case kLooperParamMode:     fCountInMode = value < 0.5f; break;
        case kLooperParamBpm:      fBpm = std::clamp(std::round(static_cast<double>(value)), 40.0, 240.0); break;
        case kLooperParamBeats:    fBeats = std::clamp(static_cast<int>(std::lround(value)), 1, 12); break;
        case kLooperParamCountIn:  fCountIn = std::clamp(static_cast<int>(std::lround(value)), 0, 16); break;
        case kLooperParamMeasures: fMeasures = std::clamp(static_cast<int>(std::lround(value)), 1, 16); break;
        case kLooperParamLoops:    fLoops = std::clamp(static_cast<int>(std::lround(value)), 1, 8); break;
        case kLooperParamClick:    fClickLevel = std::clamp(value / 100.0f, 0.0f, 2.0f); break;
        case kLooperParamLatency:
            fLatencyMs = std::clamp(static_cast<double>(value), 0.0, looper_detail::kMaxLatencyMs);
            fLatency = latencySamples(fLatencyMs);
            break;
        case kLooperParamTap:
            if (on && !fTapHeld)
            {
                // the next count-in's tempo; the UI sets the BPM control to the same (it times the taps too)
                const double bpm = fTapTempo.tap(static_cast<double>(fSamples) / fSampleRate);
                if (bpm > 0.0)
                    fBpm = std::clamp(std::round(bpm), 40.0, 240.0);
            }
            fTapHeld = on;
            break;
        }
    }

    // RT. Plays the loop into `io` (1 or 2 channels; what's recorded is what comes in) and adds the count-in's
    // clicks to `click`, or to `io` when it's null
    void process(float* const* io, uint32_t numChannels, uint32_t frames, float* click)
    {
        fClickOut = click;
        fSamples += frames;
        for (uint32_t i = 0; i < frames; ++i)
        {
            if (fTimed)
                tick();
            processSample(io, numChannels, i);
            // the clicks go out, never into the loop: to the engine's click bus, past the blocks after this one (an
            // amp after the looper would distort them), or without an engine into the block's output
            if (fClickData != nullptr)
            {
                const float sample = (*fClickData)[fClickPos] * fClickGain;
                if (++fClickPos >= fClickData->size())
                    fClickData = nullptr;
                if (fClickOut != nullptr)
                    fClickOut[i] += sample;
                else
                    for (uint32_t ch = 0; ch < numChannels; ++ch)
                        io[ch][i] += sample;
            }
        }

        LooperStatus status;
        status.state = fState;
        status.lengthTenths = static_cast<int>(fLength / fSampleRate * 10.0);
        if (fState == LooperState::CountIn)
            status.progress = std::clamp(static_cast<int>(fClock / fBeatLength) + 1, 1, std::max(1, fCountInClicks));
        else if (fState == LooperState::Recording)
            status.progress = fTimed ? std::min(99, static_cast<int>(uint64_t { fLength } * 100 / fRecLength)) : 0;
        else
            status.progress = fLength > 0 ? std::min(99, static_cast<int>(fPosition / fLength * 100.0)) : 0;
        fMeter = status.encode();
        fStreamTimer += frames;
    }

    // What the UI shows besides the waveform
    LooperReport report() const
    {
        LooperReport r;
        r.state = fState;
        r.pass = fWavePass >= 0 && (fState == LooperState::Recording || fState == LooperState::Overdubbing)
                     ? fWavePass + 1 : 0;
        r.lanes = fWaveLanes;
        r.generation = fGeneration;
        if (fState == LooperState::CountIn)
            r.count = std::clamp(static_cast<int>(fClock / fBeatLength) + 1, 1, std::max(1, fCountInClicks));
        if (fState == LooperState::Recording)
            r.position = fTimed && fRecLength > 0 ? static_cast<float>(fLength) / fRecLength : 0.0f;
        else if (fLength > 0 && fState != LooperState::CountIn)
            r.position = static_cast<float>(fPosition / fLength);
        const uint32_t length = fTimed && (fState == LooperState::CountIn || fState == LooperState::Recording)
                                    ? fRecLength : fLength;
        r.seconds = static_cast<float>(length / fSampleRate);
        return r;
    }

    // RT. Fills `cells` (count of them) with waveform cells to send the UI (encodeLooperCell()): the first is the
    // cell written last, the rest go round every cell with something in it, a few at a time, so a UI that opens
    // later (or missed some) gets the whole picture within a few seconds. They move on every 40 ms, slower than
    // UIs read their outputs.
    void streamCells(float* cells, int count)
    {
        if (count <= 0)
            return;
        cells[0] = fLastLane >= 0 ? cellValue(fLastLane, fLastColumn) : encodeLooperCell(fGeneration, 0, 0, 0);
        const int total = std::max(1, fWaveLanes) * kLooperWaveColumns;
        if (fStreamTimer >= static_cast<uint64_t>(fSampleRate * 0.04))
        {
            fStreamTimer = 0;
            fStreamNext = (fStreamNext + count - 1) % total;
        }
        for (int k = 1; k < count; ++k)
        {
            const int cell = (fStreamNext + k - 1) % total;
            cells[k] = cellValue(cell / kLooperWaveColumns, cell % kLooperWaveColumns);
        }
    }

    // A waveform cell's peak, 0..255 (tests and the UI's own copy)
    int wave(int lane, int column) const { return fWave[lane][column]; }
    bool hasLoop() const { return fState != LooperState::Empty && fState != LooperState::CountIn; }

    float meter() const { return fMeter; }

    // The loop survives chain changes (the engine reuses the block); reset() only rewinds
    void reset() { fPosition = 0.0; }

private:
    // One sample of the count-in's timeline (Count In mode, from the press on): starts the recording when the
    // count-in is over, ends the overdub passes, and starts each click (process() plays it from fClickData)
    void tick()
    {
        if (fState == LooperState::CountIn && fClock >= fRecStart + fRecLatency)
        {
            // a round trip after the downbeat's click: when what was played to it arrives
            fLength = 0;
            fState = LooperState::Recording;
        }
        if (fState == LooperState::Recording && fClock >= fRecStart + fRecLength)
        {
            // the grid's next downbeat: the loop comes round now, from its first sample (its downbeat, attack and
            // all), while the bar's last round trip, still on its way in, goes on into the loop's end
            fTailPos = fLength;
            fLength = fRecLength;
            fPosition = 0.0;
            fState = fPasses > 1 ? LooperState::Overdubbing : LooperState::Playing;
            fWavePass = -1; // the overdubs' passes start their own lanes
            if (fPasses <= 1)
                fTimed = false;
            fFade.snap(1.0f);
            if (fTailPos >= fLength)
                finishTail();
        }
        if (fState == LooperState::Overdubbing && fClock >= fEnd)
        {
            // every pass is recorded: just the loop from now on
            fState = LooperState::Playing;
            fTimed = false;
            return;
        }
        if (fClock < fClickEnd && fClock == static_cast<uint64_t>(std::llround(fNextClick * fBeatLength)))
        {
            // the bar's first beat is accented: bars start where the recording does
            const int beat = ((fNextClick - fCountInClicks) % fBarBeats + fBarBeats) % fBarBeats;
            fClickData = beat == 0 ? &fAccentClick : &fBeatClick;
            fClickPos = 0;
            fClickGain = fClickLevel * looper_detail::kClickGain * (beat == 0 ? 1.0f : 0.8f);
            ++fNextClick;
        }
        ++fClock;
    }

    void processSample(float* const* io, uint32_t numChannels, uint32_t i)
    {
        // the input's last moments, kept for the seam: a recording starting now takes them as what led into it
        if (fState == LooperState::Recording && fLength == 0)
            for (uint32_t ch = 0; ch < 2; ++ch)
                for (uint32_t k = 0; k < looper_detail::kFadeSamples; ++k)
                    fPreroll[ch][k] = fRecent[ch][(fRecentPos + k) % looper_detail::kFadeSamples];
        for (uint32_t ch = 0; ch < 2; ++ch)
            fRecent[ch][fRecentPos] = io[numChannels > 1 ? ch : 0][i];
        fRecentPos = (fRecentPos + 1) % looper_detail::kFadeSamples;

        const float level = fLevel.next();
        const float feedback = fFeedback.next();
        // playback fades in and out on start and stop
        fFade.setTarget(fState == LooperState::Playing || fState == LooperState::Overdubbing ? 1.0f : 0.0f);
        const float fade = fFade.next();

        if (fState == LooperState::CountIn)
            return;
        if (fState == LooperState::Recording)
        {
            // Count In mode's recording ends on the grid (tick()); Free mode's when pressed, or when the buffer is full
            if (fLength < kLooperMaxSamples && !(fTimed && fLength >= fRecLength))
            {
                for (uint32_t ch = 0; ch < 2; ++ch)
                    fBuffer[ch][fLength] = io[numChannels > 1 ? ch : 0][i];
                const float peak = inputPeak(io, numChannels, i);
                if (fTimed)
                    noteWave(0, fLength, fRecLength, peak);
                else
                    noteFreePeak(peak);
                ++fLength;
            }
            if (!fTimed && fLength >= kLooperMaxSamples)
                closeLoop();
            return;
        }
        // Count In mode, just after the loop came round: the last bar's last round trip arrives, into the loop's end
        const bool tailing = fTailPos < fLength;
        if (tailing)
        {
            for (uint32_t ch = 0; ch < 2; ++ch)
                fBuffer[ch][fTailPos] = io[numChannels > 1 ? ch : 0][i];
            noteWave(0, fTailPos, fLength, inputPeak(io, numChannels, i));
            if (++fTailPos >= fLength)
                finishTail();
        }
        if (fLength == 0 || (fState == LooperState::Stopped && fade <= 0.0f))
            return;

        const auto index = static_cast<uint32_t>(fPosition);
        const double frac = fPosition - index;
        const uint32_t next = index + 1 < fLength ? index + 1 : 0;
        const double step = (fHalf ? 0.5 : 1.0) * (fReverse ? -1.0 : 1.0);
        // overdubs go where the playback was a round trip ago: what the player heard and played along to
        uint32_t write = index;
        if (fState == LooperState::Overdubbing && fLatency > 0)
        {
            const double length = static_cast<double>(fLength);
            double at = std::fmod(fPosition - step * static_cast<double>(fLatency), length);
            if (at < 0.0)
                at += length;
            write = std::min(static_cast<uint32_t>(at), fLength - 1);
        }
        const float inPeak = inputPeak(io, numChannels, i); // what's played in, before the loop joins it
        for (uint32_t ch = 0; ch < numChannels; ++ch)
        {
            std::vector<float>& buffer = fBuffer[ch];
            const float played = static_cast<float>(buffer[index] + (buffer[next] - buffer[index]) * frac);
            if (fState == LooperState::Overdubbing && !tailing) // the tail just wrote this input
                buffer[write] = buffer[write] * feedback + io[ch][i];
            io[ch][i] += played * level * fade;
        }
        if (fState == LooperState::Overdubbing && !tailing && numChannels == 1)
            fBuffer[1][write] = fBuffer[0][write];
        if (fState == LooperState::Overdubbing && !tailing)
        {
            // each time round the loop is a pass of its own
            if (fWavePass < 0)
                startPass();
            noteWave(fWavePass, write, fLength, inPeak);
            fPassTravel += std::fabs(step);
            if (fPassTravel >= fLength)
            {
                fPassTravel -= fLength;
                startPass();
            }
        }
        else if (fState != LooperState::Overdubbing)
            fWavePass = -1;

        if (fState != LooperState::Stopped)
        {
            fPosition += step;
            if (fPosition >= fLength)
                fPosition -= fLength;
            if (fPosition < 0.0)
                fPosition += fLength;
        }
    }

    // Count In mode: the metronome starts, counts in, and times the recording, with the settings as they are now
    void startCountIn()
    {
        fBeatLength = fSampleRate * 60.0 / fBpm;
        fBarBeats = fBeats;
        fCountInClicks = fCountIn;
        fPasses = fLoops;
        fRecStart = static_cast<uint64_t>(std::llround(fCountIn * fBeatLength));
        fRecLength = static_cast<uint32_t>(
            std::clamp<long long>(std::llround(fMeasures * fBeats * fBeatLength), looper_detail::kFadeSamples * 4, kLooperMaxSamples));
        fRecLatency = std::min(fLatency, fRecLength / 2);
        // the clicks stop on the grid; the passes, which run a round trip late, that much after
        fClickEnd = fRecStart + uint64_t { fRecLength } * static_cast<uint64_t>(fPasses);
        fEnd = fClickEnd + fRecLatency;
        fClock = 0;
        fNextClick = 0;
        fTimed = true;
        fTimedWave = true;
        fWaveLanes = 1;
        fWavePass = 0;
        fLength = 0;
        fState = LooperState::CountIn;
    }

    void recDub()
    {
        endTail();
        switch (fState)
        {
        case LooperState::Empty:
            if (fHasCountIn && fCountInMode)
            {
                startCountIn();
                break;
            }
            fLength = 0;
            startFreePeaks();
            fState = LooperState::Recording;
            break;
        case LooperState::CountIn:
            clear(); // a second press calls it off
            break;
        case LooperState::Recording:
            fTimed = false;
            closeLoop();
            break;
        case LooperState::Playing:
            fState = LooperState::Overdubbing;
            fWavePass = -1; // a new pass starts where this overdub does
            break;
        case LooperState::Overdubbing:
            fTimed = false;
            fState = LooperState::Playing;
            break;
        case LooperState::Stopped:
            fPosition = fReverse ? fLength - 1 : 0.0;
            fState = LooperState::Overdubbing;
            fWavePass = -1;
            break;
        }
    }

    void playStop()
    {
        endTail();
        switch (fState)
        {
        case LooperState::CountIn:
            clear();
            break;
        case LooperState::Recording:
            fTimed = false;
            closeLoop();
            break;
        case LooperState::Playing:
        case LooperState::Overdubbing:
            fTimed = false;
            fState = LooperState::Stopped;
            break;
        case LooperState::Stopped:
            fPosition = fReverse ? fLength - 1 : 0.0;
            fState = LooperState::Playing;
            break;
        case LooperState::Empty:
            break;
        }
    }

    void closeLoop()
    {
        if (fLength < looper_detail::kFadeSamples * 4)
        {
            // too short to be a loop: treat as a mistake
            clear();
            return;
        }
        crossfadeSeam();
        if (!fTimedWave)
            finishFreePeaks();
        fPosition = 0.0;
        fState = LooperState::Playing;
        fFade.snap(1.0f);
    }

    // The seam: the loop's last moments crossfade into what was played just before its start, so the end runs on
    // into the start without a click, and the start (a downbeat's attack, often) stays as it was played
    void crossfadeSeam()
    {
        for (uint32_t ch = 0; ch < 2; ++ch)
        {
            std::vector<float>& buffer = fBuffer[ch];
            for (uint32_t k = 0; k < looper_detail::kFadeSamples; ++k)
            {
                const float g = static_cast<float>(k + 1) / looper_detail::kFadeSamples;
                float& sample = buffer[fLength - looper_detail::kFadeSamples + k];
                sample += (fPreroll[ch][k] - sample) * g;
            }
        }
    }

    // Count In mode: the loop's end is all in
    void finishTail()
    {
        fTailPos = kNoTail;
        crossfadeSeam();
    }

    // A press during the tail: what hasn't arrived yet is left silent
    void endTail()
    {
        if (fTailPos >= fLength)
            return;
        for (uint32_t ch = 0; ch < 2; ++ch)
            std::fill(fBuffer[ch].begin() + fTailPos, fBuffer[ch].begin() + fLength, 0.0f);
        finishTail();
    }

    void clear()
    {
        // the waveform goes too; the UI drops what it had of it (a new generation)
        for (auto& lane : fWave)
            std::fill(std::begin(lane), std::end(lane), uint8_t { 0 });
        fWaveLanes = 0;
        fWavePass = -1;
        fLastLane = -1;
        fStreamNext = 0;
        fGeneration = (fGeneration + 1) % kLooperGenerations;
        fTimed = false;
        fTailPos = kNoTail;
        fState = LooperState::Empty;
        fLength = 0;
        fPosition = 0.0;
        fFade.snap(0.0f);
    }

    static float inputPeak(float* const* io, uint32_t numChannels, uint32_t i)
    {
        return numChannels > 1 ? std::max(std::fabs(io[0][i]), std::fabs(io[1][i])) : std::fabs(io[0][i]);
    }

    static uint8_t quantizePeak(float peak)
    {
        return static_cast<uint8_t>(std::lround(255.0f * std::sqrt(std::min(1.0f, peak))));
    }

    float cellValue(int lane, int column) const
    {
        return encodeLooperCell(fGeneration, lane, column, fWave[lane][column]);
    }

    // A sample at `index` of a loop `length` long, in `lane`
    void noteWave(int lane, uint32_t index, uint32_t length, float peak)
    {
        if (length == 0 || lane < 0)
            return;
        lane = std::min(lane, kLooperWaveLanes - 1);
        const int column = std::min(kLooperWaveColumns - 1, static_cast<int>(uint64_t { index } * kLooperWaveColumns / length));
        const uint8_t q = quantizePeak(peak);
        if (q > fWave[lane][column])
            fWave[lane][column] = q;
        fWaveLanes = std::max(fWaveLanes, lane + 1);
        fLastLane = lane;
        fLastColumn = column;
    }

    void startPass()
    {
        fWavePass = fWavePass < 0 ? fWaveLanes : fWavePass + 1;
        fWavePass = std::min(fWavePass, kLooperWaveLanes - 1);
        fPassTravel = 0.0;
    }

    // Free mode's first recording: its length isn't known yet, so its peaks go into kFreePeaks entries of a growing
    // number of samples each (two merge into one when they're full), spread over the columns when the loop closes
    void startFreePeaks()
    {
        fTimedWave = false;
        std::fill(std::begin(fFreePeaks), std::end(fFreePeaks), 0.0f);
        fFreeCount = 0;
        fFreeFill = 0;
        fFreeChunk = 1024;
        fWaveLanes = 1;
        fWavePass = 0;
    }

    void noteFreePeak(float peak)
    {
        fFreePeaks[fFreeCount] = std::max(fFreePeaks[fFreeCount], peak);
        if (++fFreeFill < fFreeChunk)
            return;
        fFreeFill = 0;
        if (++fFreeCount < kFreePeaks)
            return;
        for (int k = 0; k < kFreePeaks / 2; ++k)
            fFreePeaks[k] = std::max(fFreePeaks[2 * k], fFreePeaks[2 * k + 1]);
        std::fill(fFreePeaks + kFreePeaks / 2, fFreePeaks + kFreePeaks, 0.0f);
        fFreeCount = kFreePeaks / 2;
        fFreeChunk *= 2;
    }

    void finishFreePeaks()
    {
        const int n = std::max(1, fFreeCount + (fFreeFill > 0 ? 1 : 0));
        for (int c = 0; c < kLooperWaveColumns; ++c)
        {
            const int from = c * n / kLooperWaveColumns;
            const int to = std::max(from + 1, (c + 1) * n / kLooperWaveColumns);
            float peak = 0.0f;
            for (int k = from; k < to && k < kFreePeaks; ++k)
                peak = std::max(peak, fFreePeaks[k]);
            fWave[0][c] = quantizePeak(peak);
        }
        fWaveLanes = std::max(fWaveLanes, 1);
        fLastLane = 0;
        fLastColumn = 0;
        fTimedWave = true;
    }

    // the waveform (see LooperCell)
    static constexpr int kFreePeaks = 256;
    uint8_t fWave[kLooperWaveLanes][kLooperWaveColumns] = {};
    int fWaveLanes = 0;      // lanes with something in them
    int fWavePass = -1;      // the lane being written, -1 for none (a new pass starts on the next write)
    double fPassTravel = 0.0; // how far round the loop this pass has gone, in samples
    int fLastLane = -1, fLastColumn = 0;
    int fGeneration = 0;
    uint64_t fStreamTimer = 0;
    int fStreamNext = 0;
    bool fTimedWave = true;  // false while a Free recording's peaks go into fFreePeaks
    float fFreePeaks[kFreePeaks] = {};
    int fFreeCount = 0;
    uint32_t fFreeFill = 0, fFreeChunk = 1024;

    bool fPrepared = false;
    double fSampleRate = 48000.0;
    std::vector<float> fBuffer[2];
    LooperState fState = LooperState::Empty;
    uint32_t fLength = 0;
    double fPosition = 0.0;
    bool fReverse = false, fHalf = false;
    bool fRecDubHeld = false, fPlayStopHeld = false, fClearHeld = false;
    Smoother fLevel, fFeedback, fFade;
    float fMeter = 0.0f;

    // Count In mode: its settings
    bool fHasCountIn = false; // the config's "countIn": blocks saved before Count In stay in Free mode
    bool fCountInMode = true;
    double fBpm = 120.0;
    int fBeats = 4, fCountIn = 8, fMeasures = 4, fLoops = 1;
    float fClickLevel = 1.5f;
    // Tap: the footswitch's taps, timed by the samples processed
    TapTempo fTapTempo;
    uint64_t fSamples = 0;
    bool fTapHeld = false;
    // the input's last looper_detail::kFadeSamples (a ring), and what led into the recording, for the seam
    float fRecent[2][looper_detail::kFadeSamples] = {};
    uint32_t fRecentPos = 0;
    float fPreroll[2][looper_detail::kFadeSamples] = {};
    // Latency: the round trip, in ms and in samples
    double fLatencyMs = 0.0;
    uint32_t fLatency = 0;
    // Count In mode: the next sample of the loop's end to write once it's playing (the last round trip of its last
    // bar, still arriving); kNoTail when there's none
    static constexpr uint32_t kNoTail = 0xffffffffu;
    uint32_t fTailPos = kNoTail;

    uint32_t latencySamples(double ms) const { return static_cast<uint32_t>(std::lround(ms * fSampleRate / 1000.0)); }
    float* fClickOut = nullptr; // the engine's click bus (setClickOutput()), or none
    // the timeline, in samples from the press (fTimed while it runs: counting in, recording, overdub passes)
    bool fTimed = false;
    uint64_t fClock = 0;
    double fBeatLength = 24000.0;
    int fBarBeats = 4, fCountInClicks = 4, fPasses = 1;
    int fNextClick = 0;       // clicks so far
    uint64_t fRecStart = 0;   // when the recording starts
    uint32_t fRecLength = 0;  // the loop's length
    uint64_t fEnd = 0;        // when the last pass ends
    uint64_t fClickEnd = 0;   // when the clicks stop (the last pass's end on the grid)
    uint32_t fRecLatency = 0; // the round trip, as the count-in took it
    // the clicks (the metronome's Classic sound), one at a time
    std::vector<float> fAccentClick, fBeatClick;
    const std::vector<float>* fClickData = nullptr;
    size_t fClickPos = 0;
    float fClickGain = 0.0f;
};

} // namespace xythos
