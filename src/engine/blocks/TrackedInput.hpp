// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/dsp/NoteTracker.hpp"
#include "engine/dsp/Svf.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
class TrackedInput {
public:
    static constexpr const char* kTrackNames[] = { "Block In", "Rig In" };
    static constexpr const char* kConfigKey = "lookahead";

    // Non-RT
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fNotes.prepare(sampleRate);
        fLevel.prepare(sampleRate);
        const auto size = static_cast<size_t>(std::ceil(NoteTracker::kMaxLookaheadMs / 1000.0 * sampleRate)) + 2;
        for (std::vector<float>& line : fLines)
            line.assign(size, 0.0f);
        fPos = 0;
    }

    // Non-RT: reads the lookahead from a block config
    void configure(const nlohmann::json& config)
    {
        const double ms = config.is_object() ? config.value(kConfigKey, 0.0) : 0.0;
        fNotes.setLookahead(ms);
        reset();
    }

    uint32_t latency() const { return fNotes.latency(); }

    void setTrackRig(float choice) { fTrackRig = choice >= 0.5f; }
    void setPickFilter(float ms) { fNotes.setPickFilter(ms); }

    // RT, before each process()
    void setRig(const float* const* input, uint32_t numChannels)
    {
        fRig[0] = input[0];
        fRig[1] = numChannels > 1 ? input[1] : input[0];
    }

    void reset()
    {
        fNotes.reset();
        fLevel.reset();
        fNote = NoteTracker::Note {};
        fVelocityScale = 1.0f;
        fWasOn = false;
        for (std::vector<float>& line : fLines)
            std::fill(line.begin(), line.end(), 0.0f);
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
        const NoteTracker::Note& note = fNotes.process(in);
        const auto delay = static_cast<size_t>(fNotes.latency());
        if (delay > 0)
        {
            const size_t size = fLines[0].size();
            const size_t read = (fPos + size - delay) % size;
            for (uint32_t ch = 0; ch < numChannels; ++ch)
            {
                fLines[ch][fPos] = io[ch][i];
                io[ch][i] = fLines[ch][read];
            }
            fPos = (fPos + 1) % size;
        }
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

private:
    double fSampleRate = 48000.0;
    NoteTracker fNotes;
    bool fTrackRig = true;
    LevelFollower fLevel;
    NoteTracker::Note fNote;
    float fVelocityScale = 1.0f;
    bool fWasOn = false;
    const float* fRig[2] = { nullptr, nullptr };
    std::vector<float> fLines[2];
    size_t fPos = 0;
};

} // namespace xythos
