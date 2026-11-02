# NAM Core patches

CMake applies these to the `deps/NeuralAmpModelerCore` checkout at configure time, on every platform (in name order,
skipping ones already applied; `--ignore-whitespace` lets them apply to a Windows checkout with CRLF line endings), so
the submodule keeps pointing at upstream NAM Core. `.gitmodules` sets `ignore = dirty` for it, so the patched checkout
doesn't show as modified.

To change a patch: edit `deps/NeuralAmpModelerCore`, then `git -C deps/NeuralAmpModelerCore add -N <new files>` and
`git -C deps/NeuralAmpModelerCore diff > patches/nam/<name>.patch`. After updating the submodule, reconfigure; CMake
stops with an error if a patch no longer applies.

- `0001-checked-weights-iterator.patch`: the model loaders read weights with `*(it++)` as the config dictates and
  only compare with the end afterwards (if at all: the LSTM only asserts), so a `.nam` file with fewer weights than
  its config needs read past the end of the vector (found by the `nam_model` fuzz target: heap over-reads in
  `Conv1D::set_weights_` and `LSTMCell`). `NAM/weights_iterator.h` adds `nam::WeightsIterator`, which throws
  `std::runtime_error` when read past the end, and every loader takes it in place of `std::vector<float>::iterator`.
  The NAM block already reports a model that throws as failing to load. Worth offering upstream (all three).
- `0002-checked-config-keys.patch`: the config parsers read required keys as `config["key"]`, and on a const
  `nlohmann::json` that's undefined behaviour when the key is missing (it reads past the end of the object's map:
  heap over-reads and use-after-free reports from the fuzzer). Every `x["key"]` read is now `x.at("key")`, which
  throws `json::out_of_range`. None of them wrote through the brackets, so valid models load as before. Made by
  `sed -E 's/\b([A-Za-z_][A-Za-z_0-9]*)\["([A-Za-z_0-9]+)"\]/\1.at("\2")/g; s/\)\["([A-Za-z_0-9]+)"\]/).at("\1")/g'`
  over `NAM/` (on top of 0001); after updating NAM Core, redo it the same way.
- `0003-lstm-config-sizes.patch`: an LSTM config with no layers or a size below 1 read layer -1 or empty matrices;
  such configs are refused.
- `0004-wavenet-layer-array-sizes.patch`: a WaveNet's layer arrays feed each other (array 0 takes the input, array
  i the previous array's layer outputs and head outputs), and nothing checked their sizes chain; Eigen's dimension
  checks are compiled out in release builds, so a mismatch read out of bounds in the matrix products. The
  constructor now checks every array before building any: positive sizes, a kernel size per dilation,
  input_size against the input or the previous array's channels, its head inputs against the previous head_size,
  and condition_size against the input when there's no condition DSP. Every NAM Core example model still loads.

Tests in `tests/NamBlockTests.cpp`: "a model file with fewer weights...", "a model config with a missing key or
impossible sizes...", "a WaveNet whose layer arrays don't fit together..." and "every NAM Core example model still
loads with our patches".
