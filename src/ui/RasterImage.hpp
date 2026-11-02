// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "WebpImage.hpp" // RgbaImage

#include <cstddef>

namespace xythos {

// The largest PNG or JPEG decoded: users' images arrive from anywhere (shared presets carry them), and a small file
// can claim a huge size. 50 megapixels is a 200 MB decode, past any photo worth showing in a block's panel.
constexpr unsigned kMaxImageSide = 16384;
constexpr size_t kMaxImagePixels = 50'000'000;

// Decodes a PNG or JPEG to 8-bit RGBA with a current stb_image (deps/stb), never DPF's bundled 2016 copy, which a
// crafted JPEG could make write out of bounds (found by fuzzing). Plain C++, no DPF, so it can be tested and fuzzed.
// False if it isn't one, is damaged, or is larger than the limits above.
bool decodeRaster(const unsigned char* data, size_t size, RgbaImage& out);

} // namespace xythos
