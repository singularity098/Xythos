#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 singularity098
"""Writes the fuzz targets' seed corpus, fuzz/corpus/<target>/: small valid inputs for the fuzzers to start from.
Files it doesn't write (inputs that once crashed a target, kept so the replay test keeps them fixed) are left alone.

Run from the repository root after changing a format (needs Pillow for the image seeds):
    python3 fuzz/make_corpus.py
"""

import io
import json
import math
import os
import struct
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "corpus")
NAM_MODELS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "deps", "NeuralAmpModelerCore",
                          "example_models")


def put(target, name, data):
    os.makedirs(os.path.join(ROOT, target), exist_ok=True)
    if isinstance(data, str):
        data = data.encode()
    with open(os.path.join(ROOT, target, name), "wb") as f:
        f.write(data)


def wav(frames, channels=1, bits=16, fmt=1, rate=48000, extensible=False):
    """A WAV file: fmt 1 (PCM 16/24) or 3 (float 32)"""
    data = bytearray()
    for i, v in enumerate(frames):
        for ch in range(channels):
            s = v if ch == 0 else -v
            if fmt == 3:
                data += struct.pack("<f", s)
            elif bits == 16:
                data += struct.pack("<h", max(-32768, min(32767, int(s * 32767))))
            else:
                data += struct.pack("<i", max(-8388608, min(8388607, int(s * 8388607))))[:3]
    block = channels * bits // 8
    if extensible:
        guid = struct.pack("<H", fmt) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
        fmt_chunk = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * block, block, bits, 22, bits, 0) + guid
    else:
        fmt_chunk = struct.pack("<HHIIHH", fmt, channels, rate, rate * block, block, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt_chunk)) + fmt_chunk
    body += b"LIST" + struct.pack("<I", 4) + b"INFO"  # a chunk to skip
    body += b"data" + struct.pack("<I", len(data)) + bytes(data)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def decaying(n):
    return [math.exp(-i / (n / 6)) * math.sin(i * 0.7) for i in range(n)]


def chain(blocks, split=None, midi=None):
    root = {"version": 2, "blocks": blocks}
    if split:
        root["split"] = {"begin": split[0], "end": split[1]}
    if midi:
        root["midi"] = midi
    return root


def block(type_id, slot, params=None, **extra):
    b = {"type": type_id, "slot": slot, "params": params or [0.5] * 16, "bypass": False, "config": {}}
    b.update(extra)
    return b


HASH = "0123456789abcdef0123456789abcdef"

CHAINS = [
    chain([]),
    chain([block("delay", 0), block("reverb", 1, bypass=True)]),
    chain([block("nam", 0, config={"path": "/nowhere/amp.nam", "hash": HASH, "tone": True}),
           block("ir", 3, config={"path": "lib.zip!/cabs/4x12.wav", "hash": HASH}),
           block("compressor", 5, branch=1), block("chorus", 7, latched=3, image="mine.png")],
          split=(1, 3), midi=[{"channel": 0, "cc": 7, "param": 5}]),
    chain([block("synth", 2, config={"lookahead": 10}), block("looper", 4, config={"countIn": True, "latency": 10})]),
]


def main():
    for i, c in enumerate(CHAINS):
        put("chain_state", f"chain-{i}.json", json.dumps(c))
        preset = {"version": 1, "chain": c, "globals": {"input": 3.0, "output": -6.0, "limiter": 1.0}}
        put("preset_json", f"preset-{i}.json", json.dumps(preset, indent=2))

    # preset files: a zip with preset.json and the files it embeds
    for i, c in enumerate(CHAINS):
        preset = {"version": 1, "chain": c, "globals": {}}
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("preset.json", json.dumps(preset))
            for b in c["blocks"]:
                path = b["config"].get("path")
                if path:
                    name = path.replace("!/", "/").split("/")[-1]
                    content = wav(decaying(64)) if name.endswith(".wav") else b"{}"
                    z.writestr(f"files/{b['config']['hash']}/{name}", content)
                if b.get("image"):
                    z.writestr(f"images/pedals/{b['image']}", b"\x89PNG\r\n\x1a\n")
        put("preset_file", f"preset-{i}.xythos", buf.getvalue())
        # the same as entries: preset.json, then name and bytes in turn, split at "\n--\n"
        parts = [json.dumps(preset)]
        for b in c["blocks"]:
            path = b["config"].get("path")
            if path:
                name = path.replace("!/", "/").split("/")[-1]
                parts += [f"files/{b['config']['hash']}/{name}", "{}"]
        put("preset_entries", f"entries-{i}.txt", "\n--\n".join(parts))

    put("settings", "default.json", json.dumps({
        "version": 1, "namDir": "/home/me/NAM", "irDir": "~/IRs", "a2Lite": False, "normalize": True,
        "accent": 7, "accentColor": "#aabbcc", "highlightStyle": 2, "highlightColors": ["#7278fe", "#c688d6"],
        "savedColors": ["#ffffff"], "favorites": [HASH], "recentPresets": ["/a.xythos"], "checkUpdates": True,
        "dismissedUpdate": "1.0.0", "cabImages": {HASH: "4x12-1"}, "pedalImages": {}, "ampImages": {HASH: "amp-3"},
        "blockDefaults": {"delay": {"Time": 350.0, "Mix": 30.0}}}))
    put("settings", "empty.json", "{}")

    put("update_check", "release.json", json.dumps({"tag_name": "v1.2.3", "draft": False, "prerelease": False,
                                                    "html_url": "https://example.com"}))
    put("update_check", "cache.json", json.dumps({"checkedAt": 1767225600, "latest": "1.0.1"}))
    put("update_check", "version.txt", "v10.20.30")

    put("wav", "pcm16-mono.wav", wav(decaying(32)))
    put("wav", "pcm24-stereo.wav", wav(decaying(32), channels=2, bits=24))
    put("wav", "float-stereo.wav", wav(decaying(32), channels=2, bits=32, fmt=3))
    put("wav", "extensible.wav", wav(decaying(16), channels=2, bits=24, extensible=True))
    put("ir_wav", "ir-16.wav", wav(decaying(256)))
    put("ir_wav", "ir-24-stereo-44k.wav", wav(decaying(256), channels=2, bits=24, rate=44100))
    put("ir_wav", "ir-float.wav", wav(decaying(128), bits=32, fmt=3, rate=96000))
    # regressions: float IRs with samples the convolution can't take (rejected when loading)
    put("ir_wav", "huge-samples.wav", wav([v * 1e38 for v in decaying(128)], bits=32, fmt=3))
    put("ir_wav", "nan-samples.wav", wav([float("nan")] + decaying(127), bits=32, fmt=3))

    for name in ("lstm.nam", "wavenet.nam", "slimmable_wavenet.nam"):
        with open(os.path.join(NAM_MODELS, name), "rb") as f:
            put("nam_model", name, f.read())

    try:
        from PIL import Image
    except ImportError:
        sys.exit("needs Pillow for the image seeds")
    img = Image.new("RGB", (8, 6))
    for x in range(8):
        for y in range(6):
            img.putpixel((x, y), (x * 30, y * 40, 128))
    for fmt, target, name, kwargs in (("WEBP", "image", "lossy.webp", {"quality": 80}),
                                      ("WEBP", "image", "lossless.webp", {"lossless": True}),
                                      ("PNG", "stb_image", "rgb.png", {}),
                                      ("JPEG", "stb_image", "baseline.jpg", {"quality": 90}),
                                      ("JPEG", "stb_image", "progressive.jpg", {"progressive": True})):
        buf = io.BytesIO()
        img.save(buf, fmt, **kwargs)
        put(target, name, buf.getvalue())
    buf = io.BytesIO()
    img.convert("RGBA").save(buf, "WEBP", lossless=True)
    put("image", "alpha.webp", buf.getvalue())
    buf = io.BytesIO()
    img.convert("P").save(buf, "PNG")
    put("stb_image", "palette.png", buf.getvalue())

    put("text_edit", "words.bin", b"\x10hello wordy w\xc3\xb6rld\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c")
    put("ink", "hex.txt", "#7278fe")
    put("ink", "short.txt", "c688d6")

    # the engine: a byte stream of choices (see FuzzEngine.cpp); a few starts
    for i, seed in enumerate((bytes(range(0, 256, 3)), bytes(range(255, 0, -7)) * 2, b"\x01\x00\x04" + bytes(200))):
        put("engine", f"run-{i}.bin", seed)


if __name__ == "__main__":
    main()
