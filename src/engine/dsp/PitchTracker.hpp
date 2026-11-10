// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/dsp/Biquad.hpp"
#include "engine/dsp/Svf.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace xythos {

// Real-time monophonic pitch tracking for the synths and pitch pedals (dsp/NoteTracker.hpp turns it into notes):
// YIN's difference function (de Cheveigné and Kawahara, 2002) on a copy decimated to about 12 kHz, every 4 ms, for
// 25..1500 Hz (a 5-string bass in drop A, 27.5 Hz, up; owner, 2026-11-09: was 28 Hz, and 60 Hz before 2026-10-29); the
// period is then refined at the full rate (difference function around it, parabolic peak) to within a few cents. The
// difference at each lag is over a window of one lag (at least 2 ms), so a note's estimate needs about two of its
// periods, and is relative to the windows' energy, so fast swells and decays don't blur it. A note is reported from
// about 10 ms after it starts; no latency is added to the audio.
//
// Which dip is the period (reworked on 2026-11-09 against the owner's recordings of a 7-string and a 5-string bass,
// where YIN's fixed threshold failed: a sustained low E read as an E3 wobbling by half a semitone, a low B was cut
// off after ten seconds):
// - the shortest lag whose dip is within 0.06 of the deepest one, or under 0.09, and which repeats at twice its lag;
// - while the last pitch found still has its dip, a shorter lag only if it is as deep as the deepest (a hammer-on an
//   octave up, not the decay of the odd harmonics), until a new pick (forget());
// - a dip with a clearly deeper one at twice or three times its lag is a blend of a decayed note's harmonics: the
//   deeper one is the period, the pitch that share of it;
// - a period with nothing at its own frequency (the fundamental 30 dB under the loudest harmonic of the last two
//   periods; a single string's never was more than 23 dB under on the recordings) is the common period of notes
//   ringing together, a power chord's an octave below its root: the pitch is then the lowest note that is there. A
//   note that has had its fundamental for 40 ms stays that note while its period holds, whatever becomes of the
//   fundamental (a low E's falls to 36 dB under its 2nd harmonic within seconds).
// - Notes ringing together (owner's Axel F theme, 2026-11-10: the open D3 string rang along 10 dB under a picked C#4
//   and D4, and a C#4 left ringing 8 dB under the F#4 picked after it; the common period, an octave or two below,
//   played C#3, D3 and F#2): a note found since the last pick stays that note when the period becomes one an octave
//   or more below, while it's still among the loudest there (not under 60 Hz: a low string's first few ms read an
//   octave up). And when a period's notes are clearly unequal (one's harmonics 4 dB or more under another's), the
//   louder one is the pitch, not the lowest.
class PitchTracker {
public:
    static constexpr double kMinHz = 25.0;
    static constexpr double kMaxHz = 1500.0;

    // Non-RT
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fDecimation = std::max(1, static_cast<int>(std::lround(sampleRate / 12000.0)));
        fRate = sampleRate / fDecimation;
        fMinLag = std::max(2, static_cast<int>(fRate / kMaxHz));
        fMaxLag = std::min(kMaxLags - 2, static_cast<int>(std::ceil(fRate / kMinHz)));
        fWindow = std::max(4, static_cast<int>(0.002 * fRate));
        fHop = std::max(1, static_cast<int>(0.004 * fRate));
        fHighPass.setHighPass(sampleRate, 25.0);
        for (Biquad& filter : fAntiAlias)
            filter.setLowPass(sampleRate, std::min(0.38 * fRate, 0.45 * sampleRate));
        fEnvelope.prepare(sampleRate, 1.0, 80.0);
        reset();
    }

    void reset()
    {
        fHighPass.reset();
        for (Biquad& filter : fAntiAlias)
            filter.reset();
        fEnvelope.reset();
        fHistory.fill(0.0f);
        fWrite = 0;
        fFull.fill(0.0f);
        fFullWrite = 0;
        fPhase = 0;
        fSinceHop = 0;
        fHz = 0.0;
        fAperiodicity = 1.0f;
        fVoiced = false;
        fOwnRun = fUnvoicedRun = fHeldRun = 0;
        fSticky = false;
    }

    // One input sample. Returns true when a pitch estimate was made and accepted.
    bool process(float in)
    {
        const float x = fHighPass.process(in);
        fEnvelope.process(x);
        fFull[static_cast<size_t>(fFullWrite)] = x;
        fFullWrite = (fFullWrite + 1) % kFullHistory;
        const float filtered = fAntiAlias[1].process(fAntiAlias[0].process(x));
        if (++fPhase < fDecimation)
            return false;
        fPhase = 0;
        fHistory[static_cast<size_t>(fWrite)] = filtered;
        fWrite = (fWrite + 1) % kHistory;
        if (++fSinceHop < fHop)
            return false;
        fSinceHop = 0;
        if (!sounding())
        {
            fVoiced = false;
            fOwnRun = 0;
            return false;
        }
        return estimate();
    }

    double hz() const { return fHz; } // the last accepted pitch; 0 until one is found
    bool voiced() const { return fVoiced; } // the last estimate found a clear pitch
    uint32_t estimates() const { return fEstimates; } // counts every estimate made, with or without a clear pitch
    // How aperiodic the last estimate's signal was: YIN's normalized difference at the chosen period (0 = a pure
    // tone, about 0.1 for a plucked string, over kVoicedLimit for noise)
    float aperiodicity() const { return fAperiodicity; }
    // A new pick: the next pitch is found afresh, without the last one's head start (a note picked over others left
    // ringing would otherwise never take over from the one before it)
    void forget()
    {
        fSticky = false;
        fOwnRun = 0;
        fHeldRun = 0;
    }

    // The last pitch found is the signal's own period: one note. False for the lowest of notes ringing together.
    bool ownPeriod() const { return fOwnPeriod; }
    float envelope() const { return fEnvelope.value(); }
    bool sounding() const { return fEnvelope.value() > kGateLevel; }

private:
    static constexpr int kHistory = 2048;
    static constexpr int kFullHistory = 16384; // full-rate history for the refinement (enough for 28 Hz at 192 kHz)
    static constexpr int kMaxLags = 512;
    static constexpr float kDipMargin = 0.06f;  // a dip this close to the deepest one counts as deep,
    static constexpr float kDeep = 0.09f;       // or one under this
    static constexpr float kTakeOverMargin = 0.015f; // ... this close, at a lag shorter than the last pitch's
    static constexpr float kTruePeriod = 0.02f; // a dip this much deeper at a multiple of the lag found: the period
    static constexpr float kRepeatMargin = 0.1f; // a period's dip at twice its lag is at most this much shallower
    static constexpr float kVoicedLimit = 0.35f; // above this at the best lag: no clear pitch
    static constexpr float kGateLevel = 0.002f;
    static constexpr double kSubCheckHz = 330.0;   // periods under this are checked for being notes' common period
    static constexpr float kOwnFundamental = 0.03f; // a note's own fundamental: within 30 dB of its loudest harmonic
    static constexpr int kHarmonics = 16;          // a triad on the high strings: the 10th, 12th and 15th
    static constexpr int kEstablished = 10;        // estimates in a row (40 ms) with it: the note is established
    static constexpr float kStrong = 0.25f;        // a harmonic within 12 dB of the loudest: a note there
    static constexpr float kLouder = 0.4f;         // a note there whose harmonics have under this share of another's
                                                   // energy (4 dB) is the quieter one
    static constexpr int kHeld = 3;                // estimates (12 ms) at a pitch since the pick: it stays the note
    static constexpr double kHeldMinHz = 60.0;     // ... in a period over this

    float at(int age) const { return fHistory[static_cast<size_t>((fWrite - 1 - age + 2 * kHistory) % kHistory)]; }
    float fullAt(int age) const
    {
        return fFull[static_cast<size_t>((fFullWrite - 1 - age + 2 * kFullHistory) % kFullHistory)];
    }

    // The sum of a[j] * b[j] for j < n, in eight running sums, so the compiler can vectorize it
    static float dot(const float* a, const float* b, int n)
    {
        float sums[8] = {};
        int j = 0;
        for (; j + 8 <= n; j += 8)
            for (int k = 0; k < 8; ++k)
                sums[k] += a[j + k] * b[j + k];
        float sum = ((sums[0] + sums[1]) + (sums[2] + sums[3])) + ((sums[4] + sums[5]) + (sums[6] + sums[7]));
        for (; j < n; ++j)
            sum += a[j] * b[j];
        return sum;
    }

    // The last `lag` samples against those `shift` before them: their difference relative to their energy
    float differenceAt(int lag, int shift) const
    {
        double cross = 0.0, energy = 0.0;
        for (int j = 0; j < lag; ++j)
        {
            const double a = at(j), b = at(j + shift);
            cross += a * b;
            energy += a * a + b * b;
        }
        return energy > 1e-20 ? static_cast<float>((energy - 2.0 * cross) / energy) : 1.0f;
    }

    // A long period (twice its lag is past the lags worked out): the signal repeats two periods back as well
    bool repeatsAtTwice(int lag) const
    {
        if (3 * lag + 4 >= kHistory)
            return true;
        const float once = differenceAt(lag, lag);
        float twice = 1.0f;
        for (int shift = 2 * lag - 2; shift <= 2 * lag + 2; ++shift)
            twice = std::min(twice, differenceAt(lag, shift));
        return twice <= once + kRepeatMargin;
    }

    // The period at the full rate: the plain difference function over two periods, at lags around the decimated
    // estimate, with a parabolic peak
    double refine(double coarseLag) const
    {
        const double centre = coarseLag * fDecimation;
        const int from = std::max(2, static_cast<int>(std::floor(centre)) - fDecimation - 1);
        const int to = static_cast<int>(std::ceil(centre)) + fDecimation + 1;
        const int window = std::min(to, kFullHistory - to - 2);
        if (fDecimation == 1 || window < to)
            return centre;
        float values[64];
        const int count = std::min(64, to - from + 1);
        int best = 0;
        for (int k = 0; k < count; ++k)
        {
            const int lag = from + k;
            float sum = 0.0f;
            for (int j = 0; j < window; ++j)
            {
                const float diff = fullAt(j) - fullAt(j + lag);
                sum += diff * diff;
            }
            values[k] = sum;
            if (sum < values[best])
                best = k;
        }
        double lag = from + best;
        if (best > 0 && best < count - 1)
        {
            const float a = values[best - 1], b = values[best], c = values[best + 1];
            const float denom = a - 2.0f * b + c;
            if (denom > 0.0f)
                lag += std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
        }
        return lag;
    }

    bool estimate()
    {
        // difference function over the newest samples: a window of one lag at each lag (at least fWindow), so a
        // note's estimate needs only about two of its periods, not a fixed window plus a period. The samples are
        // copied out newest first, so the products run over plain arrays, and the windows' energies come from
        // running sums of the squares.
        const int span = fMaxLag + std::max(fWindow, fMaxLag);
        fSquares[0] = 0.0;
        for (int j = 0; j < span; ++j)
        {
            const float x = at(j);
            fRecent[static_cast<size_t>(j)] = x;
            fSquares[static_cast<size_t>(j + 1)] = fSquares[static_cast<size_t>(j)] + static_cast<double>(x) * x;
        }
        std::array<float, kMaxLags>& d = fDiff;
        for (int lag = 1; lag <= fMaxLag; ++lag)
        {
            const int window = std::max(fWindow, lag);
            const float cross = dot(fRecent.data(), fRecent.data() + lag, window);
            const double now = fSquares[static_cast<size_t>(window)];
            const double before = fSquares[static_cast<size_t>(lag + window)] - fSquares[static_cast<size_t>(lag)];
            // the difference relative to the two windows' energy: a note that swells or dies away fast (a pick
            // attack, a palm mute) differs from itself a period ago as little as a steady one
            const auto energy = static_cast<float>(now + before);
            d[static_cast<size_t>(lag)] = energy > 1e-20f ? (energy - 2.0f * cross) / energy : 1.0f;
        }
        // cumulative mean normalized difference
        float running = 0.0f;
        d[0] = 1.0f;
        for (int lag = 1; lag <= fMaxLag; ++lag)
        {
            running += d[static_cast<size_t>(lag)];
            d[static_cast<size_t>(lag)] = running > 0.0f ? d[static_cast<size_t>(lag)] * lag / running : 1.0f;
        }
        // The period is the shortest lag whose dip is nearly as deep as the deepest one (down to its bottom). YIN's
        // fixed threshold took the half period of a low string whose fundamental is weak (the dip there is under
        // it: a sustained low E played E3, wobbling) and, with nothing under it, the deepest dip anywhere.
        // A period repeats at twice its lag too: a dip with none there is a blend of the harmonics' (a decaying low
        // B's, 6% off its half period). And while the last pitch found still has its dip, a shorter lag must be as
        // deep as the deepest to take over (a hammer-on an octave up; not the decay of the odd harmonics).
        float deepest = 1.0f;
        for (int lag = fMinLag; lag < fMaxLag; ++lag)
            deepest = std::min(deepest, d[static_cast<size_t>(lag)]);
        const double lastLag = fHz > 0.0 && fVoiced && fSticky ? fRate / fHz : 0.0;
        bool lastThere = false;
        if (lastLag >= fMinLag && lastLag < fMaxLag - 1)
        {
            const int from = std::max(fMinLag, static_cast<int>(lastLag * 0.97)),
                      to = std::min(fMaxLag - 1, static_cast<int>(lastLag * 1.03) + 1);
            for (int lag = from; lag <= to; ++lag)
                lastThere = lastThere || d[static_cast<size_t>(lag)] <= deepest + kDipMargin;
        }
        int best = -1;
        for (int lag = fMinLag; lag < fMaxLag && best < 0; ++lag)
        {
            // (or simply deep: a bright string's dips get deeper at each multiple of its period)
            const bool takeOver = lastThere && lag < 0.9 * lastLag;
            if (d[static_cast<size_t>(lag)] > std::max(deepest + (takeOver ? kTakeOverMargin : kDipMargin), takeOver ? 0.0f : kDeep))
                continue;
            while (lag + 1 < fMaxLag && d[static_cast<size_t>(lag + 1)] < d[static_cast<size_t>(lag)])
                ++lag;
            // (twice the lag, give or take 2%)
            const int from = static_cast<int>(1.96 * lag), to = static_cast<int>(2.04 * lag) + 1;
            bool repeats = false;
            if (to >= fMaxLag)
                repeats = repeatsAtTwice(lag); // (past the lags worked out: just this one)
            for (int twice = from; twice <= to && to < fMaxLag && !repeats; ++twice)
                repeats = d[static_cast<size_t>(twice)] <= d[static_cast<size_t>(lag)] + kRepeatMargin;
            if (repeats)
                best = lag;
        }
        if (best < 0)
        {
            best = fMinLag;
            for (int lag = fMinLag; lag < fMaxLag; ++lag)
                if (d[static_cast<size_t>(lag)] < d[static_cast<size_t>(best)])
                    best = lag;
        }
        ++fEstimates;
        fAperiodicity = d[static_cast<size_t>(best)];
        fVoiced = fAperiodicity < kVoicedLimit;
        if (!fVoiced)
        {
            if (++fUnvoicedRun >= kEstablished)
                fOwnRun = 0;
            return false;
        }

        // The dip found may be at a half or a third of the true period, when a note's fundamental and odd harmonics
        // have died away under its 2nd (a bass's low B after ten seconds): it's a blend of the harmonics' dips then,
        // up to half a semitone off. A clearly deeper dip at twice or three times its lag is the period, and the
        // pitch that share of it exactly.
        int multiple = 1;
        for (int m = 2; m <= 3 && multiple == 1; ++m)
        {
            const int from = static_cast<int>(0.96 * m * best), to = std::min(fMaxLag - 2, static_cast<int>(1.04 * m * best) + 1);
            int deepestAt = -1;
            for (int lag = from; lag <= to; ++lag)
                if (deepestAt < 0 || d[static_cast<size_t>(lag)] < d[static_cast<size_t>(deepestAt)])
                    deepestAt = lag;
            if (deepestAt > from && deepestAt < to
                && d[static_cast<size_t>(deepestAt)] < d[static_cast<size_t>(best)] - kTruePeriod)
            {
                best = deepestAt;
                multiple = m;
            }
        }
        // parabolic refinement
        double lag = best;
        if (best > fMinLag && best < fMaxLag - 1)
        {
            const float a = d[static_cast<size_t>(best - 1)], b = d[static_cast<size_t>(best)],
                        c = d[static_cast<size_t>(best + 1)];
            const float denom = a - 2.0f * b + c;
            if (denom > 0.0f)
                lag += std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
        }
        // Notes that ring together repeat at their lowest common period, with no note there: a power chord an octave
        // below its root, a third two octaves below. Then it's the lowest note that is there.
        // (the period is refined first: the lowest note's is an exact share of it, its own dip isn't as sharp)
        const double period = refine(lag);
        // (a note that has had its fundamental for a while stays that note while its period holds: a low E's decays
        // to 36 dB under its 2nd harmonic within seconds, by when nothing tells it from an E3 and a B3 ringing
        // together but what came before)
        const bool same = fHz > 0.0 && std::fabs(std::log2(fHz * period / fSampleRate)) < 1.0 / 24.0;
        int harmonic = multiple;
        if (multiple > 1 || (same && fOwnRun >= kEstablished))
            ;
        else if (fSampleRate / period < kSubCheckHz && 2 * lag + 2 < kHistory)
            harmonic = lowestNoteThere(lag, period);
        const double hz = fSampleRate * harmonic / period;
        fHeldRun = fHz > 0.0 && std::fabs(std::log2(hz / fHz)) < 1.0 / 24.0 ? fHeldRun + 1 : 1;
        fOwnRun = harmonic > 1 ? 0 : same ? std::min(fOwnRun + 1, kEstablished) : 1;
        fUnvoicedRun = 0;
        fSticky = true;
        fOwnPeriod = harmonic == 1;
        fHz = hz;
        return true;
    }

    // The levels of the first `count` harmonics of a period of `lag` samples, over the last two periods (Hann window,
    // so the harmonics land on whole bins without leaking into each other: harmonic h is bin 2h). The sines and
    // cosines are rotated sample by sample.
    template <int count>
    std::array<float, count> harmonics(double lag) const
    {
        const int n = static_cast<int>(std::lround(2.0 * lag));
        std::array<double, count> re {}, im {}, c {}, sn {}, rc {}, rs {};
        for (int h = 0; h < count; ++h)
        {
            c[h] = 1.0;
            rc[h] = std::cos(2.0 * M_PI * 2 * (h + 1) / n);
            rs[h] = std::sin(2.0 * M_PI * 2 * (h + 1) / n);
        }
        double wc = 1.0, ws = 0.0; // the window's cosine
        const double wrc = std::cos(2.0 * M_PI / n), wrs = std::sin(2.0 * M_PI / n);
        for (int j = 0; j < n; ++j)
        {
            const double v = (0.5 - 0.5 * wc) * at(n - 1 - j);
            for (int h = 0; h < count; ++h)
            {
                re[h] += v * c[h];
                im[h] -= v * sn[h];
                const double next = c[h] * rc[h] - sn[h] * rs[h];
                sn[h] = sn[h] * rc[h] + c[h] * rs[h];
                c[h] = next;
            }
            const double next = wc * wrc - ws * wrs;
            ws = ws * wrc + wc * wrs;
            wc = next;
        }
        std::array<float, count> mag {};
        for (int h = 0; h < count; ++h)
            mag[h] = static_cast<float>(std::sqrt(re[h] * re[h] + im[h] * im[h]));
        return mag;
    }

    // A period that is one note's has that note's fundamental: on the owner's recordings (2026-11-09) never more than
    // 23 dB under its loudest harmonic over the last two periods, the 7-string's low A and the bass's low B included,
    // while notes ringing together have nothing at their common period (55 dB under or less: a fifth has its 2nd,
    // 3rd and 4th harmonics, a fourth its 3rd and 4th, a major chord its 2nd to 6th). Returns which harmonic of the
    // period is the lowest note there: 1 for a note of its own.
    int lowestNoteThere(double lag, double period) const
    {
        const std::array<float, kHarmonics> mag = harmonics<kHarmonics>(lag);
        const float top = *std::max_element(mag.begin(), mag.end());
        // The note found since the pick, a harmonic of this period and still among its loudest: it stays the note.
        // The period is its and another note's ringing along (an open string, the note before); what's played is
        // the one found first. (Not on the lowest strings: a bass's first few ms read an octave up.)
        if (fSticky && fHeldRun >= kHeld && fHz > 0.0 && fSampleRate / period >= kHeldMinHz)
        {
            const double ratio = fHz * period / fSampleRate;
            const int k = static_cast<int>(std::lround(ratio));
            if (k >= 2 && k <= 4 && std::fabs(std::log2(ratio / k)) < 1.0 / 24.0
                && mag[static_cast<size_t>(k - 1)] >= kStrong * top)
                return k;
        }
        if (mag[0] >= kOwnFundamental * top)
            return 1;
        // The lowest note there, unless another is clearly louder (a note picked over one left ringing): each
        // note's level is its harmonics' energy (those of a power chord's root and fifth are close)
        auto series = [&](int h) {
            double e = 0.0;
            for (int j = h; j <= kHarmonics; j += h)
                e += static_cast<double>(mag[static_cast<size_t>(j - 1)]) * mag[static_cast<size_t>(j - 1)];
            return e;
        };
        double loudest = 0.0;
        for (int h = 2; h <= kHarmonics; ++h)
            if (mag[static_cast<size_t>(h - 1)] >= kStrong * top)
                loudest = std::max(loudest, series(h));
        for (int h = 2; h <= kHarmonics; ++h)
            if (mag[static_cast<size_t>(h - 1)] >= kStrong * top && series(h) >= kLouder * loudest)
                return h;
        return 1;
    }

    double fSampleRate = 48000.0;
    double fRate = 12000.0;
    int fDecimation = 4;
    int fMinLag = 8, fMaxLag = 200, fWindow = 200, fHop = 48;
    Biquad fHighPass;
    Biquad fAntiAlias[2];
    EnvelopeFollower fEnvelope;
    std::array<float, kHistory> fHistory {};
    int fWrite = 0;
    std::array<float, kFullHistory> fFull {};
    std::array<float, 1024> fRecent {};     // estimate()'s scratch: the newest samples, newest first,
    std::array<double, 1025> fSquares {}; // the running sum of their squares,
    std::array<float, kMaxLags> fDiff {};         // and the difference function
    int fFullWrite = 0;
    int fPhase = 0;
    int fSinceHop = 0;
    double fHz = 0.0;
    float fAperiodicity = 1.0f;
    bool fVoiced = false;
    uint32_t fEstimates = 0;
    bool fOwnPeriod = true;
    bool fSticky = false; // the last pitch found keeps its head start
    int fOwnRun = 0;      // estimates in a row at the last pitch, each with its own fundamental
    int fHeldRun = 0;     // estimates in a row at the last pitch, since the last pick
    int fUnvoicedRun = 0; // estimates in a row without a clear pitch
};

} // namespace xythos
