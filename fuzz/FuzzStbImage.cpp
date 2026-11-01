// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The PNG/JPEG decoder the UI shows user images with: DPF's bundled stb_image (in NanoVG), compiled here the same
// way. User images can arrive inside a shared preset (only their first bytes are checked, isUsableImage()), so this
// decoder sees files from anyone.

#include "FuzzTarget.hpp"

#include "state/UserImages.hpp"

// (compiled without warnings: fuzz/CMakeLists.txt)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../deps/DPF/dgl/src/nanovg/stb_image.h"

using namespace xythos;

XYTHOS_FUZZ_TARGET(stb_image)
{
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    if (!isUsableImage(bytes) || size > 0x7fffffff)
        return 0;
    int w = 0, h = 0, n = 0;
    // as nanovg.c's nvgCreateImageMem does
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
    if (pixels)
    {
        FUZZ_CHECK(w > 0 && h > 0);
        // touch the last pixel: the buffer must be w * h * 4
        volatile unsigned char last = pixels[static_cast<size_t>(w) * h * 4 - 1];
        (void)last;
        stbi_image_free(pixels);
    }
    return 0;
}
