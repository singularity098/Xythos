// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/TapTempo.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/metronome/Metronome.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
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

// DubCountIn (owner, 2026-11-01): Count In mode's count-in to a new pass over a loop there already is; the loop plays
// on through it (or, from Stopped, waits in silence and starts with the pass)
enum class LooperState { Empty, Recording, Playing, Overdubbing, Stopped, CountIn, DubCountIn };
constexpr int kLooperLastState = static_cast<int>(LooperState::DubCountIn);

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
        s.state = static_cast<LooperState>(v / 1000000 > kLooperLastState ? 0 : v / 1000000);
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
    int count = 0;       // counting in: the click being counted (1..; a count-in to an overdub: 0 until its first), else 0
    float seconds = 0;   // the loop's length (a timed recording: what it will be; a free one: so far)

    // state, pass, lanes and generation in one exact float
    float encodeState() const
    {
        return static_cast<float>(static_cast<int>(state) + 8 * pass + 128 * lanes + 2048 * generation);
    }

    void decodeState(float value)
    {
        const int v = std::max(0, static_cast<int>(value + 0.5f));
        state = static_cast<LooperState>(v % 8 > kLooperLastState ? 0 : v % 8);
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

// The loop's memory (owner, 2026-11-01: any pass can be deleted, so each pass has a lane of its own): up to
// kLooperWaveLanes lanes of two channels. The audio thread takes a ready lane for each pass (take()) and gives it
// back when the pass is deleted or the loop cleared (give()); service(), on another thread now and then, zeroes the
// lanes given back and keeps one lane ready that's long enough for the next pass (setWant(): the loop's length;
// a whole minute while there's no loop). So the memory is a minute of stereo for the first recording, plus the loop's
// length for each pass after it, and grows only as passes are recorded.
class LooperMemory {
public:
    LooperMemory() = default;
    LooperMemory(const LooperMemory&) = delete;
    LooperMemory& operator=(const LooperMemory&) = delete;

    // RT. A lane at least `length` long: a zeroed one, or with `zeroed` false (a first recording writes every sample
    // it plays) a given-back one too. -1 if there's none yet.
    int take(uint32_t length, bool zeroed)
    {
        for (int round = 0; round < (zeroed ? 1 : 2); ++round)
        {
            for (int k = 0; k < kLooperWaveLanes; ++k)
            {
                Lane& lane = fLanes[k];
                int expected = round == 0 ? kReady : kDirty;
                if (!lane.state.compare_exchange_strong(expected, kUsed, std::memory_order_acq_rel))
                    continue;
                if (lane.capacity >= length)
                    return k;
                lane.state.store(round == 0 ? kReady : kDirty, std::memory_order_release); // too short
            }
        }
        return -1;
    }

    // RT. A lane taken is given back (service() zeroes it)
    void give(int k)
    {
        if (k >= 0 && k < kLooperWaveLanes)
            fLanes[k].state.store(kDirty, std::memory_order_release);
    }

    // RT, a lane taken: its two channels
    float* data(int k, int channel) const { return fLanes[k].data[channel]; }

    // RT. How long the next pass's lane must be; 0 while that isn't known (a free recording)
    void setWant(uint32_t length) { fWant.store(length, std::memory_order_relaxed); }

    // Non-RT, one thread (the plugin's exchange thread): zeroes the lanes given back, and keeps a lane ready for the
    // next pass
    void service()
    {
        for (int k = 0; k < kLooperWaveLanes; ++k)
        {
            int expected = kDirty;
            if (fLanes[k].state.compare_exchange_strong(expected, kBusy, std::memory_order_acq_rel))
            {
                for (float* channel : fLanes[k].data)
                    std::memset(channel, 0, sizeof(float) * fCapacity[k]);
                fLanes[k].state.store(kReady, std::memory_order_release);
            }
        }
        const uint32_t want = fWant.load(std::memory_order_relaxed);
        if (want == 0)
            return;
        for (int k = 0; k < kLooperWaveLanes; ++k)
            if (fLanes[k].state.load(std::memory_order_acquire) == kReady && fCapacity[k] >= want)
                return; // one's ready
        // a lane that has no memory yet, or else the shortest ready one, gets enough
        int pick = -1;
        for (int k = 0; k < kLooperWaveLanes && pick < 0; ++k)
        {
            int expected = kNone;
            if (fLanes[k].state.compare_exchange_strong(expected, kBusy, std::memory_order_acq_rel))
                pick = k;
        }
        for (int k = 0; k < kLooperWaveLanes && pick < 0; ++k)
        {
            int expected = kReady;
            if (fLanes[k].state.compare_exchange_strong(expected, kBusy, std::memory_order_acq_rel))
                pick = k;
        }
        if (pick < 0)
            return; // every lane holds a pass
        for (int ch = 0; ch < 2; ++ch)
        {
            fStore[pick][ch].reset(new float[want]()); // zeroed
            fLanes[pick].data[ch] = fStore[pick][ch].get();
        }
        fLanes[pick].capacity = want;
        fCapacity[pick] = want;
        fLanes[pick].state.store(kReady, std::memory_order_release);
    }

    // Non-RT, the service()'s thread: the passes in use summed, `length` samples of each channel (the loop's length;
    // read while the audio thread doesn't write the lanes: the loop playing or stopped)
    void mixdown(uint32_t length, std::vector<float>& left, std::vector<float>& right) const
    {
        left.assign(length, 0.0f);
        right.assign(length, 0.0f);
        for (int k = 0; k < kLooperWaveLanes; ++k)
        {
            if (fLanes[k].state.load(std::memory_order_acquire) != kUsed)
                continue;
            const uint32_t n = std::min(length, fCapacity[k]);
            for (uint32_t i = 0; i < n; ++i)
            {
                left[i] += fLanes[k].data[0][i];
                right[i] += fLanes[k].data[1][i];
            }
        }
    }

    // Non-RT: the memory reserved, in bytes (tests)
    size_t bytes() const
    {
        size_t total = 0;
        for (const uint32_t capacity : fCapacity)
            total += size_t { capacity } * 2 * sizeof(float);
        return total;
    }

private:
    // a lane's state: no memory, ready (zeroed, the audio thread may take it), used (the audio thread's), given back
    // (to be zeroed), busy (service() has it)
    enum : int { kNone, kReady, kUsed, kDirty, kBusy };
    struct Lane {
        std::atomic<int> state { kNone };
        float* data[2] = { nullptr, nullptr };
        uint32_t capacity = 0; // written by service() while it has the lane, read by whoever has it
    };
    Lane fLanes[kLooperWaveLanes];
    uint32_t fCapacity[kLooperWaveLanes] = {}; // service()'s own copy
    std::unique_ptr<float[]> fStore[kLooperWaveLanes][2];
    std::atomic<uint32_t> fWant { kLooperMaxSamples };
};

// The looper (the global one, Engine; and the old Looper block, which wraps one): one loop of up to 60 s (at 48 kHz;
// the buffer is a fixed number of samples), each pass in a lane of its own (LooperMemory), so any pass can be deleted.
// See LooperBlock.hpp for how its controls behave.
class Looper {
public:
    Looper()
    {
        fLevel.snap(1.0f);
        fFeedback.snap(1.0f);
    }

    // Non-RT. Prepared again at the same rate (the host restarting audio) it keeps the loop. The loop's memory comes
    // separately (attach()), so an unused looper costs none.
    void prepare(double sampleRate)
    {
        if (fPrepared && sampleRate == fSampleRate)
        {
            fClickData = nullptr;
            return;
        }
        fPrepared = true;
        fSampleRate = sampleRate;
        fLevel.prepare(sampleRate);
        fFeedback.prepare(sampleRate);
        fFade.prepare(sampleRate, 0.005);
        fLatency = latencySamples(fLatencyMs);
        fAccentClick = classicMetronomeClick(true, sampleRate);
        fBeatClick = classicMetronomeClick(false, sampleRate);
        fClickData = nullptr;
        clear();
    }

    // The loop's memory, which stays the caller's and must outlive the looper's use of it; the caller runs its
    // service() now and then. RT-safe (it only takes the pointer). Until then the looper passes audio through and
    // a Rec/Dub press waits for it.
    void attach(LooperMemory* memory) { fMemory = memory; }

    uint32_t length() const { return fLength; }
    bool hasMemory() const { return fMemory != nullptr; }

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
        updateWant();
    }

    // RT. Deletes a pass (its lane, 0 for the first; owner, 2026-11-01). Not the one being recorded; deleting the
    // only pass left clears the loop. The lanes after it move up, and the waveform starts a new generation.
    void deletePass(int lane)
    {
        if (lane < 0 || lane >= fWaveLanes || fState == LooperState::Recording || fState == LooperState::CountIn)
            return;
        if (fState == LooperState::Overdubbing && lane == fWavePass)
            return;
        endTail();
        if (fWaveLanes == 1)
        {
            clear();
            return;
        }
        fMemory->give(fLane[lane]);
        for (int l = lane; l + 1 < fWaveLanes; ++l)
        {
            fLane[l] = fLane[l + 1];
            fLaneData[l][0] = fLaneData[l + 1][0];
            fLaneData[l][1] = fLaneData[l + 1][1];
            std::copy(std::begin(fWave[l + 1]), std::end(fWave[l + 1]), std::begin(fWave[l]));
        }
        --fWaveLanes;
        std::fill(std::begin(fWave[fWaveLanes]), std::end(fWave[fWaveLanes]), uint8_t { 0 });
        fLane[fWaveLanes] = -1;
        if (fWavePass > lane)
            --fWavePass;
        newGeneration();
        updateWant();
    }

    // RT. Where Play (and an overdub from Stopped) starts the loop, 0..1 of the way round (owner, 2026-11-01: a click
    // on the timeline; the start until then, and again once the loop is cleared). While the loop plays, it plays on
    // from there at once (owner, 2026-11-03), crossfading from where it was over kFadeSamples
    void setStart(float fraction)
    {
        fStart = std::clamp(fraction, 0.0f, 1.0f);
        if (fState == LooperState::Playing && fLength > 0)
        {
            fJumpFrom = fPosition;
            fJumpLeft = looper_detail::kFadeSamples;
            fPosition = startPosition();
        }
    }
    float start() const { return fStart; }

    // RT. Plays the loop into `io` (1 or 2 channels; what's recorded is what comes in) and adds the count-in's
    // clicks to `click`, or to `io` when it's null
    void process(float* const* io, uint32_t numChannels, uint32_t frames, float* click)
    {
        fClickOut = click;
        fSamples += frames;
        if (fPendingRecDub && hasMemory())
        {
            fPendingRecDub = false;
            recDub();
        }
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
        updateWant();

        LooperStatus status;
        status.state = fState;
        status.lengthTenths = static_cast<int>(fLength / fSampleRate * 10.0);
        if (fState == LooperState::CountIn)
            status.progress = std::clamp(static_cast<int>(fClock / fBeatLength) + 1, 1, std::max(1, fCountInClicks));
        else if (fState == LooperState::DubCountIn)
            status.progress = std::min(fNextClick, fCountInClicks);
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
        if (fState == LooperState::DubCountIn)
            r.count = std::min(fNextClick, fCountInClicks);
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
    // The lane the next pass needs (LooperMemory::service()): the loop's length (a timed recording's to be), a whole
    // minute for a recording still to come, nothing while a free one's length isn't known
    void updateWant()
    {
        if (fMemory == nullptr)
            return;
        if (fState == LooperState::Empty)
            fMemory->setWant(kLooperMaxSamples);
        else if (fState == LooperState::CountIn || (fState == LooperState::Recording && fTimed))
            fMemory->setWant(fRecLength);
        else
            fMemory->setWant(fState == LooperState::Recording ? 0 : fLength);
    }

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
        if (fState == LooperState::DubCountIn)
        {
            // from Stopped: the loop starts on the grid, where the count-in leads to
            if (!fCuePlaying && fClock >= fRecStart)
            {
                fCuePlaying = true;
                fPosition = startPosition();
                fFade.snap(1.0f);
            }
            // and the new pass a round trip after it, so it's written where what was played to it sits
            if (fClock >= fRecStart + fRecLatency)
            {
                fState = LooperState::Overdubbing;
                fWavePass = -1;
            }
        }
        if (fState == LooperState::Overdubbing && fClock >= fEnd)
        {
            // every pass is recorded: just the loop from now on
            fState = LooperState::Playing;
            fTimed = false;
            return;
        }
        if (fClock < fClickEnd
            && static_cast<int64_t>(fClock) >= std::llround(fClickZero + fNextClick * fBeatLength))
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
        // playback fades in and out on start and stop; a count-in from Stopped waits in silence
        const bool waiting = fState == LooperState::DubCountIn && !fCuePlaying;
        fFade.setTarget(fState == LooperState::Playing || fState == LooperState::Overdubbing
                                || (fState == LooperState::DubCountIn && !waiting)
                            ? 1.0f : 0.0f);
        const float fade = fFade.next();

        if (fState == LooperState::CountIn)
            return;
        if (fState == LooperState::Recording)
        {
            // Count In mode's recording ends on the grid (tick()); Free mode's when pressed, or when the buffer is full
            if (fLength < fRecCapacity && !(fTimed && fLength >= fRecLength))
            {
                for (uint32_t ch = 0; ch < 2; ++ch)
                    fLaneData[0][ch][fLength] = io[numChannels > 1 ? ch : 0][i];
                const float peak = inputPeak(io, numChannels, i);
                if (fTimed)
                    noteWave(0, fLength, fRecLength, peak);
                else
                    noteFreePeak(peak);
                ++fLength;
            }
            if (!fTimed && fLength >= fRecCapacity)
                closeLoop();
            return;
        }
        // Count In mode, just after the loop came round: the last bar's last round trip arrives, into the loop's end
        const bool tailing = fTailPos < fLength;
        if (tailing)
        {
            for (uint32_t ch = 0; ch < 2; ++ch)
                fLaneData[0][ch][fTailPos] = io[numChannels > 1 ? ch : 0][i];
            noteWave(0, fTailPos, fLength, inputPeak(io, numChannels, i));
            if (++fTailPos >= fLength)
                finishTail();
        }
        if (fLength == 0 || ((fState == LooperState::Stopped || waiting) && fade <= 0.0f))
            return;

        const auto index = static_cast<uint32_t>(fPosition);
        const double frac = fPosition - index;
        const uint32_t next = index + 1 < fLength ? index + 1 : 0;
        const double step = (fHalf ? 0.5 : 1.0) * (fReverse ? -1.0 : 1.0);
        // overdubs go where the playback was a round trip ago: what the player heard and played along to
        const bool dubbing = fState == LooperState::Overdubbing && !tailing && fWaveLanes > 0; // the tail just wrote
        uint32_t write = index;
        if (dubbing && fLatency > 0)
        {
            const double length = static_cast<double>(fLength);
            double at = std::fmod(fPosition - step * static_cast<double>(fLatency), length);
            if (at < 0.0)
                at += length;
            write = std::min(static_cast<uint32_t>(at), fLength - 1);
        }
        if (dubbing && fWavePass < 0)
            beginPass();
        // just after a jump (setStart()): where it was fades out
        if (fJumpLeft > 0 && fJumpFrom >= fLength)
            fJumpLeft = 0;
        const auto jumpIndex = static_cast<uint32_t>(fJumpFrom);
        const double jumpFrac = fJumpFrom - jumpIndex;
        const uint32_t jumpNext = jumpIndex + 1 < fLength ? jumpIndex + 1 : 0;
        const float jumpGain = static_cast<float>(fJumpLeft) / static_cast<float>(looper_detail::kFadeSamples);
        const float inPeak = inputPeak(io, numChannels, i); // what's played in, before the loop joins it
        for (uint32_t ch = 0; ch < numChannels; ++ch)
        {
            // the passes play together
            float played = 0.0f;
            for (int l = 0; l < fWaveLanes; ++l)
            {
                const float* buffer = fLaneData[l][ch];
                played += static_cast<float>(buffer[index] + (buffer[next] - buffer[index]) * frac);
            }
            if (fJumpLeft > 0)
            {
                float before = 0.0f;
                for (int l = 0; l < fWaveLanes; ++l)
                {
                    const float* buffer = fLaneData[l][ch];
                    before += static_cast<float>(buffer[jumpIndex] + (buffer[jumpNext] - buffer[jumpIndex]) * jumpFrac);
                }
                played = played * (1.0f - jumpGain) + before * jumpGain;
            }
            if (dubbing)
            {
                // Feedback: how much of the loop each time round keeps, every pass's lane alike
                for (int l = 0; l < fWaveLanes; ++l)
                    fLaneData[l][ch][write] *= feedback;
                fLaneData[fWavePass][ch][write] += io[ch][i];
            }
            io[ch][i] += played * level * fade;
        }
        if (dubbing && numChannels == 1)
            for (int l = 0; l < fWaveLanes; ++l)
                fLaneData[l][1][write] = fLaneData[l][0][write];
        if (dubbing)
        {
            // each time round the loop is a pass of its own
            noteWave(fWavePass, write, fLength, inPeak);
            fPassTravel += std::fabs(step);
            if (fPassTravel >= fLength)
                fWavePass = -1; // the next one starts with the next sample
        }
        else if (fState != LooperState::Overdubbing)
            fWavePass = -1;

        if (fState != LooperState::Stopped && !waiting)
        {
            fPosition += step;
            if (fPosition >= fLength)
                fPosition -= fLength;
            if (fPosition < 0.0)
                fPosition += fLength;
            if (fJumpLeft > 0)
            {
                --fJumpLeft;
                fJumpFrom += step;
                if (fJumpFrom >= fLength)
                    fJumpFrom -= fLength;
                if (fJumpFrom < 0.0)
                    fJumpFrom += fLength;
            }
        }
    }

    // Where the loop starts playing: the start marker, or the start (the end, played in reverse)
    double startPosition() const
    {
        if (fStart <= 0.0f || fLength == 0)
            return fReverse && fLength > 0 ? fLength - 1 : 0.0;
        return std::min(static_cast<double>(fStart) * fLength, static_cast<double>(fLength - 1));
    }

    // A first recording's lane, at least `length` long; false if there's none yet (the press waits for one)
    bool takeFirstLane(uint32_t length)
    {
        const int k = fMemory->take(length, false);
        if (k < 0)
            return false;
        setLane(0, k);
        fWaveLanes = 1;
        fRecCapacity = length;
        return true;
    }

    void setLane(int lane, int k)
    {
        fLane[lane] = k;
        fLaneData[lane][0] = fMemory->data(k, 0);
        fLaneData[lane][1] = fMemory->data(k, 1);
    }

    // An overdub's new pass: a lane of its own, or with none to be had (all eight in use, or the memory's not ready)
    // it joins the last one
    void beginPass()
    {
        fPassTravel = 0.0;
        if (fWaveLanes < kLooperWaveLanes)
        {
            const int k = fMemory->take(fLength, true);
            if (k >= 0)
            {
                setLane(fWaveLanes, k);
                fWavePass = fWaveLanes++;
                return;
            }
        }
        fWavePass = fWaveLanes - 1;
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
        if (!takeFirstLane(fRecLength))
        {
            fPendingRecDub = true; // done when a lane's ready (process())
            return;
        }
        fLoopBeat = fBeatLength;
        fLoopBarBeats = fBeats;
        fRecLatency = std::min(fLatency, fRecLength / 2);
        // the clicks stop on the grid; the passes, which run a round trip late, that much after
        fClickZero = 0.0;
        fClickEnd = fRecStart + uint64_t { fRecLength } * static_cast<uint64_t>(fPasses);
        fEnd = fClickEnd + fRecLatency;
        fClock = 0;
        fNextClick = 0;
        fTimed = true;
        fTimedWave = true;
        fWavePass = 0;
        fLength = 0;
        fState = LooperState::CountIn;
    }

    // Count In mode with a loop (owner, 2026-11-01): Rec/Dub counts in to a new pass as it did to the first one. The
    // clicks follow the loop's own beat; the pass starts where the loop comes round (playing: at its start, the first
    // time that leaves room for the count-in; stopped: the loop waits in silence and starts, from the start marker,
    // when the count-in ends) and it records Loops passes, then plays on.
    void startDubCountIn(bool playing)
    {
        const double step = fHalf ? 0.5 : 1.0;
        const double beat = fLoopBeat / step; // in samples heard
        const double countIn = fCountIn * beat;
        const double loop = fLength / step;
        double toStart = countIn;
        if (playing)
        {
            toStart = (fReverse ? fPosition : fLength - fPosition) / step;
            while (toStart < countIn - 0.5)
                toStart += loop;
        }
        fBeatLength = beat;
        fBarBeats = fLoopBarBeats;
        fCountInClicks = fCountIn;
        fPasses = fLoops;
        fRecStart = static_cast<uint64_t>(std::llround(toStart));
        fClickZero = toStart - countIn;
        fRecLatency = std::min(fLatency, fLength / 2);
        fClickEnd = fRecStart + static_cast<uint64_t>(std::llround(fPasses * loop));
        fEnd = fClickEnd + fRecLatency;
        fClock = 0;
        fNextClick = 0;
        fTimed = true;
        fCuePlaying = playing;
        fWavePass = -1;
        fState = LooperState::DubCountIn;
    }

    // Count In mode can count in to an overdub only on a loop it knows the beat of
    bool dubCountIn() const { return fHasCountIn && fCountInMode && fLoopBeat > 0.0; }

    void recDub()
    {
        if (!hasMemory())
        {
            fPendingRecDub = true; // done when the memory comes (process())
            return;
        }
        endTail();
        switch (fState)
        {
        case LooperState::Empty:
            if (fHasCountIn && fCountInMode)
            {
                startCountIn();
                break;
            }
            if (!takeFirstLane(kLooperMaxSamples))
            {
                fPendingRecDub = true;
                break;
            }
            fLength = 0;
            startFreePeaks();
            fState = LooperState::Recording;
            break;
        case LooperState::CountIn:
            clear(); // a second press calls it off
            break;
        case LooperState::DubCountIn:
            // called off: the loop goes on as it was
            fTimed = false;
            fState = fCuePlaying ? LooperState::Playing : LooperState::Stopped;
            break;
        case LooperState::Recording:
            fTimed = false;
            closeLoop();
            break;
        case LooperState::Playing:
            if (dubCountIn())
            {
                startDubCountIn(true);
                break;
            }
            fState = LooperState::Overdubbing;
            fWavePass = -1; // a new pass starts where this overdub does
            break;
        case LooperState::Overdubbing:
            fTimed = false;
            fState = LooperState::Playing;
            break;
        case LooperState::Stopped:
            if (dubCountIn())
            {
                startDubCountIn(false);
                break;
            }
            fPosition = startPosition();
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
        case LooperState::DubCountIn:
            fTimed = false;
            fState = LooperState::Stopped;
            break;
        case LooperState::Stopped:
            fPosition = startPosition();
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
        // the loop's beat for a count-in to an overdub: as many beats at BPM as fit it best
        const double beats = std::max(1.0, std::round(fLength / (fSampleRate * 60.0 / fBpm)));
        fLoopBeat = fLength / beats;
        fLoopBarBeats = fBeats;
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
            float* buffer = fLaneData[0][ch];
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
            std::fill(fLaneData[0][ch] + fTailPos, fLaneData[0][ch] + fLength, 0.0f);
        finishTail();
    }

    // The waveform starts a new generation: the UI drops what it had of it
    void newGeneration()
    {
        fLastLane = -1;
        fStreamNext = 0;
        fGeneration = (fGeneration + 1) % kLooperGenerations;
    }

    void clear()
    {
        // every pass's memory goes back
        if (fMemory != nullptr)
            for (int l = 0; l < fWaveLanes; ++l)
                fMemory->give(fLane[l]);
        for (int& lane : fLane)
            lane = -1;
        // the waveform goes too
        for (auto& lane : fWave)
            std::fill(std::begin(lane), std::end(lane), uint8_t { 0 });
        fWaveLanes = 0;
        fWavePass = -1;
        newGeneration();
        fTimed = false;
        fTailPos = kNoTail;
        fState = LooperState::Empty;
        fLength = 0;
        fPosition = 0.0;
        fJumpLeft = 0;
        fStart = 0.0f;
        fLoopBeat = 0.0;
        fPendingRecDub = false;
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
        fLastLane = lane;
        fLastColumn = column;
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
        fLastLane = 0;
        fLastColumn = 0;
        fTimedWave = true;
    }

    // the waveform (see LooperCell)
    static constexpr int kFreePeaks = 256;
    uint8_t fWave[kLooperWaveLanes][kLooperWaveColumns] = {};
    int fWaveLanes = 0;      // passes (lanes) in the loop
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
    // the loop's memory (attach()), and the lane each pass has in it, in pass order
    LooperMemory* fMemory = nullptr;
    int fLane[kLooperWaveLanes] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    float* fLaneData[kLooperWaveLanes][2] = {};
    uint32_t fRecCapacity = 0;                // the first recording's lane's length
    bool fPendingRecDub = false;              // Rec/Dub pressed before the memory came
    LooperState fState = LooperState::Empty;
    uint32_t fLength = 0;
    double fPosition = 0.0;
    double fJumpFrom = 0.0;  // where the loop was before a jump (setStart()), fading out
    uint32_t fJumpLeft = 0;  // samples of that fade to go
    float fStart = 0.0f;                      // the start marker, 0..1
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
    // the loop's beat, in samples of the loop, and beats to its bar: what a count-in to an overdub follows (0: none)
    double fLoopBeat = 0.0;
    int fLoopBarBeats = 4;
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
    double fClickZero = 0.0;  // when the first click sounds (a count-in to an overdub waits for the loop)
    bool fCuePlaying = false; // a count-in to an overdub: the loop is playing (it was, or the count-in is over)
    uint64_t fRecStart = 0;   // when the recording starts (on the grid)
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
