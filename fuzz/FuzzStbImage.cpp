// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// The PNG/JPEG decoder the UI shows images with (decodeRaster(), a current stb_image from deps/stb). User images can
// arrive inside a shared preset, so it sees files from anyone. (Until 2026-11-02 the UI used DPF's bundled 2016
// stb_image, which this target found writing out of bounds on a crafted JPEG.)

#include "FuzzTarget.hpp"

#include "ui/RasterImage.hpp"

using namespace xythos;

XYTHOS_FUZZ_TARGET(stb_image)
{
    RgbaImage image;
    if (decodeRaster(data, size, image))
    {
        FUZZ_CHECK(image.width > 0 && image.height > 0);
        FUZZ_CHECK(image.width <= kMaxImageSide && image.height <= kMaxImageSide);
        FUZZ_CHECK(static_cast<size_t>(image.width) * image.height <= kMaxImagePixels);
        FUZZ_CHECK(image.pixels.size() == static_cast<size_t>(image.width) * image.height * 4);
    }
    return 0;
}
