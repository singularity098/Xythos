// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"
#include "engine/dsp/NoteTracker.hpp"
#include "engine/dsp/PolyNoteFinder.hpp"
#include "engine/dsp/Svf.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace xythos {

// The level of a signal the way the note tracker follows it: a peak follower that holds for a period of the note
// (or 15 ms) and then falls fast
class LevelFollower {
public:
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fAttack = 1.0f - EnvelopeFollower::coef(sampleRate, 0.3);
        fRelease = EnvelopeFollower::coef(sampleRate, 4.0);
        fDefaultHold = static_cast<int>(0.015 * sampleRate);
        reset();
    }

    void reset()
    {
        fLevel = 0.0f;
        fHeldPeak = 0.0f;
        fHold = 0;
    }

    float process(float in, double hz)
    {
        const float a = std::fabs(in);
        const int hold = hz > 0.0 ? static_cast<int>(1.2 * fSampleRate / hz) : fDefaultHold;
        if (a > fLevel)
        {
            fLevel += (a - fLevel) * fAttack;
            fHold = hold;
            fHeldPeak = 0.0f;
        }
        else
        {
            // (down to the highest peak since, then held again: see NoteTracker::followLevel())
            fHeldPeak = std::max(fHeldPeak, a);
            if (fHold > 0)
            {
                --fHold;
            }
            else if (fLevel * fRelease <= fHeldPeak)
            {
                fLevel = std::max(fHeldPeak, fLevel * fRelease);
                fHold = hold;
                fHeldPeak = 0.0f;
            }
            else
            {
                fLevel *= fRelease;
            }
        }
        return fLevel;
    }

    float level() const { return fLevel; }

private:
    double fSampleRate = 48000.0;
    float fAttack = 1.0f, fRelease = 0.99f;
    float fLevel = 0.0f, fHeldPeak = 0.0f;
    int fHold = 0, fDefaultHold = 720;
};

// What the pitch-following pedals share around the note tracker: Track From (the rig's input, the default: the clean
// guitar, much easier to follow than the output of an amp or drive; or the pedal's input), and Lookahead (block
// config "lookahead", ms: the pedal's audio is delayed to match the tracker's output, and the delay is its
// latency). Whichever it tracks, the sound comes from the pedal's input: the note's level (envelope) is always the
// pedal's input's, and so is its velocity (the rig's, scaled by how much louder or quieter the pedal's input is),
// so a volume pedal, gate or drive before it still shapes what it plays.
//
// Tracking (block config "poly", BlockDescriptor::poly; owner, 2026-11-10): Mono, the default, follows single notes
// with the NoteTracker; Poly finds the notes of chords with the PolyNoteFinder instead. In Poly the note process()
// returns stands for the chord: its lowest note's pitch, on while any note sounds, an onset when any note starts; and
// frame() has every note, for the blocks that play them all (the synths, TrackedSynth.hpp). A new frame comes every
// hop (about 5 ms; frameReady()), and with lookahead the frames are whole hops behind.
class TrackedInput {
public:
    static constexpr const char* kTrackNames[] = { "Block In", "Rig In" };
    static constexpr const char* kConfigKey = "lookahead";
    using Frame = PolyNoteFinder::Frame;
    static constexpr int kBands = PolyNoteFinder::kBands;

    // Non-RT
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fNotes.prepare(sampleRate);
        fLevel.prepare(sampleRate);
        fRigLevel.prepare(sampleRate);
        fGainCoef = 1.0f - EnvelopeFollower::coef(sampleRate, 5.0);
        if (fPoly)
            fPoly->prepare(sampleRate);
        const double maxMs = std::max(NoteTracker::kMaxLookaheadMs, PolyNoteFinder::kMaxLookaheadMs);
        const auto size = static_cast<size_t>(std::ceil(maxMs / 1000.0 * sampleRate)) + 2;
        for (std::vector<float>& line : fLines)
            line.assign(size, 0.0f);
        fRigLine.assign(size, 0.0f);
        fPos = 0;
        reset();
    }

    // Non-RT: reads the lookahead and the tracking (Mono or Poly) from a block config
    void configure(const nlohmann::json& config)
    {
        const double ms = config.is_object() ? config.value(kConfigKey, 0.0) : 0.0;
        fNotes.setLookahead(ms);
        if (polyFromConfig(config))
        {
            if (!fPoly)
            {
                fPoly = std::make_unique<PolyNoteFinder>();
                fPoly->setVoices(PolyNoteFinder::kMaxVoices);
                fPoly->setPickFilter(fPickMs);
                fPoly->prepare(fSampleRate);
            }
            fPoly->setLookahead(ms);
        }
        else
        {
            fPoly.reset();
        }
        reset();
    }

    uint32_t latency() const { return fPoly ? fPoly->latency() : fNotes.latency(); }

    void setTrackRig(float choice) { fTrackRig = choice >= 0.5f; }

    void setPickFilter(float ms)
    {
        fPickMs = ms;
        fNotes.setPickFilter(ms);
        if (fPoly)
            fPoly->setPickFilter(ms);
    }

    // Poly tracking is on
    bool poly() const { return fPoly != nullptr; }

    // RT, before each process()
    void setRig(const float* const* input, uint32_t numChannels)
    {
        fRig[0] = input[0];
        fRig[1] = numChannels > 1 ? input[1] : input[0];
    }

    void reset()
    {
        fNotes.reset();
        if (fPoly)
            fPoly->reset();
        fLevel.reset();
        fRigLevel.reset();
        fNote = NoteTracker::Note {};
        fVelocityScale = 1.0f;
        fGain = 0.0f; // (Poly, tracking the rig: up to the block's input's level from there)
        fWasOn = false;
        fFrame = Frame {};
        fFrameReady = false;
        for (std::vector<float>& line : fLines)
            std::fill(line.begin(), line.end(), 0.0f);
        std::fill(fRigLine.begin(), fRigLine.end(), 0.0f);
        fPos = 0;
    }

    // RT. Sample i of the buffer: tracks it (or the rig's input), and delays io[..][i] by the lookahead in place.
    // Returns the note at the (delayed) output.
    const NoteTracker::Note& process(float* const* io, uint32_t numChannels, uint32_t i)
    {
        const bool rig = fTrackRig && fRig[0] != nullptr;
        float in;
        if (rig)
            in = 0.5f * (fRig[0][i] + fRig[1][i]);
        else
            in = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
        if (fPoly)
            return processPoly(io, numChannels, i, in, rig);
        const NoteTracker::Note& note = fNotes.process(in);
        delay(io, numChannels, i, in, fNotes.latency());
        if (!rig)
        {
            fNote = note;
            return fNote;
        }
        // the level from the pedal's own (delayed) input
        const float own = fLevel.process(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i], note.hz);
        if (note.on && (!fWasOn || note.onset))
            fVelocityScale = note.envelope > 1e-6f ? std::min(own / note.envelope, 16.0f) : 1.0f;
        fWasOn = note.on;
        fNote = note;
        fNote.envelope = own;
        fNote.velocity = note.velocity * fVelocityScale;
        return fNote;
    }

    const NoteTracker::Note& note() const { return fNote; }

    // Poly: a new frame of notes came at this sample (with lookahead: the one that many hops behind)
    bool frameReady() const { return fFrameReady; }
    // Poly: the notes now, a band per semitone from PolyNoteFinder::kFirstNote; their levels are the tracked
    // signal's (times polyGain() for the block's own input's)
    const Frame& frame() const { return fFrame; }
    // Poly, tracking the rig: how much louder or quieter the block's input is than the rig's (1 tracking the block's)
    float polyGain() const { return fGain; }
    // Poly: band b's pitch, Hz: as measured, or its semitone's
    double bandHz(int b) const
    {
        const auto i = static_cast<size_t>(b);
        if (fFrame.measured[i] > 0.0)
            return fFrame.measured[i] * fSampleRate;
        return fPoly ? fPoly->bandStep(b) * fSampleRate : 0.0;
    }

private:
    // the block's audio (and the rig's input, for its level) as far behind as the notes
    void delay(float* const* io, uint32_t numChannels, uint32_t i, float& rigIn, uint32_t latency)
    {
        const auto delay = static_cast<size_t>(latency);
        if (delay == 0)
            return;
        const size_t size = fLines[0].size();
        const size_t read = (fPos + size - delay) % size;
        for (uint32_t ch = 0; ch < numChannels; ++ch)
        {
            fLines[ch][fPos] = io[ch][i];
            io[ch][i] = fLines[ch][read];
        }
        fRigLine[fPos] = rigIn;
        rigIn = fRigLine[read];
        fPos = (fPos + 1) % size;
    }

    const NoteTracker::Note& processPoly(float* const* io, uint32_t numChannels, uint32_t i, float in, bool rig)
    {
        fFrameReady = fPoly->process(in);
        float rigNow = in;
        delay(io, numChannels, i, rigNow, fPoly->latency());
        const float own = fLevel.process(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i], fNote.hz);
        if (rig)
        {
            // the notes play at the level of the block's own input: scaled by how much louder or quieter it is
            const float level = fRigLevel.process(rigNow, fNote.hz);
            const float target = level > 1e-5f ? std::min(own / level, 16.0f) : fGain;
            fGain += (target - fGain) * fGainCoef;
        }
        else
        {
            fGain = 1.0f;
        }
        fNote.onset = false;
        if (fFrameReady)
        {
            fFrame = fPoly->playFrame();
            bool on = false;
            float velocity = 0.0f;
            for (int b = 0; b < kBands; ++b)
            {
                const auto k = static_cast<size_t>(b);
                if (!fFrame.sounding[k])
                    continue;
                if (!on)
                    fNote.hz = bandHz(b); // the lowest note's
                on = true;
                if (fFrame.start[k])
                {
                    fNote.onset = true;
                    velocity = std::max(velocity, fFrame.velocity[k]);
                }
            }
            fNote.on = on;
            if (fNote.onset)
                fNote.velocity = velocity * fGain;
        }
        fNote.envelope = own;
        return fNote;
    }

    double fSampleRate = 48000.0;
    NoteTracker fNotes;
    std::unique_ptr<PolyNoteFinder> fPoly; // Poly tracking only
    bool fTrackRig = true;
    float fPickMs = 15.0f; // (the PolyNoteFinder's default, for blocks without a Pick Filter)
    LevelFollower fLevel, fRigLevel;
    NoteTracker::Note fNote;
    float fVelocityScale = 1.0f;
    float fGain = 1.0f, fGainCoef = 1.0f;
    bool fWasOn = false;
    Frame fFrame;
    bool fFrameReady = false;
    const float* fRig[2] = { nullptr, nullptr };
    std::vector<float> fLines[2];
    std::vector<float> fRigLine;
    size_t fPos = 0;
};

} // namespace xythos
