// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/OnsetDetector.hpp"
#include "engine/dsp/PitchTracker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace xythos {

// Turns the pitch tracker's estimates into notes for the synths and pitch pedals: when a note starts, at which
// pitch, and when it ends.
// - The level is a peak follower that holds for a period and then falls fast, so a muted string ends its note
//   within a few ms (the tracker's own follower releases over 80 ms, so notes ran on past every mute).
// - A note starts at a pitch two estimates in a row agree on (about 4 ms apart), with a clear pitch; once it plays,
//   less clear estimates near its pitch still follow it (bends, vibrato, the decay). A doubtful new pitch needs a
//   third estimate: a less clear one (without a pick), or one at a harmonic or subharmonic of the note before or
//   over an octave from it (what the tracker finds while a pick or a finger stops the string: the chirps). A new
//   note without a pick also needs the level not to be falling away (at least half the last 32 ms' peak: else the
//   string is being stopped). Other pitches that never agree end the note after four estimates. A note ends when
//   the level falls back to the noise floor (followed, so hum doesn't hold notes on) or well below the note's
//   peak, or when no clear pitch has been found for 35 ms (palm mutes, scrapes).
// - A new pick (OnsetDetector, confirmed by the level rising after it: a mute clicks too) ends the note before it,
//   so the old pitch doesn't run into the new note; a pitch
//   change without a pick (hammer-ons, slides) moves the note once its new pitch is confirmed.
// - A note picked over and over (picked again within two semitones of its pitch, within 250 ms) plays on through
//   its next picks, each a fresh start (Note::onset) for what plays it: its pitch can't be found again between the
//   picks of a tremolo, a low string's not even once (owner's recordings, 2026-11-09).
// - A mute without a click: the level 9 dB under where it was 64 ms before, once the note is 80 ms old (a string
//   rings down a few dB a second; a finger stops it in 50 to 100 ms, its pitch rising meanwhile, which used to play as
//   a run of semitones). And a note an octave or more below the playing one, without a pick, only while the level
//   holds (a string being stopped falls away at twice its period for a moment).
// - A low note being repeated starts up to 2 semitones sharp at each pick: for 80 ms after the pick that pitch doesn't
//   take over, and the note coming back without a pick is still the repeated one (owner's recordings, 2026-11-12).
// - A note held 500 ms and decayed 6 dB plays on through estimates near its octave or two octaves, with no pick and
//   no swell: its harmonics, once its fundamental has died away (a bass's low B after seven seconds).
// - The Pick Filter: a new note must also hold its pitch for that long before it sounds.
// - Lookahead (optional, up to kMaxLookaheadMs): the output is that much behind the input, and the block delays
//   its audio to match (reporting the latency). Each note then starts back at its pick, at the pitch found
//   after it, so notes start on time without the pick's unsteady pitch, and pick-filter waits cost nothing.
class NoteTracker {
public:
    static constexpr double kMaxLookaheadMs = 30.0;

    struct Note {
        bool on = false;
        double hz = 0.0;       // the pitch; kept after the note ends (0 until the first note)
        float envelope = 0.0f; // the input's level
        bool onset = false;    // a new pick starts a note at this sample
        float velocity = 0.0f; // how hard the note was picked: the input's peak from its pick until it was found
    };

    // Non-RT
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fTracker.prepare(sampleRate);
        fOnsetDetector.prepare(sampleRate);
        fRing.assign(static_cast<size_t>(std::ceil(kMaxLookaheadMs / 1000.0 * sampleRate)) + 2, Note {});
        fAttack = 1.0f - EnvelopeFollower::coef(sampleRate, 0.3);
        fRelease = EnvelopeFollower::coef(sampleRate, 4.0);
        fRecentRelease = EnvelopeFollower::coef(sampleRate, 20.0);
        fFloorFall = 1.0f - EnvelopeFollower::coef(sampleRate, 20.0);
        fFloorRise = static_cast<float>(std::pow(10.0, 6.0 / 20.0 / sampleRate)); // 6 dB/s
        fUnvoicedLimit = static_cast<int64_t>(0.035 * sampleRate);
        fRecentNote = static_cast<int64_t>(0.15 * sampleRate);
        fPickHoldUp = static_cast<int64_t>(0.012 * sampleRate);
        fMuteAge = static_cast<int64_t>(0.08 * sampleRate);
        fRepeatGap = static_cast<int64_t>(0.25 * sampleRate);
        fRepeatHold = static_cast<int64_t>(0.15 * sampleRate);
        fSharpAttack = static_cast<int64_t>(0.08 * sampleRate);
        fOvertoneAge = static_cast<int64_t>(0.5 * sampleRate);
        fAverageCoef = 1.0f - EnvelopeFollower::coef(sampleRate, 3.0);
        fClickFilter.setHighPass(sampleRate, 2000.0);
        fClickDetector.prepare(sampleRate);
        fClickDetector.setThreshold(0.002f);
        fDefaultHold = static_cast<int>(0.015 * sampleRate);
        setPickFilter(fPickMs);
        setLookahead(fLookaheadMs);
        reset();
    }

    // Non-RT (it changes the latency). Clamped to kMaxLookaheadMs.
    void setLookahead(double ms)
    {
        fLookaheadMs = std::clamp(ms, 0.0, kMaxLookaheadMs);
        fLookahead = static_cast<int>(std::lround(fLookaheadMs / 1000.0 * fSampleRate));
        if (!fRing.empty())
            fLookahead = std::min<int>(fLookahead, static_cast<int>(fRing.size()) - 2);
    }

    // The latency the output has, in samples
    uint32_t latency() const { return static_cast<uint32_t>(fLookahead); }

    void setPickFilter(float ms)
    {
        fPickMs = ms;
        fPickHold = static_cast<int64_t>(std::lround(ms * 0.001 * fSampleRate));
    }

    void reset()
    {
        fTracker.reset();
        fOnsetDetector.reset();
        fClickFilter.reset();
        fClickDetector.reset();
        fMisses = 0;
        fEstimates = 0;
        fClickTime = -1;
        fAverage = 0.0f;
        std::fill(fRing.begin(), fRing.end(), Note {});
        fTime = 0;
        fEnvelope = 0.0f;
        fHold = 0;
        fHeldPeak = 0.0f;
        fFloor = 0.0003f;
        fOn = false;
        fSounding = false;
        fHz = 0.0;
        fPeak = 0.0f;
        fRecent = 0.0f;
        fMuted = false;
        fCandidate = Candidate {};
        fPickPending = false;
        fRepeating = fHeldThrough = false;
        fRepeatHz = 0.0;
        fOvertoneHeld = false;
        fLastPick = 0;
        fStartTime = 0;
        fPickPeak = fVelocity = 0.0f;
        fPickTime = 0;
        fLastVoiced = 0;
        fEndTime = -fRecentNote;
        fLevels.fill(0.0f);
        fLevelPos = 0;
        fOut = Note {};
    }

    // One input sample (the signal to track); returns the note at the output (fLookahead samples ago)
    const Note& process(float in)
    {
        ++fTime;
        followLevel(in);
        // a pick: a jump in the level, or in the level of the pick's click (high-passed), which stands out even
        // while a loud note rings
        bool clicked = fOnsetDetector.process(std::fabs(in));
        clicked = fClickDetector.process(std::fabs(fClickFilter.process(in))) || clicked;
        // A click ends the note at once, but it's only a pick (that may start one) once the level rises after it,
        // or holds up: a mute clicks too, and then the level falls (and its tail mustn't start a note)
        if (clicked && fClickTime < 0)
        {
            fClickTime = fTime;
            fClickLevel = fAverage;
            // (a note picked over and over, a tremolo, plays on through its picks: its pitch can't be found again
            // between them, a low one's not even once)
            if (fTime - fLastPick > fRepeatGap)
                fRepeating = false;
            fLastPick = fTime;
            if (fOn && fRepeating)
            {
                fHeldThrough = true;
            }
            else
            {
                if (fOn)
                    end(fTime, EndReason::Pick);
                fTracker.forget();
            }
            fCandidate = Candidate {};
        }
        bool picked = false;
        if (fClickTime >= 0)
        {
            const int64_t since = fTime - fClickTime;
            // (the average level, which a spike barely moves: the peak follower holds a mute's click for a while)
            if (fAverage > kPickRise * fClickLevel || (since >= fPickHoldUp && fAverage >= kPickHold * fClickLevel))
            {
                picked = true;
            }
            else if (since >= fPickHoldUp)
            {
                fClickTime = -1; // the level fell: a mute
                fMuted = true;
                fMuteLevel = fAverage;
                if (fOn && fHeldThrough)
                    end(fTime, EndReason::Mute);
                fHeldThrough = false;
            }
        }
        fTracker.process(in);
        const bool estimated = fTracker.estimates() != fEstimates;
        fEstimates = fTracker.estimates();

        Note& now = at(fTime);
        now = Note { fOn, fHz, fEnvelope, false, fVelocity };
        if (fPickPending)
            fPickPeak = std::max(fPickPeak, fEnvelope);

        if (picked)
        {
            fPickPending = true;
            fPickTime = fClickTime;
            fClickTime = -1;
            fPickPeak = std::max(fClickLevel, fEnvelope);
            fMuted = false;
            if (fOn && fHeldThrough)
            {
                // the note played on through this pick: it starts afresh there, for what plays it
                fVelocity = fPickPeak;
                const int64_t from = std::max(fPickTime, fTime - fLookahead);
                for (int64_t t = from; t <= fTime; ++t)
                    at(t).velocity = fVelocity;
                at(from).onset = true;
                fPeak = fEnvelope;
                fStartTime = fTime;
            }
        }
        if (fOn && fEnvelope < kMuteDrop * fRecent)
        {
            // falling far faster than a ringing string does: muted. Its tail mustn't start a note again.
            end(fTime, EndReason::Mute);
            fMuted = true;
            fMuteLevel = fAverage;
        }
        else if (fMuted && fAverage > 2.0f * fMuteLevel)
        {
            fMuted = false; // swelling back (a volume swell, a note from another string)
        }
        if (!fSounding)
        {
            if (fOn)
                end(fTime, EndReason::Gate);
            fCandidate = Candidate {};
            fPickPending = false;
            fMuted = false;
        }
        else if (estimated)
        {
            estimate(fTracker.voiced() ? fTracker.hz() : 0.0, fTracker.aperiodicity());
        }

        fOut = at(fTime - fLookahead);
        if (fLookahead == 0)
            fOut = Note { fOn, fHz, fEnvelope, now.onset, fVelocity };
        if (fOut.hz <= 0.0)
            fOut.hz = fHz;
        return fOut;
    }

    const Note& note() const { return fOut; }

    // A note is playing and its pitch is clear right now: one note, not a chord (for the poly note finder)
    // (or held through its own harmonic's period: overtone())
    bool clear() const
    {
        return fOn && (fOvertoneHeld || (fTracker.voiced() && fTracker.ownPeriod() && fTracker.aperiodicity() < kSingle));
    }

    // A low note is playing that is being picked over and over (a tremolo), its own period with a fairly clean pitch:
    // one note, though not clear() between the picks (for the poly note finder: the 7-string's A1 picked fast grew
    // chords out of its harmonics). Not over 200 Hz, nor less clean: a triad strummed over and over on the high
    // strings reads as its top note repeated.
    bool repeating() const
    {
        return fOn && fRepeating && fHz < kRepeatSingleHz && fTracker.ownPeriod() && fTracker.voiced()
            && fTracker.aperiodicity() < kRepeatSingle;
    }

    // Why the last note ended (for tests)
    enum class EndReason { None, Pick, Mute, Gate, Unclear, OtherPitch };
    EndReason endReason() const { return fEndReason; }

private:
    struct Candidate {
        double hz = 0.0;
        int count = 0;
        int64_t start = 0;
        float clarity = 0.0f; // the least clear estimate's aperiodicity
    };

    static constexpr float kClear = 0.3f;     // aperiodicity a new note needs
    static constexpr float kSingle = 0.08f;   // ... and under this it's one note alone
    static constexpr float kMinGate = 0.0015f; // about -56 dBFS
    static constexpr float kFloorGate = 4.0f;  // a note starts 12 dB over the noise floor
    static constexpr float kPeakGate = 0.003f; // and ends 50 dB under its peak
    static constexpr float kMuteDrop = 0.1f;   // or 20 dB under the recent level (a mute)
    static constexpr double kRepeatWidth = 2.0; // semitones: a note picked again within this of its pitch is repeated (a
                                                // low string picked hard starts over a semitone sharp)
    static constexpr double kSharpAttackHz = 120.0; // a repeated note under this starts up to 2 semitones sharp at each
                                                    // pick (a bass's E1 read F#1 and G1 for 40-80 ms)
    static constexpr double kRepeatSingleHz = 200.0; // a repeated note under this, its pitch cleaner than
    static constexpr float kRepeatSingle = 0.12f;    // this, is one note alone (repeating())
    static constexpr double kOvertoneWidth = 1.5;    // semitones around an octave or two over a long-held note,
    static constexpr float kOvertoneRise = 1.1f;     // with the level not rising over this share of the last 32 ms',
    static constexpr float kOvertoneDecay = 0.5f;    // and 6 dB or more under its peak: its harmonics (overtone())
    static constexpr float kMuteFall = 0.35f;  // or 9 dB under its level 64 ms before, once it's 80 ms old (a mute)
    static constexpr int kMaxMisses = 4;       // other pitches in a row that end the note without a new one
    static constexpr float kDoubtful = 0.1f;   // a new note less clear than this needs another estimate,
    static constexpr double kHarmonicWidth = 0.4; // and so does one this near (semitones) a harmonic or subharmonic
    static constexpr double kFarJump = 12.5;   // of the note before, or this far from it
    static constexpr float kLegatoLevel = 0.5f; // a new note without a pick needs the level over this share of the
                                                // last 32 ms' (the string isn't being stopped)
    static constexpr float kBelowLevel = 0.8f;  // ... one an octave or more below, over this share of 64 ms before
    static constexpr float kPickRise = 1.25f;  // a click is a pick once the level rises this much,
    static constexpr float kPickHold = 0.3f;   // or once it has held over this share of its level for 12 ms
    static constexpr double kHumWidth = 0.2;  // semitones around 50 and 60 Hz where a new note needs a pick (G1 is
                                               // 0.37 under 50 Hz, A#1 and B1 0.5 around 60 Hz)

    Note& at(int64_t time)
    {
        const auto size = static_cast<int64_t>(fRing.size());
        return fRing[static_cast<size_t>(((time % size) + size) % size)]; // (time is negative at the start)
    }

    static double semitones(double a, double b) { return std::fabs(12.0 * std::log2(a / b)); }

    void followLevel(float in)
    {
        const float a = std::fabs(in);
        fAverage += (a - fAverage) * fAverageCoef;
        // hold for a period of the note (or 15 ms), so the level doesn't ripple between the wave's peaks
        const int hold = fOn && fHz > 0.0 ? static_cast<int>(1.2 * fSampleRate / fHz) : fDefaultHold;
        if (a > fEnvelope)
        {
            fEnvelope += (a - fEnvelope) * fAttack;
            fHold = hold;
            fHeldPeak = 0.0f;
        }
        else
        {
            // then it falls fast, down to the highest peak since: a decaying note's next peak is a little under the
            // held level, so it never restarted the hold, and the level fell away between a low note's peaks (a bass's
            // open A, 36 ms apart: it read as a mute)
            fHeldPeak = std::max(fHeldPeak, a);
            if (fHold > 0)
            {
                --fHold;
            }
            else if (fEnvelope * fRelease <= fHeldPeak)
            {
                fEnvelope = std::max(fHeldPeak, fEnvelope * fRelease);
                fHold = hold;
                fHeldPeak = 0.0f;
            }
            else
            {
                fEnvelope *= fRelease;
            }
        }
        // the recent level: a slower follower, which a muted string's level falls far below
        fRecent = fEnvelope > fRecent ? fEnvelope : fRecent * fRecentRelease;
        // the noise floor: follows the level down quickly, and up only slowly, between notes
        if (fEnvelope < fFloor)
            fFloor += (fEnvelope - fFloor) * fFloorFall;
        else if (!fOn)
            fFloor = std::min(0.03f, fFloor * fFloorRise);

        const float open = std::max(kMinGate, kFloorGate * fFloor);
        if (fOn)
            fPeak = std::max(fPeak, fEnvelope);
        if (!fSounding)
            fSounding = fEnvelope > open;
        else if (fEnvelope < std::max(0.5f * open, fOn ? kPeakGate * fPeak : 0.0f))
            fSounding = false;
    }

    // One estimate: a pitch, or 0 for none
    void estimate(double hz, float aperiodicity)
    {
        // the level over the last few estimates (a string being stopped falls fast)
        fLevels[fLevelPos] = fEnvelope;
        fLevelPos = (fLevelPos + 1) % kLevels;
        // A string rings down a few dB a second; a finger stops it within 50 to 100 ms (a bass's slower), its pitch
        // rising meanwhile: the note ends once the level has fallen 9 dB in 64 ms, and its tail starts no other.
        // (not between a repeated note's picks: each pick stops the string for a moment)
        if (fOn && !holding() && fTime - fStartTime > fMuteAge && fEnvelope < kMuteFall * fLevels[fLevelPos])
        {
            end(fTime, EndReason::Mute);
            fMuted = true;
            fMuteLevel = fAverage;
            return;
        }
        if (fOn && hz > 0.0 && semitones(hz, fHz) < 0.6)
        {
            // the playing note: follow it (bends, vibrato), even when it's less clear
            fHz = hz;
            fLastVoiced = fTime;
            fMisses = 0;
            fCandidate = Candidate {};
            if (fHeldThrough && aperiodicity <= kClear && fClickTime < 0)
            {
                fHeldThrough = false; // found again after the pick it played through: the same note
                fPickPending = false;
            }
            return;
        }
        if (fOn && hz > 0.0 && overtone(hz))
        {
            // within a semitone and a half of a long-held note's octave or two octaves, without a pick, the note decayed
            // and its level not rising: the tracker taking the note's harmonics for its period as its fundamental
            // dies away (owner's recordings, bass-sustain: a low B after seven seconds wobbled between A#1 and C2
            // every 12 ms, each a new note). The note plays on.
            fLastVoiced = fTime;
            fMisses = 0;
            fCandidate = Candidate {};
            fOvertoneHeld = true;
            return;
        }
        fOvertoneHeld = false;
        if (hz <= 0.0 || aperiodicity > kClear)
        {
            // no clear pitch (a chord, a scrape, the decay): the note plays on, for a while
            // (a low note's pitch takes two of its periods to come back after a pick or a slide)
            const int64_t limit = std::max(fUnvoicedLimit, static_cast<int64_t>(2.5 * fSampleRate / std::max(fHz, 20.0)));
            if (fOn && !holding() && fTime - fLastVoiced > limit)
                end(std::max(fTime - fLookahead, fLastVoiced), EndReason::Unclear);
            return;
        }
        // a clear, different pitch: a candidate for the next note
        if (fOn && ++fMisses == 1)
            fFirstMiss = fTime;
        if (!fMuted)
        {
            if (fCandidate.count > 0 && semitones(hz, fCandidate.hz) < 0.5)
            {
                fCandidate.hz = hz;
                ++fCandidate.count;
                fCandidate.clarity = std::max(fCandidate.clarity, aperiodicity);
            }
            else
            {
                fCandidate = Candidate { hz, 1, fTime, aperiodicity };
            }
        }
        // (at the mains' 50 or 60 Hz, between the notes, a new note needs a pick: after an amp the hum is loud
        // enough to start notes in the silence, now that the tracker goes down to a bass's low B)
        const bool hum = !fPickPending && !fOn
                      && (semitones(fCandidate.hz, 50.0) < kHumWidth || semitones(fCandidate.hz, 60.0) < kHumWidth);
        // (an octave or more below the playing note, without a pick, only while the level holds: a string being
        // stopped by a finger falls away at twice its period for a moment)
        const bool below = fOn && !fPickPending && fCandidate.hz < 0.6 * fHz
                        && fEnvelope < kBelowLevel * fLevels[static_cast<size_t>(fLevelPos)];
        // (a low note being repeated, just picked again, read up to 2 semitones sharp: its pick, not a new note)
        const bool sharpAttack = fOn && fHeldThrough && fHz < kSharpAttackHz && fCandidate.hz > fHz
                              && semitones(fCandidate.hz, fHz) < kRepeatWidth && fTime - fLastPick < fSharpAttack;
        const bool confirmed = !fMuted && !sharpAttack && fCandidate.count >= needed() && fTime - fCandidate.start >= fPickHold
                            && fClickTime < 0 && (fPickPending || fEnvelope >= kLegatoLevel * recentLevel()) && !hum
                            && !below;
        if (fOn && (confirmed || (fMisses >= kMaxMisses && !holding())))
            end(fFirstMiss - static_cast<int64_t>(0.004 * fSampleRate), EndReason::OtherPitch);
        if (confirmed)
            start();
    }

    // A pitch near the 2nd or 4th harmonic of the playing note, held 500 ms and decayed 6 dB, with no pick and no
    // swell: its harmonics, not a note. (A note picked an octave up through a high-gain amp shows no pick and no
    // swell, but it comes at the note's full level.)
    bool overtone(double hz) const
    {
        if (fPickPending || fClickTime >= 0 || fTime - fStartTime < fOvertoneAge || hz < fHz
            || fEnvelope > kOvertoneDecay * fPeak || fEnvelope > kOvertoneRise * recentLevel())
            return false;
        const double interval = 12.0 * std::log2(hz / fHz);
        for (const double h : { 12.0, 24.0 })
            if (std::fabs(interval - h) < kOvertoneWidth)
                return true;
        return false;
    }

    // The highest level of the last 32 ms' estimates
    // A note being repeated, a pick not long ago: it plays on while its pitch can't be found between the picks
    bool holding() const { return fRepeating && fTime - fLastPick < fRepeatHold; }

    float recentLevel() const
    {
        float level = 0.0f;
        for (int k = 1; k <= 8; ++k)
            level = std::max(level, fLevels[static_cast<size_t>((fLevelPos - k + kLevels) % kLevels)]);
        return level;
    }

    // How many estimates in a row a new note needs: three for a doubtful one
    int needed() const
    {
        int count = 2;
        // (less clear without a pick; after one, the strings left ringing make even the right pitch less clear)
        if (fCandidate.clarity > kDoubtful && !fPickPending)
            ++count;
        const double reference = fOn || fTime - fEndTime < fRecentNote ? fHz : 0.0;
        if (reference > 0.0)
        {
            const double interval = 12.0 * std::log2(fCandidate.hz / reference);
            const double size = std::fabs(interval);
            // a harmonic or subharmonic of the note before (2, 3 or 4 times, or a 2, 3 or 4th of it): what the
            // tracker finds while a pick or a finger stops the string
            bool harmonic = false;
            for (const double h : { 12.0, 19.02, 24.0 })
                harmonic = harmonic || std::fabs(size - h) < kHarmonicWidth;
            if (harmonic || size > kFarJump)
                ++count;
            // (between a repeated note's picks its harmonics show for longer)
            if (harmonic && holding())
                return 6;
        }
        return std::min(count, 3);
    }

    // The candidate becomes the note
    void start()
    {
        // where the note began: at its pick, else about half an analysis window before its first estimate
        int64_t begin = fCandidate.start - static_cast<int64_t>(0.008 * fSampleRate);
        if (fPickPending)
            begin = fPickTime;
        begin = std::clamp<int64_t>(begin, fTime - fLookahead, fTime);
        // picked again at the pitch it had, soon after: a note being repeated, which plays on through its next picks.
        // (A low one back without a pick, after another pitch took over for a moment: still that note, its pick
        // read sharp; it went on losing its next picks.)
        const bool wasRepeating = fRepeating && fTime - fLastPick < fRepeatGap && fRepeatHz > 0.0 && fRepeatHz < kSharpAttackHz
                               && semitones(fCandidate.hz, fRepeatHz) < kRepeatWidth;
        fRepeating = (fPickPending && fEndReason != EndReason::Gate && fTime - fEndTime < fRepeatGap && fHz > 0.0
                      && semitones(fCandidate.hz, fHz) < kRepeatWidth)
                  || (!fPickPending && wasRepeating && fEndReason == EndReason::OtherPitch);
        if (!fRepeating)
            fRepeatHz = 0.0;
        else if (!wasRepeating)
            fRepeatHz = fCandidate.hz;
        fHeldThrough = false;
        fOn = true;
        fStartTime = fTime;
        fMisses = 0;
        fHz = fCandidate.hz;
        fPeak = fEnvelope;
        fVelocity = fPickPending ? std::max(fPickPeak, fEnvelope) : fEnvelope;
        fLastVoiced = fTime;
        for (int64_t t = begin; t <= fTime; ++t)
        {
            Note& n = at(t);
            n.on = true;
            n.hz = fHz;
            n.onset = false;
            n.velocity = fVelocity;
        }
        at(begin).onset = fPickPending;
        fPickPending = false;
        fCandidate = Candidate {};
    }

    // Ends the note from `from` (back to at most the lookahead)
    void end(int64_t from, EndReason reason)
    {
        fOvertoneHeld = false;
        fOn = false;
        fEndTime = fTime;
        fEndReason = reason;
        fHeldThrough = false;
        for (int64_t t = std::max(from, fTime - fLookahead + 1); t <= fTime; ++t)
            at(t).on = false;
    }

    double fSampleRate = 48000.0;
    PitchTracker fTracker;
    OnsetDetector fOnsetDetector;
    Biquad fClickFilter;
    OnsetDetector fClickDetector;
    std::vector<Note> fRing;
    int fLookahead = 0;
    double fLookaheadMs = 0.0;
    float fPickMs = 0.0f;
    int64_t fPickHold = 0;
    int64_t fTime = 0;

    float fAttack = 1.0f, fRelease = 0.99f, fFloorFall = 0.001f, fFloorRise = 1.0f;
    float fRecentRelease = 0.999f;
    float fEnvelope = 0.0f, fFloor = 0.0003f, fPeak = 0.0f, fRecent = 0.0f, fHeldPeak = 0.0f;
    bool fMuted = false;
    int fHold = 0, fDefaultHold = 720;
    bool fSounding = false;

    bool fOn = false;
    double fHz = 0.0;
    Candidate fCandidate;
    bool fPickPending = false;
    float fPickPeak = 0.0f, fVelocity = 0.0f;
    int64_t fPickTime = 0, fLastVoiced = 0, fStartTime = 0;
    int64_t fMuteAge = 3840;
    bool fRepeating = false;   // the note is being picked over and over
    bool fHeldThrough = false; // ... and is playing on through a pick, its pitch not found again yet
    double fRepeatHz = 0.0;    // ... at this pitch
    int64_t fSharpAttack = 4800, fOvertoneAge = 24000;
    bool fOvertoneHeld = false; // the last estimate was the playing note's harmonic (overtone())
    int64_t fLastPick = 0, fRepeatGap = 12000, fRepeatHold = 7200;
    int64_t fUnvoicedLimit = 1680;
    int fMisses = 0;
    int64_t fFirstMiss = 0;
    uint32_t fEstimates = 0;
    int64_t fClickTime = -1, fPickHoldUp = 384;
    float fClickLevel = 0.0f;
    float fAverage = 0.0f, fAverageCoef = 0.007f;
    float fMuteLevel = 0.0f; // the average level when it was muted: a swell back over twice it unmutes
    Note fOut;
    EndReason fEndReason = EndReason::None;
    int64_t fEndTime = 0, fRecentNote = 7200;
    static constexpr int kLevels = 16; // 64 ms of estimates
    std::array<float, kLevels> fLevels {};
    int fLevelPos = 0;
};

} // namespace xythos
