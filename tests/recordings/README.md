# The owner's recordings

Dry DI recordings of the owner's playing, for the note tracking benchmark (`tests/RealTrackingTests.cpp`,
`tests/RealTracking.hpp`): each clip has a note list, `<clip>.notes`, that says what is played when, and the
trackers are scored against it.

```sh
tools/decode_recordings.sh                      # ffmpeg: build/recordings/<clip>.wav (48 kHz, float)
XYTHOS_TRACKING_BENCH=1 ./build/xythos-tests -tc="*real recordings*"
```

`XYTHOS_TRACKING_CLIPS=1` prints each clip, `XYTHOS_TRACKING_DEBUG=1` each missed and stray note,
`XYTHOS_TRACKING_ONLY=<part of a name>` takes only those clips, `XYTHOS_TRACKING_FOCUS=<percent>` tries another
Focus in the poly finder.

## The clips

**The first eight** (2026-10-24, MP3, a 6-string in standard tuning): `open-g-chord`, `open-d-chord`,
`power-chords`, `octave-shapes`, `ringing-arpeggio`, `slow-riff`, `chugging-riff`, `string-bends`. Their note lists
were made by hand.

**The guitar** (2026-11-09, `gtr-*`; a 7-string in drop A: A1 E2 A2 D3 G3 B3 E4) and **the bass** (`bass-*`; a
5-string), the files asked for in `docs/RecordingList.md` and the owner's extra ones (`*-custom-*`). The owner sent
24-bit FLAC; they are kept as 16-bit FLAC with triangular dither (19 MB, not 66 MB: the DI's own noise is at -95
dBFS, the dither's just under it).

The bass is in **standard B tuning (B0 E1 A1 D2 G2)** in every file but the two string walks and the open A's
sustain, which are in drop A (A0 = 27.5 Hz): the owner tuned to the wrong note for the files asked for and recorded
those three to make up for it. `bass-custom-fast-picked` is in standard B too (its notes are B0 and E1).

| Clip | What's played |
|---|---|
| `gtr-noise`, `bass-noise` | Nothing: plugged in, hands on the strings. Any note is a stray one. |
| `gtr-frets`, `bass-frets` | Every string, open to the 12th fret, a note at a time, each muted after a second. |
| `gtr-frets-bridge` | The same on the three lowest strings, bridge pickup. |
| `bass-frets-pick` | The same on the two lowest strings, with a pick. |
| `gtr-custom-high-frets` | Every string, 12th to 24th fret. |
| `gtr-sustain`, `bass-sustain`, `bass-custom-sustain-a-open` | Notes left to ring until they're gone. |
| `gtr-fast`, `bass-fast`, `bass-custom-fast-picked` | One note picked over and over, fast, three notes each (the guitar's second one palm-muted). |
| `gtr-legato`, `bass-legato` | Hammer-ons, pull-offs, slides; on the guitar bends and vibrato too. |
| `bass-custom-drop-a-string-walk-finger`, `-pick` | Up the low string in drop A, A0 to A1. |
| `gtr-chords` | E, A, D, G, C, Em, Am, Dm, F and Bm, each strummed once and left ringing. |
| `gtr-low-chords` | Power chords on the three lowest strings (open, 3rd, 5th, 7th fret), ringing, then chugged palm-muted. |
| `gtr-custom-high-string_chords` | Triads on the three highest strings. |
| `gtr-custom-slow-ringing-arpeggios-e-g-d-c` | The open chords E, G, D and C, a string at a time, all left ringing. |
| `gtr-custom-e-minor-pentatonic-noodling` | Improvised: bends, slides, double stops. |
| `gtr-custom-tetris-theme` | A melody. |
| `bass-custom-311-what-was-i-thinking` | A riff with many hammer-ons. |
| `bass-custom-limp-bizkit-rearranged` | A riff. |
| `bass-custom-metallica-anesthesia` | From a bass solo: notes left ringing over an open string, double stops. |

## Note lists

A line is `start end notes` (seconds). A note listed is played and must sound; `?X` may sound or not (a quiet tail,
a string ringing along, a harmonic that takes over as a note decays); `A|B` is one of them (a bend, a pitch between
two semitones, a decayed note's octave); `*` is anything (a few ms around a pick). Any other note sounding is a
stray note. Lines starting with `#` are comments.

The first eight are hand-made. The others are made by `tools/annotate/annotate.sh` (Python with numpy, scipy and
soundfile), so they can be made again when the method improves:

- **Single-note clips**: an offline pitch analysis (`tools/annotate/mono.py`: a long window around every 5 ms, so
  it knows what comes after, which the plugin's trackers can't) is cut into notes (`gen_notes.py`). A note is
  required from its pick until its level has fallen 34 dB or a mute begins (9 dB within 60 ms), then may sound
  until the analysis loses it and 30 ms more (60 ms on the bass: the long windows end notes early). Where the
  analysis finds a pitch with none of its low harmonics in the spectrum, it's two notes' common period (a double
  stop), and both notes are listed. A note left ringing under the next ones may sound for as long as its
  fundamental stays within 20 dB of where it was. `<clip>.ov` holds overrides: `set T0 T1 notes` for stretches the
  analysis can't follow (the fast picking: one note throughout), `repicks` to start a new note at each pick of a
  repeated note.
- **Chord clips** have a `<clip>.spec`, written by hand: each strum or picked string with its notes, from when to
  when (`poly_notes.py` makes the list: required until 0.12 s before the mute, 2.5 s at most). The voicings were
  checked against long spectra (16384 samples); each pick of the arpeggios was found from the partials that rise
  at it.

Every clip's notes were checked against what the clip is meant to hold: both fret clips of the guitar give
exactly their 91 notes in order, the bass's its 65, and so on.

What the lists can't settle: which octave doublings ring in a strummed chord (all of the voicing's are listed as
played), and exactly when a muted note stops (hence the tails). The riffs' lists come from the analysis alone and
were only spot-checked.

## Scores

Per clip, per set and in all (`realBench()`): the notes, those never found, the median and 90th percentile delay
until a note is found, the notes' time, the time a note is missing (after 60 ms of grace), the time it is missing
in every octave, and the stray notes and their time, split into a semitone or two off a played note, harmonics of
one, below one, and other. The single-note tracker is scored one note at a time: a chord counts as found when any
of its notes plays.
