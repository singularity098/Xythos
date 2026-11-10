# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098

# Writes a .notes file for a monophonic recording from the offline analysis (mono.py), with optional overrides.
# usage: gen_notes.py in.wav out.notes [overrides.txt]
# Override lines:  picked T0 T1 NOTE [minflux]   (repeated picks of one note: a segment per pick)
#                  set T0 T1 TOKENS...            (replace the labels there; TOKENS as in .notes, or "-" for nothing)
#                  main_db X                      (the level a run's peak needs to be a played note)
import sys, os
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])))
import mono
from scipy import signal

def tok_note(n): return mono.name(n)

def spectrum_peaks(x, centre, win):
    a = max(0, centre - win // 2); seg = x[a:a + win]
    w = np.hanning(len(seg))
    sp = np.abs(np.fft.rfft(seg * w, 4 * win)) / (w.sum() / 2)
    d = 20 * np.log10(sp + 1e-12)
    pk, _ = signal.find_peaks(d, height=d.max() - 45, prominence=8)
    return pk * 48000.0 / (4 * win), d[pk]

def double_stop(x, i, j, hz):
    """None if the run is one note at hz; else the notes really there."""
    dur = (j - i) * mono.HOP
    win = int(np.clip(dur, 2048, 8192))
    if win < 6 * 48000.0 / hz: return None  # too short to tell this pitch's harmonics apart
    centre = i * mono.HOP + min(dur // 2, 7200 + win // 2)
    f, d = spectrum_peaks(x, centre, win)
    if len(f) == 0: return None
    def level(target, tol=0.03):
        sel = np.abs(f / target - 1) < tol
        return d[sel].max() if sel.any() else -200.0
    h = [level(hz * k) for k in range(1, 11)]
    top = max(h)
    if h[0] >= top - 30 or sum(1 for k in range(8) if h[k] >= top - 35) >= 6: return None
    strong = [(fr, dd) for fr, dd in zip(f, d) if dd >= d.max() - 25 and 38 < fr < 1500]
    notes = []
    for fr, dd in strong:
        if any(abs(fr / (g * k) - 1) < 0.025 for (g, _) in strong if g < fr * 0.6 for k in range(2, 9)): continue
        k = fr / hz
        if round(k) < 2 or abs(k / round(k) - 1) > 0.03: continue  # not one of the pitch's harmonics
        notes.append(int(round(mono.midi(fr))))
    notes = sorted(set(notes))
    return notes if len(notes) >= 2 else None

def main():
    wav, out = sys.argv[1], sys.argv[2]
    overrides = [l.split() for l in open(sys.argv[3])] if len(sys.argv) > 3 else []
    overrides = [o for o in overrides if o and not o[0].startswith('#')]
    a = mono.analyse(wav)
    x = mono.load(wav)
    tail_frames = 12 if 'bass' in os.path.basename(wav) else 6
    runs, m, db, voiced = mono.segment(a, gate_db=-72.0)
    long_peaks = [db[i:j].max() for (i, j, c) in runs if (j - i) * 5 >= 150]
    main_db = float(np.clip((np.median(long_peaks) if long_peaks else -40.0) - 13.0, -56.0, -44.0))
    repicks = False
    for o in overrides:
        if o[0] == 'main_db': main_db = float(o[1])
        if o[0] == 'repicks': repicks = True
    # a run an octave (or a twelfth, two octaves) from the run right before it, without a pick: the same note, its
    # fundamental or a harmonic taking over as it decays; either counts
    flux_ = a['flux']
    typ0 = np.median([flux_[max(0, i - 12):i + 5].max() for (i, j, c) in runs if (j - i) * 5 >= 150] or [100.0])
    chains = []  # lists of runs
    for r in runs:
        (i, j, c) = r
        if chains:
            last = chains[-1][-1]
            notes = {x[2] for x in chains[-1]}
            fast_ = a['fast']
            rise = fast_[max(0, i - 2):i + 8].max() / max(1e-9, fast_[max(0, i - 10):max(1, i - 3)].mean())
            if i - last[1] <= 12 and (abs(c - last[2]) in (12, 19, 24) or c in notes) and rise < 1.4:
                chains[-1].append(r)
                continue
        chains.append([r])
    runs = []
    framelab = {}
    for ch in chains:
        dur = {}
        for (i, j, c) in ch: dur[c] = dur.get(c, 0) + (j - i)
        dom = max(dur, key=dur.get)
        for (i, j, c) in ch:
            if j - i >= 20:
                dom = c
                break
        seen = set(); before = 0
        for (i, j, c) in ch:
            seen.add(c)
            if len(seen) > 1:
                lab = mono.name(dom) if (c == dom and before - dur.get(dom, 0) * 0 < 20 and all(
                    x[2] == dom or (x[1] - x[0]) < 20 for x in ch if x[0] < i)) else '|'.join(mono.name(x) for x in sorted(seen | {dom}))
                for k in range(i, j): framelab[k] = lab
            before += j - i
        # early short runs of another note before the dominant one: either
        for (i, j, c) in ch:
            if c != dom and (j - i) < 20 and i < min(x[0] for x in ch if x[2] == dom):
                for k in range(i, j): framelab[k] = '|'.join(mono.name(x) for x in sorted({c, dom}))
        runs.append((ch[0][0], ch[-1][1], dom))
    flux = a['flux']; fast = a['fast']
    n = len(m)
    labels = [''] * n  # token string per frame
    def group(i, cur):
        lo = int(round(m[i] - 0.3)); hi = int(round(m[i] + 0.3))
        if i in framelab: return framelab[i]
        if not voiced[i] or abs(m[i] - cur) > 1.5: return tok_note(cur)
        return tok_note(lo) if lo == hi else f"{tok_note(lo)}|{tok_note(hi)}"
    mains = [(i, j, c) for (i, j, c) in runs if (j - i) * 5 >= 40 and db[i:j].max() >= main_db]
    main_set = set(mains)
    onset_flux = [flux[max(0, i - 12):i + 5].max() for (i, j, c) in mains]
    typical = np.median(onset_flux) if onset_flux else 100.0
    summary = []
    prev_end = -100
    cur_run = [None]
    for (i, j, c) in runs:
        cur_run[0] = (i, j, c)
        if (i, j, c) not in main_set:
            for k in range(i, j):
                if not labels[k]: labels[k] = '?' + group(k, c).replace('|', ' ?')
            continue
        # start: back to the pick (the flux peak) unless it follows another note directly (legato)
        start = i
        vv = voiced[i:j] & (np.abs(m[i:j] - c) < 0.7)
        stops = double_stop(x, i, j, 440.0 * 2 ** ((np.median(m[i:j][vv]) - 69) / 12)) if vv.any() else None
        if stops:
            for k in range(i, j): framelab[k] = ' '.join(tok_note(q) for q in stops)
            c = stops[0]
        w0 = max(0, i - 12)
        pk = w0 + int(np.argmax(flux[w0:i + 5]))
        if flux[pk] >= 0.3 * typical and pk > prev_end:
            start = min(pk, i)
        peak = db[i:j].max()
        thr = max(peak - 34.0, -62.0)
        above = np.where(db[i:j] >= thr)[0]
        req_end = i + above[-1] + 1
        # a mute: the level falls 9 dB within 60 ms and stays down. The note is required up to where the fall begins
        # (it may sound through it).
        for f in range(i + 20, req_end):
            if db[f] < db[f - 12] - 9.0 and db[f:min(j, f + 12)].max() < db[f - 12] - 6.0:
                req_end = f - 12
                break
        # re-picks of the same note inside the run
        splits = []
        cand = []
        if repicks and req_end > i + 12:
            cand, _ = signal.find_peaks(flux[i + 10:req_end], height=0.5 * typical, distance=10)
            cand = cand + i + 10
        for p in cand:
            before = fast[max(0, p - 5):p - 1].mean(); after = fast[p + 1:p + 8].max()
            if after > 1.4 * before: splits.append(p)
        for k in range(start, i):
            labels[k] = tok_note(c)
        for k in range(i, req_end):
            labels[k] = group(k, c)
        for k in range(req_end, j):
            labels[k] = '?' + group(k, c).replace('|', ' ?')
        # (the analysis windows are long, so a note seems to end up to half of one early: it may sound a little longer)
        for k in range(j, min(n, j + tail_frames)):
            if not labels[k]: labels[k] = '?' + tok_note(c)
        for p in splits:
            labels[p - 1] = '*'
        for k in range(max(0, start - 3), start):
            if not labels[k] or labels[k].startswith('?'): labels[k] = '*'
        if 0 < i - prev_end <= 6:  # a short unvoiced gap between two notes
            for k in range(prev_end, i):
                if not labels[k]: labels[k] = '*'
        prev_end = j
        summary.append((start * 0.005, ('[' + labels[(i + min(j, req_end)) // 2].replace(' ', '+') + ']') if stops else c, len(splits)))
    # A note left ringing under the next ones (the analysis follows one note at a time, the loudest): it may sound for
    # as long as its fundamental's peak stays within 20 dB of where it was when the next note took over, up to 3 s
    win = 4096; w = np.hanning(win)
    def level_at(frame, hz):
        c = frame * mono.HOP; seg = x[max(0, c - win // 2):c + win // 2]
        if len(seg) < win: return -200.0
        t = np.arange(win) / 48000.0
        return 20 * np.log10(abs(np.sum(seg * w * np.exp(-2j * np.pi * hz * t))) / (w.sum() / 2) + 1e-12)
    for (i, j, c) in mains:
        vv = voiced[i:j] & (np.abs(m[i:j] - c) < 0.7)
        if not vv.any() or j + 20 >= n or not labels[min(n - 1, j + 12)]: continue  # (only when another note follows)
        hz = 440.0 * 2 ** ((np.median(m[i:j][vv]) - 69) / 12)
        ref = level_at(max(i, j - 6), hz)
        tok = tok_note(c)
        for k in range(j, min(n, j + 600), 2):
            lv = level_at(k, hz)
            if lv < ref - 20.0 or lv < -72.0: break
            for kk in (k, k + 1):
                if kk >= n: break
                lab = labels[kk]
                if lab == '*' or tok in lab.replace('|', ' ').replace('?', '').split(): continue
                labels[kk] = (lab + ' ?' + tok) if lab else '?' + tok
    for o in overrides:
        if o[0] == 'set':
            i0, i1 = int(float(o[1]) * 200), int(float(o[2]) * 200)
            for k in range(i0, min(i1, n)): labels[k] = '' if o[3] == '-' else ' '.join(o[3:])
        elif o[0] == 'picked':
            i0, i1 = int(float(o[1]) * 200), int(float(o[2]) * 200)
            height = float(o[4]) if len(o) > 4 else 0.4 * typical
            pk, _ = signal.find_peaks(flux[i0:i1], height=height, distance=8)
            pk = list(pk + i0)
            for k in range(i0, min(i1, n)): labels[k] = o[3]
            for p in pk[1:]: labels[p - 1] = '*'
            for k in range(max(0, pk[0] - 3), pk[0]): labels[k] = '*'
            for k in range(i0, pk[0] - 3): labels[k] = ''
            summary.append((pk[0] * 0.005, f"{o[3]} x{len(pk)} picks", 0))
    with open(out, 'w') as f:
        f.write(f"# {os.path.basename(wav)}: made by the offline annotator (long-window pitch analysis), checked against\n"
                f"# what the file is meant to hold. See tests/recordings/README.md.\n")
        k = 0
        while k < n:
            if not labels[k]: k += 1; continue
            e = k
            while e < n and labels[e] == labels[k]: e += 1
            f.write(f"{k * 0.005:.3f} {e * 0.005:.3f} {labels[k]}\n")
            k = e
    summary.sort(key=lambda s: s[0])
    print(f"{os.path.basename(wav)}: {len(summary)} notes")
    line = ''
    for (t, c, s) in summary:
        item = f"{t:.2f}:{mono.name(c) if not isinstance(c, str) else c}" + (f"(+{s})" if s else '')
        line += item + ' '
    print(line)

main()
