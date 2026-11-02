// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "RasterImage.hpp"

#include <climits>
#include <cstring>

// Only the decoders we need, with internal linkage (DPF's NanoVG links its own old copy under other names)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace xythos {

static_assert(kMaxImageSide == 16384, "keep STBI_MAX_DIMENSIONS in step");

bool decodeRaster(const unsigned char* data, size_t size, RgbaImage& out)
{
    if (data == nullptr || size == 0 || size > static_cast<size_t>(INT_MAX))
        return false;
    const int length = static_cast<int>(size);
    int width = 0, height = 0, channels = 0;
    // the header first, so an absurd size is refused before anything is allocated
    if (!stbi_info_from_memory(data, length, &width, &height, &channels) || width <= 0 || height <= 0 ||
        static_cast<unsigned>(width) > kMaxImageSide || static_cast<unsigned>(height) > kMaxImageSide ||
        static_cast<size_t>(width) * static_cast<size_t>(height) > kMaxImagePixels)
        return false;
    stbi_uc* pixels = stbi_load_from_memory(data, length, &width, &height, &channels, 4);
    if (pixels == nullptr)
        return false;
    out.width = static_cast<unsigned>(width);
    out.height = static_cast<unsigned>(height);
    out.pixels.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace xythos
