# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098

# Writes a .notes file from a hand-written spec of what's played: lines "T0 T1 TOKENS..." (a strum or a picked note
# ringing from T0 to its mute at T1). Each is required for its first REQ seconds (default: to 0.12 s before T1, at
# most 2.5 s), then may sound until 0.1 s after T1. Lines starting with "raw " are copied as they are.
import sys
spec = [l.split() for l in open(sys.argv[1]) if l.strip() and not l.startswith('#')]
header = [l for l in open(sys.argv[1]) if l.startswith('#')]
events = []  # (t0, t1, token, required)
raw = []
for s in spec:
    if s[0] == 'raw':
        raw.append((float(s[1]), float(s[2]), ' '.join(s[3:]))); continue
    t0, t1 = float(s[0]), float(s[1])
    req_end = min(t1 - 0.12, t0 + 2.5)
    for tok in s[2:]:
        if tok.startswith('?'):
            events.append((t0, t1 + 0.1, tok[1:], False))
        else:
            events.append((t0, req_end, tok, True))
            for part in tok.split('|'):
                events.append((req_end, t1 + 0.1, part, False))
times = sorted({round(e[0], 3) for e in events} | {round(e[1], 3) for e in events})
order = {}
lines = []
for a, b in zip(times, times[1:]):
    mid = (a + b) / 2
    req = []; maybe = []
    for (t0, t1, tok, r) in events:
        if t0 <= mid < t1:
            (req if r else maybe).append(tok)
    reqnotes = {p for t in req for p in t.split('|')}
    maybe = [m for m in dict.fromkeys(maybe) if m not in reqnotes]
    toks = list(dict.fromkeys(req)) + ['?' + m for m in maybe]
    if toks: lines.append((a, b, ' '.join(toks)))
lines += raw
lines.sort()
with open(sys.argv[2], 'w') as f:
    for h in header: f.write(h)
    for a, b, t in lines: f.write(f"{a:.3f} {b:.3f} {t}\n")
print(sys.argv[2], len(lines), "segments")
