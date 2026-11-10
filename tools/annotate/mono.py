# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098

# Offline monophonic annotation: frame pitch track (long-window YIN, non-causal), segmented into notes.
import sys, os, pickle
import numpy as np, soundfile as sf
from scipy import signal

NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
def name(m):
    m = int(round(m)); return f"{NAMES[m % 12]}{m // 12 - 1}"
def midi(hz): return 69 + 12 * np.log2(np.maximum(hz, 1e-9) / 440.0)
HOP = 240  # 5 ms

def load(path):
    x, sr = sf.read(path, dtype='float32'); assert sr == 48000
    return x.astype(np.float64)

def rms_track(x, win=480):
    # RMS over `win` samples centred on each hop
    c = np.concatenate([[0], np.cumsum(x * x)])
    n = len(x) // HOP
    centre = np.arange(n) * HOP
    a = np.clip(centre - win // 2, 0, len(x)); b = np.clip(centre + win // 2, 0, len(x))
    return np.sqrt((c[b] - c[a]) / np.maximum(b - a, 1))

def yin_track(x, fmin, fmax=1450.0, dec=4):
    sr = 48000
    y = signal.decimate(x, dec, ftype='fir', zero_phase=True)
    r = sr / dec
    maxlag = int(r / fmin) + 2
    minlag = max(2, int(r / fmax))
    win = int(1.5 * maxlag)
    h = HOP // dec
    n = len(x) // HOP
    hz = np.zeros(n); ap = np.ones(n)
    half = (win + maxlag) // 2
    ypad = np.concatenate([np.zeros(half), y, np.zeros(half + win + maxlag)])
    lags = np.arange(maxlag)
    for i in range(n):
        s = i * h  # window centred on the frame time
        seg = ypad[s:s + win + maxlag]
        a = seg[:win]
        e0 = (a * a).sum()
        if e0 < 1e-12:
            continue
        cs = np.concatenate([[0], np.cumsum(seg * seg)])
        e_l = cs[win + lags] - cs[lags]
        corr = signal.correlate(seg, a, mode='valid')[:maxlag]
        d = e0 + e_l - 2 * corr
        d[0] = 0
        cm = np.ones(maxlag)
        cm[1:] = d[1:] * lags[1:] / np.maximum(np.cumsum(d[1:]), 1e-20)
        best = -1
        for th in (0.08, 0.15, 0.3):
            below = np.where(cm[minlag:maxlag - 1] < th)[0]
            if len(below):
                k = below[0] + minlag
                while k + 1 < maxlag - 1 and cm[k + 1] < cm[k]:
                    k += 1
                best = k
                break
        if best < 0:
            best = minlag + int(np.argmin(cm[minlag:maxlag - 1]))
        lag = float(best)
        a_, b_, c_ = cm[best - 1], cm[best], cm[best + 1]
        den = a_ - 2 * b_ + c_
        if den > 0:
            lag += np.clip(0.5 * (a_ - c_) / den, -0.5, 0.5)
        hz[i] = r / lag; ap[i] = cm[best]
    return hz, ap

def flux_track(x):
    win = 1024
    xp = np.concatenate([np.zeros(win // 2), x, np.zeros(win)])
    n = len(x) // HOP
    idx = np.arange(n)[:, None] * HOP + np.arange(win)[None, :]
    out = np.zeros(n)
    w = np.hanning(win)
    prev = None
    B = 4096
    for s in range(0, n, B):
        fr = xp[idx[s:s + B]] * w
        mag = np.abs(np.fft.rfft(fr, axis=1))[:, 2:200]  # 94 Hz .. 9.4 kHz
        lg = np.log(1e-4 + mag)
        if prev is None:
            prev = lg[0]
        d = np.diff(np.vstack([prev, lg]), axis=0)
        d[d < 0] = 0
        out[s:s + B] = d.sum(axis=1)
        prev = lg[-1]
    return out

def analyse(path):
    cache = path + '.mono.pkl'
    if os.path.exists(cache):
        return pickle.load(open(cache, 'rb'))
    x = load(path)
    bass = 'bass' in os.path.basename(path)
    hz, ap = yin_track(x, 25.0 if bass else 50.0, 700.0 if bass else 1450.0)
    res = dict(hz=hz, ap=ap, lev=rms_track(x, 960), fast=rms_track(x, 240), flux=flux_track(x), n=len(x))
    pickle.dump(res, open(cache, 'wb'))
    return res

def segment(a, gate_db=-62.0, min_ms=40, clear=0.15):
    """Notes: list of dict(start, end, tail, midi, peak_db, cents) (times in s)."""
    hz, ap, lev = a['hz'], a['ap'], a['lev']
    n = len(hz)
    m = midi(hz)
    db = 20 * np.log10(lev + 1e-9)
    voiced = (ap < clear) & (db > gate_db) & (hz > 0)
    # smooth the pitch over 25 ms
    ms = signal.medfilt(np.where(voiced, m, 0), 5)
    note = np.where(voiced & (ms > 0), np.round(ms), 0).astype(int)
    # runs of one note (gaps of up to 3 frames bridged)
    runs = []
    i = 0
    while i < n:
        if note[i] == 0:
            i += 1; continue
        j = i; last = i; cur = note[i]
        while j < n and (note[j] == cur or (j - last) <= 3 and note[j] == 0):
            if note[j] == cur: last = j
            j += 1
        runs.append([i, last + 1, cur])
        i = last + 1
    return runs, m, db, voiced

if __name__ == '__main__':
    path = sys.argv[1]
    a = analyse(path)
    runs, m, db, voiced = segment(a)
    flux = a['flux']
    for (i, j, cur) in runs:
        dur = (j - i) * 5
        if dur < 30: continue
        pk = db[i:j].max()
        mm = np.median(m[i:j][voiced[i:j]])
        fl = flux[max(0, i - 12):i + 6].max()
        print(f"{i*0.005:8.3f} {dur:6d} ms {name(cur):4s} {100*(mm-cur):+4.0f}c peak {pk:6.1f} dB  flux {fl:5.1f}")
