// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/dsp/Biquad.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace xythos {

// Removes mains hum (a single-coil pickup's 50/60 Hz buzz) by subtracting it, never by filtering: the guitar passes
// through untouched, only an estimate of the hum is taken away.
//
// Mains hum is periodic: one waveform, repeated exactly at the mains frequency, with harmonics far up the spectrum
// (a single coil's buzz has most of its energy between 2 and 10 kHz, which is why gain makes it so loud). Its shape and
// level hold still while the guitar does, but its timing drifts, since the grid's frequency wanders by thousandths of a
// hertz, and the high harmonics need the timing within microseconds. So the canceller keeps:
//
//  - the mains period: an LMS fit of the lag at which the input repeats (the input against itself one period ago,
//    through a Catmull-Rom fractional delay, on the band the buzz is strongest in), independent of the template, so
//    it finds the frequency from anywhere within about 0.1 Hz of 50/60;
//  - a template: one period of the hum, a table of one point per sample, read and learned through Catmull-Rom weights
//    at the current phase (an LMS fit: each sample nudges the 4 points it was read from by the error);
//  - the phase, advanced by that period and pulled by the template: the error between the input and the template,
//    times the template's slope, says how far ahead or behind the template is.
//
// The period and the template learn only while the input is near its floor (a minimum follower: no notes playing),
// so notes never leak into them. The phase follows the template also under a dying note, as long as the template
// explains a good part of what's left. Both stop at once when a note starts. A long note can leave the phase out by
// more cycles of the high harmonics than the loop can tell apart, so once it's quiet again the phase is searched for
// (relock()); meanwhile, from 0.4 to 1.2 s into the note, the subtraction fades out, so it never adds hum to the tail.
//
// Real-time safe after prepare(): process() doesn't allocate.
class HumCanceller {
public:
    // Non-RT: sizes the template and the history for this sample rate (and a mains frequency down to 49 Hz).
    void prepare(double sampleRate)
    {
        fSampleRate = sampleRate;
        fTable.assign(static_cast<size_t>(sampleRate / 50.0) + 8, 0.0f);
        fHistory.assign(static_cast<size_t>(sampleRate / (50.0 - kMaxDeviationHz)) + 8, 0.0f);
        fRaw.assign(fHistory.size() + 4, 0.0f);
        fRelockIndex.assign(fTable.size(), 0);
        fFloorRise = static_cast<float>(std::pow(10.0, kFloorRiseDbPerSec / 10.0 / sampleRate));
        fPowerCoef = static_cast<float>(std::exp(-1.0 / (0.02 * sampleRate)));
        fFastCoef = static_cast<float>(std::exp(-1.0 / (0.0005 * sampleRate)));
        fWeightCoef = static_cast<float>(std::exp(-1.0 / (0.01 * sampleRate)));
        fInvSampleRate = static_cast<float>(1.0 / sampleRate);
        fFloorRiseFast = static_cast<float>(std::pow(10.0, kFloorRiseFastDbPerSec / 10.0 / sampleRate));
        fKp = 2.0 * M_PI * kLoopHz / sampleRate;
        fKi = std::pow(2.0 * M_PI * kOffsetHz / sampleRate, 2.0);
        fMaxOffset = kMaxOffsetHz / sampleRate;
        fLagRate = 1.0 / (kLagTime * sampleRate);
        fRelockAfter = static_cast<int>(kRelockAfter * sampleRate);
        fCooldown = static_cast<int>(sampleRate);
        fSettle = static_cast<int>(0.2 * sampleRate);
        fLagHighPass.setHighPass(sampleRate, 400.0);
        fLagLowPass.setLowPass(sampleRate, std::min(7000.0, 0.4 * sampleRate));
        fPrepared = true;
        configure();
        reset();
    }

    // 50 or 60 (Hz). Forgets the hum when it changes.
    void setMains(double hz)
    {
        hz = hz < 55.0 ? 50.0 : 60.0;
        if (hz == fMains)
            return;
        fMains = hz;
        if (fPrepared)
        {
            configure();
            reset();
        }
    }

    // Seconds the template takes to follow a change in the hum while the input is quiet
    void setLearnTime(double seconds)
    {
        fLearnTime = std::clamp(seconds, 0.02, 10.0);
        // LMS step per sample: each point is read (and so nudged) once a period, with Catmull-Rom weights whose
        // squares sum to 0.6-1
        fLearnRate = static_cast<float>(std::min(0.5, 1.0 / (fMains * fLearnTime) * 1.25));
    }

    // How much of the hum estimate process() subtracts, 0..1
    void setAmount(float amount) { fAmount = std::clamp(amount, 0.0f, 1.0f); }

    // RT. Returns x minus the hum, and learns from x.
    float process(float x)
    {
        trackPeriod(x);

        // how loud the input is: its mean square over the last period (which reads a steady hum without ripple, and
        // has forgotten a note one period after it stops)
        {
            const int size = static_cast<int>(fRaw.size());
            const int leaving = fRawPos - fSize < 0 ? fRawPos - fSize + size : fRawPos - fSize;
            fWindowSum += double(x) * x - double(fRaw[leaving]) * fRaw[leaving];
            fRaw[fRawPos] = x;
            if (fRawPos == 0) // now and then, against rounding
            {
                fWindowSum = 0.0;
                for (int m = 0; m < fSize; ++m)
                {
                    const float v = fRaw[m - fSize + 1 < 0 ? m - fSize + 1 + size : m - fSize + 1];
                    fWindowSum += double(v) * v;
                }
            }
            fInPower = static_cast<float>(std::max(fWindowSum, 0.0) / fSize);
        }
        // against its floor, which falls at once and rises slowly, so a dying note stays above it; faster until
        // the template holds the hum (from silence, or when the hum grows)
        fFastPower = x * x + (fFastPower - x * x) * fFastCoef;
        const float fill = fHumPower / fFloor;
        if (fWarmup > 0)
        {
            --fWarmup;
            fFloor = std::max(fInPower, kMinFloor);
        }
        else
        {
            const float rise = fill < 0.25f ? fFloorRiseFast : fFloorRise;
            fFloor = std::max(std::min(fFloor * rise, std::max(fInPower, kMinFloor)), kMinFloor);
        }
        // learn while the input is within a few dB of its floor: fully up to 3 dB above it, not at all from 9 dB; and
        // not from the first millisecond of a pick, before the slower level has risen (the buzz's own peaks stay
        // well under 20 dB above its RMS)
        const float ratio = fInPower / fFloor;
        const bool onset = fFastPower > 100.0f * fFloor;
        const float learnTarget = onset ? 0.0f : std::clamp((7.94f - ratio) / (7.94f - 2.0f), 0.0f, 1.0f);
        // stop at once when a note starts, come back smoothly
        fLearn = std::min(learnTarget, learnTarget + (fLearn - learnTarget) * fWeightCoef);

        // after a long note the phase may have drifted by many cycles of the high harmonics, too far for the loop
        // to say which way: once a full period has been quiet, find it by correlation, before learning again
        fQuietRun = learnTarget >= 0.99f ? std::min(fQuietRun + 1, fSize) : 0;
        if (fHeld > fRelockAfter && fQuietRun >= fSize)
        {
            relock();
            fHeld = 0;
            fSinceRelock = 0;
        }
        const float learn = fHeld > fRelockAfter ? 0.0f : fLearn;

        // the template at the current phase: Catmull-Rom weights for points i-1, i, i+1, i+2, and their slopes
        const double pos = fPhase * fSize;
        int i = static_cast<int>(pos);
        const float t = static_cast<float>(pos - i);
        if (i >= fSize)
            i -= fSize;
        float c[4], d[4];
        catmullRom(t, c, d);
        const int i0 = i == 0 ? fSize - 1 : i - 1;
        const int i2 = i + 1 >= fSize ? i + 1 - fSize : i + 1;
        const int i3 = i + 2 >= fSize ? i + 2 - fSize : i + 2;
        float* T = fTable.data();
        const float hum = c[0] * T[i0] + c[1] * T[i] + c[2] * T[i2] + c[3] * T[i3];
        const float slope = (d[0] * T[i0] + d[1] * T[i] + d[2] * T[i2] + d[3] * T[i3]) * static_cast<float>(fSize);
        const float err = x - hum;
        fErrPower = err * err + (fErrPower - err * err) * fPowerCoef;

        if (learn > 1e-4f)
        {
            // five times faster while the template holds little of the floor (starting, or the hum just grew), or
            // far more than it (it learnt something that wasn't hum)
            const float speed = fill > 4.0f ? 5.0f : 5.0f - 4.0f * std::clamp(2.0f * fill, 0.0f, 1.0f);
            const float step = fLearnRate * speed * learn * err;
            T[i0] += step * c[0];
            T[i] += step * c[1];
            T[i2] += step * c[2];
            T[i3] += step * c[3];
        }

        // the phase follows the template while it's quiet and the template holds a good part of the input (not
        // while it's still filling), and under a dying note while the template explains a good part of what's left:
        // in quiet, even misaligned, the error is at most a few times the hum; under a note it's far more
        const float mature = std::clamp(4.0f * fHumPower / std::max(fInPower, 1e-14f) - 0.5f, 0.0f, 1.0f);
        const float trackTarget = fHeld > fRelockAfter || onset ? 0.0f : std::max(learnTarget * mature,
            std::clamp(3.0f * fHumPower / std::max(fErrPower, 1e-14f) - 0.5f, 0.0f, 1.0f));
        fTrack = std::min(trackTarget, trackTarget + (fTrack - trackTarget) * fWeightCoef);
        // how long the phase has gone without following the template
        if (fTrack > 0.5f)
            fHeld = 0;
        else if (fTrack < 0.1f && fHumPower > 0.0f && fHeld < fCooldown * 4)
            ++fHeld;
        // in quiet, taking the template away should leave less than came in; if it leaves more for a whole period,
        // it's out of step (the block was bypassed a while, say): search for the phase again (at most once a second,
        // so a hum that has gone away is unlearnt instead)
        fMisfit = learnTarget >= 0.99f && mature > 0.5f && fErrPower > 1.25f * fInPower ? std::min(fMisfit + 1, fCooldown)
                                                                                        : 0;
        if (fSinceRelock < fCooldown)
            ++fSinceRelock;
        else if (fMisfit > fSize)
            fHeld = fRelockAfter + 1;
        if (++fRawPos >= static_cast<int>(fRaw.size()))
            fRawPos = 0;

        double advance = 1.0 / fPeriod + fOffset;
        if (fTrack > 1e-4f && fSlopePower > 1e-20)
        {
            // err ~ slope * (true phase - our phase), in cycles
            const double pe = std::clamp(static_cast<double>(err) * slope / fSlopePower, -0.1, 0.1) * fTrack;
            advance += fKp * pe;
            // what the period estimate leaves (it lags a mains on the move), so a note's hold starts on frequency;
            // not just after a relock, while the loop is still settling
            if (fSinceRelock > fSettle)
                fOffset = std::clamp(fOffset + fKi * std::clamp(pe, -0.01, 0.01), -fMaxOffset, fMaxOffset);
        }
        fPhase += advance;
        if (fPhase >= 1.0)
        {
            fPhase -= 1.0;
            measureTemplate();
        }
        else if (fPhase < 0.0)
            fPhase += 1.0;

        // after a long note the template may be out of step, and taking it away would add hum: fade it out between
        // 0.4 and 1.2 s into a note, until the phase is found again
        const float hold = std::clamp((1.2f - static_cast<float>(fHeld) * fInvSampleRate) / 0.8f, 0.0f, 1.0f);
        fScale = hold + (fScale - hold) * fWeightCoef;
        // and never take away more than could be in the input: a template that learnt something else (a note, at
        // the very start) is held to about the input's level while it unlearns it
        const float most = fHumPower > 2.0f * fInPower ? std::sqrt(2.0f * fInPower / fHumPower) : 1.0f;
        return x - fAmount * std::min(fScale, most) * hum;
    }

    // RT. Forgets the hum.
    void reset()
    {
        std::fill(fTable.begin(), fTable.end(), 0.0f);
        std::fill(fHistory.begin(), fHistory.end(), 0.0f);
        fHistoryPos = 0;
        std::fill(fRaw.begin(), fRaw.end(), 0.0f);
        fRawPos = 0;
        fWindowSum = 0.0;
        fQuietRun = fHeld = fMisfit = fSinceRelock = 0;
        fLagHighPass.reset();
        fLagLowPass.reset();
        fPeriod = fSampleRate / fMains;
        fPhase = 0.0;
        fOffset = 0.0;
        fInPower = fErrPower = fFastPower = 0.0f;
        fFloor = kMinFloor;
        fWarmup = static_cast<int>(0.06 * fSampleRate);
        fLearn = fTrack = 0.0f;
        fScale = 1.0f;
        fHumPower = 0.0f;
        fSlopePower = 0.0;
        fLagSlopePower = 0.0f;
    }

    // The hum the template holds, RMS (linear)
    float humLevel() const { return std::sqrt(fHumPower); }
    // How much it learns and tracks right now (0..1)
    float learning() const { return fLearn; }
    float tracking() const { return fTrack; }
    // The mains frequency it has found
    double frequency() const { return fSampleRate / fPeriod + fOffset * fSampleRate; }

private:
    static constexpr double kLoopHz = 10.0;         // how fast the phase follows the template
    static constexpr double kOffsetHz = 2.0;       // how fast the loop learns a frequency offset
    static constexpr double kMaxOffsetHz = 0.05;   // and how big it may be
    static constexpr double kRelockAfter = 0.25;   // seconds without tracking after which the phase is searched for
    static constexpr double kRelockRange = 0.12;   // how far, in periods, either way
    static constexpr double kLagTime = 0.25;       // seconds the period estimate averages over
    static constexpr double kMaxDeviationHz = 1.0; // how far from 50/60 Hz the mains may be
    static constexpr double kFloorRiseDbPerSec = 1.0;
    static constexpr double kFloorRiseFastDbPerSec = 8.0; // until the template holds the hum
    static constexpr float kMinFloor = 1e-11f;     // -110 dB

    void configure()
    {
        fSize = std::clamp(static_cast<int>(std::lround(fSampleRate / fMains)), 4, static_cast<int>(fTable.size()) - 3);
        fMinPeriod = fSampleRate / (fMains + kMaxDeviationHz);
        fMaxPeriod = std::min(fSampleRate / (fMains - kMaxDeviationHz), static_cast<double>(fHistory.size()) - 4.0);
        setLearnTime(fLearnTime);
    }

    static void catmullRom(float t, float* c, float* d)
    {
        const float t2 = t * t, t3 = t2 * t;
        c[0] = 0.5f * (-t3 + 2.0f * t2 - t);
        c[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
        c[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
        c[3] = 0.5f * (t3 - t2);
        d[0] = 0.5f * (-3.0f * t2 + 4.0f * t - 1.0f);
        d[1] = 0.5f * (9.0f * t2 - 10.0f * t);
        d[2] = 0.5f * (-9.0f * t2 + 8.0f * t + 1.0f);
        d[3] = 0.5f * (3.0f * t2 - 2.0f * t);
    }

    // The input against itself one period ago: while quiet, moves the period toward the lag at which they match
    void trackPeriod(float x)
    {
        // the band the buzz is strongest in: less of the hiss and of a dying note's low end
        x = fLagLowPass.process(fLagHighPass.process(x));
        const int size = static_cast<int>(fHistory.size());
        fHistory[fHistoryPos] = x;
        if (fLearn > 1e-4f)
        {
            // x(now - period): between the samples n and n + 1 ago, t of the way
            const int n = static_cast<int>(fPeriod);
            const float t = static_cast<float>(fPeriod - n);
            float c[4], d[4];
            catmullRom(t, c, d);
            auto at = [&](int ago) {
                int k = fHistoryPos - ago;
                return fHistory[k < 0 ? k + size : k];
            };
            const float p0 = at(n - 1), p1 = at(n), p2 = at(n + 1), p3 = at(n + 2);
            const float past = c[0] * p0 + c[1] * p1 + c[2] * p2 + c[3] * p3;
            const float slope = d[0] * p0 + d[1] * p1 + d[2] * p2 + d[3] * p3; // d past / d period
            fLagSlopePower = slope * slope + (fLagSlopePower - slope * slope) * fPowerCoef;
            if (fLagSlopePower > 1e-20f)
            {
                // x - past ~ slope * (true period - period)
                const double pe = std::clamp(static_cast<double>(x - past) * slope / fLagSlopePower, -5.0, 5.0);
                fPeriod = std::clamp(fPeriod + fLagRate * fLearn * pe, fMinPeriod, fMaxPeriod);
            }
        }
        if (++fHistoryPos >= size)
            fHistoryPos = 0;
    }

    // Finds the phase at which the template best matches the last period of input: the cross-correlation's peak,
    // within kRelockRange either way, first every other table point, then around the best
    void relock()
    {
        const int size = static_cast<int>(fRaw.size());
        const double now = fPhase * fSize;
        const double perSample = fSize / fPeriod;
        for (int m = 0; m < fSize; ++m)
        {
            int k = static_cast<int>(std::floor(now - m * perSample + 0.5)) % fSize;
            fRelockIndex[m] = k < 0 ? k + fSize : k;
        }
        const float* T = fTable.data();
        auto score = [&](int shift) {
            double acc = 0.0;
            int r = fRawPos;
            for (int m = 0; m < fSize; ++m)
            {
                int k = fRelockIndex[m] + shift;
                k = k >= fSize ? k - fSize : (k < 0 ? k + fSize : k);
                acc += double(fRaw[r]) * T[k];
                if (--r < 0)
                    r = size - 1;
            }
            return acc;
        };
        const int range = static_cast<int>(kRelockRange * fSize);
        int best = 0;
        double bestScore = score(0);
        for (int j = -range; j <= range; j += 2)
            if (const double sc = score(j); sc > bestScore)
            {
                bestScore = sc;
                best = j;
            }
        const int coarse = best;
        for (int j = coarse - 1; j <= coarse + 1; j += 2)
            if (const double sc = score(j); sc > bestScore)
            {
                bestScore = sc;
                best = j;
            }
        if (bestScore > 0.0 && best != 0)
        {
            fPhase += static_cast<double>(best) / fSize;
            fPhase -= std::floor(fPhase);
        }
    }

    // once per period: the template's power and its slope's (for the phase's gain)
    void measureTemplate()
    {
        double p = 0.0, s = 0.0;
        const float* T = fTable.data();
        for (int k = 0; k < fSize; ++k)
        {
            p += double(T[k]) * T[k];
            const float d = T[k + 1 < fSize ? k + 1 : 0] - T[k == 0 ? fSize - 1 : k - 1];
            s += double(d) * d;
        }
        fHumPower = static_cast<float>(p / fSize);
        fSlopePower = s / fSize * 0.25 * double(fSize) * double(fSize);
    }

    bool fPrepared = false;
    double fSampleRate = 48000.0;
    double fMains = 60.0;
    int fSize = 800;
    std::vector<float> fTable;
    std::vector<float> fRaw;          // the input, for relock()
    int fRawPos = 0;
    double fWindowSum = 0.0;
    std::vector<int> fRelockIndex;
    int fQuietRun = 0, fHeld = 0, fRelockAfter = 24000;
    int fMisfit = 0, fSinceRelock = 0, fCooldown = 48000, fSettle = 9600;
    std::vector<float> fHistory;
    int fHistoryPos = 0;
    Biquad fLagHighPass, fLagLowPass;
    double fPeriod = 800.0, fMinPeriod = 787.0, fMaxPeriod = 814.0;
    double fPhase = 0.0;
    double fKp = 0.0, fKi = 0.0, fOffset = 0.0, fMaxOffset = 0.0, fLagRate = 0.0;
    double fLearnTime = 0.5;
    float fLearnRate = 0.04f;
    float fAmount = 1.0f;

    float fPowerCoef = 0.0f, fWeightCoef = 0.0f, fFloorRise = 1.0f, fFloorRiseFast = 1.0f;
    int fWarmup = 0;
    float fFastCoef = 0.0f, fFastPower = 0.0f;
    float fInPower = 0.0f, fErrPower = 0.0f, fFloor = kMinFloor;
    float fLearn = 0.0f, fTrack = 0.0f, fScale = 1.0f, fInvSampleRate = 1.0f / 48000.0f;
    float fHumPower = 0.0f;
    double fSlopePower = 0.0;
    float fLagSlopePower = 0.0f;
};

} // namespace xythos
