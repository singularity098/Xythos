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
  The NAM block already reports a model that throws as failing to load. Test: "a model file with fewer weights..."
  in `tests/NamBlockTests.cpp`. Worth offering upstream.
