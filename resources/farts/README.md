# Fart samples

The Fart pedal's samples (`src/engine/blocks/InstrumentBlocks.cpp`, `kFartSamples`): 16-bit mono 48 kHz WAVs, each
starting where its fart starts, peak-normalized to -0.5 dBFS. The build embeds every WAV here
(`cmake/EmbedFartSamples.cmake`).

They were made on 2026-11-10 by `tools/prepare_fart_samples.py` from public-domain recordings the owner collected
(file names as downloaded: apebble, beanfrog, dragon-studio, freesound_community, mikrotop22 and waltermidnight, plus
two files of many farts, cut apart by the tool: `chop1-*` and `chop2-*`). The tool's `IDS` maps each source file to
its id here.

A sample's place in `kFartSamples` is its place in the Type control's choices, so once released none may be added
or removed; a new set of sounds would be a new block type.
