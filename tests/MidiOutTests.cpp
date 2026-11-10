// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// MIDI output: the engine's MIDI path (timing, latency, note-offs when a block goes) and the MIDI Out block

#include "GuitarSignals.hpp"
#include "TestSupport.hpp"

#include "engine/CpuHints.hpp"
#include "engine/Engine.hpp"
#include "engine/blocks/MidiOutBlock.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace xythos;

namespace {

constexpr double kRate = 48000.0;

size_t ms(double milliseconds) { return static_cast<size_t>(milliseconds / 1000.0 * kRate); }

struct Sent {
    int64_t time;
    int status, a, b;
};

// Runs a block over `x` in buffers of 128, collecting what it sends, timed from the first sample
std::vector<Sent> runMidi(Block& block, std::vector<std::vector<float>>& x)
{
    std::vector<Sent> sent;
    MidiOutput out;
    MidiSounding sounding {};
    block.setMidiOutput(&out);
    const auto channels = static_cast<uint32_t>(x.size());
    for (size_t t = 0; t < x[0].size(); t += 128)
    {
        const auto n = static_cast<uint32_t>(std::min<size_t>(128, x[0].size() - t));
        out.clear();
        out.setNode(static_cast<int64_t>(t), 0, &sounding);
        float* io[2] = { x[0].data() + t, x[channels > 1 ? 1 : 0].data() + t };
        testing::AllocationGuard guard;
        block.process(io, channels, n);
        REQUIRE(guard.count() == 0);
        for (int i = 0; i < out.count(); ++i)
        {
            const MidiMessage& m = out.at(i);
            sent.push_back({ m.time, m.data[0], m.data[1], m.data[2] });
        }
    }
    return sent;
}

std::vector<Sent> only(const std::vector<Sent>& sent, int status)
{
    std::vector<Sent> found;
    for (const Sent& s : sent)
        if ((s.status & 0xf0) == status)
            found.push_back(s);
    return found;
}

guitar::Performance play(std::vector<guitar::Note> notes, double seconds)
{
    return guitar::perform(notes, seconds, { 0.0f, 0.0f });
}

guitar::Note pick(double midi, double start, double length, int string = 2)
{
    guitar::Note note;
    note.midi = midi;
    note.start = start;
    note.length = length;
    note.string = string;
    return note;
}

std::unique_ptr<Block> midiOut(double lookaheadMs = 0.0)
{
    auto block = testing::createWithDefaults(kMidiOutBlockDescriptor, kRate);
    REQUIRE(block->configure({ { "lookahead", lookaheadMs } }));
    for (int p = 0; p < kMidiOutBlockDescriptor.numParams; ++p)
        block->setParam(p, kMidiOutBlockDescriptor.params[static_cast<size_t>(p)].def);
    return block;
}

// A test block that holds a note on (60) from its first buffer, on slot `slot`'s channel 0
class HoldNoteBlock : public Block {
public:
    void prepare(double, uint32_t) override {}
    void setParam(int, float) override {}
    void setMidiOutput(MidiOutput* out) override { fOut = out; }
    void process(float* const*, uint32_t, uint32_t) override
    {
        if (!fSent && fOut)
        {
            fOut->noteOn(5, 0, 60, 100);
            fSent = true;
        }
    }

private:
    MidiOutput* fOut = nullptr;
    bool fSent = false;
};

const BlockDescriptor kHoldNoteDescriptor = {
    "test.holdnote", "Hold Note", Category::Synth, 0, {}, [] { return std::unique_ptr<Block>(new HoldNoteBlock()); },
};

const BlockRegistry& midiRegistry()
{
    static const BlockRegistry registry({ &kHoldNoteDescriptor, &testing::kGainDescriptor });
    return registry;
}

struct MidiRig {
    Engine engine { midiRegistry() };
    std::vector<float> in = std::vector<float>(256, 0.0f), out = std::vector<float>(256, 0.0f);
    std::vector<Sent> sent;

    MidiRig() { engine.prepare(kRate, 256); }

    void run(int buffers)
    {
        for (int k = 0; k < buffers; ++k)
        {
            const float* inputs[2] = { in.data(), in.data() };
            float* outputs[2] = { out.data(), out.data() };
            const int64_t start = engine.midiTime();
            engine.process(inputs, outputs, 256);
            engine.drainMidi(start + 256, [&](const MidiMessage& m) {
                sent.push_back({ m.time - start, m.data[0], m.data[1], m.data[2] });
            });
        }
    }
};

ChainState chainOf(std::vector<std::pair<const char*, nlohmann::json>> blocks)
{
    ChainState chain;
    int slot = 0;
    for (auto& [type, config] : blocks)
    {
        BlockState block;
        block.typeId = type;
        block.slot = slot++;
        block.config = config;
        chain.blocks.push_back(block);
    }
    return chain;
}

} // namespace

TEST_CASE("midi output ends what a slot has sounding")
{
    MidiOutput out;
    MidiSounding sounding {};
    out.setNode(1000, 3, &sounding);
    out.noteOn(10, 0, 60, 90);
    out.noteOn(12, 2, 64, 0); // velocity 0 is sent as 1 (0 would be a note off)
    out.noteOff(20, 0, 60);
    out.pitchBend(30, 2, 20000);
    REQUIRE(out.count() == 4);
    CHECK(out.at(0).time == 1010);
    CHECK(out.at(0).data[0] == 0x90);
    CHECK(out.at(1).data[2] == 1);
    CHECK(out.at(3).data[0] == 0xe2);
    CHECK(out.at(3).data[1] == 0x7f); // clamped to 16383
    CHECK(out.at(3).data[2] == 0x7f);
    out.clear();
    out.endSlot(3, 5000);
    // the note still on (64 on channel 3), and the bend back to the middle
    REQUIRE(out.count() == 2);
    CHECK(out.at(0).time == 5000);
    CHECK(out.at(0).data[0] == 0x82);
    CHECK(out.at(0).data[1] == 64);
    CHECK(out.at(1).data[0] == 0xe2);
    CHECK(out.at(1).data[2] == 64);
    out.clear();
    out.endSlot(3, 6000);
    CHECK(out.count() == 0);
}

TEST_CASE("midi velocity follows the pick, as far as Dynamics asks")
{
    CHECK(midiVelocity(0.25f, 1.0f) == 127);
    CHECK(midiVelocity(0.0f, 1.0f) == 13);
    CHECK(midiVelocity(0.25f / 8.0f, 1.0f) < midiVelocity(0.25f / 2.0f, 1.0f));
    CHECK(midiVelocity(0.001f, 0.0f) == 100);
    CHECK(midiVelocity(0.25f, 0.0f) == 100);
}

TEST_CASE("the engine sends a block's midi, after the latency behind it, and ends its notes when it goes")
{
    MidiRig rig;
    // the note block, then a block with 300 samples of latency: the note comes 300 samples later, with the audio
    REQUIRE(rig.engine.setChain(chainOf({ { "test.holdnote", nlohmann::json::object() },
                                          { "test.gain", { { "latency", 300 } } } })));
    rig.run(8);
    REQUIRE(rig.sent.size() == 1);
    CHECK(rig.sent[0].status == 0x90);
    const int64_t sentAt = rig.sent[0].time;
    CHECK(sentAt >= 0);

    // bypassed: its note ends
    rig.sent.clear();
    rig.engine.setParameter(slotBypassParam(0), 1.0f);
    rig.run(20);
    REQUIRE(!rig.sent.empty());
    CHECK(rig.sent[0].status == 0x80);
    CHECK(rig.sent[0].a == 60);

    // back on (a new note: it sends once per block), then removed: the note ends with it
    rig.engine.setParameter(slotBypassParam(0), 0.0f);
    REQUIRE(rig.engine.setChain(chainOf({ { "test.holdnote", { { "x", 1 } } } })));
    rig.run(20);
    rig.sent.clear();
    REQUIRE(rig.engine.setChain(chainOf({ { "test.gain", nlohmann::json::object() } })));
    rig.run(20);
    bool ended = false;
    for (const Sent& s : rig.sent)
        ended |= s.status == 0x80 && s.a == 60;
    CHECK(ended);
}

TEST_CASE("midi is delayed by the latency of the blocks after its block")
{
    auto timeOf = [](uint32_t latency) {
        MidiRig rig;
        REQUIRE(rig.engine.setChain(chainOf({ { "test.holdnote", nlohmann::json::object() },
                                              { "test.gain", { { "latency", latency } } } })));
        // the first buffers fade the new chain in; the block sends in its first processed buffer
        rig.run(8);
        REQUIRE(rig.sent.size() == 1);
        return rig.sent[0].time;
    };
    // drained per buffer, the time is within the buffer it came out in: 5 + latency, modulo the buffer
    const int64_t none = timeOf(0), late = timeOf(300);
    CHECK((late - none + 256) % 256 == 300 % 256);
}

TEST_CASE("midi out (mono) sends the note you play, with its velocity, and ends it when muted")
{
    auto block = midiOut();
    const auto p = play({ pick(57, 0.1, 0.5), pick(62, 0.8, 0.4) }, 1.5);
    std::vector<std::vector<float>> x = { p.signal };
    const auto sent = runMidi(*block, x);
    const auto ons = only(sent, 0x90), offs = only(sent, 0x80);
    REQUIRE(ons.size() == 2);
    REQUIRE(offs.size() == 2);
    CHECK(ons[0].a == 57);
    CHECK(ons[1].a == 62);
    // on within about 25 ms of the pick; off within about 40 ms of the mute
    CHECK(ons[0].time >= static_cast<int64_t>(ms(100)));
    CHECK(ons[0].time < static_cast<int64_t>(ms(125)));
    CHECK(offs[0].time > static_cast<int64_t>(ms(550)));
    CHECK(offs[0].time < static_cast<int64_t>(ms(640)));
    CHECK(ons[0].b > 40);
    // the audio passes through
    CHECK(testing::rms(x[0], ms(200), ms(500)) > 0.01);
}

TEST_CASE("midi out: octave and transpose shift the notes, Mute silences the audio")
{
    auto block = midiOut();
    block->setParam(kMidiOutParamOctave, 3.0f);    // +1
    block->setParam(kMidiOutParamTranspose, 14.0f); // +2
    block->setParam(kMidiOutParamAudio, 1.0f);
    block->setParam(kMidiOutParamChannel, 9.0f); // channel 10
    const auto p = play({ pick(57, 0.1, 0.5) }, 1.0);
    std::vector<std::vector<float>> x = { p.signal };
    const auto sent = runMidi(*block, x);
    const auto ons = only(sent, 0x90);
    REQUIRE(ons.size() == 1);
    CHECK(ons[0].a == 57 + 12 + 2);
    CHECK((ons[0].status & 0x0f) == 9);
    CHECK(testing::rms(x[0], 0, x[0].size()) == 0.0);
}

TEST_CASE("midi out bends with the note, and Bend Off sends plain notes")
{
    auto bends = [](float bend) {
        auto block = midiOut();
        block->setParam(kMidiOutParamBend, bend);
        guitar::Note note = pick(57, 0.1, 1.2);
        note.bend = 1.0; // up a semitone over 0.6 s
        const auto p = play({ note }, 1.5);
        std::vector<std::vector<float>> x = { p.signal };
        return only(runMidi(*block, x), 0xe0);
    };
    const auto two = bends(1.0f); // 2 semitones
    REQUIRE(two.size() > 10);
    // a semitone up on a range of two: half way up, 8192 + 4096
    int highest = 0;
    for (const Sent& s : two)
        highest = std::max(highest, s.a | (s.b << 7));
    CHECK(highest == doctest::Approx(8192 + 4096).epsilon(0.02));
    CHECK(bends(0.0f).empty());
}

TEST_CASE("midi out sends from the pick with lookahead")
{
    auto block = midiOut(20.0);
    const auto p = play({ pick(57, 0.1, 0.5) }, 1.0);
    std::vector<std::vector<float>> x = { p.signal };
    const auto ons = only(runMidi(*block, x), 0x90);
    REQUIRE(ons.size() == 1);
    // in the block's (delayed) timeline: the pick plus the latency, give or take a few ms
    const auto at = static_cast<double>(ons[0].time - block->latency()) / kRate * 1000.0;
    CHECK(at > 99.0);
    CHECK(at < 104.0);
}

TEST_CASE("midi out (poly) sends the notes of a chord, and ends them")
{
    auto block = midiOut();
    block->setParam(kMidiOutParamMode, 1.0f);
    // A major, strummed: A2 E3 A3 C#4 E4 (the octave doublings may count as overtones)
    std::vector<guitar::Note> chord;
    const int notes[] = { 45, 52, 57, 61, 64 };
    for (int k = 0; k < 5; ++k)
        chord.push_back(pick(notes[k], 0.1 + 0.012 * k, 0.8 - 0.012 * k, k + 1));
    const auto p = play(chord, 1.5);
    std::vector<std::vector<float>> x = { p.signal };
    const auto sent = runMidi(*block, x);
    const auto ons = only(sent, 0x90), offs = only(sent, 0x80);
    std::vector<int> played;
    for (const Sent& s : ons)
        played.push_back(s.a);
    CAPTURE(played.size());
    // the root, the third and the fifth are all sent, and nothing but the chord's notes and their harmonics (up to
    // the 8th: a strummed string's harmonics show before its fundamental has settled, and can pass for notes for a
    // moment)
    for (const int n : { 45, 52, 61 })
        CHECK(std::count(played.begin(), played.end(), n) >= 1);
    for (const int n : played)
    {
        bool known = false;
        for (const int c : notes)
            for (const int harmonic : { 0, 12, 19, 24, 28, 31, 34, 36 })
                known = known || n == c + harmonic;
        CAPTURE(n);
        CHECK(known);
    }
    // every note that went on went off again
    CHECK(offs.size() == ons.size());
}


TEST_CASE("midi out: CPU use" * doctest::skip(std::getenv("XYTHOS_CPU") == nullptr))
{
    // as hosts do on their audio threads: denormals flushed to zero
    const FloatState csr = flushDenormals();
    for (const float mode : { 0.0f, 1.0f })
    {
        auto block = midiOut();
        block->setParam(kMidiOutParamMode, mode);
        const auto p = play(guitar::fastRun(), 10.0);
        std::vector<std::vector<float>> x = { p.signal, p.signal };
        const auto start = std::chrono::steady_clock::now();
        runMidi(*block, x);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("midi out %s: %.2f%% of a core\n", mode > 0.5f ? "poly" : "mono", 100.0 * seconds / 10.0);
    }
    setFloatState(csr);
}
