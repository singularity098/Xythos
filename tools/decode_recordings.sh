#!/bin/sh
# Decodes the owner's recordings in tests/recordings (MP3 for the first eight clips, FLAC since) to 48 kHz float WAVs
# in build/recordings, for the real-recording tracking benchmark (tests/RealTrackingTests.cpp). Needs ffmpeg.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/recordings}
mkdir -p "$out"
for f in "$root"/tests/recordings/*.mp3 "$root"/tests/recordings/*.flac; do
    name=$(basename "$f")
    ffmpeg -loglevel error -y -i "$f" -ar 48000 -ac 1 -c:a pcm_f32le "$out/${name%.*}.wav"
done
echo "decoded to $out"
