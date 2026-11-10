// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The tracking benchmark on real playing: the owner's DI recordings (tests/recordings, see its README.md: a 6-string's
// chords, riffs and bends, then a 7-string in drop A and a 5-string bass playing every fret, sustained notes, fast
// picking, legato, chords and riffs) against note lists (<clip>.notes). Decode them first with
// tools/decode_recordings.sh (ffmpeg), then:
//   XYTHOS_TRACKING_BENCH=1 ./build/xythos-tests -tc="*real recordings*"
// XYTHOS_TRACKING_CLIPS=1 prints each clip, XYTHOS_TRACKING_DEBUG=1 each miss and stray note, and
// XYTHOS_TRACKING_ONLY=<part of a name> takes only those clips. The scoring is in RealTracking.hpp.
//
// A .notes line is "start end notes" (seconds): a note listed is played and must sound (after a grace for its band
// to ring up, scored as delay), ?X may sound or not (a doubling or harmonic the recording can't settle), A|B is one of
// them (bends), and * is anything. Any other note sounding is a stray note.

#include "TestSupport.hpp"

#include "RealTracking.hpp"

#include <doctest/doctest.h>

using namespace xythos;

using namespace xythos::realtracking;

TEST_CASE("tracking benchmark on real recordings" * doctest::skip(std::getenv("XYTHOS_TRACKING_BENCH") == nullptr))
{
    if (clips().empty())
    {
        std::printf("no decoded recordings: run tools/decode_recordings.sh\n");
        return;
    }
    realBench("poly note finder", polyFinder(0.0), false);
    realBench("poly note finder, lookahead 20 ms", polyFinder(20.0), false);
    realBench("poly note finder, with hiss and hum at -66 dB", polyFinder(0.0, 0.0005f), false);
    realBench("note tracker (mono)", monoTracker(0.0f, 0.0), true);
    realBench("note tracker (mono), lookahead 15 ms", monoTracker(0.0f, 15.0), true);
}

TEST_CASE("the poly note finder on the owner's recordings")
{
    if (clips().empty() || std::getenv("XYTHOS_TRACKING_ONLY") != nullptr)
    {
        MESSAGE("no decoded recordings (tools/decode_recordings.sh), or only some of them: skipped");
        return;
    }
    // The finder rewritten on 2026-11-09 (PolyNoteFinder.hpp) against the filter bank before it, on all 34 clips
    // (908 notes, 842 s of note time): never found 55 (was 37: a strummed chord's octave doublings are often one note
    // with their lower octave now), missing note time 125 s (was 193 s), missing in every octave 26 s (was 76 s),
    // stray notes 1453, 84 s (was 4351, 401 s: 328 s of it harmonics of single notes). On the first eight clips alone
    // (chords, with their doublings listed as played) the filter bank, tuned on them, did better: 4 never found, 16 s
    // missing, 7 s of strays.
    const auto plain = realBench("poly note finder", polyFinder(0.0), false, false);
    const RealScore& first = plain.sets[static_cast<int>(ClipSet::First)];
    CHECK(first.missed <= 25);
    CHECK(first.missingMs <= 32000.0);
    CHECK(first.missingClassMs <= 4800.0);
    CHECK(first.strayMs <= 20000.0);
    const RealScore& guitar = plain.sets[static_cast<int>(ClipSet::Guitar)];
    CHECK(guitar.missed <= 28);
    CHECK(guitar.missingMs <= 92000.0);
    CHECK(guitar.missingClassMs <= 17500.0);
    CHECK(guitar.strayMs <= 34000.0);
    const RealScore& bass = plain.sets[static_cast<int>(ClipSet::Bass)];
    CHECK(bass.missed <= 8);
    CHECK(bass.missingMs <= 7000.0);
    CHECK(bass.missingClassMs <= 6200.0);
    CHECK(bass.strayMs <= 35000.0);
    // a live input's hiss and hum (-66 dB) cost some of the quiet bass notes' time, and add few strays
    // (the filter bank with them: 199 s missing, 365 s of strays)
    const auto noisy = realBench("poly note finder, with hiss and hum", polyFinder(0.0, 0.0005f), false, false);
    CHECK(noisy.total.missed <= 65);
    CHECK(noisy.total.missingMs <= 165000.0);
    CHECK(noisy.total.missingClassMs <= 65000.0);
    CHECK(noisy.total.strayMs <= 96000.0);
}

TEST_CASE("the note tracker on the owner's recordings")
{
    if (clips().empty() || std::getenv("XYTHOS_TRACKING_ONLY") != nullptr)
    {
        MESSAGE("no decoded recordings (tools/decode_recordings.sh), or only some of them: skipped");
        return;
    }
    // (a chord counts as found when any of its notes plays)
    // Before the pitch tracker's rework of 2026-11-09 (PitchTracker.hpp; NoteTracker.hpp's repeated notes and mutes),
    // on all 34 clips (770 notes, 629 s of note time): 25 never found, 160 s missing (a sustained low string read an
    // octave up within seconds, a bass's fundamental under 60 Hz was taken for strings ringing together, notes
    // ended at every pick of a tremolo), 2501 stray notes, 72 s. Now 2 never found, 28 s missing, 1017 strays, 29 s.
    const auto mono = realBench("note tracker (mono)", monoTracker(0.0f, 0.0), true, false);
    const RealScore& first = mono.sets[static_cast<int>(ClipSet::First)];
    CHECK(first.missed <= 1);
    CHECK(first.missingMs <= 8600.0);
    CHECK(first.strayMs <= 8400.0);
    CHECK(first.straySubMs <= 7700.0); // (palm-muted power chords, an octave below: the mute's thump is their fundamental)
    const RealScore& guitar = mono.sets[static_cast<int>(ClipSet::Guitar)];
    CHECK(guitar.missed <= 2);
    CHECK(guitar.missingMs <= 11600.0);
    CHECK(guitar.strayMs <= 10700.0);
    const RealScore& bass = mono.sets[static_cast<int>(ClipSet::Bass)];
    CHECK(bass.missed <= 1);
    CHECK(bass.missingMs <= 9000.0);
    CHECK(bass.strayMs <= 11900.0);
}
