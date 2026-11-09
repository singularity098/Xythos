# Recordings wanted: note tracking for 7-string guitar and 5-string bass, both in drop A

What I need from you to (1) make the mono synths track better, (2) get polyphonic tracking working well
enough to turn back on, and (3) track a 7-string guitar in drop A (down to A1, 55 Hz) and a 5-string bass in
drop A (down to A0, 27.5 Hz). Each file
below becomes a benchmark clip next to the eight in `tests/recordings/`. I write its note list from this sheet
and the click grid, then check it against the audio, so you only have to play what's written.

Priority: **[1]** essential, **[2]** important, **[3]** nice to have. If you only have one evening, record every
[1]. There are 46 files and about 80 minutes of playing; the [1] files are about 40 minutes.

---

## 0. How to record (same for every file)

**Signal**
- Record the **dry DI**: exactly what reaches Xythos's input. Guitar or bass straight into the interface's
  instrument (Hi-Z) input. No amp sim, pedals, noise gate, compressor or EQ, in front or in the DAW.
- Set the input gain the way you set it to play live. Peaks around -12 to -6 dBFS, never clipping. Use the same
  gain for every file on the same instrument and don't change it between takes.
- Guitar and bass volume knobs fully up, tone knobs fully open, unless a file says otherwise.

**Format**
- **WAV (or FLAC), 48 kHz, 24-bit, mono.** Please not MP3: MP3 adds a delay at the start, which shifts every
  note's timing, and it smears the low end, which matters most for the bass's low A.
- Use the **exact filenames** given below (`gtr-...` guitar, `bass-...` bass).

**Tuning**
- Guitar: 7-string **drop A**: A1 E2 A2 D3 G3 B3 E4 (the low B string tuned down a whole step to A; the other
  six strings are standard), **A = 440 Hz**. Tune with a tuner before each file, and re-tune the low A often: a
  slack string drifts. (The old recordings were about 20 cents sharp, which I can work with, but in tune is
  better.)
- Bass: 5-string **drop A**: A0 E1 A1 D2 G2 (the low B string tuned down a whole step to A), A = 440 Hz.
  Re-tune the low A before every bass file: a slack string drifts.

**Timing (this matters most: it's how I know when every note should start)**
- In the DAW, set 4/4 and the tempo the file gives. Turn on the click **in your headphones only. Don't record
  it.**
- Start recording at **bar 1, beat 1**. Bars 1 and 2 are the count-in: **don't play** during them. The first
  note is on **bar 3, beat 1** unless the file says otherwise.
- Export or bounce the dry track **from bar 1, beat 1 (time 0) to a couple of seconds after the last note**.
  Don't trim the start. The count-in silence is also my noise-floor reference.
- So bar *n*, beat *b* is at ((n − 1) × 4 + (b − 1)) × 60 / BPM seconds. You don't have to work this out; it's
  why the start has to stay untrimmed.

**Mistakes**
- Small timing slop is fine and realistic. Wrong notes are fine too if you write them down: in
  `recording-log.txt` (template at the end), give the file, the bar and what you played instead. If a take goes
  badly wrong, just redo it.
- One continuous take per file. Don't punch in or comp.

**Notation used below**
- Notes in scientific pitch: guitar low A = A1, its E string = E2, its high E = E4; bass low A = A0.
- `s:f` = string:fret. Guitar strings are 7 (low A) to 1 (high E). Bass strings are 5 (low A) to 1 (G).
  Example: guitar `2:8` = B string, 8th fret = G4; `7:3` = low A string, 3rd fret = C2.
- Chord shapes are written low string to high, guitar style. A six-character shape is strings 6 to 1, with
  the low A string (7) **not played** (mute it): `x32010` = C major. A seven-character shape includes
  string 7: `0x02220` = A major with the low A1 under it.
- "Mute" means stop the note dead with either hand.

**Standard pattern "2 per bar"** (used by the chromatic files): pick a note on beat 1 and let it ring through
beat 1, mute it on beat 2; pick the next note on beat 3, mute it on beat 4. That gives two notes per bar, each
ringing one beat, with one beat of silence after it.

---

## 1. Noise floor

These tell the trackers what "nothing playing" sounds like on your rig: hum, hiss and handling noise. They're
what keeps notes from being cut short or invented.

| File | Pri | What to do |
|---|---|---|
| `gtr-noise.wav` | [1] | 30 s, no click needed. 0-10 s: guitar plugged in, volume up, hands off the strings. 10-20 s: hands resting on the strings, muting them, still. 20-30 s: move your hands around on muted strings (fret-hand shifts, lifting and placing), but don't pick. Sit where you normally play (near the computer screen, etc.): the hum depends on it. |
| `bass-noise.wav` | [1] | The same 30 s, with the bass. |

---

## 2. Guitar, single notes (the mono synths)

### `gtr-mono-chromatic-neck.wav` [1]
- **80 BPM, neck pickup.** Pattern "2 per bar".
- Every string, open to the 12th fret, low A first: 7:0, 7:1, ... 7:12, then 6:0 ... 6:12, then 5, 4, 3, 2, 1.
- That's 91 notes in 46 bars (bars 3-48), A1 up to E5.
- Tests: every note of the guitar's main range, how quickly each one is found, and the note-off when muted.

### `gtr-mono-chromatic-bridge.wav` [1]
- Exactly the same as above, on the **bridge pickup**. (The bridge pickup has a weaker fundamental, which is
  what makes octave errors.)

### `gtr-mono-high.wav` [2]
- **80 BPM, bridge pickup.** Pattern "2 per bar".
- String 2, frets 12-20 (B4 to G5), then string 1, frets 12-22 (E5 to D6; as high as your neck goes).
- 20 notes, bars 3-12.

### `gtr-mono-scale.wav` [1]
- **90 BPM, neck pickup, alternate picking, legato** (each note rings right up to the next, no gaps).
- A major, two octaves, 5th position:
  `6:5 6:7 5:4 5:5 5:7 4:4 4:6 4:7 3:4 3:6 3:7 2:5 2:7 1:4 1:5`
  = A2 B2 C#3 D3 E3 F#3 G#3 A3 B3 C#4 D4 E4 F#4 G#4 A4.
- Bars 3-10: quarter notes, up the scale and back down (29 notes, then hold the last A2 to the end of bar 10).
- Bars 11-14: the same in eighth notes, up and down (29 notes, then hold A2 to the end of bar 14).
- Tests: fast note-to-note changes with no silence between notes, which is the hardest case for the note ends.

### `gtr-mono-fast.wav` [1]
- **110 BPM, bridge pickup, alternate picking.**
- Bars 3-4: sixteenth notes on one note, A3 (`3:2`). That's 32 picks of the same note; each should retrigger.
- Bars 5-6: sixteenth notes on open low E (E2).
- Bars 7-8: sixteenth-note run, one bar up and down then repeat:
  `4:7 3:4 3:6 3:7 2:5 2:7 1:4 1:5 1:4 2:7 2:5 3:7 3:6 3:4 4:7 (rest)`
  = A3 B3 C#4 D4 E4 F#4 G#4 A4 G#4 F#4 E4 D4 C#4 B3 A3, then a 16th rest.
- Bars 9-10: sixteenth notes alternating A3 (`3:2`) and E4 (open 1), string skipping.
- Bars 11-12: staccato eighth notes on A3, each cut short (mute right after the pick).
- Tests: retriggering the same note, speed, and short notes.

### `gtr-mono-slurs.wav` [1]
- **80 BPM, neck pickup.** Pick only the first note of each slur. Mute at beat 4 of each bar unless the bar is
  full.
- Bar 3: `3:5` hammer-on to `3:7` on beat 2 (C4 → D4).
- Bar 4: `3:7` pull-off to `3:5` on beat 2 (D4 → C4).
- Bar 5: `3:5` h `3:7` p `3:5` on beats 1, 2 and 3 (C4 D4 C4).
- Bar 6: trill `3:5`-`3:7` in sixteenths, the whole bar (C4/D4).
- Bar 7: slide `3:5` → `3:7`, arriving on beat 2 (C4 → D4).
- Bar 8: slide `3:7` → `3:5`, arriving on beat 2.
- Bar 9: long slide `5:3` → `5:12` (C3 → A3), pick on beat 1, arrive on beat 3.
- Bar 10: slide into D4 (`3:7`) from about fret 2, unpicked, arriving on beat 1.
- Bar 11: pick D4 (`3:7`) on beat 1, then on beat 3 slide down off the neck (a fall-off).
- Bar 12: one pick, eighth notes: `2:5 h 2:7 h 2:8 p 2:7 p 2:5` (E4 F#4 G4 F#4 E4), then let the E4 ring.
- Bar 13: low string: `6:3` h `6:5` on beat 2 (G2 → A2).
- Bar 14: `6:5` p `6:3` on beat 2.
- Bar 15: `6:3` slide to `6:5` on beat 2.
- Tests: pitch changes without a new pick (the synths' glide and legato), and what a slide sounds like between
  its two notes.

### `gtr-mono-bends.wav` [1]
- **70 BPM, bridge pickup.**
- Bar 3: `2:10` (A4), bend a whole step to B4 by beat 2, hold, release by beat 3, mute on beat 4.
- Bar 4: `3:7` (D4), bend a half step to D#4 by beat 2, hold, release by beat 3, mute on beat 4.
- Bar 5: `2:10`, a slow bend from A4 to B4 across all four beats.
- Bar 6: pre-bend `2:10` to B4 *before* picking, pick on beat 1, release slowly across the bar to A4.
- Bar 7: `1:12` (E5), quick half-step bends up and back, about four per beat (a bend trill).
- Bars 8-9: `2:8` (G4) held for two bars with wide vibrato.
- Bars 10-11: `3:9` (E4) held for two bars with narrow finger vibrato.
- Bar 12: `6:3` (G2), a quarter-tone blues curl and back, then let it ring.
- Bars 13-14: if your guitar has a tremolo arm: A3 (`3:2`), dip the bar on beat 1 and beat 3. Otherwise leave
  these bars silent.
- Tests: following a bend smoothly, and not jumping to a new note during vibrato.

### `gtr-mono-dynamics.wav` [2]
- **80 BPM, neck pickup.** Notes on beats 1 and 3, each ringing two beats.
- Bars 3-6: 8 picks of G3 (`4:5`), from barely touching the string to as hard as you can, evenly spaced in
  loudness.
- Bars 7-10: the same on E4 (`2:5`).
- Bars 11-14: the same on open low E (E2).
- Bars 15-16: 16 eighth notes on G3, a crescendo from very soft to very hard.
- Bars 17-18: 16 eighth notes on G3, a decrescendo back to very soft.
- Bars 19-22: 8 picks of open low A (A1), barely touching to as hard as you can, on beats 1 and 3.
- Tests: velocity tracking, and that soft notes are still found.

### `gtr-mono-sustain.wav` [1]
- **60 BPM (4 s per bar), neck pickup.** Pick one note and let it ring with no vibrato, for 16 s (4 bars).
  Mute it on beat 4 of the 4th bar, then pick the next note on the following downbeat.
- Bar 3: E2 (open 6). Bar 7: G3 (`4:5`). Bar 11: D4 (`3:7`). Bar 15: E5 (`1:12`). Bar 19: a G chord
  (`320003`), strummed once. Bar 23: A1 (open 7). Bar 27: C2 (`7:3`). Bar 31: an A5 power chord on the three
  low strings, all open (`000xxxx`: A1 E2 A2), strummed once.
- Tests: holding the note as long as it's audible, and ending it cleanly as it fades into the noise, without
  octave jumps in the tail.

### `gtr-mono-muted.wav` [1]
- **100 BPM, bridge pickup.**
- Bars 3-4: palm-muted eighth notes on open low E (E2).
- Bars 5-6: palm-muted eighths, the riff `6:0 6:0 6:3 6:0 6:5 6:0 6:6 6:5` (E2 E2 G2 E2 A2 E2 A#2 A2), twice.
- Bars 7-8: dead notes only. Fretting hand lightly on strings 4-7, sixteenth-note "chicka" strums. **No pitched
  notes:** these should trigger nothing.
- Bars 9-10: A3 (`3:2`) on beats 1 and 3, ringing for a quarter note, with dead-note sixteenths filling the rest.
- Bars 11-12: a muted rake across strings 6 to 2, landing on E4 (`2:5`) on beat 1 of each bar, letting the E4
  ring.
- Tests: palm mutes found with the right pitch, and no notes from dead strums.

### `gtr-mono-harmonics.wav` [2]
- **70 BPM, neck pickup.** Pattern "2 per bar", natural harmonics.
- 12th fret, strings 7 to 1 (A2 E3 A3 D4 G4 B4 E5); then 7th fret, strings 7 to 1 (E3 B3 E4 A4 D5 F#5 B5);
  then 5th fret, strings 7 to 1 (A3 E4 A4 D5 G5 B5 E6). That's 21 harmonics, bars 3-13.
- Tests: pure tones with no fundamental below them.

### `gtr-mono-lick.wav` [1]
- **85 BPM, bridge pickup.** A blues lick in A minor pentatonic, 5th position, played twice (bars 3-6, then
  bars 7-10). Phrase it the way you normally would.
- Bar 3: beat 1 `2:8` bent to A4 (quarter) · beat 2 `1:5` A4, `2:8` G4 (eighths) · beat 3 `2:5` E4 (quarter) ·
  beat 4 `3:7` D4, `3:5` C4 (eighths).
- Bar 4: beat 1 `3:7` bent a whole step to E4 (quarter) · beat 2 released to D4, no new pick (quarter) ·
  beats 3-4 `4:7` A3, half note with vibrato.
- Bar 5: eighth-note triplets: `1:8 1:5 2:8` (C5 A4 G4) · `2:5 3:7 3:5` (E4 D4 C4) · `4:7 4:5 5:7` (A3 G3 E3) ·
  beat 4 `5:5` D3 (quarter).
- Bar 6: beat 1 slide `5:5` → `5:7` (D3 → E3) · beat 2 `6:8` C3 · beats 3-4 `6:5` A2, half note with vibrato.
- Tests: a realistic phrase that mixes everything.

### `gtr-mono-low.wav` [1]
- **100 BPM, bridge pickup.** The low A string, the way a drop-A riff uses it.
- Bars 3-4: eighth notes on open A1 (string 7).
- Bars 5-6: sixteenth notes on A1.
- Bars 7-8: palm-muted chugs on A1 in sixteenths with rests, each bar: beat 1 four sixteenths · beat 2 rest ·
  beat 3 two sixteenths then rest · beat 4 four sixteenths.
- Bars 9-10: palm-muted eighths, the riff `7:0 7:0 7:3 7:0 7:5 7:0 7:6 7:5` (A1 A1 C2 A1 D2 A1 D#2 D2), twice.
- Bars 11-18: A natural minor from A1, two octaves, quarter notes, legato, up and back down:
  `7:0 7:2 7:3 7:5 6:0 6:1 6:3 5:0 5:2 5:3 4:0 4:2 4:3 3:0 3:2`
  = A1 B1 C2 D2 E2 F2 G2 A2 B2 C3 D3 E3 F3 G3 A3 (29 notes, then hold A1 to the end of bar 18).
- Bar 19: `7:3` hammer-on to `7:5` on beat 2 (C2 → D2), mute on beat 4.
- Bar 20: `7:5` pull-off to `7:3` on beat 2, mute on beat 4.
- Bar 21: long slide `7:2` → `7:12` (B1 → A2), pick on beat 1, arrive on beat 3.
- Bars 22-23: a gallop on A1, each beat an eighth note and two sixteenths (A1, A1-A1).
- Tests: the new bottom of the guitar's range, where the mono tracker now has to look an octave lower than a
  6-string needs.

### `gtr-mono-drone.wav` [2]
- **80 BPM, neck pickup.** Open D3 (string 4) picked on beat 1 of every bar and left ringing. A melody on beats
  2, 3 and 4 on strings 3 and 2 over it:
- Bar 3: `3:2 3:4 2:3` (A3 B3 D4) · Bar 4: `2:5 2:3 3:4` (E4 D4 B3) · Bar 5: `3:2 3:4 2:2` (A3 B3 C#4) ·
  Bar 6: `2:3` (D4) on beat 2, ringing to the end of the bar.
- Bars 7-10: the same again.
- Tests: the mono synths following the newest note while an older one still rings.

---

## 3. Guitar, chords (the polyphonic tracking)

Use the **middle pickup position (neck + bridge)** for these, unless the file says otherwise.

### `gtr-poly-open-chords.wav` [1]
- **70 BPM.** One downstrum on beat 1, let it ring through beat 3, mute on beat 4. One chord per bar, from bar 3:

| Bar | Chord | Shape | Notes |
|---|---|---|---|
| 3 | E | `022100` | E2 B2 E3 G#3 B3 E4 |
| 4 | A | `x02220` | A2 E3 A3 C#4 E4 |
| 5 | D | `xx0232` | D3 A3 D4 F#4 |
| 6 | G | `320003` | G2 B2 D3 G3 B3 G4 |
| 7 | C | `x32010` | C3 E3 G3 C4 E4 |
| 8 | Em | `022000` | E2 B2 E3 G3 B3 E4 |
| 9 | Am | `x02210` | A2 E3 A3 C4 E4 |
| 10 | Dm | `xx0231` | D3 A3 D4 F4 |
| 11 | E7 | `020100` | E2 B2 D3 G#3 B3 E4 |
| 12 | A7 | `x02020` | A2 E3 G3 C#4 E4 |
| 13 | D7 | `xx0212` | D3 A3 C4 F#4 |
| 14 | G7 | `320001` | G2 B2 D3 G3 B3 F4 |
| 15 | Cmaj7 | `x32000` | C3 E3 G3 B3 E4 |
| 16 | Fmaj7 | `xx3210` | F3 A3 C4 E4 |
| 17 | Asus2 | `x02200` | A2 E3 A3 B3 E4 |
| 18 | Dsus4 | `xx0233` | D3 A3 D4 G4 |
| 19 | Cadd9 | `x32030` | C3 E3 G3 D4 E4 |

- Then bars 20-36: the whole list again, **upstrums**.

### `gtr-poly-barre-chords.wav` [1]
- **70 BPM.** Same pattern: a downstrum on beat 1, ring, mute on beat 4.

| Bar | Chord | Shape | Notes |
|---|---|---|---|
| 3 | F | `133211` | F2 C3 F3 A3 C4 F4 |
| 4 | Fm | `133111` | F2 C3 F3 G#3 C4 F4 |
| 5 | F7 | `131211` | F2 C3 D#3 A3 C4 F4 |
| 6 | Bb | `x13331` | A#2 F3 A#3 D4 F4 |
| 7 | Bbm | `x13321` | A#2 F3 A#3 C#4 F4 |
| 8 | B7 | `x21202` | B2 D#3 A3 B3 F#4 |
| 9 | Bm | `x24432` | B2 F#3 B3 D4 F#4 |
| 10 | C#m | `x46654` | C#3 G#3 C#4 E4 G#4 |
| 11 | G | `355433` | G2 D3 G3 B3 D4 G4 |
| 12 | A | `577655` | A2 E3 A3 C#4 E4 A4 |
| 13 | Cm | `x35543` | C3 G3 C4 D#4 G4 |
| 14 | Dmaj7 | `x57675` | D3 A3 C#4 F#4 A4 |
| 15 | B | `799877` | B2 F#3 B3 D#4 F#4 B4 |

### `gtr-poly-power-chords.wav` [1]
- **90 BPM, bridge pickup.** Bars 3-8: one downstroke per half note (beats 1 and 3), letting it ring until the
  next. Then bars 9-16: the eight chords palm-muted in eighth notes, one bar each.
- Order: E5 `022xxx` (E2 B2 E3) · F5 `133xxx` (F2 C3 F3) · G5 `355xxx` (G2 D3 G3) · A5 `577xxx` (A2 E3 A3) ·
  C5 `x355xx` (C3 G3 C4) · D5 `x577xx` (D3 A3 D4). Then two-note E5 `02xxxx` (E2 B2) and G5 `35xxxx` (G2 D3).
  In the half-note bars that's two chords a bar: bars 3-6 the first eight, bars 7-8 the first four again.
- Tests: low, dense chords, where octave errors are worst.

### `gtr-poly-strumming.wav` [1]
- **90 BPM.** Normal strumming, your usual feel.
- Bars 3-10: G `320003` - D `xx0232` - Em `022000` - C `x32010`, one bar each, twice. Pattern
  D, D, U, U, D, U (beat 1, beat 2, 2&, 3&, beat 4, 4&).
- Bars 11-18: the same chords, changing every two beats (two chords per bar), straight eighth notes D U D U.
- Tests: chords in real rhythm, chord changes, and up versus down strums.

### `gtr-poly-fingerpicked.wav` [1]
- **80 BPM, neck pickup, fingers.** Let every note ring. Each pattern is eight eighth notes (one bar), played
  twice per chord:
- Am `x02210`: A2 E3 A3 C4 E4 C4 A3 E3 (bars 3-4)
- C `x32010`: C3 E3 G3 C4 E4 C4 G3 E3 (bars 5-6)
- G `320003`: G2 B2 D3 G3 B3 G4 B3 G3 (bars 7-8)
- D `xx0232`: D3 A3 D4 F#4 D4 A3 D4 F#4 (bars 9-10)
- Bars 11-18: the same again.
- Tests: notes added one at a time to a ringing chord. (Also the mono synths' "follow the newest note".)

### `gtr-poly-dyads.wav` [1]
- **70 BPM, neck pickup.** Two-note shapes, one per bar: picked together on beat 1, ring, mute on beat 4.
- Thirds on strings 3+2: `0+0` G3 B3 · `2+1` A3 C4 · `4+3` B3 D4 · `5+5` C4 E4 · `7+7` D4 F#4 · `9+8` E4 G4 ·
  `11+10` F#4 A4 · `12+12` G4 B4.
- Octaves: `6:3+4:5` G2 G3 · `5:5+3:7` D3 D4 · `4:2+2:5` E3 E4.
- Close intervals: `3:4+2:1` B3 C4 (a semitone) · `2:5+1:1` E4 F4 (a semitone) · `2:3+1:0` D4 E4 (a whole
  tone).
- 14 bars (bars 3-16).
- Tests: two notes close together, and octaves, which hide in each other's harmonics.

### `gtr-poly-voice-leading.wav` [2]
- **70 BPM, neck pickup.** Strum the first chord on beat 1, then change one note per beat by hammer-on or
  pull-off, **without strumming again**. Let everything ring.
- Bar 3: D `xx0232` → Dsus4 `xx0233` → D → Dsus2 `xx0230` (top note F#4 → G4 → F#4 → E4).
- Bar 4: A `x02220` → Asus4 `x02230` → A → Asus2 `x02200` (C#4 → D4 → C#4 → B3).
- Bar 5: C `x32010` → `x32000` → `x32010` → `x32000` (C4 → B3 → C4 → B3).
- Bars 6-8: the same three bars, but **pick** the changing note each time instead of hammering.
- Tests: one note changing while the others keep sounding.

### `gtr-poly-triads.wav` [2]
- **70 BPM, neck pickup.** Three-note triads on strings 3, 2 and 1, picked together on beat 1 and beat 3, each
  ringing two beats:
- G: `0-0-3` (G3 B3 G4) · `4-3-3` (B3 D4 G4) · `7-8-7` (D4 G4 B4) · `12-12-10` (G4 B4 D5)
- C: `5-5-3` (C4 E4 G4) · `9-8-8` (E4 G4 C5) · `12-13-12` (G4 C5 E5)
- D: `2-3-2` (A3 D4 F#4) · `7-7-5` (D4 F#4 A4) · `11-10-10` (F#4 A4 D5)
- Bars 3-7 (two per bar).
- Tests: high, thin voicings, where pick noise is loud next to the notes.

### `gtr-poly-mixed.wav` [1]
- **90 BPM, middle pickup.** A chord on beat 1 (ringing for a beat), then single notes on beats 2, 3 and 4:
- Bar 3: A `x02220` | `3:2` A3, `2:1` C4, `2:2` C#4
- Bar 4: D `xx0232` | `2:3` D4, `1:1` F4, `1:2` F#4
- Bar 5: E `022100` | `4:2` E3, `3:0` G3, `3:1` G#3
- Bar 6: A `x02220`, ringing the whole bar
- Bars 7-10: the same again.
- Tests: switching between chords and single notes, the way people actually play.

### `gtr-poly-drop-a.wav` [1]
- **90 BPM, bridge pickup.** Chords on the low A string.
- Bars 3-8: one downstroke per half note (beats 1 and 3), ringing until the next, two chords a bar:

| Chord | Shape (`s:f`) | Notes |
|---|---|---|
| A5 | `7:0 6:0 5:0` | A1 E2 A2 |
| C5 | `7:3 6:3 5:3` | C2 G2 C3 |
| D5 | `7:5 6:5 5:5` | D2 A2 D3 |
| E5 | `7:7 6:7 5:7` | E2 B2 E3 |
| F5 | `7:8 6:8 5:8` | F2 C3 F3 |
| G5 | `7:10 6:10 5:10` | G2 D3 G3 |
| A5, two notes | `7:0 6:0` | A1 E2 |
| A5, four notes | `7:0 6:0 5:0 4:2` | A1 E2 A2 E3 |
| A over low A | `0x02220` | A1 A2 E3 A3 C#4 E4 |
| Am over low A | `0x02210` | A1 A2 E3 A3 C4 E4 |
| D/A | `0xx0232` | A1 D3 A3 D4 F#4 |
| Asus2 over low A | `0x02200` | A1 A2 E3 A3 B3 E4 |

- Bars 9-16: palm-muted eighth notes, three-string power chords (shapes as above; D#5 = `7:6 6:6 5:6`):
  - Bar A: A5 A5 A5 C5 A5 A5 D5 D#5
  - Bar B: A5 A5 A5 A5 E5 E5 F5 E5
  - A B A B A B A B.
- Tests: chords whose lowest note is an octave below a 6-string's, which is where octave errors are worst.

### `gtr-poly-funk.wav` [2]
- **100 BPM, bridge pickup.**
- Bars 3-6: short E9 stabs (`x7677x`: E3 G#3 D4 F#4) on the "&" of every beat, choked quickly, with muted
  sixteenth-note scratches between them.
- Bars 7-8: muted scratches only, no pitched notes: these should trigger nothing.
- Tests: very short chords, and ignoring percussive strums.

---

## 4. Bass, 5-string in drop A (A0 E1 A1 D2 G2)

Use **both pickups, tone fully open** and play **fingerstyle**, unless the file says otherwise.

Fret reference for the low string (5): 0 = A0 (27.5 Hz), 2 = B0, 3 = C1, 5 = D1, 6 = D#1, 7 = E1, 8 = F1,
10 = G1, 12 = A1.

### `bass-chromatic-fingers.wav` [1]
- **60 BPM, fingers.** Pattern "2 per bar".
- Every string, open to the 12th fret, low A first: 5:0 ... 5:12, then 4:0 ... 4:12, then 3, 2, 1.
- 65 notes, bars 3-35, A0 up to G3.
- Tests: the whole range, above all the bottom octave A0-A1, which is new.

### `bass-chromatic-pick.wav` [1]
- Exactly the same, **with a pick**.

### `bass-chromatic-neck.wav` [2]
- The same with fingers, **neck pickup only**, but just strings 5 and 4 (26 notes, bars 3-15).

### `bass-chromatic-bridge.wav` [2]
- The same with fingers, **bridge pickup only**, strings 5 and 4. (The bridge pickup hardly picks up the
  fundamental of A0; this is the hardest case.)

### `bass-high.wav` [2]
- **70 BPM, fingers.** Pattern "2 per bar". String 2, frets 12-17 (D3 to G3), then string 1, frets 12-21
  (G3 to E4; as high as your neck goes). 16 notes, bars 3-10.

### `bass-sustain.wav` [1]
- **60 BPM (4 s per bar), fingers.** As for the guitar: one note left to ring for 16 s (4 bars), muted on beat 4
  of the 4th bar, the next one on the following downbeat.
- Bar 3: A0 (open 5). Bar 7: B0 (`5:2`). Bar 11: D1 (`5:5`). Bar 15: E1 (open 4). Bar 19: A1 (open 3).
  Bar 23: G1 (`4:3`).
- Tests: how long a low note keeps being found, and its end as it fades into the hum.

### `bass-eighths.wav` [1]
- **120 BPM, fingers, alternating fingers.** Steady rock eighth notes, two bars per note:
- Bars 3-4 A0 (`5:0`) · 5-6 C1 (`5:3`) · 7-8 D1 (`5:5`) · 9-10 F1 (`5:8`) · 11-12 G1 (`5:10`) ·
  13-14 E1 (open 4) · 15-16 A1 (open 3).
- Bars 17-18: sixteenth notes on A0. Bars 19-20: sixteenth notes on E1.
- Tests: retriggering low notes quickly. At A0 one cycle is 36 ms, and sixteenths at this tempo are 125 ms
  apart, so this is the tightest case.

### `bass-walking.wav` [1]
- **100 BPM, fingers.** Quarter notes, legato. Finger it however you like; the notes are what count:
- Bar 3 (Am7): A0 C1 E1 G1
- Bar 4 (Am7): A1 G1 E1 C1
- Bar 5 (Dm7): D1 F1 A1 C2
- Bar 6 (Dm7): D2 C2 A1 F1
- Bar 7 (E7): E1 G#1 B1 D2
- Bar 8 (E7): E2 D2 B1 G#1
- Bar 9 (Am7): A1 E1 C1 B0
- Bar 10: A0, a whole note, ringing.
- Bars 11-18: the same again.
- Tests: note to note with no gaps, over the whole low range.

### `bass-groove.wav` [1]
- **90 BPM, fingers.** A drop-A riff, counted "1 & 2 & 3 & 4 &":
- Bar A: 1 A0 · & A0 · 2 rest · & A0 · 3 C1 · & A0 · 4 D1 · & D#1
- Bar B: 1 E1 · & rest · 2 E1 · & D1 · 3 C1 · & A0 · 4 G1 · & A1
- Bars 3-10: A B A B A B A B.
- Bars 11-12: a gallop on A0, each beat an eighth note and two sixteenths (A0, A0-A0).
- Tests: a realistic low groove with rests.

### `bass-slurs.wav` [1]
- **80 BPM, fingers.** Pick only the first note of each slur. Mute on beat 4 unless the bar is full.
- Bar 3: `3:5` h `3:7` on beat 2 (D2 → E2).
- Bar 4: `3:7` p `3:5` on beat 2 (E2 → D2).
- Bar 5: `5:3` h `5:5` on beat 2 (C1 → D1, low hammer-on).
- Bar 6: `5:5` p `5:3` p `5:0` on beats 1, 2 and 3 (D1 C1 A0).
- Bar 7: slide `3:5` → `3:7` arriving on beat 2.
- Bar 8: long slide `4:2` → `4:12` (F#1 → E2), arriving on beat 3.
- Bar 9: slide up into A1 (`5:12`) from about fret 5, unpicked, arriving on beat 1.
- Bar 10: pick D2 (`3:5`), on beat 3 slide down off the neck.
- Bar 11: trill `3:5`-`3:7` in sixteenths, the whole bar.
- Bar 12: trill `5:3`-`5:5` in sixteenths, the whole bar (C1/D1).
- Tests: glides and legato down low.

### `bass-muted.wav` [1]
- **90 BPM.**
- Bars 3-4: palm-muted eighth notes (palm or thumb, Motown style): bar 3 E1 (open 4) ×8, bar 4 G1 G1 G1 G1
  A1 A1 A1 A1 (`4:3`, then `4:5`).
- Bars 5-6: dead notes only, fretting hand muting, sixteenths. **No pitched notes.**
- Bars 7-8: A0 on beat 1 and E1 on beat 3, each ringing a quarter note, dead-note sixteenths in between.
- Bars 9-12: palm-muted chugs on A0 in sixteenths with rests, each bar: beat 1 four sixteenths · beat 2 rest ·
  beat 3 two sixteenths then rest · beat 4 four sixteenths.
- Tests: muted low notes still found, and nothing from dead notes.

### `bass-slap.wav` [2]
- **90 BPM.** Thumb slaps and pops, in eighth notes:
- Bars 3-4: slap A1 (open 3) / pop A2 (`1:2`), alternating.
- Bars 5-6: slap E1 (open 4) / pop E2 (`2:2`).
- Bars 7-8: slap A0 (open 5) / pop A2 (`1:2`).
- Bars 9-10: sixteenths per beat: slap A1, dead slap, dead slap, pop A2.
- Tests: the slap's huge click before the note, and pops (very bright, a weak fundamental).

### `bass-octaves.wav` [2]
- **110 BPM, fingers.** Disco octaves: the root on the beat, the octave on the "&", two bars each:
- A0/A1 · D1/D2 · E1/E2 · G1/G2 · A1/A2 (bars 3-12).
- Tests: alternating octaves, which octave-error-prone trackers hate.

### `bass-dynamics.wav` [2]
- **70 BPM, fingers.** Notes on beats 1 and 3, ringing two beats.
- Bars 3-6: 8 notes of A0 (open 5), from barely touching to as hard as you can.
- Bars 7-10: the same on E1 (open 4).
- Bars 11-14: the same on D2 (`3:5`).
- Bars 15-16: 16 eighth notes on A1, a crescendo from very soft to very hard.

### `bass-harmonics.wav` [3]
- **70 BPM.** Pattern "2 per bar", natural harmonics.
- 12th fret, strings 5 to 1 (A1 E2 A2 D3 G3); 7th fret (E2 B2 E3 A3 D4); 5th fret (A2 E3 A3 D4 G4).
  15 harmonics, bars 3-10.

### `bass-double-stops.wav` [2]
- **70 BPM, fingers.** Two or three notes together on beat 1, ringing, mute on beat 4. One per bar:
- `5:0+4:0` A0 E1 · `5:0+4:5` A0 A1 · `4:0+3:2` E1 B1 · `3:0+2:2` A1 E2 · `5:3+4:3` C1 G1 ·
  `3:5+2:7` D2 A2 · `3:0+1:6` A1 C#3 (a tenth) · `4:0+1:1` E1 G#2 (a tenth) · `3:5+1:11` D2 F#3 (a tenth) ·
  `3:0+2:2+1:6` A1 E2 C#3 · `2:12+1:11` D3 F#3
- 11 bars (bars 3-13).
- Tests: polyphonic tracking on bass.

### `bass-fast-scale.wav` [2]
- **90 BPM, fingers.** A natural minor from A0, two octaves, in sixteenth notes, up and back down:
  A0 B0 C1 D1 E1 F1 G1 A1 B1 C2 D2 E2 F2 G2 A2, then down to A0, then hold A0 to the end of the bar.
- Each run takes two bars. Play it twice (bars 3-4, then bars 5-6).

---

## 5. Free playing [3]

Played however you normally play, 1-2 minutes each, with no click needed. I'll write their note lists by ear
and by analysis, so they take me longest. Add a line to the log about what you played.

| File | What |
|---|---|
| `gtr-free-lead.wav` | Single-note lead playing, your usual style. |
| `gtr-free-chords.wav` | Chord playing / rhythm, your usual style. |
| `bass-free.wav` | Bass lines, your usual style, using the low A. |

---

## 6. The log: `recording-log.txt`

One plain text file to send with the recordings. Something like this:

```
Guitar: <make/model> (7-string), pickups: <e.g. humbuckers / single coils>, strings: <gauge>
Bass:   <make/model>, pickups: <...>, strings: <gauge>
Interface: <model>, input gain: <setting>
DAW: <name>

gtr-mono-chromatic-neck.wav   ok
gtr-mono-fast.wav             bar 8: missed two notes of the run
gtr-poly-open-chords.wav      bar 14: played G7 as 320003 (no F)
bass-groove.wav               ok
...
```

---

## Checklist

| # | File | Pri | BPM |
|---|---|---|---|
| 1 | gtr-noise.wav | 1 | - |
| 2 | bass-noise.wav | 1 | - |
| 3 | gtr-mono-chromatic-neck.wav | 1 | 80 |
| 4 | gtr-mono-chromatic-bridge.wav | 1 | 80 |
| 5 | gtr-mono-high.wav | 2 | 80 |
| 6 | gtr-mono-scale.wav | 1 | 90 |
| 7 | gtr-mono-fast.wav | 1 | 110 |
| 8 | gtr-mono-slurs.wav | 1 | 80 |
| 9 | gtr-mono-bends.wav | 1 | 70 |
| 10 | gtr-mono-dynamics.wav | 2 | 80 |
| 11 | gtr-mono-sustain.wav | 1 | 60 |
| 12 | gtr-mono-muted.wav | 1 | 100 |
| 13 | gtr-mono-harmonics.wav | 2 | 70 |
| 14 | gtr-mono-lick.wav | 1 | 85 |
| 15 | gtr-mono-low.wav | 1 | 100 |
| 16 | gtr-mono-drone.wav | 2 | 80 |
| 17 | gtr-poly-open-chords.wav | 1 | 70 |
| 18 | gtr-poly-barre-chords.wav | 1 | 70 |
| 19 | gtr-poly-power-chords.wav | 1 | 90 |
| 20 | gtr-poly-strumming.wav | 1 | 90 |
| 21 | gtr-poly-fingerpicked.wav | 1 | 80 |
| 22 | gtr-poly-dyads.wav | 1 | 70 |
| 23 | gtr-poly-voice-leading.wav | 2 | 70 |
| 24 | gtr-poly-triads.wav | 2 | 70 |
| 25 | gtr-poly-mixed.wav | 1 | 90 |
| 26 | gtr-poly-drop-a.wav | 1 | 90 |
| 27 | gtr-poly-funk.wav | 2 | 100 |
| 28 | bass-chromatic-fingers.wav | 1 | 60 |
| 29 | bass-chromatic-pick.wav | 1 | 60 |
| 30 | bass-chromatic-neck.wav | 2 | 60 |
| 31 | bass-chromatic-bridge.wav | 2 | 60 |
| 32 | bass-high.wav | 2 | 70 |
| 33 | bass-sustain.wav | 1 | 60 |
| 34 | bass-eighths.wav | 1 | 120 |
| 35 | bass-walking.wav | 1 | 100 |
| 36 | bass-groove.wav | 1 | 90 |
| 37 | bass-slurs.wav | 1 | 80 |
| 38 | bass-muted.wav | 1 | 90 |
| 39 | bass-slap.wav | 2 | 90 |
| 40 | bass-octaves.wav | 2 | 110 |
| 41 | bass-dynamics.wav | 2 | 70 |
| 42 | bass-harmonics.wav | 3 | 70 |
| 43 | bass-double-stops.wav | 2 | 70 |
| 44 | bass-fast-scale.wav | 2 | 90 |
| 45 | gtr-free-lead.wav / gtr-free-chords.wav | 3 | - |
| 46 | bass-free.wav | 3 | - |
| | recording-log.txt | 1 | |
