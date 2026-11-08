// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"
#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <cmath>

namespace xythos {

// What the synths share: the note tracker (TrackedInput: Track From, the Pick Filter and Lookahead) with glide and
// the octave, and an amplitude that follows your playing with Attack and Release. A synth renders one stereo sample
// per call from the current note.
class TrackedSynth : public Block {
public:
    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fInput.prepare(sampleRate);
        fMix.prepare(sampleRate);
        updateAmp();
        updateGlide();
        prepareVoice();
        reset();
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
            // (io[..][i] is the dry signal from here on: delayed by the lookahead, if any)
            const NoteTracker::Note& tracked = fInput.process(io, numChannels, i);
            if (tracked.hz > 0.0 && tracked.hz != fTrackedHz)
            {
                fTrackedHz = tracked.hz;
                fTargetHz = targetHz(tracked.hz);
            }
            Note note;
            note.onset = tracked.onset;
            fHz += (fTargetHz - fHz) * fGlideCoef;
            note.hz = std::min(fHz * fOctaveRatio, 0.45 * fSampleRate);
            note.target = std::min(fTargetHz * fOctaveRatio, 0.45 * fSampleRate);
            note.dt = note.hz / fSampleRate;
            note.envelope = tracked.envelope;
            note.on = tracked.on;
            // the guitar's envelope, with Attack as a swell at the start of each note and Release as its tail
            const float level = std::min(1.0f, note.envelope * 4.0f);
            const float target = note.on ? level : 0.0f;
            fAmp += (target - fAmp) * (target > fAmp ? fAmpAttack : fAmpRelease);
            note.amp = fAmp;

            float left, right;
            render(note, left, right);
            const float mix = fMix.next();
            if (fAddsToDry)
            {
                // the synth under the guitar, which always passes (Mix is the synth's level)
                if (numChannels == 1)
                {
                    io[0][i] += 0.5f * (left + right) * mix;
                }
                else
                {
                    io[0][i] += left * mix;
                    io[1][i] += right * mix;
                }
            }
            else if (numChannels == 1)
            {
                io[0][i] = io[0][i] * (1.0f - mix) + 0.5f * (left + right) * mix;
            }
            else
            {
                io[0][i] = io[0][i] * (1.0f - mix) + left * mix;
                io[1][i] = io[1][i] * (1.0f - mix) + right * mix;
            }
        }
    }

    void reset() override
    {
        fInput.reset();
        fTrackedHz = 0.0;
        fTargetHz = fHz = 110.0;
        fAmp = 0.0f;
        resetVoice();
        fSnap = true;
    }

protected:
    struct Note {
        double hz = 110.0;   // with glide and octave
        double target = 110.0; // the tracked note (with the octave), without glide
        double dt = 0.0;     // phase step per sample
        float envelope = 0.0f; // the guitar's level
        float amp = 0.0f;    // the amplitude to play at (0..1)
        bool on = false;     // a note is sounding
        bool onset = false;  // a new pick, this sample
    };

    virtual void prepareVoice() {}
    virtual void resetVoice() {}
    virtual void render(const Note& note, float& left, float& right) = 0;
    // The pitch to play for a tracked pitch (the chip synth snaps it to semitones)
    virtual double targetHz(double tracked) { return tracked; }

    void setOctave(float choice) { fOctaveRatio = std::exp2(std::clamp(static_cast<int>(choice), 0, 3) - 2); }
    void setMix(float percent) { fMix.setTarget(percent / 100.0f); }

    void setAttack(float ms)
    {
        fAttackMs = ms;
        updateAmp();
    }

    void setRelease(float ms)
    {
        fReleaseMs = ms;
        updateAmp();
    }

    void setGlide(float ms)
    {
        fGlideMs = ms;
        updateGlide();
    }

    void setPickFilter(float ms) { fInput.setPickFilter(ms); }
    // The synth is added to the dry signal instead of replacing it by Mix (the Drone Keeper)
    void setAddsToDry(bool adds) { fAddsToDry = adds; }
    void setTrack(float choice) { fInput.setTrackRig(choice); }

public:
    bool configure(const nlohmann::json& config) override
    {
        fInput.configure(config);
        return true;
    }

    uint32_t latency() const override { return fInput.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override { fInput.setRig(input, numChannels); }

protected:

    double fSampleRate = 48000.0;

private:
    void updateAmp()
    {
        fAmpAttack = 1.0f - EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fAttackMs));
        fAmpRelease = 1.0f - EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fReleaseMs));
    }

    void updateGlide() { fGlideCoef = 1.0 - EnvelopeFollower::coef(fSampleRate, std::max(0.1f, fGlideMs)); }

    TrackedInput fInput;
    double fTrackedHz = 0.0;
    Smoother fMix;
    double fOctaveRatio = 1.0;
    float fAttackMs = 5.0f, fReleaseMs = 30.0f, fGlideMs = 10.0f;
    double fTargetHz = 110.0, fHz = 110.0;
    double fGlideCoef = 1.0;
    float fAmp = 0.0f;
    float fAmpAttack = 1.0f, fAmpRelease = 1.0f;
    bool fSnap = true;
    bool fAddsToDry = false;
};

} // namespace xythos
