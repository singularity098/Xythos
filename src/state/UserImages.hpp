// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace xythos {

// The user's own block images: three libraries in the config directory (images/amps, images/cabs, images/pedals),
// one for NAM blocks, one for IR blocks and one for every other block. A block that uses one stores its id (the
// file's name in its library, `BlockState::image`); the UI shows it as it is, without the face knobs, LED or
// footswitch of the built-in images. When the file is gone (deleted), the block shows its stock look again.

enum class ImageKind { Amp, Cab, Pedal };

// Which library a block type's images come from: NAM blocks are amps, IR blocks cabs, the rest pedals
ImageKind imageKindFor(const std::string& typeId);

// "Amp", "Cab", "Pedal"
const char* imageKindName(ImageKind kind);

// The library's folder, images/<amps|cabs|pedals>/ in configDir()
std::filesystem::path userImagesDir(ImageKind kind);

struct UserImage {
    std::string id;   // the file name in the library, e.g. "My Amp.png"
    std::string name; // without the extension
    std::string path; // UTF-8
};

// Every image in a library, sorted by name (ignoring case)
std::vector<UserImage> listUserImages(ImageKind kind);

// The file of the image `id`, or "" if there's no such image (deleted, or an id that isn't a plain file name)
std::string userImagePath(ImageKind kind, const std::string& id);

// Whether `bytes` are an image the UI can show: PNG (8 or 16-bit), JPEG or WebP, by their first bytes. Else false, with the reason in `error`.
bool isUsableImage(const std::string& bytes, std::string* error = nullptr);

// Adds an image to a library under `fileName` (the extension made lowercase), with a number added when that name
// is taken by another image ("My Amp 2.png"). If the library already has an image with the same content, that
// one's id is returned and nothing is written. Nullopt, with the reason in `error`, if it isn't a usable image or
// can't be written.
std::optional<std::string> addUserImage(ImageKind kind, const std::string& fileName, const std::string& bytes,
                                        std::string* error = nullptr);

// Copies the image file `source` (UTF-8) into a library, as above
std::optional<std::string> importUserImage(ImageKind kind, const std::string& source, std::string* error = nullptr);

// Removes an image from its library. Blocks that use it show their stock image again.
bool deleteUserImage(ImageKind kind, const std::string& id);

} // namespace xythos
