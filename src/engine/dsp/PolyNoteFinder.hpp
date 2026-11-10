// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/NoteTracker.hpp"
#include "engine/dsp/OnsetDetector.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace xythos {

// Finds the notes of chords (the pitch-following pedals' Poly tracking, blocks/TrackedInput.hpp, and the MIDI Out's
// Poly mode), A0..E6: a 5-string bass in drop A up to
// the top of a 24-fret guitar. Rewritten on 2026-11-09 around the owner's DI recordings of a 7-string guitar and a
// 5-string bass (tests/recordings); the semitone filter bank before it could not tell a note's harmonics from notes
// (on those recordings: 401 s of notes nobody played in 842 s of note time, 328 s of it harmonics of single notes).
//
// What it rests on, measured on the recordings: a played note always has its own fundamental in the signal, never more
// than about 23 dB under its loudest harmonic. So every note is a peak of the spectrum, and the question for each
// peak is only whether it is a note or a harmonic of a note below it.
//
// - One note alone is found sooner and more surely by its period than by its spectrum, so the single-note tracker
//   (NoteTracker.hpp) runs alongside. The note it plays is a note here at once, if its fundamental is in the long
//   spectrum too (notes ringing together can pass for one note at their common period for a moment). While it plays
//   one clearly (and for 150 ms after: between a repeated note's picks it isn't clear), nothing else starts, and what
//   was found in the 100 ms before was its pick. All that is at its harmonics is learnt as its own.
// - For chords the input is decimated to about 12 kHz, and every 5.3 ms (a hop) three spectra are taken of it (Hann
//   windows): 171 ms long for what's under 150 Hz, where the partials of a low chord are 25 Hz apart (an open G's G2,
//   B2 and D3), 85 ms up to 1 kHz, and 43 ms above. Their peaks (frequency and level from the two highest bins) are
//   gone through from the lowest up.
// - A peak is a note when enough of it is left unexplained by the notes found below it (half; a quarter keeps a
//   playing one). A note explains its harmonics' peaks up to what it has been seen to have there: for its first 50 ms
//   up to Focus times its fundamental (3 by default; twice that for its 3rd and 6th harmonics and four times for the
//   others, which taken for notes add a third or a seventh nobody played), then what it had, with 3 dB to spare,
//   followed slowly (strings ringing together beat). The fundamental is read from the same spectrum as the harmonic:
//   a new note fills the short windows long before the long one. So a note picked later on a harmonic of one still
//   ringing (an arpeggio's octave) shows as a jump, and is found.
// - Picked together, a note and its octave are one note, unless the 2nd harmonic is over 1.9 times the 1st and 3rd and
//   the even harmonics 1.7 times the odd ones: on the recordings a single string's almost never is (1 note in 100),
//   an octave shape's mostly is.
// - A new note in a chord must be found four hops in a row at a steady pitch, with two of its own 2nd to 5th
//   harmonics there, not falling, within 24 dB of the loudest peak, 40 ms or more after a pick (so a single note is
//   found first; longer under a low peak) and at most 400 ms after one (fingers moving on the strings make peaks
//   too), and not at a 2nd to 4th harmonic of a peak that isn't a playing note yet (a string's harmonics show in the
//   shorter windows before its fundamental has settled). All notes stand over the noise floor of their bins (learnt in
//   the first 250 ms, then down quickly, up only while nothing sounds), and not over the input's own level (a note
//   louder than the input only rings on in the window, after a mute). A playing note that is briefly not found keeps
//   sounding for up to 45 ms, unless the input falls meanwhile. The Pick Filter: a new note sounds once found for that
//   long. Voices: the quietest notes over the count go.
//
// What it doesn't do (2026-11-09): palm-muted chords chugged fast are mostly gone before they are let in; octaves
// and fifths doubled within a strummed chord are often one note (the chord's every pitch is there, in another
// octave); a chord's notes come 50 to 150 ms after the strum, the low ones last.
//
// Each hop's result is a Frame. With lookahead the frames are played that many whole hops later, and a new note is
// written back over the time it took to find, so it starts on time.
class PolyNoteFinder {
public:
    static constexpr int kFirstNote = 21; // A0 (MIDI), a 5-string bass in drop A (owner, 2026-11-09; was B0)
    static constexpr int kLastNote = 88;  // E6, the top of a 24-fret guitar
    static constexpr int kBands = kLastNote - kFirstNote + 1;
    static constexpr int kMaxVoices = 6;
    static constexpr double kMaxLookaheadMs = 30.0;

    struct Frame {
        std::array<float, kBands> level {};     // each note's level (a band is a semitone)
        std::array<double, kBands> measured {}; // its pitch, as a phase step per sample (0: not measured)
        std::array<bool, kBands> sounding {};   // a note plays in it
        std::array<bool, kBands> start {};      // ... and starts in this frame
        std::array<float, kBands> velocity {};  // the starting note's level (when its start is set)
    };

    // Non-RT
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fDecimation = std::max(1, static_cast<int>(std::lround(sampleRate / 12000.0)));
        fRate = sampleRate / fDecimation;
        fHop = kHopSize * fDecimation;
        fHopMs = 1000.0f * static_cast<float>(fHop / sampleRate);
        fHighPass.setHighPass(sampleRate, 22.0);
        for (Biquad& filter : fAntiAlias)
            filter.setLowPass(sampleRate, std::min(0.4 * fRate, 0.45 * sampleRate));
        fLong.prepare(kLongSize);
        fMid.prepare(kMidSize);
        fShort.prepare(kShortSize);
        fFrames.assign(static_cast<size_t>(std::ceil(kMaxLookaheadMs / fHopMs)) + 2, Frame {});
        for (int b = 0; b < kBands; ++b)
            fBandStep[static_cast<size_t>(b)] = 440.0 * std::exp2((kFirstNote + b - 69) / 12.0) / sampleRate;
        fMono.prepare(sampleRate);
        fOnset.prepare(sampleRate);
        fClick.prepare(sampleRate);
        fClick.setThreshold(0.002f);
        fClickFilter.setHighPass(sampleRate, 2000.0);
        fEnvAttack = 1.0f - EnvelopeFollower::coef(sampleRate, 0.3);
        fEnvRelease = EnvelopeFollower::coef(sampleRate, 4.0);
        fEnvHold = static_cast<int>(sampleRate / 30.0);
        fFloorFall = 1.0f - static_cast<float>(std::exp(-fHopMs / kFloorFallMs));
        for (int r = 0; r < 2; ++r)
            fFloorRise[r] = static_cast<float>(std::pow(10.0, kFloorRiseDb[r] / 20.0 * fHopMs / 1000.0));
        fLearnHops = static_cast<int>(std::lround(kLearnMs / fHopMs));
        fLearnCoef = 1.0f - static_cast<float>(std::exp(-fHopMs / 30.0));
        fLostHops = static_cast<int>(std::lround(kLostMs / fHopMs));
        fRepickHops = static_cast<int>(std::lround(kRepickMs / fHopMs));
        fPickedHops = static_cast<int>(std::lround(kPickedMs / fHopMs));
        fRatioHops = static_cast<int>(std::lround(kRatioMs / fHopMs));
        fYoungHops = static_cast<int>(std::lround(kYoungMs / fHopMs));
        fChordWaitHops = static_cast<int>(std::lround(kChordWaitMs / fHopMs));
        fClearHops = static_cast<int>(std::lround(150.0 / fHopMs));
        fPickWindowHops = static_cast<int>(std::lround(kPickWindowMs / fHopMs));
        fOctaveHops = static_cast<int>(std::lround(300.0 / fHopMs));
        fRatioRise = static_cast<float>(std::pow(10.0, kRatioRiseDb / 20.0 * fHopMs / 1000.0));
        fRatioCoef = 1.0f - static_cast<float>(std::exp(-fHopMs / kRatioFollowMs));
        setLookahead(fLookaheadMs);
        reset();
    }

    // Non-RT (it changes the latency)
    void setLookahead(double ms)
    {
        fLookaheadMs = std::clamp(ms, 0.0, kMaxLookaheadMs);
        if (!fFrames.empty())
            // (whole hops, and never more than kMaxLookaheadMs: the blocks size their delay lines for that)
            fDelayHops = std::clamp(static_cast<int>(std::lround(fLookaheadMs / fHopMs)), 0,
                                    std::min(static_cast<int>(fFrames.size()) - 2, static_cast<int>(kMaxLookaheadMs / fHopMs)));
    }

    uint32_t latency() const { return static_cast<uint32_t>(fDelayHops * fHop); }
    void setPickFilter(float ms) { fPickMs = ms; }
    // How far over its fundamental a chord note's harmonic can be and still be its harmonic, not a note there: Focus
    // x 1.3 (3 by default: the old Poly Chip's 70%)
    void setFocus(float ratio) { fPrior = std::clamp(1.3f * ratio, 0.15f, 10.5f); }
    void setVoices(int voices) { fVoices = std::clamp(voices, 1, kMaxVoices); }

    // How many picks it has heard (for tests)
    int picks() const { return fPicks; }

    // The phase step per sample of a band's own semitone
    double bandStep(int b) const { return fBandStep[static_cast<size_t>(b)]; }

    void reset()
    {
        fHighPass.reset();
        for (Biquad& filter : fAntiAlias)
            filter.reset();
        fRing.fill(0.0f);
        fWrite = 0;
        fPhase = 0;
        fSinceHop = 0;
        fLong.reset();
        fMid.reset();
        fShort.reset();
        for (Track& track : fTracks)
            track = Track {};
        fMono.reset();
        fMonoOn = false;
        fMonoHz = 0.0;
        fOnset.reset();
        fClick.reset();
        fClickFilter.reset();
        fEnvelope = 0.0f;
        fHeldPeak = 0.0f;
        fHold = 0;
        fPickPending = false;
        fSincePick = 1 << 20;
        fPickHop = 0;
        fPicks = 0;
        std::fill(fFrames.begin(), fFrames.end(), Frame {});
        fHopCount = 0;
        fLearning = true;
        fSounding = 0;
        fLevels.fill(0.0f);
        fLevelPos = 0;
        fFalling = fMonoWasOn = false;
        fSinceClear = 1 << 20;
        fMonoAge = 0;
        fMonoWasHz = 1.0;
    }

    // One input sample; true when a new frame is ready to play (playFrame())
    bool process(float in)
    {
        followLevel(in);
        // a pick: a jump in the level, or in the pick's click (high-passed), which stands out while notes ring
        bool picked = fOnset.process(std::fabs(in));
        picked = fClick.process(std::fabs(fClickFilter.process(in))) || picked;
        if (picked)
        {
            fPickPending = true;
            ++fPicks;
        }
        const NoteTracker::Note& mono = fMono.process(in);
        fMonoOn = mono.on;
        if (mono.on)
            fMonoHz = mono.hz;

        const float x = fAntiAlias[1].process(fAntiAlias[0].process(fHighPass.process(in)));
        if (++fPhase >= fDecimation)
        {
            fPhase = 0;
            fRing[static_cast<size_t>(fWrite)] = x;
            fWrite = (fWrite + 1) % kRing;
        }
        if (++fSinceHop < fHop)
            return false;
        fSinceHop = 0;
        findNotes();
        return true;
    }

    // The frame to play now (the newest, or the one the lookahead behind)
    const Frame& playFrame() { return frame(fHopCount - 1 - fDelayHops); }

private:
    static constexpr int kHopSize = 64;    // decimated samples a hop (5.3 ms)
    static constexpr int kLongSize = 2048; // 171 ms: the low partials
    static constexpr int kMidSize = 1024;  // 85 ms: the middle
    static constexpr int kShortSize = 512; // 43 ms: the top
    static constexpr int kRing = 2048;
    static constexpr double kSplitHz = 150.0; // peaks under this come from the long spectrum,
    static constexpr double kMidHz = 1000.0;  // under this from the middle one
    static constexpr double kTopHz = 5000.0;
    static constexpr int kMaxPeaks = 96;
    static constexpr int kPartials = 12;
    static constexpr int kMaxHarmonic = 40;

    static constexpr float kFloor = 0.0001f;    // a peak a note needs (-80 dBFS; DI recordings are quiet)
    static constexpr float kFloorGate = 4.0f;   // ... and 12 dB over its bins' noise floor
    static constexpr float kPeakRange = 0.003f; // peaks 50 dB under the loudest don't count at all
    static constexpr float kNewShare = 0.5f;    // a new note: this much of its peak not a lower note's harmonic,
    static constexpr float kKeepShare = 0.25f;  // a playing one this much
    static constexpr float kNewLevel = 0.06f;   // a new note: within 24 dB of the loudest peak,
    static constexpr float kKeepLevel = 0.01f;  // a playing one within 40 dB
    static constexpr float kSupport = 0.1f;     // a new note needs two of its 2nd to 5th harmonics, within 20 dB of it
    static constexpr double kTwoHarmonicsHz = 1000.0; // (one, from here up)
    static constexpr double kSettled = 0.008;   // ... its pitch within 14 cents of where it was the hop before,
    static constexpr float kNotFalling = 0.8f;  // its level over this share of what it was when first found,
    static constexpr double kChordWaitMs = 40.0; // and this long since the pick: a single note is found by then
    static constexpr double kPickWindowMs = 400.0; // ... and no longer than this: a note in a chord starts with a pick
                                                 // (fingers moving on the strings make peaks too)
    static constexpr float kOctave = 1.9f;      // a 2nd harmonic this many times the note's 1st and 3rd: a note there
    static constexpr float kEvenOdd = 1.7f;
    static constexpr float kOctaveShare = 0.35f; // ... which needs this much of the peak its own
    static constexpr float kBelowPeak = 0.1f;   // a peak under a new one, within 20 dB of it: its fundamental
    static constexpr float kMonoFundamental = 0.06f; // the single note's fundamental against its harmonics in the long spectrum
    static constexpr float kLowPeak = 0.1f;     // (longer under a low peak, within 20 dB of the loudest)
    static constexpr float kMuteFall = 0.35f;   // the input 9 dB under its level 64 ms before: muted, nothing starts
    static constexpr double kYoungMs = 100.0;   // notes this new are dropped when one note turns out to play alone
    static constexpr float kAlonePrior = 16.0f; // a note alone has all that's at its harmonics, up to 24 dB over it
    static constexpr float kHeadroom = 1.4f;    // a note's harmonics may be 3 dB over what they were
    static constexpr double kRatioMs = 50.0;    // a note's harmonics are learnt this long,
    static constexpr double kRatioFollowMs = 120.0; // then followed this slowly,
    static constexpr double kRatioRiseDb = 4.0; // and under a note playing there may only grow this fast (dB/s)
    static constexpr int kNewHops = 4;          // hops in a row a new note must be found,
    static constexpr int kPickedNewHops = 2;    // ... just after a pick
    static constexpr double kPickedMs = 80.0;
    static constexpr double kLostMs = 45.0;     // a playing note not found keeps sounding this long
    static constexpr float kLostFall = 0.6f;    // ... unless it falls under this share of its level meanwhile
    static constexpr float kStale = 1.6f;       // a note this much louder than the input rings on in the window only
    static constexpr float kPickDecay = 0.6f;   // a new note falling below this share of its peak while held: the pick
    static constexpr double kRepickMs = 40.0;   // after a pick, a jump in a playing note's level within this: picked again
    static constexpr float kRepickJump = 1.5f;
    static constexpr double kLearnMs = 250.0;   // the noise floors are learnt this long after a reset (no notes)
    static constexpr float kLearnLevel = 0.005f; // ... while the input stays under this (-46 dBFS): playing
    static constexpr double kFloorFallMs = 300.0;
    // dB/s: while nothing sounds; not while notes do (a floor creeping up under a note held for 20 seconds cut it off)
    static constexpr double kFloorRiseDb[2] = { 20.0, 0.0 };
    static constexpr double kMoveWidth = 0.6;   // semitones from its band's note where a bent note changes band

    // A spectrum of the newest samples: a Hann window and a radix-2 FFT, the bins' levels (a sine of level 1 reads 1)
    // and their noise floors
    template <int size>
    struct Spectrum {
        void prepare(int n)
        {
            fSize = n;
            fBits = 0;
            while ((1 << fBits) < n)
                ++fBits;
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
            {
                fWindow[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * (i + 0.5) / n));
                sum += fWindow[static_cast<size_t>(i)];
                int r = 0;
                for (int b = 0; b < fBits; ++b)
                    r |= ((i >> b) & 1) << (fBits - 1 - b);
                fReverse[static_cast<size_t>(i)] = static_cast<uint16_t>(r);
            }
            for (int i = 0; i < n / 2; ++i)
            {
                fCos[static_cast<size_t>(i)] = static_cast<float>(std::cos(2.0 * M_PI * i / n));
                fSin[static_cast<size_t>(i)] = static_cast<float>(-std::sin(2.0 * M_PI * i / n));
            }
            fScale = static_cast<float>(2.0 / sum);
            reset();
        }

        void reset()
        {
            fLevel.fill(0.0f);
            fNoise.fill(0.0f);
        }

        // ring: the samples, `write` the next to be written (so write - 1 is the newest)
        void analyse(const std::array<float, kRing>& ring, int write)
        {
            const int n = fSize;
            for (int i = 0; i < n; ++i)
            {
                const float x = ring[static_cast<size_t>((write - n + i + 2 * kRing) % kRing)];
                const auto r = static_cast<size_t>(fReverse[static_cast<size_t>(i)]);
                fRe[r] = x * fWindow[static_cast<size_t>(i)];
                fIm[r] = 0.0f;
            }
            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1, step = n / len;
                for (int start = 0; start < n; start += len)
                    for (int k = 0; k < half; ++k)
                    {
                        const float c = fCos[static_cast<size_t>(k * step)], s = fSin[static_cast<size_t>(k * step)];
                        const auto a = static_cast<size_t>(start + k), b = static_cast<size_t>(start + k + half);
                        const float re = fRe[b] * c - fIm[b] * s, im = fRe[b] * s + fIm[b] * c;
                        fRe[b] = fRe[a] - re;
                        fIm[b] = fIm[a] - im;
                        fRe[a] += re;
                        fIm[a] += im;
                    }
            }
            for (int k = 0; k <= n / 2; ++k)
            {
                const auto i = static_cast<size_t>(k);
                fLevel[i] = fScale * std::sqrt(fRe[i] * fRe[i] + fIm[i] * fIm[i]);
            }
        }

        int fSize = size, fBits = 0;
        float fScale = 1.0f;
        std::array<float, size> fWindow {}, fRe {}, fIm {};
        std::array<float, size / 2> fCos {}, fSin {};
        std::array<uint16_t, size> fReverse {};
        std::array<float, size / 2 + 1> fLevel {}, fNoise {};
    };

    struct Peak {
        double hz = 0.0;
        float level = 0.0f; // its level
        float rest = 0.0f;  // ... and what the notes found so far leave of it
        float noise = 0.0f; // its bins' noise floor
        float slack = 0.0f; // how far off its frequency may be (Hz): a tenth of a bin
        int window = 0;     // which spectrum it's from: 0 long, 1 middle, 2 short
        bool octave = false; // a note an octave over a lower one, by how loud it is against that one's 1st and 3rd
    };

    // A note being followed, in the band of its semitone
    struct Track {
        bool active = false;   // found, now or lately
        bool sounding = false; // ... and past the wait for new notes and the Pick Filter
        bool found = false;    // in this hop
        double hz = 0.0;
        float level = 0.0f;    // its level: its fundamental's and its harmonics' together
        float own = 0.0f;      // its fundamental's
        float first = 0.0f;    // its level when first found
        int age = 0;           // hops since it was first found
        int run = 0;           // hops in a row it has been found
        int waited = 0;        // hops found while the Pick Filter holds it
        float pending = 0.0f;  // its peak level meanwhile
        int lost = 0;          // hops a sounding note hasn't been found
        float lostLevel = 0.0f;
        float atPick = 0.0f;   // its level at the last pick (a jump from it: picked again)
        bool mono = false;     // found by the single-note tracker
        bool hadPeak = false;  // ... and with a peak of its own in the spectra, at some time
        bool octave = false;   // found as the octave over another note
        std::array<float, kPartials> ratio {}; // its harmonics' levels against its fundamental's
    };

    // The input's level: a peak follower holding for a low B's period, then falling fast to the highest peak since
    void followLevel(float in)
    {
        const float a = std::fabs(in);
        if (a > fEnvelope)
        {
            fEnvelope += (a - fEnvelope) * fEnvAttack;
            fHold = fEnvHold;
            fHeldPeak = 0.0f;
        }
        else
        {
            fHeldPeak = std::max(fHeldPeak, a);
            if (fHold > 0)
            {
                --fHold;
            }
            else if (fEnvelope * fEnvRelease <= fHeldPeak)
            {
                fEnvelope = std::max(fHeldPeak, fEnvelope * fEnvRelease);
                fHold = fEnvHold;
                fHeldPeak = 0.0f;
            }
            else
            {
                fEnvelope *= fEnvRelease;
            }
        }
    }

    Frame& frame(int64_t hop)
    {
        const auto size = static_cast<int64_t>(fFrames.size());
        return fFrames[static_cast<size_t>(((hop % size) + size) % size)];
    }

    static double midiOf(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

    // The bins' noise floors: learnt after a reset, then down quickly to the bin's level and up slowly
    template <int size>
    void followFloor(Spectrum<size>& s) const
    {
        const float rise = fFloorRise[fSounding == 0 ? 0 : 1];
        for (size_t k = 0; k < s.fNoise.size(); ++k)
        {
            float& floor = s.fNoise[k];
            const float level = s.fLevel[k];
            if (fLearning)
                floor += (level - floor) * fLearnCoef;
            else if (level < floor)
                floor += (level - floor) * fFloorFall;
            else
                floor = std::min(0.03f, std::max(floor, 1e-7f) * rise);
        }
    }

    // The peaks of a spectrum between two frequencies, added to fPeaks (lowest first)
    template <int size>
    void addPeaks(const Spectrum<size>& s, double fromHz, double toHz, float loudest, int window)
    {
        const double binHz = fRate / s.fSize;
        const int from = std::max(2, static_cast<int>(std::ceil(fromHz / binHz)));
        const int to = std::min(s.fSize / 2 - 2, static_cast<int>(toHz / binHz));
        for (int k = from; k <= to && fPeakCount < kMaxPeaks; ++k)
        {
            const auto i = static_cast<size_t>(k);
            const float b = s.fLevel[i];
            if (b <= s.fLevel[i - 1] || b < s.fLevel[i + 1] || b < kFloor || b < kPeakRange * loudest)
                continue;
            // A Hann window's peak from its two highest bins (Grandke): their ratio gives how far between them the
            // sine is, and its level. (A parabola through three bins was up to a quarter of a semitone off for a
            // low note, enough to miss its own harmonics.)
            const bool up = s.fLevel[i + 1] > s.fLevel[i - 1];
            const double ratio = (up ? s.fLevel[i + 1] : s.fLevel[i - 1]) / b;
            const double shift = std::clamp((2.0 * ratio - 1.0) / (ratio + 1.0), 0.0, 0.5);
            const double sinc = shift > 1e-6 ? std::sin(M_PI * shift) / (M_PI * shift) : 1.0;
            Peak& peak = fPeaks[static_cast<size_t>(fPeakCount++)];
            peak.hz = (k + (up ? shift : -shift)) * binHz;
            peak.level = static_cast<float>(b * (1.0 - shift * shift) / sinc);
            peak.rest = peak.level;
            peak.noise = std::max({ s.fNoise[i - 1], s.fNoise[i], s.fNoise[i + 1] });
            peak.slack = static_cast<float>(0.1 * binHz);
            peak.window = window;
            peak.octave = false;
        }
    }

    // The peak nearest a frequency, within `low` under and `high` over it (as shares of it), or -1
    int peakNear(double hz, double low, double high, double slack) const
    {
        int best = -1;
        double nearest = 1e9;
        const double from = hz * (1.0 - low) - slack, to = hz * (1.0 + high) + slack;
        for (int p = 0; p < fPeakCount; ++p)
        {
            const double f = fPeaks[static_cast<size_t>(p)].hz;
            if (f < from)
                continue;
            if (f > to)
                break;
            if (std::fabs(f - hz) < nearest)
            {
                nearest = std::fabs(f - hz);
                best = p;
            }
        }
        return best;
    }

    // The level of a sine at a frequency in a spectrum, from the bin nearest it (0 under the 2nd bin)
    template <int size>
    float levelIn(const Spectrum<size>& s, double hz) const
    {
        const double bin = hz * s.fSize / fRate;
        const auto k = static_cast<int>(std::lround(bin));
        if (k < 2 || k >= s.fSize / 2)
            return 0.0f;
        const double off = std::fabs(bin - k);
        const double lobe = off > 1e-6 ? std::sin(M_PI * off) / (M_PI * off) / (1.0 - off * off) : 1.0;
        return static_cast<float>(s.fLevel[static_cast<size_t>(k)] / lobe);
    }

    bool stale(float level) const { return level > kStale * fEnvelope; }

    // One hop: the spectra, their peaks, the notes among them, and the frame
    void findNotes()
    {
        fLong.analyse(fRing, fWrite);
        fMid.analyse(fRing, fWrite);
        fShort.analyse(fRing, fWrite);
        if (fLearning && (fHopCount >= fLearnHops || fEnvelope > kLearnLevel))
            fLearning = false;
        followFloor(fLong);
        followFloor(fMid);
        followFloor(fShort);

        const double splitBin = kSplitHz / (fRate / kLongSize);
        float loudest = 0.0f;
        for (int k = 4; k < static_cast<int>(splitBin); ++k)
            loudest = std::max(loudest, fLong.fLevel[static_cast<size_t>(k)]);
        for (int k = static_cast<int>(kSplitHz / (fRate / kMidSize)); k < static_cast<int>(kMidHz / (fRate / kMidSize)); ++k)
            loudest = std::max(loudest, fMid.fLevel[static_cast<size_t>(k)]);
        for (int k = static_cast<int>(kMidHz / (fRate / kShortSize)); k < kShortSize / 2 - 2; ++k)
            loudest = std::max(loudest, fShort.fLevel[static_cast<size_t>(k)]);
        fPeakCount = 0;
        addPeaks(fLong, 24.0, kSplitHz, loudest, 0);
        addPeaks(fMid, kSplitHz, kMidHz, loudest, 1);
        addPeaks(fShort, kMidHz, kTopHz, loudest, 2);

        // New notes wait after a pick until the single-note tracker would have found a note playing alone: two and
        // a half periods of the lowest peak there is, and a little
        fWaitHops = fChordWaitHops;
        for (int p = 0; p < fPeakCount; ++p)
            if (fPeaks[static_cast<size_t>(p)].level >= kLowPeak * loudest)
            {
                const double ms = 2500.0 / fPeaks[static_cast<size_t>(p)].hz + 15.0;
                fWaitHops = std::max(fWaitHops, static_cast<int>(std::lround(std::min(ms, 110.0) / fHopMs)));
                break;
            }
        for (Track& track : fTracks)
            track.found = false;
        // the input falling fast: muted
        fFalling = fEnvelope < kMuteFall * fLevels[static_cast<size_t>(fLevelPos)];
        fLevels[static_cast<size_t>(fLevelPos)] = fEnvelope;
        fLevelPos = (fLevelPos + 1) % static_cast<int>(fLevels.size());
        // the single-note tracker's note ended by a mute or by dying away: gone here too (it rings on in the spectra)
        // (or, the input falling, lost for no clear pitch: a mute with no click)
        const NoteTracker::EndReason why = fMono.endReason();
        const bool quieter = fEnvelope < 0.6f * fLevels[static_cast<size_t>(fLevelPos)];
        if (fMonoWasOn && !fMonoOn
            && (why == NoteTracker::EndReason::Mute || why == NoteTracker::EndReason::Gate
                || (quieter && (why == NoteTracker::EndReason::Unclear || why == NoteTracker::EndReason::OtherPitch))))
            for (Track& track : fTracks)
                if (track.mono)
                    track = Track {};
        fMonoWasOn = fMonoOn;

        // the single-note tracker's note: there at once, and its harmonics start no notes while it's clear
        const bool monoOn = fMonoOn && fMonoHz > 0.0 && !fLearning;
        int monoBand = -1;
        if (monoOn)
        {
            const int band = static_cast<int>(std::lround(midiOf(fMonoHz))) - kFirstNote;
            if (band >= 0 && band < kBands)
                monoBand = band;
        }

        // while one note plays clearly, nothing else starts: what was found in the last 100 ms without it was
        // its pick
        // (clear now or a moment ago: between a repeated note's picks it isn't)
        // (and with its fundamental in the long spectrum, within 24 dB of its 2nd to 5th harmonics there: notes
        // ringing together can seem one note at their common period for a moment, a bass's D3 and F3 an A#0. The
        // same window for all of them, so it holds while a new note is still filling it.)
        bool hasPeak = false;
        if (monoBand >= 0)
        {
            float harmonics = 0.0f;
            for (int k = 2; k <= 6; ++k)
                harmonics = std::max(harmonics, levelIn(fLong, k * fMonoHz));
            // (and its 2nd or 3rd harmonic among the loudest of them: under 40 Hz there's always something)
            const float low = std::max(levelIn(fLong, 2.0 * fMonoHz), levelIn(fLong, 3.0 * fMonoHz));
            hasPeak = levelIn(fLong, fMonoHz) >= kMonoFundamental * harmonics && low >= 0.2f * harmonics;
        }
        fMonoAge = monoBand >= 0 && std::fabs(12.0 * std::log2(fMonoHz / fMonoWasHz)) < 1.5 ? fMonoAge + 1 : 0;
        fMonoWasHz = monoBand >= 0 ? fMonoHz : 1.0;
        // (a note that had one stays without: a bass's fundamental can die away altogether under its harmonics)
        for (const int d : { 0, -1, 1 })
        {
            const int b = monoBand + d;
            if (monoBand >= 0 && b >= 0 && b < kBands && fTracks[static_cast<size_t>(b)].active
                && fTracks[static_cast<size_t>(b)].mono && fTracks[static_cast<size_t>(b)].hadPeak)
                hasPeak = true;
        }
        const bool alone = monoBand >= 0 && hasPeak;
        if (alone && fMono.clear())
            fSinceClear = 0;
        else if (!alone || ++fSinceClear > fClearHops || std::fabs(12.0 * std::log2(fMonoHz / fClearHz)) > 1.5)
            fSinceClear = 1 << 20;
        const bool single = monoBand >= 0 && fSinceClear <= fClearHops;
        if (fSinceClear == 0)
            fClearHz = fMonoHz;
        if (single)
            for (Track& track : fTracks)
                if (track.active && !track.mono && !track.octave && track.age < fYoungHops)
                    track = Track {};

        // the peaks, lowest first: each is a note if enough of it is its own
        int count = 0;
        for (int p = 0; p < fPeakCount && !fLearning; ++p)
        {
            Peak& peak = fPeaks[static_cast<size_t>(p)];
            const bool isMono = alone && std::fabs(12.0 * std::log2(peak.hz / fMonoHz)) < 0.7;
            // (the single-note tracker's pitch is the better one)
            double hz = isMono ? fMonoHz : peak.hz;
            const int band = static_cast<int>(std::lround(midiOf(hz))) - kFirstNote;
            if (band < 0 || band >= kBands)
                continue;
            // the note followed there: in its band, or next to it (a bend between two semitones)
            int at = -1;
            double nearest = 0.8;
            for (const int d : { 0, -1, 1 })
            {
                const int b = band + d;
                if (b < 0 || b >= kBands || !fTracks[static_cast<size_t>(b)].active || fTracks[static_cast<size_t>(b)].found)
                    continue;
                const double off = std::fabs(12.0 * std::log2(hz / fTracks[static_cast<size_t>(b)].hz));
                if (off < nearest)
                {
                    nearest = off;
                    at = b;
                }
            }
            const bool playing = at >= 0 && fTracks[static_cast<size_t>(at)].sounding;
            const float own = peak.rest;
            if (own < kFloor || own < kFloorGate * peak.noise || stale(own))
                continue;
            if (!isMono)
            {
                if (own < (playing ? kKeepShare : peak.octave ? kOctaveShare : kNewShare) * peak.level
                    || own < (playing ? kKeepLevel : kNewLevel) * loudest)
                    continue;
                if (!playing && single && !peak.octave)
                    continue;
                // A new peak at a 2nd, 3rd or 4th harmonic of a peak that isn't a playing note yet: that one's
                // harmonic, most likely (a string's harmonics show in the shorter windows before its fundamental
                // has settled). An octave picked together with its note is lost with it; one picked later is found by
                // its jump.
                bool harmonic = false;
                for (int k = 2; k <= 4 && !playing && !harmonic && !peak.octave; ++k)
                {
                    const int q = peakNear(peak.hz / k, 0.02, 0.02, 2.0);
                    harmonic = q >= 0 && q != p && fPeaks[static_cast<size_t>(q)].level >= kBelowPeak * peak.level
                            && !noteAt(fPeaks[static_cast<size_t>(q)].hz);
                }
                if (harmonic)
                    continue;
            }

            // its harmonics: what it explains of their peaks
            Track scratch;
            Track& track = at >= 0 ? fTracks[static_cast<size_t>(at)] : scratch;
            const bool learning = track.age < fRatioHops;
            // (alone, all that's at its harmonics is its own; in a chord, other strings are there too)
            const float prior = single && isMono ? kAlonePrior : fPrior;
            std::array<int, kPartials> match {};
            std::array<float, kPartials + 2> there {}; // the levels of its partials' peaks (1st at [1])
            std::array<float, kPartials> claim {};
            int supported = 0;
            there[1] = own;
            for (int k = 2; k <= kPartials; ++k)
            {
                const auto i = static_cast<size_t>(k - 1);
                match[i] = -1;
                if (k * hz > kTopHz)
                    break;
                // (a stiff string's harmonics are sharp, the more the higher; and the peaks' own frequencies are
                // only so good)
                const int q = peakNear(k * hz, 0.012, 0.012 + 0.0002 * k * k, (isMono ? 0.0 : static_cast<double>(k) * peak.slack) + 2.0);
                if (q < 0 || q == p)
                    continue;
                match[i] = q;
                there[static_cast<size_t>(k)] = fPeaks[static_cast<size_t>(q)].level;
                if (k <= 5 && there[static_cast<size_t>(k)] >= kSupport * own)
                    ++supported;
            }
            // Its fundamental's level as each spectrum has it: a new note fills the short windows long before the
            // long one, so against the long one's reading its harmonics would seem far too loud for a while
            const float share = own / peak.level;
            const std::array<float, 3> ownIn = { own, std::max(own, share * levelIn(fMid, peak.hz)),
                                                 std::max(own, share * levelIn(fShort, peak.hz)) };
            std::array<float, kPartials> base {};
            double sum = static_cast<double>(own) * own;
            for (int k = 2; k <= kPartials; ++k)
            {
                const auto i = static_cast<size_t>(k - 1);
                if (match[i] < 0)
                    continue;
                base[i] = ownIn[static_cast<size_t>(std::max(peak.window, fPeaks[static_cast<size_t>(match[i])].window))];
                // What it explains of a harmonic's peak: once learnt, what it has had there. Before: up to the prior
                // times its fundamental, or times half the harmonics next to it if that's more (a low string's
                // fundamental is weak, but its harmonics are of a size).
                const float beside = std::max(there[static_cast<size_t>(k - 1)], there[static_cast<size_t>(k + 1)]);
                // (An octave of it is the likeliest string to be there too, and the least wrong when it isn't; a
                // 5th or 7th harmonic taken for a note adds a third or a seventh nobody played.)
                const float eager = (k & (k - 1)) == 0 ? 1.0f : k % 3 == 0 ? 2.0f : 4.0f;
                float allowed = learning ? eager * prior * std::max(base[i], 0.5f * beside)
                                         : kHeadroom * track.ratio[i] * base[i];
                Peak& other = fPeaks[static_cast<size_t>(match[i])];
                if (k == 2 && track.age < fOctaveHops)
                {
                    // A string's 2nd harmonic is about as loud as its 1st and 3rd: on the owner's recordings under
                    // 1.9 times the louder of them for 99 notes in 100 (all three read in one spectrum), while the
                    // octave of an octave shape was 2.2 to 2.5 times. Over that there's a note an octave up, and
                    // this one's share of the peak is what its 1st and 3rd suggest. (In a note's first 300 ms: a
                    // low string's 1st and 3rd die away under its 2nd in time.)
                    const float first = other.window == 2 ? levelIn(fShort, hz) : other.window == 1 ? levelIn(fMid, hz)
                                                                                                    : levelIn(fLong, hz);
                    const float third = other.window == 2 ? levelIn(fShort, 3.0 * hz)
                                      : other.window == 1 ? levelIn(fMid, 3.0 * hz) : levelIn(fLong, 3.0 * hz);
                    const float fourth = other.window == 2 ? levelIn(fShort, 4.0 * hz)
                                       : other.window == 1 ? levelIn(fMid, 4.0 * hz) : levelIn(fLong, 4.0 * hz);
                    const float usual = std::max(share * first, third);
                    // (and its even harmonics together 1.7 times its odd ones: 1.4 at most for 9 single notes in 10,
                    // 2.3 and more for the octave shapes)
                    if (first > 0.0f && other.level > kOctave * usual
                        && other.level + fourth > kEvenOdd * (share * first + third))
                    {
                        other.octave = true;
                        allowed = std::min(allowed, usual);
                    }
                }
                claim[i] = std::min(other.rest, allowed);
                sum += static_cast<double>(claim[i]) * claim[i];
            }
            // (a peak without harmonics of its own is the pick's noise, a thump or hum)
            if (!playing && !isMono && supported < (hz < kTwoHarmonicsHz ? 2 : 1))
                continue;

            // a note
            if (at < 0)
            {
                at = band;
                if (fTracks[static_cast<size_t>(at)].active && fTracks[static_cast<size_t>(at)].found)
                    continue; // (two peaks in one semitone: the first one has it)
                fTracks[static_cast<size_t>(at)] = Track {};
                fTracks[static_cast<size_t>(at)].active = true;
                fTracks[static_cast<size_t>(at)].hz = hz;
                fTracks[static_cast<size_t>(at)].first = own;
            }
            Track& note = fTracks[static_cast<size_t>(at)];
            // (a new note's pitch must have settled: a peak still forming wanders)
            if (!note.sounding && std::fabs(hz / note.hz - 1.0) > kSettled)
                note.run = 0;
            note.found = true;
            note.mono = isMono;
            note.octave = note.octave || peak.octave;
            note.hadPeak = note.hadPeak || isMono;
            note.hz = isMono ? hz : note.hz + (hz - note.hz) * 0.5;
            note.own = own;
            note.level = static_cast<float>(std::sqrt(sum));
            for (int k = 2; k <= kPartials; ++k)
            {
                const auto i = static_cast<size_t>(k - 1);
                if (match[i] < 0)
                    continue;
                Peak& other = fPeaks[static_cast<size_t>(match[i])];
                const float measured = other.rest / base[i];
                if (learning)
                {
                    // what it has had there so far, at most the prior
                    note.ratio[i] = std::max(note.ratio[i], claim[i] / base[i]);
                }
                else if (!noteAt(other.hz) && !other.octave)
                {
                    // it follows what's there, slowly both ways: strings ringing together beat, and a dip
                    // followed at once left the harmonic's next swell looking like a note
                    note.ratio[i] += (measured - note.ratio[i]) * fRatioCoef;
                }
                else if (measured > note.ratio[i])
                {
                    // (while a note plays on that harmonic, only as fast as a harmonic grows against its
                    // fundamental)
                    note.ratio[i] = std::min(measured, std::max(note.ratio[i], 0.01f) * fRatioRise);
                }
                other.rest -= claim[i];
            }
            // (a low note's harmonics go on past those followed one by one: a bass's low B has forty under 1.3 kHz,
            // and they are no notes either)
            for (int k = kPartials + 1; k <= kMaxHarmonic && k * hz <= kTopHz; ++k)
            {
                const int q = peakNear(k * hz, 0.015, std::min(0.06, 0.012 + 0.00015 * k * k), (isMono ? 0.0 : static_cast<double>(k) * peak.slack) + 2.0);
                if (q >= 0 && q != p)
                    fPeaks[static_cast<size_t>(q)].rest = std::max(0.0f, fPeaks[static_cast<size_t>(q)].rest - prior * own);
            }
            peak.rest = 0.0f;
            ++count;
        }

        // the single-note tracker's note with no peak of its own yet (it finds a note in two periods): there already
        if (monoBand >= 0 && single)
        {
            bool there = false;
            for (const int d : { 0, -1, 1 })
            {
                const int b = monoBand + d;
                if (b >= 0 && b < kBands && fTracks[static_cast<size_t>(b)].active && fTracks[static_cast<size_t>(b)].found
                    && fTracks[static_cast<size_t>(b)].mono)
                    there = true;
            }
            if (!there && !fTracks[static_cast<size_t>(monoBand)].found)
            {
                Track& note = fTracks[static_cast<size_t>(monoBand)];
                if (!note.active)
                {
                    note = Track {};
                    note.active = true;
                }
                note.hz = fMonoHz;
                note.found = true;
                note.mono = true;
                note.own = note.level = std::max(note.level, 0.5f * fEnvelope);
            }
        }

        // no more notes than voices: the quietest of those found go, new ones before playing ones
        for (count = 0; ; count = 0)
        {
            int quietest = -1;
            for (int b = 0; b < kBands; ++b)
            {
                const Track& track = fTracks[static_cast<size_t>(b)];
                if (!track.active || !track.found)
                    continue;
                ++count;
                if (quietest < 0)
                {
                    quietest = b;
                    continue;
                }
                const Track& other = fTracks[static_cast<size_t>(quietest)];
                if (track.sounding != other.sounding ? other.sounding : track.level < other.level)
                    quietest = b;
            }
            if (count <= fVoices)
                break;
            fTracks[static_cast<size_t>(quietest)] = Track {};
        }

        updateTracks();
        writeFrame();
        ++fHopCount;
    }

    // A note is playing at this frequency (its fundamental)
    bool noteAt(double hz) const
    {
        const int band = static_cast<int>(std::lround(midiOf(hz))) - kFirstNote;
        for (const int d : { 0, -1, 1 })
        {
            const int b = band + d;
            if (b >= 0 && b < kBands && fTracks[static_cast<size_t>(b)].sounding
                && std::fabs(12.0 * std::log2(hz / fTracks[static_cast<size_t>(b)].hz)) < 0.5)
                return true;
        }
        return false;
    }

    // The notes followed: those found count on, those not found are held a little or let go, bent ones change band
    void updateTracks()
    {
        const bool picked = fPickPending;
        if (fPickPending)
        {
            fPickPending = false;
            // remember each playing note's level, to tell a fresh pick of it from its ringing on
            for (Track& track : fTracks)
                track.atPick = track.sounding ? track.level : 0.0f;
            fSincePick = 0;
            fPickHop = fHopCount;
        }
        else if (fSincePick < (1 << 20))
        {
            ++fSincePick;
        }

        for (int b = 0; b < kBands; ++b)
        {
            Track& track = fTracks[static_cast<size_t>(b)];
            if (!track.active)
                continue;
            if (track.found)
            {
                ++track.age;
                ++track.run;
                track.lost = 0;
                continue;
            }
            track.run = 0;
            // not found: a playing note is held a little, unless its level falls (a mute), it only rings on in the
            // window, or a pick comes
            if (track.sounding)
            {
                if (track.lost == 0)
                    track.lostLevel = fEnvelope;
                if (++track.lost <= fLostHops && !picked && !stale(track.own) && fEnvelope >= kLostFall * track.lostLevel)
                    continue;
            }
            track = Track {};
        }

        // a bent note changes band: it carries on in the next one
        for (int b = 0; b < kBands; ++b)
        {
            Track& track = fTracks[static_cast<size_t>(b)];
            if (!track.active || !track.found)
                continue;
            const double off = midiOf(track.hz) - (kFirstNote + b);
            const int to = off > kMoveWidth ? b + 1 : off < -kMoveWidth ? b - 1 : b;
            if (to == b || to < 0 || to >= kBands || fTracks[static_cast<size_t>(to)].active)
                continue;
            fTracks[static_cast<size_t>(to)] = track;
            track = Track {};
        }
    }

    void writeFrame()
    {
        Frame& now = frame(fHopCount);
        now = Frame {};
        fSounding = 0;
        const int needed = fSincePick <= fPickedHops ? kPickedNewHops : kNewHops;
        for (int b = 0; b < kBands; ++b)
        {
            Track& track = fTracks[static_cast<size_t>(b)];
            if (!track.active)
                continue;
            const auto i = static_cast<size_t>(b);
            bool started = false;
            if (track.sounding)
            {
                if (track.atPick > 0.0f && fSincePick <= fRepickHops && track.level > kRepickJump * track.atPick)
                {
                    // picked again: a fresh start at the pick (with lookahead, back where it was)
                    track.atPick = 0.0f;
                    const int64_t at = std::max(fPickHop, fHopCount - fDelayHops);
                    frame(at).start[i] = true;
                    frame(at).velocity[i] = track.level;
                }
            }
            else if (track.found)
            {
                track.pending = std::max(track.pending, track.level);
                if (track.level < kPickDecay * track.pending)
                {
                    track.waited = 0;
                    track.pending = track.level;
                }
                ++track.waited;
                // (the single-note tracker's note has been waited for already)
                if (track.mono
                    || (track.run >= needed && static_cast<float>(track.waited) * fHopMs >= fPickMs && !fFalling
                        && track.own >= kNotFalling * track.first && fSincePick >= fWaitHops
                        && fSincePick <= fPickWindowHops))
                {
                    track.sounding = true;
                    started = true;
                }
            }
            if (!track.sounding)
                continue;
            ++fSounding;
            now.level[i] = track.level;
            now.measured[i] = track.hz / fSampleRate;
            now.sounding[i] = true;
            if (started)
            {
                // with lookahead the note starts where it began: back over the hops it took to find, to the pick
                // that started it at most, in the frames not yet played
                int back = std::min(track.age, fSincePick);
                back = std::min(back, fDelayHops);
                int64_t first = fHopCount;
                for (int k = 1; k <= back; ++k)
                {
                    Frame& past = frame(fHopCount - k);
                    if (past.sounding[i])
                        break;
                    past.sounding[i] = true;
                    past.level[i] = track.level;
                    past.measured[i] = now.measured[i];
                    first = fHopCount - k;
                }
                Frame& at = frame(first);
                at.start[i] = true;
                at.velocity[i] = std::max(track.pending, track.level);
            }
        }
    }

    double fSampleRate = 48000.0, fRate = 12000.0;
    int fDecimation = 4;
    int fHop = 256, fSinceHop = 0, fPhase = 0;
    float fHopMs = 5.33f;
    float fPickMs = 15.0f;
    float fPrior = 3.0f;
    int fVoices = 4;
    double fLookaheadMs = 0.0;
    int fDelayHops = 0;

    Biquad fHighPass;
    Biquad fAntiAlias[2];
    std::array<float, kRing> fRing {};
    int fWrite = 0;
    Spectrum<kLongSize> fLong;
    Spectrum<kMidSize> fMid;
    Spectrum<kShortSize> fShort;
    std::array<Peak, kMaxPeaks> fPeaks {};
    int fPeakCount = 0;
    std::array<Track, kBands> fTracks {};
    std::array<double, kBands> fBandStep {};
    std::vector<Frame> fFrames;
    int64_t fHopCount = 0;

    NoteTracker fMono;
    bool fMonoOn = false;
    double fMonoHz = 0.0;

    OnsetDetector fOnset, fClick;
    Biquad fClickFilter;
    bool fPickPending = false;
    int fSincePick = 1 << 20;
    int64_t fPickHop = 0;
    int fRepickHops = 8, fPickedHops = 15, fRatioHops = 9, fYoungHops = 19, fChordWaitHops = 8;
    std::array<float, 12> fLevels {}; // the input's level over the last 64 ms of hops
    int fLevelPos = 0;
    bool fFalling = false;
    bool fMonoWasOn = false;
    int fSinceClear = 1 << 20, fClearHops = 28; // hops since the single-note tracker's note was clear; 150 ms
    double fClearHz = 440.0, fMonoWasHz = 1.0;
    int fMonoAge = 0; // hops the single-note tracker has been on this note
    int fWaitHops = 8; // how long after a pick new notes wait, this hop
    int fPickWindowHops = 75;
    int fOctaveHops = 56;
    int fLearnHops = 47, fLostHops = 8;
    bool fLearning = true;
    float fLearnCoef = 0.16f, fFloorFall = 0.02f, fRatioRise = 1.006f, fRatioCoef = 0.04f;
    std::array<float, 2> fFloorRise { 1.0f, 1.0f };
    float fEnvelope = 0.0f, fHeldPeak = 0.0f;
    float fEnvAttack = 1.0f, fEnvRelease = 0.99f;
    int fHold = 0, fEnvHold = 1600;
    int fSounding = 0;
    int fPicks = 0;
};

} // namespace xythos
