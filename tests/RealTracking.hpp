// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The tracking benchmark on real playing, shared by tests/RealTrackingTests.cpp and tools that tune the trackers: the
// owner's DI recordings (tests/recordings; see its README.md) against note lists (<clip>.notes). Decode them first
// with tools/decode_recordings.sh (ffmpeg).
//
// A .notes line is "start end notes" (seconds): a note listed is played and must sound (after a grace for it to be
// found, scored as delay), ?X may sound or not (a doubling, a harmonic taking over as a note decays, a quiet tail), A|B
// is one of them (bends, a pitch between two semitones), and * is anything. Any other note sounding is a stray note.

#pragma once

#include "engine/dsp/NoteTracker.hpp"
#include "engine/dsp/PolyNoteFinder.hpp"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace xythos::realtracking {


using Notes = std::bitset<128>; // by MIDI note

// The lowest note in a set (std::bitset has no portable find-first)
inline int lowestNote(const Notes& notes)
{
    for (size_t n = 0; n < notes.size(); ++n)
        if (notes[n])
            return static_cast<int>(n);
    return 0;
}

constexpr double kRate = 48000.0;

struct Segment {
    double start = 0.0, end = 0.0;
    Notes played, maybe;
    std::vector<Notes> groups; // one of each must sound
    bool any = false;
};

// The sets of recordings: the first eight clips (2026-10-24, MP3, a 6-string), then the 7-string guitar in drop A and
// the 5-string bass (2026-11-09, lossless; the files asked for in docs/RecordingList.md and the owner's extra ones)
enum class ClipSet { First, Guitar, Bass };
inline const char* clipSetName(ClipSet set)
{
    return set == ClipSet::First ? "first clips" : set == ClipSet::Guitar ? "guitar" : "bass";
}

struct Clip {
    std::string name;
    std::vector<float> signal;
    std::vector<Segment> segments;
    ClipSet set = ClipSet::First;
    bool chords = false; // two or more notes played together somewhere in it
};

inline int parseNote(const std::string& s)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int pc = 11; pc >= 0; --pc)
    {
        const size_t len = std::strlen(names[pc]);
        if (s.compare(0, len, names[pc]) == 0 && s.size() > len && (s[len] == '-' || std::isdigit(s[len])))
            return pc + 12 * (std::stoi(s.substr(len)) + 1);
    }
    return -1;
}

inline std::string noteName(int midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string(names[midi % 12]) + std::to_string(midi / 12 - 1);
}

// A float WAV as tools/decode_recordings.sh writes it (mono, 32-bit float, 48 kHz)
inline bool loadWav(const std::string& path, std::vector<float>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t i = 12;
    while (i + 8 <= data.size())
    {
        uint32_t size = 0;
        std::memcpy(&size, &data[i + 4], 4);
        if (std::memcmp(&data[i], "data", 4) == 0)
        {
            out.resize(std::min<size_t>(size, data.size() - i - 8) / 4);
            std::memcpy(out.data(), &data[i + 8], out.size() * 4);
            return true;
        }
        i += 8 + size + (size & 1);
    }
    return false;
}

inline std::vector<Segment> loadNotes(const std::string& path)
{
    std::vector<Segment> segments;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
    {
        // comments start with '#' after a space (a sharp follows a letter)
        if (!line.empty() && line[0] == '#')
            continue;
        line = line.substr(0, line.find(" #"));
        std::istringstream in(line);
        Segment seg;
        if (!(in >> seg.start >> seg.end))
            continue;
        std::string token;
        while (in >> token)
        {
            if (token == "*")
                seg.any = true;
            else if (token[0] == '?')
                seg.maybe.set(static_cast<size_t>(parseNote(token.substr(1))));
            else if (token.find('|') != std::string::npos)
            {
                Notes group;
                std::istringstream parts(token);
                std::string part;
                while (std::getline(parts, part, '|'))
                    group.set(static_cast<size_t>(parseNote(part)));
                seg.groups.push_back(group);
            }
            else
                seg.played.set(static_cast<size_t>(parseNote(token)));
        }
        segments.push_back(seg);
    }
    return segments;
}

inline const std::vector<Clip>& clips()
{
    static const std::vector<Clip> all = [] {
        std::vector<Clip> list;
        const char* dir = std::getenv("XYTHOS_RECORDINGS");
        const std::string wavs = dir ? dir : XYTHOS_RECORDINGS_WAV;
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(XYTHOS_RECORDINGS, ec))
            if (entry.path().extension() == ".notes")
                names.push_back(entry.path().stem().string());
        std::sort(names.begin(), names.end());
        const char* only = std::getenv("XYTHOS_TRACKING_ONLY"); // part of a clip's name: just those
        for (const std::string& name : names)
        {
            if (only && name.find(only) == std::string::npos)
                continue;
            Clip clip { name, {}, loadNotes(std::string(XYTHOS_RECORDINGS) + "/" + name + ".notes") };
            if (!loadWav(wavs + "/" + name + ".wav", clip.signal))
                continue;
            clip.set = name.rfind("gtr-", 0) == 0 ? ClipSet::Guitar : name.rfind("bass-", 0) == 0 ? ClipSet::Bass
                                                                                                  : ClipSet::First;
            for (const Segment& seg : clip.segments)
                clip.chords = clip.chords || seg.played.count() + seg.groups.size() > 1;
            list.push_back(std::move(clip));
        }
        // the first clips first, then the guitar's, then the bass's
        std::stable_sort(list.begin(), list.end(), [](const Clip& a, const Clip& b) { return a.set < b.set; });
        return list;
    }();
    return all;
}

struct RealScore {
    int notes = 0, missed = 0;
    double medianDelayMs = 0.0;
    double lateDelayMs = 0.0; // the 90th percentile
    std::vector<double> delays;
    double wantedMs = 0.0;  // the played notes' time (after their grace)
    double missingMs = 0.0; // a played note not sounding (after its grace)
    double missingClassMs = 0.0; // ... nor in another octave
    int stray = 0;          // stretches of a note nobody played
    double strayMs = 0.0;
    double strayNeighbourMs = 0.0, strayHarmonicMs = 0.0, straySubMs = 0.0; // a semitone or two off, a harmonic, below
};

constexpr double kGraceMs = 60.0; // a new note's band ringing up: scored as delay, not as missing
constexpr double kTailMs = 40.0;  // a note may ring this long past its segment

// Scores the notes sounding at each sample against the clip's note list. `mono`: one note at a time, so a chord
// only needs one of its notes (any played or maybe note counts).
inline RealScore score(const Clip& clip, const std::vector<Notes>& out, size_t latency, bool mono, bool debug)
{
    RealScore s;
    const size_t length = clip.signal.size();
    auto played = [&](size_t t) { return t + latency < out.size() ? out[t + latency] : Notes {}; };
    auto at = [](double sec) { return static_cast<size_t>(sec * kRate); };
    const size_t grace = at(kGraceMs / 1000.0), tail = at(kTailMs / 1000.0);

    // what may sound at each sample: the segment's played and maybe notes, and the last segment's for a tail
    std::vector<Notes> allowed(length);
    std::vector<bool> anything(length, false);
    for (size_t k = 0; k < clip.segments.size(); ++k)
    {
        const Segment& seg = clip.segments[k];
        Notes ok = seg.played | seg.maybe;
        for (const Notes& g : seg.groups)
            ok |= g;
        for (size_t t = at(seg.start); t < std::min(length, at(seg.end) + tail); ++t)
        {
            allowed[t] |= ok;
            if (seg.any && t < at(seg.end))
                anything[t] = true;
        }
    }

    // played notes (and groups): found, how late, missing while held
    std::vector<double> delays;
    for (size_t k = 0; k < clip.segments.size(); ++k)
    {
        const Segment& seg = clip.segments[k];
        const Segment* before = k > 0 ? &clip.segments[k - 1] : nullptr;
        std::vector<Notes> wants;
        if (mono)
        {
            Notes all = seg.played;
            for (const Notes& g : seg.groups)
                all |= g;
            if (all.any())
                wants.push_back(all | seg.maybe);
        }
        else
        {
            for (int n = 0; n < 128; ++n)
                if (seg.played[static_cast<size_t>(n)])
                    wants.push_back(Notes().set(static_cast<size_t>(n)));
            for (const Notes& g : seg.groups)
                wants.push_back(g);
        }
        for (const Notes& want : wants)
        {
            // a note carried on from the segment before has no grace (and no new start to time)
            bool carried = false;
            if (before && seg.start - before->end < 0.001)
            {
                Notes prev = before->played | before->maybe;
                for (const Notes& g : before->groups)
                    prev |= g;
                carried = (prev & want).any();
            }
            const size_t from = at(seg.start), to = std::min(length, at(seg.end));
            // when it's found: in this segment, or in those that carry it on (a note's first segment can be short: a
            // pitch that starts sharp is A1, then A1|A#1)
            bool found = false;
            size_t foundAt = to;
            if (!carried)
            {
                Notes accepted = want;
                for (size_t j = k; j < clip.segments.size() && !found; ++j)
                {
                    const Segment& part = clip.segments[j];
                    if (j > k)
                    {
                        if (part.start - clip.segments[j - 1].end >= 0.001)
                            break;
                        Notes next;
                        if (mono)
                        {
                            Notes all = part.played | part.maybe;
                            for (const Notes& g : part.groups)
                                all |= g;
                            if ((all & accepted).any())
                                next = all;
                        }
                        else
                        {
                            next = (part.played | part.maybe) & accepted;
                            for (const Notes& g : part.groups)
                                if ((g & accepted).any())
                                    next |= g;
                        }
                        if (next.none())
                            break;
                        accepted = next;
                    }
                    for (size_t t = at(part.start); t < std::min(length, at(part.end)); ++t)
                        if ((played(t) & accepted).any())
                        {
                            found = true;
                            foundAt = t;
                            break;
                        }
                }
            }
            if (!carried)
            {
                ++s.notes;
                if (!found)
                {
                    ++s.missed;
                    if (debug)
                        std::printf("    missed %s at %.3f s\n", noteName(lowestNote(want)).c_str(),
                                    seg.start);
                }
                else
                    delays.push_back(1000.0 * static_cast<double>(foundAt - from) / kRate);
            }
            double gapMs = 0.0;
            Notes classes; // the wanted notes in every octave
            for (int n = 0; n < 128; ++n)
                if (want[static_cast<size_t>(n)])
                    for (int o = n % 12; o < 128; o += 12)
                        classes.set(static_cast<size_t>(o));
            for (size_t t = carried ? from : from + grace; t < to; ++t)
            {
                s.wantedMs += 1000.0 / kRate;
                if (!(played(t) & want).any())
                    gapMs += 1000.0 / kRate;
                if (!(played(t) & classes).any())
                    s.missingClassMs += 1000.0 / kRate;
            }
            if (debug && gapMs >= 20.0)
                std::printf("    %s missing %.0f ms in %.2f-%.2f s\n",
                            noteName(lowestNote(want)).c_str(), gapMs, seg.start, seg.end);
            s.missingMs += gapMs;
        }
    }
    if (!delays.empty())
    {
        std::sort(delays.begin(), delays.end());
        s.medianDelayMs = delays[delays.size() / 2];
        s.lateDelayMs = delays[std::min(delays.size() - 1, delays.size() * 9 / 10)];
    }
    s.delays = delays;

    // stray notes: per note, stretches of 3 ms or more (gaps under 5 ms joined)
    const size_t minimum = at(0.003), join = at(0.005);
    for (size_t n = 0; n < 128; ++n)
    {
        size_t begin = 0, end = 0;
        bool open = false;
        auto close = [&]() {
            if (open && end - begin >= minimum)
            {
                ++s.stray;
                if (debug)
                    std::printf("    stray %s at %.3f s for %.1f ms\n", noteName(static_cast<int>(n)).c_str(),
                                begin / kRate, 1000.0 * static_cast<double>(end - begin) / kRate);
            }
        };
        for (size_t t = 0; t < length; ++t)
        {
            if (!played(t)[n] || allowed[t][n] || anything[t])
                continue;
            s.strayMs += 1000.0 / kRate;
            auto near = [&](std::initializer_list<int> ds) {
                for (int d : ds)
                    if (static_cast<int>(n) + d >= 0 && static_cast<int>(n) + d < 128 && allowed[t][n + d])
                        return true;
                return false;
            };
            if (near({ -1, 1, -2, 2 }))
                s.strayNeighbourMs += 1000.0 / kRate;
            else if (near({ -12, -19, -24, -28, -31, -34, -36 }))
                s.strayHarmonicMs += 1000.0 / kRate;
            else if (near({ 12, 19, 24 }))
                s.straySubMs += 1000.0 / kRate;
            if (!open || t > end + join)
            {
                close();
                begin = t;
                open = true;
            }
            end = t + 1;
        }
        close();
    }
    return s;
}

// A tracker under test: fills `out` with the notes sounding at each sample; returns its latency
using RealTracker = std::function<size_t(const std::vector<float>&, std::vector<Notes>&)>;

constexpr float kAudible = 0.01f; // a voice's level (band level x 5, as the Poly Chip plays it)
constexpr float kRealGate = 0.03f; // the recordings are DI, about 30 dB quieter than the synthetic guitar

// Hiss and mains hum (60 Hz and its 3rd harmonic), each at `level` (RMS of the hiss, peak of the hum), as a live
// input might have under the guitar
inline float liveNoise(size_t t, float level, uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    const float hiss = (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 1.732f;
    const double s = static_cast<double>(t) / kRate;
    const float hum = static_cast<float>(std::sin(2.0 * M_PI * 60.0 * s) + 0.5 * std::sin(2.0 * M_PI * 180.0 * s));
    return level * (hiss + hum);
}

inline RealTracker polyFinder(double lookaheadMs, float noise = 0.0f)
{
    return [lookaheadMs, noise](const std::vector<float>& x, std::vector<Notes>& out) {
        PolyNoteFinder finder;
        finder.prepare(kRate);
        finder.setLookahead(lookaheadMs);
        finder.setVoices(6);
        // (the Poly Chip's default Focus, 70%; XYTHOS_TRACKING_FOCUS=<percent> tries another)
        const char* focus = std::getenv("XYTHOS_TRACKING_FOCUS");
        finder.setFocus(std::exp2(6.0f * (focus ? static_cast<float>(std::atof(focus)) / 100.0f : 0.7f) - 3.0f));
        finder.setPickFilter(15.0f);
        out.assign(x.size(), Notes {});
        Notes now;
        uint32_t seed = 1;
        for (size_t t = 0; t < x.size(); ++t)
        {
            if (finder.process(x[t] + (noise > 0.0f ? liveNoise(t, noise, seed) : 0.0f)))
            {
                const PolyNoteFinder::Frame& f = finder.playFrame();
                now.reset();
                for (int b = 0; b < PolyNoteFinder::kBands; ++b)
                    if (f.sounding[b] && f.level[b] * 5.0f > kAudible * kRealGate)
                        now.set(static_cast<size_t>(b + PolyNoteFinder::kFirstNote));
            }
            out[t] = now;
        }
        return static_cast<size_t>(finder.latency());
    };
}

inline RealTracker monoTracker(float pickMs, double lookaheadMs)
{
    return [pickMs, lookaheadMs](const std::vector<float>& x, std::vector<Notes>& out) {
        NoteTracker tracker;
        tracker.prepare(kRate);
        tracker.setPickFilter(pickMs);
        tracker.setLookahead(lookaheadMs);
        out.assign(x.size(), Notes {});
        for (size_t t = 0; t < x.size(); ++t)
        {
            const NoteTracker::Note& n = tracker.process(x[t]);
            if (n.on && n.hz > 0.0)
            {
                const int midi = static_cast<int>(std::lround(69.0 + 12.0 * std::log2(n.hz / 440.0)));
                if (midi >= 0 && midi < 128)
                    out[t].set(static_cast<size_t>(midi));
            }
        }
        return static_cast<size_t>(tracker.latency());
    };
}

// The scores of a tracker over the clips: per set (ClipSet order), then over all of them (back())
struct BenchResult {
    std::vector<RealScore> clips; // one per clip
    RealScore sets[3];
    RealScore total;
    const RealScore& back() const { return total; }
};

inline void addScore(RealScore& total, const RealScore& s)
{
    total.notes += s.notes;
    total.missed += s.missed;
    total.missingMs += s.missingMs;
    total.wantedMs += s.wantedMs;
    total.missingClassMs += s.missingClassMs;
    total.stray += s.stray;
    total.strayMs += s.strayMs;
    total.strayNeighbourMs += s.strayNeighbourMs;
    total.strayHarmonicMs += s.strayHarmonicMs;
    total.straySubMs += s.straySubMs;
    total.delays.insert(total.delays.end(), s.delays.begin(), s.delays.end());
    std::sort(total.delays.begin(), total.delays.end());
    if (!total.delays.empty())
    {
        total.medianDelayMs = total.delays[total.delays.size() / 2];
        total.lateDelayMs = total.delays[std::min(total.delays.size() - 1, total.delays.size() * 9 / 10)];
    }
}

inline void printScore(const char* name, const RealScore& s)
{
    std::printf("  %-46s %5d %6d %6.1f %6.1f %8.0f %10.0f %10.0f %6d %8.0f  (%.0f %.0f %.0f %.0f)\n", name, s.notes,
                s.missed, s.medianDelayMs, s.lateDelayMs, s.wantedMs, s.missingMs, s.missingClassMs, s.stray, s.strayMs,
                s.strayNeighbourMs,
                s.strayHarmonicMs, s.straySubMs, s.strayMs - s.strayNeighbourMs - s.strayHarmonicMs - s.straySubMs);
}

// Runs a tracker over the clips and scores it. Printed: per set and in all, the notes, those never found, the median
// and 90th percentile delay, the notes' time, the missing note time (and missing in every octave), the stray notes and their time
// (a semitone or two off, harmonics, below a note, other). XYTHOS_TRACKING_CLIPS=1 prints each clip too,
// XYTHOS_TRACKING_DEBUG=1 each miss and stray note.
inline BenchResult realBench(const char* name, const RealTracker& tracker, bool mono, bool print = true)
{
    const bool debug = std::getenv("XYTHOS_TRACKING_DEBUG") != nullptr && print;
    const bool each = (std::getenv("XYTHOS_TRACKING_CLIPS") != nullptr || debug) && print;
    if (print)
    {
        std::printf("\n%s\n", name);
        std::printf("  %-46s %5s %6s %6s %6s %8s %10s %10s %6s %8s\n", "", "notes", "missed", "delay", "90%", "note ms",
                    "missing ms", "no octave", "stray", "stray ms");
    }
    BenchResult result;
    for (const Clip& clip : clips())
    {
        if (debug)
            std::printf("  %s\n", clip.name.c_str());
        std::vector<Notes> out;
        const size_t latency = tracker(clip.signal, out);
        const RealScore s = score(clip, out, latency, mono, debug);
        if (each)
            printScore(clip.name.c_str(), s);
        result.clips.push_back(s);
        addScore(result.sets[static_cast<int>(clip.set)], s);
        addScore(result.total, s);
    }
    if (print)
    {
        for (int set = 0; set < 3; ++set)
            if (result.sets[set].notes > 0 || result.sets[set].stray > 0)
                printScore(clipSetName(static_cast<ClipSet>(set)), result.sets[set]);
        printScore("all", result.total);
    }
    return result;
}

} // namespace xythos::realtracking
