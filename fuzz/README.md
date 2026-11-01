# Fuzz testing

Fuzz targets for everything that reads input Xythos doesn't control: files people share or download, what a host
hands the plugin, and the audio engine under any sequence of host events.

| Target | What it feeds | Checks |
| --- | --- | --- |
| `chain_state` | the chain state JSON (projects, the exchange file) | no crash; round trip unchanged; editing a valid chain keeps it valid |
| `preset_json` | a preset's `preset.json` | no crash; round trip unchanged |
| `preset_file` | a whole `.xythos` file (damaged zips) | no crash; extracted files stay in the cache |
| `preset_entries` | a preset's zip contents (`preset.json`, entry names and bytes; zipped by the target) | the same, reaching the path handling |
| `settings` | `settings.json` | no crash; round trip unchanged |
| `update_check` | the update check's cache file, GitHub's release JSON, version strings | round trips; "newer" is a strict order |
| `wav` | WAV reading (`AudioFile`) | float WAV round trip is bit exact; 24-bit and FLAC encoders take any float |
| `ir_wav` | an IR file loaded by the IR block, then audio through it | no crash; a loaded IR outputs finite audio |
| `nam_model` | a `.nam` file: `describeNamFile()`, then loaded by the NAM block and run | no crash |
| `image` | the WebP decoder, the user image check | no crash; pixel buffer matches the size |
| `stb_image` | DPF's stb_image, which shows users' PNG and JPEG images | no crash, no out-of-bounds access |
| `engine` | the engine: random chains of any blocks (splits, bypasses, MIDI maps), then buffers of any size and content, parameter changes, MIDI CCs, tempo and transport, chain changes, fault rebuilds, the looper's memory | never a non-finite output sample; never writes past the buffers |
| `text_edit` | text field editing, any keys on any (even invalid UTF-8) text | cursor and selection stay in the text, on character boundaries |
| `ink` | hex colours, HSV, colour names, the gradient texture, home path shortening | hex and HSV round trips; lightened colours are light enough |

Each target is a `XYTHOS_FUZZ_TARGET(name)` in `fuzz/Fuzz*.cpp`; `FUZZ_CHECK` aborts on a broken invariant.

## Two builds

**Replay (always, every platform):** `xythos-fuzz-replay` holds every target without libFuzzer. ctest runs it: the
seed corpus in `fuzz/corpus/<name>/`, then 300 deterministic mutations per target (a few seconds). It also
reproduces a crash from a libFuzzer run on any compiler:

```sh
./build/xythos-fuzz-replay fuzz/corpus                       # what ctest runs (--mutations N, --seed S, --only name)
./build/xythos-fuzz-replay --target preset_file crash-file   # one target on given inputs
```

A replay failure leaves the input that failed as `crash-<name>.bin` in the working directory.

**libFuzzer (Linux, clang):** one binary per target, `xythos-fuzz-<name>` (underscores as dashes), with
AddressSanitizer and UndefinedBehaviorSanitizer over the whole build:

```sh
apt-get install -y clang libclang-rt-18-dev
flags="-fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -fsanitize-recover=nonnull-attribute"
CC=clang CXX=clang++ cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXYTHOS_FUZZ=ON \
  -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-fuzz --target $(ninja -C build-fuzz -t targets all | grep -o '^xythos-fuzz-[a-z-]*' | sort -u)
fuzz/run_all.sh 60                      # every target 60 s; corpora grow in fuzz-work/, crashes in fuzz-work/crashes/
./build-fuzz/xythos-fuzz-engine fuzz-work/engine fuzz/corpus/engine -max_total_time=600   # one target, longer
```

CI runs the libFuzzer targets weekly and by hand (`.github/workflows/fuzz.yml`, 5 minutes each by default) and
uploads any crash.

## Adding a target

Write `fuzz/FuzzSomething.cpp` with `XYTHOS_FUZZ_TARGET(something) { ... return 0; }`, add the file to
`fuzz/CMakeLists.txt`, and seeds to `fuzz/make_corpus.py` (then run it; needs Pillow). A crash a fuzzer found
becomes a seed too (copy it into `fuzz/corpus/<name>/` once fixed), so the replay test keeps it fixed.

Targets that touch files use `fuzz::scratchDir()`, which points the config and cache folders at a temporary folder.
