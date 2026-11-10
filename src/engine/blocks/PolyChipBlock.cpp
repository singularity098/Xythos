// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/PolyChipBlock.hpp"

#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/Oscillator.hpp"
#include "engine/dsp/PolyNoteFinder.hpp"
#include "engine/dsp/Smoother.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace xythos {

namespace {

constexpr const char* kWaveNames[] = { "Pulse 12%", "Pulse 25%", "Pulse 50%", "Triangle" };
constexpr double kWidths[] = { 0.125, 0.25, 0.5 };
constexpr const char* kVoiceNames[] = { "1", "2", "3", "4", "5", "6" };
constexpr const char* kArpNames[] = { "Off", "Up", "Down" };
constexpr const char* kOctaveNames[] = { "-2", "-1", "0", "+1" };

constexpr int kBands = PolyNoteFinder::kBands;
constexpr float kGain = 5.0f;       // band level to voice amplitude
constexpr float kVoiceLevel = 0.25f;

// The notes come from dsp/PolyNoteFinder.hpp; this block plays them
class PolyChipBlock : public Block {
public:
    PolyChipBlock() { fMix.snap(1.0f); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fSampleRate = sampleRate;
        fMix.prepare(sampleRate);
        fFinder.prepare(sampleRate);
        fAttackCoef = 1.0f - EnvelopeFollower::coef(sampleRate, 3.0); // (the notes come in 5 ms hops)
        updateRelease();
        const auto maxDelay = static_cast<size_t>(std::ceil(PolyNoteFinder::kMaxLookaheadMs / 1000.0 * sampleRate)) + 64;
        for (std::vector<float>& line : fDryLines)
            line.assign(maxDelay + 2, 0.0f);
        fRigLine.assign(maxDelay + 2, 0.0f);
        fOwnLevel.prepare(sampleRate);
        fRigLevel.prepare(sampleRate);
        fGainCoef = 1.0f - EnvelopeFollower::coef(sampleRate, 5.0);
        reset();
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kPolyChipParamWave:    fWave = std::clamp(static_cast<int>(value), 0, 3); break;
        case kPolyChipParamVoices:  fFinder.setVoices(static_cast<int>(value)); break;
        // a lower peak at a harmonic's distance drops this one when it's louder than 1/ratio of it: from 1/8
        // (only much louder notes) at 0% to 8 (harmonics 18 dB over their note) at 100%; a guitar's 2nd harmonic
        // is often twice its note, more while the note's own band rings up
        case kPolyChipParamFocus:   fFinder.setFocus(std::exp2(6.0f * value / 100.0f - 3.0f)); break;
        case kPolyChipParamArp:     fArp = static_cast<PolyChipArp>(std::clamp(static_cast<int>(value), 0, 2)); break;
        case kPolyChipParamArpRate: fArpHz = value; break;
        case kPolyChipParamRelease:
            fReleaseMs = value;
            updateRelease();
            break;
        case kPolyChipParamCrush:   fCrush = value / 100.0f; break;
        case kPolyChipParamOctave:  fOctaveRatio = std::exp2(std::clamp(static_cast<int>(value), 0, 3) - 2); break;
        case kPolyChipParamMix:     fMix.setTarget(value / 100.0f); break;
        case kPolyChipParamQuantize: fQuantize = value >= 0.5f; break;
        case kPolyChipParamPickFilter: fFinder.setPickFilter(value); break;
        case kPolyChipParamTrack: fTrackRig = value >= 0.5f; break;
        }
    }

    // Lookahead (config "lookahead", ms): the voices and the dry signal play that much (in whole hops) behind the
    // analysis, and each new note starts back where it began
    bool configure(const nlohmann::json& config) override
    {
        fFinder.setLookahead(config.is_object() ? config.value(TrackedInput::kConfigKey, 0.0) : 0.0);
        reset();
        return true;
    }

    uint32_t latency() const override { return fFinder.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override
    {
        fRig[0] = input[0];
        fRig[1] = numChannels > 1 ? input[1] : input[0];
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fSnap)
        {
            fSnap = false;
            fMix.snap(fMix.target());
        }
        const size_t delay = fFinder.latency();
        for (uint32_t i = 0; i < frames; ++i)
        {
            const bool rig = fTrackRig && fRig[0] != nullptr;
            float mono;
            if (rig)
                mono = 0.5f * (fRig[0][i] + fRig[1][i]);
            else
                mono = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
            float rigNow = mono;
            if (fFinder.process(mono))
                playFrame(fFinder.playFrame());
            if (delay > 0)
            {
                // the dry signal, as far behind as the voices
                const size_t size = fDryLines[0].size();
                const size_t read = (fDryPos + size - delay) % size;
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                {
                    fDryLines[ch][fDryPos] = io[ch][i];
                    io[ch][i] = fDryLines[ch][read];
                }
                fRigLine[fDryPos] = mono;
                rigNow = fRigLine[read];
                fDryPos = (fDryPos + 1) % size;
            }
            // tracking the rig, the voices still play at the level of the block's own input: scaled by how much
            // louder or quieter it is than the rig's
            float gain = 1.0f;
            if (rig)
            {
                const float own = fOwnLevel.process(numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i], 0.0);
                const float level = fRigLevel.process(rigNow, 0.0);
                const float target = level > 1e-5f ? std::min(own / level, 16.0f) : fGain;
                fGain += (target - fGain) * fGainCoef;
                gain = fGain;
            }
            if (fArp != PolyChipArp::Off && fPlaying > 0 && advancePhase(fArpPhase, fArpHz / fSampleRate))
                ++fArpStep;

            const float synth = render() * gain;
            const float mix = fMix.next();
            for (uint32_t ch = 0; ch < numChannels; ++ch)
                io[ch][i] = io[ch][i] * (1.0f - mix) + synth * mix;
        }
    }

    void reset() override
    {
        fFinder.reset();
        for (int b = 0; b < kBands; ++b)
        {
            Voice& voice = fVoices[b];
            voice.amp = voice.target = 0.0f;
            voice.phase = 0.0;
            voice.playedDt = fFinder.bandStep(b);
            voice.playing = false;
        }
        for (std::vector<float>& line : fDryLines)
            std::fill(line.begin(), line.end(), 0.0f);
        std::fill(fRigLine.begin(), fRigLine.end(), 0.0f);
        fOwnLevel.reset();
        fRigLevel.reset();
        fGain = 1.0f;
        fDryPos = 0;
        fPlaying = 0;
        fArpPhase = 0.0;
        fArpStep = 0;
        fSnap = true;
    }

private:
    // A band's voice: what it plays from the frames
    struct Voice {
        float target = 0.0f, amp = 0.0f;
        double phase = 0.0;
        double playedDt = 0.0;
        bool playing = false;
    };

    // The voices take up a frame: their target levels and pitches, and new notes
    void playFrame(const PolyNoteFinder::Frame& f)
    {
        int playing = 0;
        for (int b = 0; b < kBands; ++b)
        {
            Voice& voice = fVoices[b];
            voice.target = f.sounding[b] ? std::min(1.0f, f.level[b] * kGain) : 0.0f;
            if (f.measured[b] > 0.0)
                voice.playedDt = f.measured[b];
            if (f.sounding[b] && !voice.playing)
                takeOverVoice(b);
            voice.playing = f.sounding[b];
            playing += voice.playing ? 1 : 0;
        }
        if (playing == 0 && fPlaying > 0)
        {
            // the next chord starts its arpeggio from the start
            fArpPhase = 0.0;
            fArpStep = 0;
        }
        fPlaying = playing;
    }

    // A note that starts next to one that is fading (a bend or slide into the next semitone) carries on that voice,
    // its phase and level, instead of starting a second voice against it
    void takeOverVoice(int b)
    {
        int from = -1;
        for (int d = -2; d <= 2; ++d)
        {
            const int n = b + d;
            if (d == 0 || n < 0 || n >= kBands || fVoices[n].playing || fVoices[n].amp <= 0.0f)
                continue;
            if (from < 0 || fVoices[n].amp > fVoices[from].amp)
                from = n;
        }
        if (from < 0)
            return;
        Voice& voice = fVoices[b];
        Voice& old = fVoices[from];
        voice.phase = old.phase;
        voice.amp = old.amp;
        old.amp = 0.0f;
        old.phase = 0.0;
    }

    // The band the arpeggio is on, or -1 when every playing note sounds
    int arpBand() const
    {
        if (fArp == PolyChipArp::Off || fPlaying == 0)
            return -1;
        int index = fArpStep % fPlaying;
        if (fArp == PolyChipArp::Down)
            index = fPlaying - 1 - index;
        for (int b = 0; b < kBands; ++b)
            if (fVoices[b].playing && index-- == 0)
                return b;
        return -1;
    }

    float render()
    {
        const int arp = arpBand();
        float sum = 0.0f;
        for (int b = 0; b < kBands; ++b)
        {
            Voice& band = fVoices[b];
            const float target = arp < 0 || arp == b ? band.target : 0.0f;
            if (target == 0.0f && band.amp == 0.0f)
                continue;
            // notes the arpeggio has moved off stop at once, like the console switching notes
            const bool fast = target > band.amp || (arp >= 0 && band.playing);
            band.amp += (target - band.amp) * (fast ? fAttackCoef : fReleaseCoef);
            if (target == 0.0f && band.amp < 1e-5f)
            {
                band.amp = 0.0f;
                band.phase = 0.0;
                continue;
            }
            const double dt = (fQuantize ? fFinder.bandStep(b) : band.playedDt) * fOctaveRatio;
            if (dt >= 0.45)
                continue;
            advancePhase(band.phase, dt);
            float osc;
            if (fWave == 3)
            {
                // the console's triangle: 16 steps
                const double tri = band.phase < 0.5 ? 4.0 * band.phase - 1.0 : 3.0 - 4.0 * band.phase;
                osc = static_cast<float>(std::round(tri * 7.5) / 7.5);
            }
            else
            {
                osc = blepPulse(band.phase, dt, kWidths[fWave]);
            }
            // with Crush, the console's 16 volume steps
            const float amp = fCrush > 0.0f ? std::round(band.amp * 15.0f) / 15.0f : band.amp;
            sum += osc * amp * kVoiceLevel;
        }
        if (fCrush > 0.0f)
        {
            // fewer bits for the samples as Crush goes up
            const float step = std::exp2(-(15.0f - 11.0f * fCrush));
            sum = std::round(sum / step) * step;
        }
        return sum;
    }

    void updateRelease() { fReleaseCoef = 1.0f - EnvelopeFollower::coef(fSampleRate, std::max(1.0f, fReleaseMs)); }

    double fSampleRate = 48000.0;
    int fWave = 1;
    PolyChipArp fArp = PolyChipArp::Off;
    float fArpHz = 15.0f;
    float fReleaseMs = 40.0f;
    float fCrush = 0.3f;
    double fOctaveRatio = 1.0;
    bool fQuantize = false;
    bool fTrackRig = true;
    const float* fRig[2] = { nullptr, nullptr };
    std::vector<float> fRigLine; // the rig's input, delayed with the dry signal
    LevelFollower fOwnLevel, fRigLevel;
    float fGain = 1.0f, fGainCoef = 1.0f;
    PolyNoteFinder fFinder;
    std::array<Voice, kBands> fVoices;
    std::vector<float> fDryLines[2];
    size_t fDryPos = 0;
    Smoother fMix;
    int fPlaying = 0;
    double fArpPhase = 0.0;
    int fArpStep = 0;
    float fAttackCoef = 1.0f, fReleaseCoef = 1.0f;
    bool fSnap = true;
};

} // namespace

const BlockDescriptor kPolyChipBlockDescriptor = {
    "polychip",
    "Poly Chip",
    Category::Synth,
    kPolyChipParamCount,
    {
        ParamSpec { "Wave", "", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kWaveNames },
        ParamSpec { "Voices", "", 1.0f, 6.0f, 4.0f, ParamScale::Choice, kVoiceNames },
        ParamSpec { "Focus", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Arp", "", 0.0f, 2.0f, 0.0f, ParamScale::Choice, kArpNames },
        ParamSpec { "Arp Rate", "Hz", 2.0f, 40.0f, 15.0f, ParamScale::Log },
        ParamSpec { "Release", "ms", 10.0f, 2000.0f, 40.0f, ParamScale::Log },
        ParamSpec { "Crush", "%", 0.0f, 100.0f, 30.0f, ParamScale::Linear },
        ParamSpec { "Octave", "", 0.0f, 3.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Mix", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Quantize", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 15.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new PolyChipBlock()); },
    -1,
    -1,
    false,
    nullptr,
    24.0f,
    MeterStyle::Reduction,
    true,
};

} // namespace xythos
