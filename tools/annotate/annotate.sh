#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098
#
# Makes the note lists (tests/recordings/<clip>.notes) of the recordings of 2026-11-09 (gtr-*, bass-*) again, from the
# decoded WAVs (tools/decode_recordings.sh first) and, where a clip has one, its <clip>.spec (what's played, written by
# hand: the chord clips) or <clip>.ov (overrides for the single-note analysis). See tests/recordings/README.md.
# Needs Python 3 with numpy, scipy and soundfile. The first run takes a few minutes (the analysis of each WAV is kept
# next to it, in build/recordings).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && cd .. && pwd)
wavs=${1:-$root/build/recordings}
notes=$root/tests/recordings
for f in "$wavs"/gtr-*.wav "$wavs"/bass-*.wav; do
    n=$(basename "$f" .wav)
    if [ -f "$notes/$n.spec" ]; then
        python3 "$here/poly_notes.py" "$notes/$n.spec" "$notes/$n.notes"
    elif [ -f "$notes/$n.ov" ]; then
        python3 "$here/gen_notes.py" "$f" "$notes/$n.notes" "$notes/$n.ov" | head -1
    else
        python3 "$here/gen_notes.py" "$f" "$notes/$n.notes" | head -1
    fi
done
