// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Block.hpp"
#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/Smoother.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace xythos {

// What the synths share: the note tracker (TrackedInput: Track From, the Pick Filter and Lookahead) with glide and
// the octave, and an amplitude that follows your playing with Attack and Release. A synth renders one stereo sample
// per call from the current note.
//
// Poly tracking (the "poly" config; owner, 2026-11-10): the notes of chords (PolyNoteFinder) each get a voice, up to
// six. A voice is another instance of the same synth (Voiced<> below makes them and passes them every control), so
// each note has its own oscillators, filters and envelopes, rendered from its own Note: its pitch (with glide from
// the note it took over, if any, and the octave), and its level in the chord as its envelope. A note that starts a
// semitone or two from one that just ended (a bend or slide) carries on that voice. Mix, Attack and Release are the
// block's.
class TrackedSynth : public Block {
public:
    static constexpr int kVoices = PolyNoteFinder::kMaxVoices;

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        fSampleRate = sampleRate;
        fMaxFrames = maxFrames;
        fInput.prepare(sampleRate);
        fMix.prepare(sampleRate);
        updateAmp();
        updateGlide();
        prepareVoice();
        for (int v = 1; v < kVoices; ++v)
            if (fVoiceSynths[static_cast<size_t>(v)])
                fVoiceSynths[static_cast<size_t>(v)]->prepare(sampleRate, maxFrames);
        reset();
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        if (fInput.poly())
        {
            processPoly(io, numChannels, frames);
            return;
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
            output(io, numChannels, i, left, right);
        }
    }

    void reset() override
    {
        fInput.reset();
        fTrackedHz = 0.0;
        fTargetHz = fHz = 110.0;
        fAmp = 0.0f;
        resetVoice();
        for (PolyVoice& voice : fPolyVoices)
            voice = PolyVoice {};
        fSinceStart = 1e9;
        for (int v = 1; v < kVoices; ++v)
            if (fVoiceSynths[static_cast<size_t>(v)])
                fVoiceSynths[static_cast<size_t>(v)]->resetVoice();
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
    // Poly: another instance of the synth, for a voice (Voiced<> makes them)
    virtual std::unique_ptr<TrackedSynth> makeVoice() const { return nullptr; }
    // Poly: true when the synth plays one sound at a time across all its voices (the Fart's Overlap Single): of the
    // notes starting together, only the lowest starts, none within kSingleWindow of the last start (a strum gives
    // one), and a start silences the other voices (stopSounds())
    virtual bool singleSound() const { return false; }
    // Poly, Single: fades out whatever this voice is playing, quickly
    virtual void stopSounds() {}

    // RT. Passes a control on to the voices (Voiced<> calls it from setParam())
    void forwardParam(int index, float value)
    {
        if (index < 0 || index >= kParamsPerSlot)
            return;
        fParamValues[static_cast<size_t>(index)] = value;
        fParamSet[static_cast<size_t>(index)] = true;
        for (int v = 1; v < kVoices; ++v)
            if (fVoiceSynths[static_cast<size_t>(v)])
                fVoiceSynths[static_cast<size_t>(v)]->setParam(index, value);
    }

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
        // Poly: the voices (the first is this synth itself)
        fVoiceSynths[0] = nullptr;
        for (int v = 1; v < kVoices; ++v)
        {
            std::unique_ptr<TrackedSynth>& synth = fVoiceSynths[static_cast<size_t>(v)];
            if (!fInput.poly())
            {
                synth.reset();
                continue;
            }
            if (!synth)
            {
                synth = makeVoice();
                if (!synth)
                    continue;
                synth->prepare(fSampleRate, fMaxFrames);
                for (int p = 0; p < kParamsPerSlot; ++p)
                    if (fParamSet[static_cast<size_t>(p)])
                        synth->setParam(p, fParamValues[static_cast<size_t>(p)]);
            }
        }
        reset();
        return true;
    }

    uint32_t latency() const override { return fInput.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override { fInput.setRig(input, numChannels); }

protected:

    double fSampleRate = 48000.0;

private:
    // A voice in Poly
    struct PolyVoice {
        int band = -1;     // the band (semitone) it plays, or -1 once released
        int lastBand = -1; // ... and the last it played
        bool on = false;
        bool onset = false; // a new note: the next sample's Note has its onset
        double trackedHz = 0.0, targetHz = 110.0, hz = 110.0;
        float level = 0.0f; // its note's level in the chord
        float amp = 0.0f;
    };

    // a band's level to the envelope a single note of it would have (a note's peaks are higher than its
    // fundamental's level; measured on the synths, so a single note in Poly plays about as loud as in Mono)
    static constexpr float kPolyEnvelope = 1.5f;
    // the voices' sum, so a chord isn't that many times louder than a single note (a single note in Poly is 85% of
    // its Mono level; a triad about 1.5 times it)
    static constexpr float kPolyLevel = 0.85f;

    TrackedSynth* voiceSynth(int v)
    {
        return v == 0 ? this : fVoiceSynths[static_cast<size_t>(v)].get();
    }

    void output(float* const* io, uint32_t numChannels, uint32_t i, float left, float right)
    {
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

    // Single: how long after a start other notes' starts are ignored, in seconds
    static constexpr double kSingleWindow = 0.08;

    void processPoly(float* const* io, uint32_t numChannels, uint32_t frames)
    {
        for (uint32_t i = 0; i < frames; ++i)
        {
            // (io[..][i] is the dry signal from here on: delayed by the lookahead, if any)
            fInput.process(io, numChannels, i);
            if (fInput.frameReady())
                takeFrame(fInput.frame());
            if (fSinceStart < 1e9)
                fSinceStart += 1.0 / fSampleRate;
            if (singleSound())
                singleStarts();
            const float gain = fInput.polyGain();
            float left = 0.0f, right = 0.0f;
            for (int v = 0; v < kVoices; ++v)
            {
                PolyVoice& voice = fPolyVoices[static_cast<size_t>(v)];
                TrackedSynth* synth = voiceSynth(v);
                if (!synth || (!voice.on && voice.amp == 0.0f))
                    continue;
                voice.hz += (voice.targetHz - voice.hz) * fGlideCoef;
                Note note;
                note.onset = voice.onset;
                voice.onset = false;
                note.hz = std::min(voice.hz * fOctaveRatio, 0.45 * fSampleRate);
                note.target = std::min(voice.targetHz * fOctaveRatio, 0.45 * fSampleRate);
                note.dt = note.hz / fSampleRate;
                note.envelope = voice.level * kPolyEnvelope * gain;
                note.on = voice.on;
                const float level = std::min(1.0f, note.envelope * 4.0f);
                const float target = note.on ? level : 0.0f;
                voice.amp += (target - voice.amp) * (target > voice.amp ? fAmpAttack : fAmpRelease);
                if (!voice.on && voice.amp < 1e-5f)
                    voice.amp = 0.0f;
                note.amp = voice.amp;
                float l, r;
                synth->render(note, l, r);
                left += l;
                right += r;
            }
            output(io, numChannels, i, left * kPolyLevel, right * kPolyLevel);
        }
    }

    // Single: keeps the lowest of the notes starting now (if it's not too soon after the last start) and silences the
    // other voices for it
    void singleStarts()
    {
        int start = -1;
        for (int v = 0; v < kVoices; ++v)
        {
            PolyVoice& voice = fPolyVoices[static_cast<size_t>(v)];
            if (!voice.onset || !voiceSynth(v))
                continue;
            if (start < 0 || voice.targetHz < fPolyVoices[static_cast<size_t>(start)].targetHz)
                start = v;
        }
        if (start < 0)
            return;
        const bool tooSoon = fSinceStart < kSingleWindow;
        for (int v = 0; v < kVoices; ++v)
        {
            if (v == start && !tooSoon)
                continue;
            fPolyVoices[static_cast<size_t>(v)].onset = false;
            if (!tooSoon)
                if (TrackedSynth* synth = voiceSynth(v))
                    synth->stopSounds();
        }
        if (!tooSoon)
            fSinceStart = 0.0;
    }

    // The voices take up a frame of notes: new notes get a voice, ended ones release theirs
    void takeFrame(const TrackedInput::Frame& f)
    {
        for (PolyVoice& voice : fPolyVoices)
        {
            if (voice.band >= 0 && !f.sounding[static_cast<size_t>(voice.band)])
            {
                voice.on = false;
                voice.lastBand = voice.band;
                voice.band = -1;
            }
        }
        for (int b = 0; b < TrackedInput::kBands; ++b)
        {
            const auto k = static_cast<size_t>(b);
            if (!f.sounding[k])
                continue;
            int v = findVoice(b);
            bool fresh = false;
            if (v < 0)
            {
                v = takeVoice(b, fresh);
                if (v < 0)
                    continue;
            }
            PolyVoice& voice = fPolyVoices[static_cast<size_t>(v)];
            if (voice.band != b || f.start[k])
                voice.onset = true;
            voice.band = b;
            voice.on = true;
            voice.level = f.level[k];
            const double hz = fInput.bandHz(b);
            if (hz > 0.0 && (hz != voice.trackedHz || fresh))
            {
                voice.trackedHz = hz;
                voice.targetHz = voiceSynth(v)->targetHz(hz);
                if (fresh)
                    voice.hz = voice.targetHz;
            }
        }
    }

    int findVoice(int band) const
    {
        for (int v = 0; v < kVoices; ++v)
            if (fPolyVoices[static_cast<size_t>(v)].band == band)
                return v;
        return -1;
    }

    // A voice for a new note: one that just ended a semitone or two away carries on (a bend or slide: it glides
    // there, and it's `fresh` = false); else a silent one, else the quietest released one
    int takeVoice(int band, bool& fresh)
    {
        int best = -1;
        for (int v = 0; v < kVoices; ++v)
        {
            const PolyVoice& voice = fPolyVoices[static_cast<size_t>(v)];
            if (!voiceSynth(v) || voice.band >= 0 || voice.amp <= 0.0f || std::abs(voice.lastBand - band) > 2)
                continue;
            if (best < 0 || voice.amp > fPolyVoices[static_cast<size_t>(best)].amp)
                best = v;
        }
        if (best >= 0)
        {
            fresh = false;
            return best;
        }
        for (int v = 0; v < kVoices; ++v)
        {
            const PolyVoice& voice = fPolyVoices[static_cast<size_t>(v)];
            if (!voiceSynth(v) || voice.band >= 0)
                continue;
            if (best < 0 || voice.amp < fPolyVoices[static_cast<size_t>(best)].amp)
                best = v;
        }
        fresh = true;
        return best;
    }

    void updateAmp()
    {
        fAmpAttack = 1.0f - EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fAttackMs));
        fAmpRelease = 1.0f - EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fReleaseMs));
    }

    void updateGlide() { fGlideCoef = 1.0 - EnvelopeFollower::coef(fSampleRate, std::max(0.1f, fGlideMs)); }

    TrackedInput fInput;
    double fTrackedHz = 0.0;
    double fSinceStart = 1e9; // Poly, Single: seconds since the last start
    Smoother fMix;
    double fOctaveRatio = 1.0;
    float fAttackMs = 5.0f, fReleaseMs = 30.0f, fGlideMs = 10.0f;
    double fTargetHz = 110.0, fHz = 110.0;
    double fGlideCoef = 1.0;
    float fAmp = 0.0f;
    float fAmpAttack = 1.0f, fAmpRelease = 1.0f;
    bool fSnap = true;
    bool fAddsToDry = false;
    uint32_t fMaxFrames = 0;
    std::array<PolyVoice, kVoices> fPolyVoices {};
    std::array<std::unique_ptr<TrackedSynth>, kVoices> fVoiceSynths {}; // [0] unused: the first voice is this synth
    std::array<float, kParamsPerSlot> fParamValues {};
    std::array<bool, kParamsPerSlot> fParamSet {};
};

// The synth as a block: its controls also go to its voices in Poly, which are more of the same synth. Every synth's
// descriptor makes a Voiced<ItsBlock>.
template <class Synth>
class Voiced final : public Synth {
public:
    void setParam(int index, float value) override
    {
        Synth::setParam(index, value);
        this->forwardParam(index, value);
    }

protected:
    std::unique_ptr<TrackedSynth> makeVoice() const override { return std::make_unique<Synth>(); }
};

} // namespace xythos
