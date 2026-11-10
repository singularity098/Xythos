#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098
"""Makes the Fart pedal's samples (src/engine/blocks/InstrumentBlocks.cpp, kFartSamples) from the owner's recordings.

python3 tools/prepare_fart_samples.py <folder of .mp3/.wav files>

Every file is one fart, ready to play from its first sample, except files named chop_these_up_*: they hold many,
which are found here (by level, gaps of 70 ms or more between them) and cut out, each from the moment it starts.
Every sample is then:
  - mixed down to mono and resampled to 48 kHz (ffmpeg),
  - trimmed: it starts where its level first comes within 30 dB of its peak (backed up to 15 ms while the sound
    is still within 48 dB of it, then to the zero crossing before), and ends once it stays 50 dB under its peak,
  - given a 0.5 ms fade-in (only where the start was cut) and a 20 ms fade-out,
  - peak-normalized to -0.5 dBFS, and written as 16-bit WAV: resources/farts/<id>.wav.
A cut that is nearly the same as another (correlation over 0.9) is left out.

Then each sample's pitch is measured: YIN over 90 ms frames, low-passed at 1.2 kHz, 25 to 800 Hz, the
level-weighted median of the frames that have a clear period. A sample with a period in under a fifth of its
frames (a hiss or a splat) takes its strongest spectral peak (30 Hz to 2 kHz) instead. Its loudness is the 90th
percentile of its 50 ms RMS levels. The script prints kFartSamples' rows: id, name, pitch, gain (so every sample
plays about as loud), lowest to highest; paste them into InstrumentBlocks.cpp. The build embeds every WAV in
resources/farts/ (cmake/EmbedFartSamples.cmake).

The names are in NAMES below. A sample's position in the list is its place in the Type control's choices, so once
released the list must never grow or shrink (CLAUDE.md, Choice parameters); the names can change.

Needs ffmpeg, NumPy and SciPy.
"""
import argparse
import pathlib
import subprocess
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, fftconvolve, sosfilt

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "resources" / "farts"
SR = 48000

# id -> the name the Type control shows
NAMES = {
    "chop1-10": "Raspberry",
    "chop1-06": "Rumble",
    "chop1-09": "Thunder",
    "apebble-5": "Tuba",
    "apebble-8": "Brass",
    "wet-6139": "Wet One",
    "proud": "Proud",
    "chop1-01": "Toot",
    "chop1-12": "Trombone",
    "chop2-03": "Long Haul",
    "wet-long": "Marathon",
    "chop1-08": "Splat",
    "chop2-02": "Groaner",
    "furtz": "Pop",
    "chop1-07": "Honk",
    "short-83471": "Blip",
    "chop1-02": "Puff",
    "chop1-03": "Pfft",
    "chop1-04": "Sputter",
    "apebble-4": "Squelch",
    "wet-short": "Squish",
    "chop2-01": "Whoopee",
    "chop1-05": "Squeak",
    "balloon": "Balloon",
    "apebble-7": "Mouse",
}

# source file stem -> id (the chopped files' cuts are chop<n>-<cut>)
IDS = {
    "apebble-fart-4-228244": "apebble-4",
    "apebble-fart-5-228245": "apebble-5",
    "apebble-fart-7-228247": "apebble-7",
    "apebble-fart-8-228248": "apebble-8",
    "beanfrog-proud-fart-288263": "proud",
    "dragon-studio-wet-fart-335478": "wet-short",
    "dragon-studio-wet-fart-335478-2": "wet-long",
    "freesound_community-fart-83471": "short-83471",
    "freesound_community-wet-fart-6139": "wet-6139",
    "mikrotop22-furtz22-198542": "furtz",
    "waltermidnight-fart-sound-310129": "balloon",
}


def decode(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def levels_db(x, hop):
    n = len(x) // hop
    if n == 0:
        return np.array([-200.0])
    return 20 * np.log10(np.sqrt((x[:n * hop].reshape(n, hop) ** 2).mean(1)) + 1e-9)


def find_cuts(x):
    """Rough (start, end) samples of each fart in a file of many."""
    hop = 480  # 10 ms
    db = levels_db(x, hop)
    active = db > db.max() - 32
    cuts, i, n = [], 0, len(active)
    while i < n:
        if not active[i]:
            i += 1
            continue
        j = i
        while j < n and (active[j] or active[j:j + 8].any()):  # bridge gaps under 70 ms
            j += 1
        # a blip of a few tens of ms is no fart
        if (j - i) * hop >= 0.1 * SR:
            cuts.append((i * hop, j * hop))
        i = j
    return cuts


def trim(x, start, end, earliest, latest):
    """The precise start and end of the fart roughly at start..end (searching earliest..latest)."""
    hop = 48  # 1 ms
    lo = max(earliest, start - int(0.04 * SR))
    db = levels_db(x[lo:end], hop)
    peak = db.max()
    k = int(np.argmax(db > peak - 30))
    k0 = k
    while k > 0 and k0 - k < 15 and db[k - 1] > peak - 48:
        k -= 1
    s = lo + k * hop
    for t in range(s, max(lo + 1, s - 96), -1):
        if (x[t - 1] <= 0 < x[t]) or (x[t - 1] >= 0 > x[t]):
            s = t
            break
    hop = 480
    db = levels_db(x[s:latest], hop)
    top = int(np.argmax(db))
    e = len(db)
    for j in range(top, len(db)):
        if (db[j:j + 5] < db.max() - 50).all():
            e = j
            break
    return s, min(latest, s + e * hop)


def finish(x, fade_in):
    x = x.copy()
    if fade_in:
        n = 24
        x[:n] *= 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    n = min(len(x) // 4, int(0.02 * SR))
    x[-n:] *= 0.5 + 0.5 * np.cos(np.pi * np.arange(n) / n)
    return x / np.abs(x).max() * 10 ** (-0.5 / 20)


def yin(frame, fmin, fmax):
    w = len(frame) // 2
    tmin, tmax = int(SR / fmax), int(SR / fmin)
    d = np.array([np.sum((frame[:w] - frame[t:t + w]) ** 2) for t in range(tmax + 2)])
    cm = np.ones_like(d)
    cm[1:] = d[1:] * np.arange(1, len(d)) / np.maximum(np.cumsum(d[1:]), 1e-12)
    for t in range(tmin, tmax):
        if cm[t] < 0.25 and cm[t] <= cm[t + 1]:
            a, b, c = cm[t - 1], cm[t], cm[t + 1]
            den = a - 2 * b + c
            return SR / (t + (0.5 * (a - c) / den if den else 0.0)), True
    return 0.0, False


def pitch(x):
    xf = sosfilt(butter(4, 1200, "low", fs=SR, output="sos"), x)
    n, hop = int(0.09 * SR), 240
    hz, weight, frames = [], [], 0
    for i in range(0, len(x) - n, hop):
        frames += 1
        frame = xf[i:i + n]
        level = np.sqrt(np.mean(frame ** 2))
        if level < 0.02:
            continue
        f, ok = yin(frame, 25, 800)
        if ok:
            hz.append(f)
            weight.append(level)
    if frames and len(hz) >= 0.2 * frames:
        hz, weight = np.array(hz), np.array(weight)
        order = np.argsort(hz)
        cum = np.cumsum(weight[order])
        return float(hz[order][np.searchsorted(cum, cum[-1] / 2)]), "period"
    m = 4096
    spectrum = np.zeros(m // 2 + 1)
    for i in range(0, max(1, len(x) - m), 1024):
        seg = np.zeros(m)
        part = x[i:i + m]
        seg[:len(part)] = part
        spectrum += np.abs(np.fft.rfft(seg * np.hanning(m))) ** 2
    freqs = np.fft.rfftfreq(m, 1 / SR)
    smooth = np.convolve(spectrum, np.ones(5) / 5, "same")
    band = (freqs > 30) & (freqs < 2000)
    return float(freqs[band][np.argmax(smooth[band])]), "spectral peak"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", type=pathlib.Path)
    args = parser.parse_args()

    samples = {}
    for path in sorted(p for p in args.folder.iterdir() if p.suffix.lower() in (".mp3", ".wav", ".flac")):
        x = decode(path)
        if path.stem.startswith("chop_these_up_"):
            prefix = "chop" + path.stem[len("chop_these_up_"):]
            cuts = find_cuts(x)
            earliest = 0
            for i, (a, b) in enumerate(cuts):
                latest = cuts[i + 1][0] - int(0.15 * SR) if i + 1 < len(cuts) else len(x)
                s, e = trim(x, a, b, earliest, latest)
                earliest = e
                samples[f"{prefix}-{i + 1:02d}"] = finish(x[s:e], True)
        elif path.stem in IDS:
            s, e = trim(x, 0, len(x), 0, len(x))
            samples[IDS[path.stem]] = finish(x[s:e], s > 0)
        else:
            sys.exit(f"{path.name}: not in IDS; give it an id")

    # leave out near copies
    ids = list(samples)
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            if a not in samples or b not in samples:
                continue
            x, y = samples[a], samples[b]
            if min(len(x), len(y)) < 0.6 * max(len(x), len(y)):
                continue
            r = fftconvolve(x, y[::-1]).max() / np.sqrt((x ** 2).sum() * (y ** 2).sum())
            if r > 0.9:
                print(f"// {b} left out: the same as {a} ({r:.2f})", file=sys.stderr)
                del samples[b]

    missing = [k for k in samples if k not in NAMES]
    if missing:
        sys.exit(f"no name for {', '.join(missing)}: add them to NAMES")

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.wav"):
        old.unlink()
    rows = []
    for key, x in samples.items():
        wavfile.write(OUT / f"{key}.wav", SR, np.round(x * 32767).astype(np.int16))
        hz, how = pitch(x)
        loud = np.percentile(levels_db(x, 2400), 90)
        # about -16 dBFS RMS, and peaks no higher than 0 dBFS
        gain = min(10 ** ((-16 - loud) / 20), 1.0 / np.abs(x).max())
        rows.append((hz, key, how, gain, len(x) / SR))
    rows.sort()
    for hz, key, how, gain, seconds in rows:
        print(f'    {{ "{key}", "{NAMES[key]}", {hz:.1f}f, {gain:.3f}f }}, // {seconds:.2f} s, {how}')


if __name__ == "__main__":
    main()
