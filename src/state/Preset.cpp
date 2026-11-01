// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/Preset.hpp"

#include "engine/Params.hpp"
#include "state/Library.hpp"
#include "state/Paths.hpp"
#include "state/UserImages.hpp"

#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

namespace xythos {

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kPresetEntry = "preset.json";

bool fail(std::string* error, std::string message)
{
    if (error)
        *error = std::move(message);
    return false;
}

// A file or folder name that works on Linux and Windows
bool validName(const std::string& name)
{
    if (name.empty() || name == "." || name == ".." || name.back() == '.')
        return false;
    for (const unsigned char c : name)
    {
        if (c < 32 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos)
            return false;
    }
    return true;
}

// A file name that stays in the folder it's put in: no separators, not "." or ".."
bool isPlainFileName(const std::string& name)
{
#ifdef _WIN32
    constexpr std::string_view kSeparators = "/\\:";
#else
    constexpr std::string_view kSeparators = "/";
#endif
    return !name.empty() && name != "." && name != ".." && name.find_first_of(kSeparators) == std::string::npos;
}

// 32 lowercase hex digits, as hashBytes() writes them
bool isContentHash(const std::string& text)
{
    return text.size() == 32 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string fileName(const std::string& path)
{
    if (const std::optional<ArchivePath> archive = splitArchivePath(path))
    {
        const size_t slash = archive->entry.find_last_of('/');
        return slash == std::string::npos ? archive->entry : archive->entry.substr(slash + 1);
    }
    return pathToUtf8(pathFromUtf8(path).filename());
}

bool exists(const std::string& path)
{
    std::error_code ec;
    const std::optional<ArchivePath> archive = splitArchivePath(path);
    if (!archive)
        return fs::is_regular_file(pathFromUtf8(path), ec);
    return readLibraryFile(path).has_value();
}

// Zip writing through miniz's heap writer, saved atomically at the end
class ZipWriter {
public:
    ZipWriter() { fOk = mz_zip_writer_init_heap(&fZip, 0, 0); }
    ~ZipWriter() { mz_zip_writer_end(&fZip); }

    bool add(const std::string& name, const std::string& data)
    {
        return fOk && mz_zip_writer_add_mem(&fZip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    }

    bool save(const fs::path& path)
    {
        void* buffer = nullptr;
        size_t size = 0;
        if (!fOk || !mz_zip_writer_finalize_heap_archive(&fZip, &buffer, &size))
            return false;
        // the finished archive belongs to us now
        const std::string data(static_cast<const char*>(buffer), size);
        mz_free(buffer);
        return writeFileAtomic(path, data);
    }

private:
    mz_zip_archive fZip {};
    bool fOk = false;
};

// Where a block's own image is in a preset file: images/<amps|cabs|pedals>/<id>
std::string imageEntry(ImageKind kind, const std::string& id)
{
    return "images/" + pathToUtf8(userImagesDir(kind).filename()) + "/" + id;
}

} // namespace

const std::vector<uint32_t>& presetGlobalParams()
{
    static const std::vector<uint32_t> params = {
        kParamInputMode, kParamInput, kParamBass, kParamMiddle, kParamTreble, kParamOutput, kParamLimiter,
        kParamMergeBalance, kParamMergeLevelA, kParamMergeLevelB, kParamMergePanA, kParamMergePanB,
        kParamMergeInvertA, kParamMergeInvertB, kParamMergeAlign,
    };
    return params;
}

std::string Preset::toJson() const
{
    json root {
        { "version", kVersion },
        { "chain", json::parse(chain.toJson()) },
        { "globals", globals },
    };
    root["chain"].erase("preset"); // where a preset was loaded from isn't part of it
    return root.dump(2);
}

std::optional<Preset> Preset::fromJson(std::string_view text, std::string* error)
{
    try
    {
        const json root = json::parse(text);
        const int version = jsonInt(root, "version", 0);
        if (version < 1 || version > kVersion)
        {
            fail(error, "unsupported preset version " + std::to_string(version));
            return std::nullopt;
        }
        std::optional<ChainState> chain = ChainState::fromJson(root.at("chain").dump(), error);
        if (!chain)
            return std::nullopt;
        Preset preset;
        preset.chain = std::move(*chain);
        if (const auto globals = root.find("globals"); globals != root.end() && !globals->is_null())
        {
            if (!globals->is_object())
                throw json::type_error::create(302, "globals isn't an object", nullptr);
            for (const auto& [symbol, value] : globals->items())
                preset.globals[symbol] = jsonFloat(value);
        }
        return preset;
    }
    catch (const json::exception& e)
    {
        fail(error, e.what());
        return std::nullopt;
    }
}

bool savePresetFile(const std::string& path, const Preset& preset, bool embedFiles, std::string* error)
{
    Preset saved = preset;
    ZipWriter zip;

    std::set<std::string> written;
    for (BlockState& block : saved.chain.blocks)
    {
        const std::string file = block.config.value("path", std::string());
        if (file.empty())
            continue;
        // the content hash lets a missing file be found again (embedded copy or library match)
        const std::optional<std::string> bytes = readLibraryFile(file);
        if (!bytes)
        {
            if (embedFiles)
                return fail(error, "can't read " + file);
            continue;
        }
        const std::string hash = hashBytes(*bytes);
        block.config["hash"] = hash;
        if (!embedFiles || !written.insert(hash).second)
            continue;

        const std::string dir = "files/" + hash + "/";
        if (!zip.add(dir + fileName(file), *bytes))
            return fail(error, "can't write the preset");
    }

    // the user's own block images; one the library no longer has is left out (the block shows its stock image)
    if (embedFiles)
    {
        for (const BlockState& block : saved.chain.blocks)
        {
            if (block.image.empty())
                continue;
            const std::string entry = imageEntry(imageKindFor(block.typeId), block.image);
            const std::string image = userImagePath(imageKindFor(block.typeId), block.image);
            if (image.empty() || !written.insert(entry).second)
                continue;
            if (const std::optional<std::string> bytes = readFile(pathFromUtf8(image)))
            {
                if (!zip.add(entry, *bytes))
                    return fail(error, "can't write the preset");
            }
        }
    }

    if (!zip.add(kPresetEntry, saved.toJson()))
        return fail(error, "can't write the preset");
    if (!zip.save(pathFromUtf8(path)))
        return fail(error, "can't save " + path);
    return true;
}

std::optional<LoadedPreset> loadPresetFile(const std::string& path,
                                           const std::function<std::string(const std::string& hash)>& findByHash,
                                           std::string* error)
{
    const std::string archive = path + std::string(kArchiveSeparator);
    const std::optional<std::string> text = readLibraryFile(archive + kPresetEntry);
    if (!text)
    {
        fail(error, "not a Xythos preset: " + path);
        return std::nullopt;
    }
    std::optional<Preset> preset = Preset::fromJson(*text, error);
    if (!preset)
        return std::nullopt;

    LoadedPreset loaded;
    for (BlockState& block : preset->chain.blocks)
    {
        const std::string file = block.config.value("path", std::string());
        const std::string hash = block.config.value("hash", std::string());
        FileSource source = FileSource::Original;
        if (!file.empty() && !exists(file))
        {
            source = FileSource::Missing;
            const std::string name = fileName(file);
            const std::string embedded = archive + "files/" + hash + "/" + name;
            // Only a real content hash and a plain file name make the path it's extracted to: the preset may come
            // from anyone, and "../" or an absolute path there would write anywhere. The bytes must match the hash
            // too, as copies are shared between presets by hash.
            const std::optional<std::string> bytes =
                isContentHash(hash) && isPlainFileName(name) ? readLibraryFile(embedded) : std::nullopt;
            if (bytes && hashBytes(*bytes) == hash)
            {
                // extracted under the content hash, so presets share copies
                const fs::path target = cacheDir() / "presets" / hash / pathFromUtf8(name);
                std::error_code ec;
                if (!fs::exists(target, ec))
                    writeFileAtomic(target, *bytes);
                if (fs::exists(target, ec))
                {
                    block.config["path"] = pathToUtf8(target);
                    source = FileSource::Embedded;
                }
            }
            if (source == FileSource::Missing && !hash.empty() && findByHash)
            {
                const std::string match = findByHash(hash);
                if (!match.empty())
                {
                    block.config["path"] = match;
                    source = FileSource::Library;
                }
            }
        }
        loaded.sources.push_back(source);

        // an embedded image of the user's goes into their library (where an identical one is reused, and one with
        // the same name but other content gets a new name)
        if (!block.image.empty())
        {
            const ImageKind kind = imageKindFor(block.typeId);
            if (const std::optional<std::string> bytes = readLibraryFile(archive + imageEntry(kind, block.image)))
            {
                if (const std::optional<std::string> id = addUserImage(kind, block.image, *bytes))
                    block.image = *id;
            }
        }
    }
    preset->chain.preset = path;
    loaded.preset = std::move(*preset);
    return loaded;
}

namespace {

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

std::string relativeFolder(const fs::path& dir, const fs::path& folder)
{
    std::error_code ec;
    const fs::path relative = fs::relative(folder, dir, ec);
    if (ec || relative == ".")
        return {};
    std::string text = pathToUtf8(relative);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

} // namespace

fs::path presetsDir()
{
    return configDir() / "presets";
}

std::vector<PresetEntry> listPresets(const fs::path& dir)
{
    std::vector<PresetEntry> presets;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (!it->is_regular_file(ec) || lowered(pathToUtf8(it->path().extension())) != Preset::kExtension)
            continue;
        presets.push_back({ pathToUtf8(it->path()), relativeFolder(dir, it->path().parent_path()),
                            pathToUtf8(it->path().stem()) });
    }
    std::sort(presets.begin(), presets.end(), [](const PresetEntry& a, const PresetEntry& b) {
        const std::string fa = lowered(a.folder), fb = lowered(b.folder);
        return fa != fb ? fa < fb : lowered(a.name) < lowered(b.name);
    });
    return presets;
}

std::vector<std::string> listPresetFolders(const fs::path& dir)
{
    std::vector<std::string> folders;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_directory(ec))
            folders.push_back(relativeFolder(dir, it->path()));
    }
    std::sort(folders.begin(), folders.end(),
              [](const std::string& a, const std::string& b) { return lowered(a) < lowered(b); });
    return folders;
}

std::optional<fs::path> presetPath(const fs::path& dir, const std::string& folder, const std::string& name)
{
    const std::string fileName = trimmed(name);
    if (!validName(fileName))
        return std::nullopt;
    fs::path path = dir;
    size_t pos = 0;
    while (pos <= folder.size())
    {
        const size_t end = std::min(folder.find_first_of("/\\", pos), folder.size());
        const std::string part = trimmed(folder.substr(pos, end - pos));
        if (!part.empty())
        {
            if (!validName(part))
                return std::nullopt;
            path /= pathFromUtf8(part);
        }
        pos = end + 1;
    }
    // a name that already ends in the extension keeps it once
    std::string file = fileName;
    if (!lowered(file).ends_with(Preset::kExtension))
        file += Preset::kExtension;
    return path / pathFromUtf8(file);
}

bool deletePreset(const fs::path& dir, const std::string& path)
{
    const fs::path file = pathFromUtf8(path);
    std::error_code ec;
    if (lowered(pathToUtf8(file.extension())) != Preset::kExtension || !isInsideDir(dir, file)
        || !fs::is_regular_file(file, ec))
        return false;
    return fs::remove(file, ec) && !ec;
}

bool isInsideDir(const fs::path& dir, const fs::path& path)
{
    std::error_code ec;
    const fs::path relative = fs::weakly_canonical(path, ec).lexically_relative(fs::weakly_canonical(dir, ec));
    return !ec && !relative.empty() && *relative.begin() != ".." && relative != ".";
}

} // namespace xythos
