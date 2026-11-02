// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/UserImages.hpp"

#include "state/Paths.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace xythos {

namespace fs = std::filesystem;

namespace {

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

bool imageExtension(const fs::path& path)
{
    const std::string ext = lowered(pathToUtf8(path.extension()));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp";
}

// An id names a file directly in its library: no folders, no "..", and an image extension
bool validId(const std::string& id)
{
    if (id.empty() || id == "." || id == ".." || id.find_first_of("/\\:") != std::string::npos)
        return false;
    return imageExtension(pathFromUtf8(id));
}

bool fail(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
    return false;
}

} // namespace

ImageKind imageKindFor(const std::string& typeId)
{
    // the NAM and IR blocks' typeIds (NamBlock.cpp, IrBlock.cpp)
    if (typeId == "nam")
        return ImageKind::Amp;
    if (typeId == "ir")
        return ImageKind::Cab;
    return ImageKind::Pedal;
}

const char* imageKindName(ImageKind kind)
{
    switch (kind)
    {
    case ImageKind::Amp:
        return "Amp";
    case ImageKind::Cab:
        return "Cab";
    case ImageKind::Pedal:
        break;
    }
    return "Pedal";
}

fs::path userImagesDir(ImageKind kind)
{
    const char* folder = kind == ImageKind::Amp ? "amps" : kind == ImageKind::Cab ? "cabs" : "pedals";
    return configDir() / "images" / folder;
}

std::vector<UserImage> listUserImages(ImageKind kind)
{
    std::vector<UserImage> images;
    std::error_code ec;
    for (fs::directory_iterator it(userImagesDir(kind), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::error_code fileEc;
        const std::string id = pathToUtf8(it->path().filename());
        if (!it->is_regular_file(fileEc) || !validId(id))
            continue;
        images.push_back({ id, pathToUtf8(it->path().stem()), pathToUtf8(it->path()) });
    }
    std::sort(images.begin(), images.end(), [](const UserImage& a, const UserImage& b) {
        const std::string la = lowered(a.name), lb = lowered(b.name);
        return la != lb ? la < lb : a.id < b.id;
    });
    return images;
}

std::string userImagePath(ImageKind kind, const std::string& id)
{
    if (!validId(id))
        return {};
    const fs::path path = userImagesDir(kind) / pathFromUtf8(id);
    std::error_code ec;
    return fs::is_regular_file(path, ec) ? pathToUtf8(path) : std::string();
}

bool isUsableImage(const std::string& bytes, std::string* error)
{
    const auto starts = [&bytes](std::string_view magic, size_t at = 0) {
        return bytes.size() >= at + magic.size() && bytes.compare(at, magic.size(), magic) == 0;
    };
    // Any bit depth: the UI decodes PNG and JPEG itself (decodeRaster(), src/ui/RasterImage.*), 16-bit PNG
    // included (owner, 2026-11-02; DPF's old decoder couldn't)
    if (starts("\x89PNG\r\n\x1a\n") || starts("\xff\xd8\xff") || (starts("RIFF") && starts("WEBP", 8)))
        return true;
    return fail(error, "not a PNG, JPEG or WebP image");
}

std::optional<std::string> addUserImage(ImageKind kind, const std::string& fileName, const std::string& bytes,
                                        std::string* error)
{
    if (!isUsableImage(bytes, error))
        return std::nullopt;
    fs::path name = pathFromUtf8(fileName).filename();
    if (!validId(pathToUtf8(name)))
    {
        fail(error, "not a .png, .jpg or .webp file: " + fileName);
        return std::nullopt;
    }
    for (const UserImage& image : listUserImages(kind))
    {
        if (readFile(pathFromUtf8(image.path)) == bytes)
            return image.id;
    }

    const std::string stem = pathToUtf8(name.stem());
    const std::string ext = lowered(pathToUtf8(name.extension()));
    const fs::path dir = userImagesDir(kind);
    std::string id = stem + ext;
    std::error_code ec;
    for (int n = 2; fs::exists(dir / pathFromUtf8(id), ec); ++n)
        id = stem + " " + std::to_string(n) + ext;
    if (!writeFileAtomic(dir / pathFromUtf8(id), bytes))
    {
        fail(error, "can't write to " + pathToUtf8(dir));
        return std::nullopt;
    }
    return id;
}

std::optional<std::string> importUserImage(ImageKind kind, const std::string& source, std::string* error)
{
    const std::optional<std::string> bytes = readFile(pathFromUtf8(source));
    if (!bytes)
    {
        fail(error, "can't read " + source);
        return std::nullopt;
    }
    return addUserImage(kind, source, *bytes, error);
}

bool deleteUserImage(ImageKind kind, const std::string& id)
{
    const std::string path = userImagePath(kind, id);
    std::error_code ec;
    return !path.empty() && fs::remove(pathFromUtf8(path), ec);
}

} // namespace xythos
