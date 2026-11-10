// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The pitch tracking benchmark: a synthetic guitar (GuitarSignals.hpp) played through the trackers the synths and
// pitch pedals use, counting notes missed, wrong notes and notes nobody played, and how late notes start.

#include "GuitarSignals.hpp"
#include "TestSupport.hpp"

#include "engine/blocks/SynthBlock.hpp"
#include "engine/dsp/NoteTracker.hpp"
#include "engine/dsp/PickFilter.hpp"
#include "engine/dsp/PitchTracker.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

using namespace xythos;

namespace {

// What a tracker plays at one sample: on, at hz, at a level (the synths' amplitude, before Attack and Release)
struct Played {
    bool on = false;
    double hz = 0.0;
    float level = 0.0f;
};

struct Input {
    const std::vector<float>* block; // what reaches the pedal
    const std::vector<float>* rig;   // the clean guitar
};

// A tracker under test: fills `out` (one per sample) and returns its latency in samples
using Tracker = std::function<size_t(const Input&, std::vector<Played>&)>;

// The tracking of the synths up to now: PitchTracker, then the Pick Filter
Tracker legacyTracker(float pickMs)
{
    return [pickMs](const Input& in, std::vector<Played>& out) {
        PitchTracker tracker;
        PickFilter pick;
        tracker.prepare(guitar::kRate);
        pick.prepare(guitar::kRate);
        pick.setMs(pickMs);
        double hz = 0.0;
        out.resize(in.block->size());
        for (size_t t = 0; t < out.size(); ++t)
        {
            const bool estimated = tracker.process((*in.block)[t]);
            if (pick.process(tracker.sounding(), estimated ? tracker.hz() : 0.0))
                hz = pick.hz();
            const bool on = tracker.sounding() && pick.open() && hz > 0.0;
            out[t] = { on, hz, on ? std::min(1.0f, tracker.envelope() * 4.0f) : 0.0f };
        }
        return size_t { 0 };
    };
}

// The note tracker, tracking the pedal's input or the rig's
Tracker noteTracker(float pickMs, double lookaheadMs, bool rig)
{
    return [=](const Input& in, std::vector<Played>& out) {
        NoteTracker tracker;
        tracker.prepare(guitar::kRate);
        tracker.setPickFilter(pickMs);
        tracker.setLookahead(lookaheadMs);
        const std::vector<float>& x = rig ? *in.rig : *in.block;
        out.resize(x.size());
        static const char* const kReasons[] = { "none", "pick", "mute", "gate", "unclear", "other pitch" };
        const bool debug = std::getenv("XYTHOS_TRACKING_ENDS") != nullptr;
        for (size_t t = 0; t < out.size(); ++t)
        {
            const bool was = t > 0 && out[t - 1].on;
            const NoteTracker::Note& n = tracker.process(x[t]);
            if (debug && was && !n.on)
                std::printf("    end at %.3f s: %s (level %.4f)\n", t / guitar::kRate,
                            kReasons[static_cast<int>(tracker.endReason())], n.envelope);
            out[t] = { n.on, n.hz, n.on ? std::min(1.0f, n.envelope * 4.0f) : 0.0f };
        }
        return static_cast<size_t>(tracker.latency());
    };
}

struct Score {
    int notes = 0;
    int missed = 0;
    double medianDelayMs = 0.0, worstDelayMs = 0.0;
    int wrong = 0;   // audible stretches at a wrong pitch (over half a semitone off) while a note is played
    int phantom = 0; // audible stretches while nothing is played
    double dropoutMs = 0.0; // silent while a note plays, after it was found
    double wrongMs = 0.0, phantomMs = 0.0;
};

constexpr float kAudible = 0.01f; // -40 dB of the synths' full level

Score score(const guitar::Performance& p, const std::vector<Played>& out, size_t latency)
{
    Score s;
    const size_t length = p.signal.size();
    auto played = [&](size_t t) -> const Played& {
        static const Played none;
        return t + latency < out.size() ? out[t + latency] : none;
    };
    auto right = [&](size_t t) {
        const Played& x = played(t);
        return x.on && x.level > 0.0f && std::fabs(12.0 * std::log2(x.hz / guitar::midiHz(p.midi[t]))) < 0.5;
    };

    // notes: found once played right for 5 ms in a row
    const auto run = static_cast<size_t>(0.005 * guitar::kRate);
    std::vector<double> delays;
    for (const guitar::Note& note : p.notes)
    {
        const auto from = static_cast<size_t>(note.start * guitar::kRate);
        const auto to = std::min(length, static_cast<size_t>((note.start + guitar::truthLength(note)) * guitar::kRate));
        ++s.notes;
        size_t streak = 0;
        bool found = false;
        for (size_t t = from; t < to; ++t)
        {
            if (p.truth[t] != guitar::Truth::Note)
                break;
            streak = right(t) ? streak + 1 : 0;
            if (streak >= run)
            {
                delays.push_back(1000.0 * static_cast<double>(t + 1 - run - from) / guitar::kRate);
                found = true;
                size_t gap = 0;
                for (size_t u = t; u < to && p.truth[u] == guitar::Truth::Note; ++u)
                {
                    if (!played(u).on)
                    {
                        s.dropoutMs += 1000.0 / guitar::kRate;
                        if (gap++ == 0 && std::getenv("XYTHOS_TRACKING_DEBUG"))
                            std::printf("    dropout in %.1f at %.3f s (level %.4f)\n", note.midi, u / guitar::kRate,
                                        played(u).level);
                    }
                    else
                    {
                        gap = 0;
                    }
                }
                break;
            }
        }
        if (!found)
        {
            ++s.missed;
            if (std::getenv("XYTHOS_TRACKING_DEBUG"))
                std::printf("    missed %.1f at %.3f s\n", note.midi, note.start);
        }
    }
    if (!delays.empty())
    {
        std::sort(delays.begin(), delays.end());
        s.medianDelayMs = delays[delays.size() / 2];
        s.worstDelayMs = delays.back();
    }

    // wrong and phantom stretches: at least 3 ms long, gaps under 5 ms joined
    const bool debug = std::getenv("XYTHOS_TRACKING_DEBUG") != nullptr;
    auto midiOf = [](double hz) { return hz > 0.0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0; };
    auto report = [&](size_t begin, size_t end, int& count) {
        ++count;
        if (debug)
            std::printf("    at %7.3f s for %5.1f ms: played %5.1f, truth %5.1f (%s)\n", begin / guitar::kRate,
                        1000.0 * static_cast<double>(end - begin) / guitar::kRate, midiOf(played(begin).hz),
                        p.truth[begin] == guitar::Truth::Note ? p.midi[begin] : 0.0,
                        p.truth[begin] == guitar::Truth::Note ? "note" : "silent");
    };
    auto stretches = [&](auto bad, int& count, double& ms) {
        const auto minimum = static_cast<size_t>(0.003 * guitar::kRate);
        const auto join = static_cast<size_t>(0.005 * guitar::kRate);
        size_t begin = 0, end = 0, total = 0;
        bool open = false;
        for (size_t t = 0; t <= length; ++t)
        {
            const bool b = t < length && bad(t);
            if (b)
            {
                if (!open || t > end + join)
                {
                    if (open && end - begin >= minimum)
                        report(begin, end, count);
                    begin = t;
                    open = true;
                }
                end = t + 1;
                ++total;
            }
        }
        if (open && end - begin >= minimum)
            report(begin, end, count);
        ms = 1000.0 * static_cast<double>(total) / guitar::kRate;
    };
    stretches(
        [&](size_t t) {
            return p.truth[t] == guitar::Truth::Note && played(t).on && played(t).level > kAudible && !right(t);
        },
        s.wrong, s.wrongMs);
    stretches([&](size_t t) { return p.truth[t] == guitar::Truth::Silent && played(t).on && played(t).level > kAudible; },
              s.phantom, s.phantomMs);
    return s;
}

struct Scene {
    const char* name;
    guitar::Performance performance;
    std::vector<float> amped; // through a high-gain amp
};

const std::vector<Scene>& scenes()
{
    static const std::vector<Scene> all = [] {
        std::vector<Scene> s;
        auto add = [&](const char* name, std::vector<guitar::Note> notes, double seconds) {
            guitar::Performance p = guitar::perform(notes, seconds);
            std::vector<float> amped = guitar::throughAmp(p.signal);
            s.push_back({ name, std::move(p), std::move(amped) });
        };
        add("single notes", guitar::singleNotes(), 12.5);
        add("fast run", guitar::fastRun(), 3.5);
        add("legato, repeats", guitar::legatoAndRepeats(), 5.0);
        add("ringing arpeggio", guitar::ringingArpeggio(), 4.5);
        add("palm mutes", guitar::palmMutes(), 6.5);
        add("bends, vibrato", guitar::bendsAndVibrato(), 6.5);
        add("long decay", guitar::longDecay(), 12.5);
        return s;
    }();
    return all;
}

// A 5-string bass, down to its low B (31 Hz), and a 7-string's drop A (55 Hz) on the way (owner, 2026-10-29)
const std::vector<Scene>& bassScenes()
{
    static const std::vector<Scene> all = [] {
        std::vector<Scene> s;
        auto add = [&](const char* name, std::vector<guitar::Note> notes, double seconds) {
            guitar::Performance p = guitar::perform(notes, seconds);
            std::vector<float> amped = guitar::throughAmp(p.signal);
            s.push_back({ name, std::move(p), std::move(amped) });
        };
        add("bass notes", guitar::bassNotes(), 15.0);
        add("bass line", guitar::bassLine(), 9.0);
        return s;
    }();
    return all;
}

// Runs every scene clean and through the amp (tracking the amp's output, or the clean rig input when `rig`)
std::vector<Score> bench(const char* name, const Tracker& tracker, bool amped, bool rig, bool print = true,
                         const std::vector<Scene>& list = scenes())
{
    std::vector<Score> scores;
    auto printf = [print](const char* format, auto... args) {
        if (print)
            std::printf(format, args...);
    };
    printf("\n%s%s\n", name, amped ? (rig ? " (after an amp, tracking the rig input)" : " (after an amp)") : "");
    printf("  %-18s %5s %6s %9s %9s %6s %8s %7s %9s %8s\n", "scene", "notes", "missed", "delay ms", "worst ms",
                "wrong", "wrong ms", "phantom", "phant. ms", "drop ms");
    Score total;
    for (const Scene& scene : list)
    {
        const std::vector<float>& block = amped ? scene.amped : scene.performance.signal;
        std::vector<Played> out;
        const size_t latency = tracker({ &block, &scene.performance.signal }, out);
        (void)rig;
        const Score s = score(scene.performance, out, latency);
        printf("  %-18s %5d %6d %9.1f %9.1f %6d %8.0f %7d %9.0f %8.0f\n", scene.name, s.notes, s.missed,
                    s.medianDelayMs, s.worstDelayMs, s.wrong, s.wrongMs, s.phantom, s.phantomMs, s.dropoutMs);
        scores.push_back(s);
        total.notes += s.notes;
        total.missed += s.missed;
        total.wrong += s.wrong;
        total.phantom += s.phantom;
        total.wrongMs += s.wrongMs;
        total.phantomMs += s.phantomMs;
        total.dropoutMs += s.dropoutMs;
    }
    printf("  %-18s %5d %6d %9s %9s %6d %8.0f %7d %9.0f %8.0f\n", "total", total.notes, total.missed, "", "",
                total.wrong, total.wrongMs, total.phantom, total.phantomMs, total.dropoutMs);
    scores.push_back(total);
    return scores;
}

} // namespace

TEST_CASE("tracking benchmark" * doctest::skip(std::getenv("XYTHOS_TRACKING_BENCH") == nullptr))
{
    bench("legacy, pick filter off", legacyTracker(0.0f), false, false);
    bench("legacy, pick filter 15 ms", legacyTracker(15.0f), false, false);
    bench("legacy, pick filter 15 ms", legacyTracker(15.0f), true, false);
    bench("note tracker", noteTracker(0.0f, 0.0, false), false, false);
    bench("note tracker, pick filter 15 ms", noteTracker(15.0f, 0.0, false), false, false);
    bench("note tracker", noteTracker(0.0f, 0.0, false), true, false);
    bench("note tracker", noteTracker(0.0f, 0.0, true), true, true);
    bench("note tracker, lookahead 15 ms", noteTracker(0.0f, 15.0, false), false, false);
    bench("note tracker, lookahead 15 ms, pick filter 15 ms", noteTracker(15.0f, 15.0, false), false, false);
    bench("note tracker, bass", noteTracker(0.0f, 0.0, false), false, false, true, bassScenes());
    bench("note tracker, bass", noteTracker(0.0f, 0.0, false), true, false, true, bassScenes());
    bench("note tracker, bass, lookahead 15 ms", noteTracker(0.0f, 15.0, false), false, false, true, bassScenes());
}

TEST_CASE("tracking debug" * doctest::skip(std::getenv("XYTHOS_TRACKING_DEBUG") == nullptr))
{
    bench("note tracker", noteTracker(0.0f, 0.0, false), false, false);
    bench("note tracker", noteTracker(0.0f, 0.0, false), true, false);
}

TEST_CASE("the note tracker plays the benchmark's notes with few wrong or phantom notes")
{
    // the scores before the note tracker (the pitch tracker and the Pick Filter, off): 165 wrong-pitch stretches,
    // 56 phantom ones (6.7 s), 1 missed note. Since the chirp rules (doubtful pitches need a third estimate,
    // 2026-10-24): 25 wrong, 4 missed (one more in the ringing arpeggio, where the strings left ringing blur it).
    // Since the pitch tracker's rework on the owner's 7-string and bass recordings (2026-11-09; PitchTracker.hpp): 10
    // missed and 37 wrong (3.0 s), 9 of the missed and 2.5 s of the wrong in the ringing arpeggio: with strings left
    // ringing the pitch is now the lowest note there (as for a chord), so a note picked above it isn't found. Without
    // that scene 1 missed and 27 wrong (0.5 s), and single notes are found in 12 ms with none wrong (was 2). On the
    // recordings, which count: 4.4% of the notes' time missing, was 25%.
    const std::vector<Score> clean = bench("note tracker", noteTracker(0.0f, 0.0, false), false, false, false);
    const Score& total = clean.back();
    CHECK(total.missed <= 10);
    CHECK(total.missed - clean[3].missed <= 1); // (all but the ringing arpeggio's)
    CHECK(total.wrong <= 38);
    CHECK(total.wrong - clean[3].wrong <= 28);
    CHECK(clean[0].wrong == 0); // single notes
    CHECK(total.phantom <= 20);
    CHECK(total.phantomMs <= 200.0);
    CHECK(clean[0].medianDelayMs <= 14.0); // single notes

    // after an amp, tracking the rig's input is as good as tracking the clean guitar
    const std::vector<Score> rig = bench("note tracker", noteTracker(0.0f, 0.0, true), true, true, false);
    CHECK(rig.back().wrong == total.wrong);

    // with lookahead, notes start (nearly) on time
    const std::vector<Score> ahead = bench("note tracker, lookahead 15 ms", noteTracker(0.0f, 15.0, false), false, false, false);
    CHECK(ahead[0].medianDelayMs <= 4.0);
    CHECK(ahead.back().wrong <= total.wrong);
    CHECK(ahead.back().missed <= 10);
}

TEST_CASE("the note tracker plays a 5-string bass down to its low B")
{
    // (owner, 2026-10-29: a 7-string's drop A, 55 Hz, wasn't tracked at all: the tracker stopped at 60 Hz. Before
    // the range went down to 28 Hz, 37 of these 53 notes were missed.)
    const std::vector<Score> clean = bench("note tracker, bass", noteTracker(0.0f, 0.0, false), false, false, false,
                                           bassScenes());
    CHECK(clean.back().missed == 0);
    CHECK(clean.back().wrong <= 20);
    CHECK(clean.back().phantom <= 10);
    CHECK(clean[0].medianDelayMs <= 50.0); // a low note needs about two of its periods (B0's are 32 ms)
    const std::vector<Score> amped = bench("note tracker, bass", noteTracker(0.0f, 0.0, false), true, false, false,
                                           bassScenes());
    CHECK(amped.back().missed == 0);
}

TEST_CASE("the note tracker doesn't chirp when the pick stops the string")
{
    // As the pick meets the string to play the next note, the string stops and the tracker can find one of its
    // harmonics for a few ms (here its 4th, two octaves up), with the level falling fast; it used to play that.
    const double rate = guitar::kRate;
    const auto tone = [](double hz, double t, double level) {
        return static_cast<float>(level * (std::sin(2 * M_PI * hz * t) + 0.7 * std::sin(2 * M_PI * 2 * hz * t + 1.0)
                                           + 0.4 * std::sin(2 * M_PI * 3 * hz * t + 2.0)));
    };
    std::vector<float> x;
    for (int i = 0; i < static_cast<int>(0.25 * rate); ++i)
        x.push_back(tone(392.0, i / rate, 0.02));
    for (int i = 0; i < static_cast<int>(0.014 * rate); ++i)
    {
        const double t = i / rate;
        x.push_back(static_cast<float>(0.02 * std::exp(-t / 0.006) * std::sin(2 * M_PI * 1568.0 * t)));
    }
    for (int i = 0; i < static_cast<int>(0.2 * rate); ++i)
    {
        const double t = i / rate;
        x.push_back(tone(445.0, t, 0.03 * (1.0 - std::exp(-t / 0.0005))));
    }
    NoteTracker tracker;
    tracker.prepare(rate);
    double highest = 0.0, last = 0.0;
    for (const float v : x)
    {
        const NoteTracker::Note& note = tracker.process(v);
        if (note.on)
        {
            highest = std::max(highest, note.hz);
            last = note.hz;
        }
    }
    CHECK(highest < 460.0);
    CHECK(last == doctest::Approx(445.0).epsilon(0.01));
}

namespace {

// Sets a pitch-following block to pass only its dry signal
void dryOnly(const BlockDescriptor& type, Block& block)
{
    for (int p = 0; p < type.numParams; ++p)
    {
        const std::string name = type.params[static_cast<size_t>(p)].name;
        if (name == "Mix" || name == "Level" || name == "Level 1" || name == "Level 2" || name == "Sub 1"
            || name == "Sub 2" || name == "Up")
            block.setParam(p, 0.0f);
        if (name == "Dry")
            block.setParam(p, 100.0f);
    }
}

} // namespace

TEST_CASE("pitch-following blocks: lookahead delays the dry signal by the latency they report")
{
    int count = 0;
    for (const BlockDescriptor* type : BlockRegistry::builtin().types())
    {
        if (!type->lookahead)
            continue;
        ++count;
        CAPTURE(std::string(type->typeId));
        for (const bool poly : { false, true })
        {
            if (poly && !type->poly)
                continue;
            CAPTURE(poly);
            auto block = testing::createWithDefaults(*type, guitar::kRate);
            CHECK(block->latency() == 0);
            REQUIRE(block->configure({ { "lookahead", 15.0 }, { "poly", poly } }));
            // 720 samples (Poly works in whole hops of 5.3 ms)
            CHECK(std::abs(static_cast<int>(block->latency()) - 720) <= 128);
            for (int p = 0; p < type->numParams; ++p)
                block->setParam(p, type->params[static_cast<size_t>(p)].def);
            dryOnly(*type, *block);

            const guitar::Performance p = guitar::perform(guitar::singleNotes(), 1.0);
            std::vector<std::vector<float>> x = { p.signal, p.signal };
            int allocations = 0;
            testing::runBlock(*block, x, 128, &allocations);
            CHECK(allocations == 0);
            const size_t latency = block->latency();
            double error = 0.0;
            for (size_t t = 24000; t < x[0].size(); ++t)
                error = std::max(error, static_cast<double>(std::fabs(x[0][t] - p.signal[t - latency])));
            CHECK(error < 1e-3);

            REQUIRE(block->configure({ { "lookahead", 0.0 }, { "poly", poly } }));
            CHECK(block->latency() == 0);
        }
    }
    CHECK(count == 24); // (25 with the Poly Chip, removed on 2026-11-10)
}

TEST_CASE("with lookahead, a synth's notes start with the pick")
{
    // where the synth first plays (relative to the pick, in the block's own timeline), with and without lookahead
    auto start = [](double lookaheadMs) {
        auto block = testing::createWithDefaults(kSynthBlockDescriptor, guitar::kRate);
        REQUIRE(block->configure({ { "lookahead", lookaheadMs } }));
        for (int p = 0; p < kSynthBlockDescriptor.numParams; ++p)
            block->setParam(p, kSynthBlockDescriptor.params[static_cast<size_t>(p)].def);
        block->setParam(kSynthParamAttack, 1.0f);
        guitar::Note note;
        note.midi = 57;
        note.start = 0.1;
        note.length = 0.5;
        const guitar::Performance p = guitar::perform({ note }, 0.8, { 0.0f, 0.0f });
        std::vector<std::vector<float>> x = { p.signal };
        testing::runBlock(*block, x);
        float peak = 0.0f;
        for (const float v : x[0])
            peak = std::max(peak, std::fabs(v));
        size_t t = 0;
        while (t < x[0].size() && std::fabs(x[0][t]) < 0.1f * peak)
            ++t;
        return 1000.0 * ((static_cast<double>(t) - block->latency()) / guitar::kRate - note.start);
    };
    const double late = start(0.0), onTime = start(20.0);
    CAPTURE(late);
    CAPTURE(onTime);
    CHECK(late > 6.0);
    CHECK(onTime < 3.0);
    CHECK(onTime > -1.0);
}

