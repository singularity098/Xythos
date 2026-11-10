// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/MidiOutBlock.hpp"

#include "engine/blocks/TrackedInput.hpp"
#include "engine/dsp/PolyNoteFinder.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace xythos {

namespace {

constexpr const char* kModeNames[] = { "Mono", "Poly" };
constexpr const char* kChannelNames[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14",
                                          "15", "16" };
constexpr const char* kOctaveNames[] = { "-2", "-1", "0", "+1", "+2" };
constexpr const char* kTransposeNames[] = { "-12", "-11", "-10", "-9", "-8", "-7", "-6", "-5", "-4", "-3", "-2", "-1", "0",
                                            "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9", "+10", "+11", "+12" };
constexpr const char* kBendNames[] = { "Off", "2", "12", "24" };
constexpr int kBendRanges[] = { 0, 2, 12, 24 };
constexpr const char* kAudioNames[] = { "Through", "Mute" };

constexpr double kNewNoteJump = 0.6; // semitones: a jump this big without a pick is a new note (a hammer-on)
constexpr int kBendStep = 16;        // of 16384: smaller bend changes aren't sent
constexpr int kBendEvery = 32;       // samples between bend messages, at most
constexpr float kPolyPickMs = 15.0f; // Poly's shortest Pick Filter

class MidiOutBlock : public Block {
public:
    MidiOutBlock() { fPolyNotes.fill(-1); }

    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        fInput.prepare(sampleRate);
        fPoly.prepare(sampleRate);
        fPoly.setVoices(PolyNoteFinder::kMaxVoices);
        reset();
    }

    bool configure(const nlohmann::json& config) override
    {
        fInput.configure(config);
        fPoly.setLookahead(config.is_object() ? config.value(TrackedInput::kConfigKey, 0.0) : 0.0);
        fPoly.reset();
        return true;
    }

    uint32_t latency() const override { return fInput.latency(); }

    void setRigInput(const float* const* input, uint32_t numChannels) override
    {
        fInput.setRig(input, numChannels);
        fRig[0] = input[0];
        fRig[1] = numChannels > 1 ? input[1] : input[0];
    }

    void setMidiOutput(MidiOutput* output) override { fOut = output; }

    void setParam(int index, float value) override
    {
        const int choice = static_cast<int>(value);
        switch (index)
        {
        case kMidiOutParamMode:      change(fPolyMode, choice == 1); break;
        case kMidiOutParamChannel:   change(fChannel, std::clamp(choice, 0, 15)); break;
        case kMidiOutParamOctave:    change(fOctave, std::clamp(choice, 0, 4) - 2); break;
        case kMidiOutParamTranspose: change(fTranspose, std::clamp(choice, 0, 24) - 12); break;
        case kMidiOutParamBend:      change(fBendRange, kBendRanges[std::clamp(choice, 0, 3)]); break;
        case kMidiOutParamDynamics:  fDynamics = value / 100.0f; break;
        case kMidiOutParamFocus:     fPoly.setFocus(std::exp2(6.0f * value / 100.0f - 3.0f)); break;
        case kMidiOutParamAudio:     fMute = choice == 1; break;
        case kMidiOutParamPickFilter:
            fInput.setPickFilter(value);
            // chords always wait a little: the pick attack rings in the higher bands first
            fPoly.setPickFilter(std::max(kPolyPickMs, value));
            break;
        case kMidiOutParamTrack:
            fInput.setTrackRig(value);
            fTrackRig = value >= 0.5f;
            break;
        }
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        if (fRelease)
        {
            // a setting that changes what's sent: the notes sounding end first
            fRelease = false;
            releaseAll(0);
        }
        // the poly finder's frames come a little behind or ahead of the lookahead in whole hops: its notes are
        // shifted to line up with the audio
        const int64_t polyShift = static_cast<int64_t>(fInput.latency()) - static_cast<int64_t>(fPoly.latency());
        for (uint32_t i = 0; i < frames; ++i)
        {
            float track;
            if (fTrackRig && fRig[0] != nullptr)
                track = 0.5f * (fRig[0][i] + fRig[1][i]);
            else
                track = numChannels > 1 ? 0.5f * (io[0][i] + io[1][i]) : io[0][i];
            // (io[..][i] is delayed by the lookahead from here on)
            const NoteTracker::Note& note = fInput.process(io, numChannels, i);
            if (fPolyMode)
            {
                if (fPoly.process(track))
                    playFrame(fPoly.playFrame(), static_cast<uint32_t>(std::max<int64_t>(0, i + polyShift)));
            }
            else
            {
                playNote(note, i);
            }
            if (fMute)
                for (uint32_t ch = 0; ch < numChannels; ++ch)
                    io[ch][i] = 0.0f;
        }
    }

    void reset() override
    {
        fInput.reset();
        fPoly.reset();
        // (the engine ends the notes a block had sounding when it's rebuilt; this block forgets them)
        fOn = false;
        fPolyNotes.fill(-1);
        fBendSent = -1;
        fSinceBend = kBendEvery;
    }

private:
    template <typename T>
    void change(T& setting, T value)
    {
        if (setting != value)
            fRelease = true;
        setting = value;
    }

    void releaseAll(uint32_t frame)
    {
        if (fOut == nullptr)
            return;
        if (fOn)
        {
            fOut->noteOff(frame, fSentChannel, fNote);
            fOn = false;
        }
        if (fBendSent >= 0 && fBendSent != 8192)
            fOut->pitchBend(frame, fSentChannel, 8192);
        fBendSent = -1;
        for (int& n : fPolyNotes)
        {
            if (n >= 0)
                fOut->noteOff(frame, fSentChannel, n);
            n = -1;
        }
        fSentChannel = fChannel;
    }

    double sentPitch(double hz) const { return 69.0 + 12.0 * std::log2(hz / 440.0) + 12.0 * fOctave + fTranspose; }

    // Mono: a note on at each note's start (a pick, or a jump to a new pitch), bends in between, a note off at its end
    void playNote(const NoteTracker::Note& note, uint32_t frame)
    {
        if (fOut == nullptr)
            return;
        fSentChannel = fChannel;
        ++fSinceBend;
        if (!note.on || note.hz <= 0.0)
        {
            if (fOn)
            {
                fOut->noteOff(frame, fChannel, fNote);
                fOn = false;
            }
            if (fBendSent >= 0 && fBendSent != 8192)
            {
                fOut->pitchBend(frame, fChannel, 8192);
                fBendSent = 8192;
            }
            return;
        }
        const double pitch = sentPitch(note.hz);
        if (!fOn || note.onset || std::fabs(pitch - fLastPitch) >= kNewNoteJump)
        {
            if (fOn)
                fOut->noteOff(frame, fChannel, fNote);
            fNote = static_cast<int>(std::lround(pitch));
            if (fBendRange > 0)
            {
                // the bend before the note, so it starts at its pitch
                sendBend(frame, pitch, true);
            }
            fOut->noteOn(frame, fChannel, fNote, midiVelocity(note.velocity, fDynamics));
            fOn = true;
        }
        else if (fBendRange > 0)
        {
            sendBend(frame, pitch, false);
        }
        fLastPitch = pitch;
    }

    void sendBend(uint32_t frame, double pitch, bool now)
    {
        const double bend = std::clamp((pitch - fNote) / fBendRange, -1.0, 1.0);
        const auto value = static_cast<int>(std::lround(8192.0 + bend * 8191.0));
        if (!now && (std::abs(value - fBendSent) < kBendStep || fSinceBend < kBendEvery))
            return;
        if (value == fBendSent)
            return;
        fOut->pitchBend(frame, fChannel, value);
        fBendSent = value;
        fSinceBend = 0;
    }

    // Poly: each band's note on when it starts (or is picked again), off when it stops
    void playFrame(const PolyNoteFinder::Frame& f, uint32_t frame)
    {
        if (fOut == nullptr)
            return;
        fSentChannel = fChannel;
        for (int b = 0; b < PolyNoteFinder::kBands; ++b)
        {
            int& sent = fPolyNotes[static_cast<size_t>(b)];
            if (f.sounding[b] && (f.start[b] || sent < 0))
            {
                if (sent >= 0)
                    fOut->noteOff(frame, fChannel, sent);
                sent = PolyNoteFinder::kFirstNote + b + 12 * fOctave + fTranspose;
                const float level = f.start[b] ? f.velocity[b] : f.level[b];
                fOut->noteOn(frame, fChannel, sent, midiVelocity(2.0f * level, fDynamics));
            }
            else if (!f.sounding[b] && sent >= 0)
            {
                fOut->noteOff(frame, fChannel, sent);
                sent = -1;
            }
        }
    }

    TrackedInput fInput;
    PolyNoteFinder fPoly;
    MidiOutput* fOut = nullptr;
    const float* fRig[2] = { nullptr, nullptr };
    bool fTrackRig = true;
    bool fPolyMode = false;
    int fChannel = 0, fSentChannel = 0, fOctave = 0, fTranspose = 0, fBendRange = 2;
    float fDynamics = 0.7f;
    bool fMute = false;
    bool fRelease = false;

    bool fOn = false;
    int fNote = 60;
    double fLastPitch = 60.0;
    int fBendSent = -1;
    int fSinceBend = kBendEvery;
    std::array<int, PolyNoteFinder::kBands> fPolyNotes {};
};

} // namespace

int midiVelocity(float level, float dynamics)
{
    // the pick's level, in dB under full: -36 dB (and below) is 13, full level is 127
    const float linear = std::clamp(level * 4.0f, 1e-4f, 1.0f);
    const float picked = 127.0f * std::clamp(1.0f + 20.0f * std::log10(linear) / 36.0f, 0.1f, 1.0f);
    const float v = 100.0f + (picked - 100.0f) * std::clamp(dynamics, 0.0f, 1.0f);
    return std::clamp(static_cast<int>(std::lround(v)), 1, 127);
}

const BlockDescriptor kMidiOutBlockDescriptor = {
    "midiout",
    "MIDI Out",
    Category::Synth,
    kMidiOutParamCount,
    {
        ParamSpec { "Mode", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kModeNames },
        ParamSpec { "Channel", "", 0.0f, 15.0f, 0.0f, ParamScale::Choice, kChannelNames },
        ParamSpec { "Octave", "", 0.0f, 4.0f, 2.0f, ParamScale::Choice, kOctaveNames },
        ParamSpec { "Transpose", "", 0.0f, 24.0f, 12.0f, ParamScale::Choice, kTransposeNames },
        ParamSpec { "Bend", "st", 0.0f, 3.0f, 1.0f, ParamScale::Choice, kBendNames },
        ParamSpec { "Dynamics", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Focus", "%", 0.0f, 100.0f, 70.0f, ParamScale::Linear },
        ParamSpec { "Audio", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kAudioNames },
        ParamSpec { "Pick Filter", "ms", 0.0f, 60.0f, 0.0f, ParamScale::Linear },
        ParamSpec { "Track", "", 0.0f, 1.0f, 1.0f, ParamScale::Choice, TrackedInput::kTrackNames },
    },
    [] { return std::unique_ptr<Block>(new MidiOutBlock()); },
    -1,
    -1,
    false,
    nullptr,
    24.0f,
    MeterStyle::Reduction,
    true,
};

} // namespace xythos
