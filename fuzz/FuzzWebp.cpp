// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

// Image files the user adds (Add Image…, images inside shared presets): the WebP decoder and the image check.

#include "FuzzTarget.hpp"

#include "state/UserImages.hpp"
#include "ui/WebpImage.hpp"

using namespace xythos;

XYTHOS_FUZZ_TARGET(image)
{
    RgbaImage image;
    if (decodeWebp(data, size, image))
    {
        FUZZ_CHECK(isWebp(data, size));
        FUZZ_CHECK(image.width > 0 && image.height > 0);
        FUZZ_CHECK(image.pixels.size() == static_cast<size_t>(image.width) * image.height * 4);
    }
    std::string error;
    (void)isUsableImage(std::string(reinterpret_cast<const char*>(data), size), &error);
    return 0;
}
