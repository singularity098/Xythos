// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "state/ChainState.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xythos {

// A preset: the chain (blocks, values, MIDI map) plus the global controls that belong to a sound.
//
// File (.xythos): a zip with preset.json and, when saved with "embed files", every file the chain uses
// (NAM models and IRs) under files/<content hash>/<file name>, and the user's own block images it uses under
// images/<amps|cabs|pedals>/<id>, so it can be shared.
struct Preset {
    static constexpr int kVersion = 1;
    static constexpr const char* kExtension = ".xythos";

    ChainState chain;
    std::map<std::string, float> globals; // by parameter symbol (see presetGlobalParams)

    std::string toJson() const;
    static std::optional<Preset> fromJson(std::string_view json, std::string* error = nullptr);
};

// The global parameters a preset stores: input mode, input, bass, middle, treble, output, limiter and the
// split's merge controls
const std::vector<uint32_t>& presetGlobalParams();

bool savePresetFile(const std::string& path, const Preset& preset, bool embedFiles, std::string* error = nullptr);

// How a block's file was found when loading a preset
enum class FileSource { Original, Embedded, Library, Missing };

struct LoadedPreset {
    Preset preset;
    std::vector<FileSource> sources; // one per block (Original for blocks without a file)
};

// Loads a preset file and resolves each block's file: the original path if it still exists, else the embedded
// copy (extracted to the cache directory), else a library file with the same content hash (`findByHash` returns
// its path or ""), else the block keeps its path and is reported Missing. Embedded block images are added to the
// user's image libraries (UserImages.hpp), and the blocks take their ids there.
std::optional<LoadedPreset> loadPresetFile(const std::string& path,
                                           const std::function<std::string(const std::string& hash)>& findByHash,
                                           std::string* error = nullptr);

// Only a preset file's preset.json, nothing extracted or resolved (the global values of a preset the DSP loaded)
std::optional<Preset> readPresetFile(const std::string& path, std::string* error = nullptr);

// The user's preset folder, presets/ in configDir(). Save keeps presets here, in subfolders if wanted, without the
// files they use; Export writes a copy anywhere, with the files embedded so it can be shared.
std::filesystem::path presetsDir();

struct PresetEntry {
    std::string path;   // UTF-8
    std::string folder; // relative to the preset folder, '/'-separated; "" at the top
    std::string name;   // file name without the extension
};

// Every preset under `dir`, sorted by folder and then name (ignoring case)
std::vector<PresetEntry> listPresets(const std::filesystem::path& dir);

// Every subfolder of `dir`, relative and '/'-separated, sorted (ignoring case)
std::vector<std::string> listPresetFolders(const std::filesystem::path& dir);

// The file for a preset called `name` in `folder` ('/'-separated, relative to `dir`; "" for the top). Spaces around
// the name and each folder are trimmed. Nullopt for an empty name, or a name or folder that can't be a file name on
// Linux and Windows ("..", or one of < > : " / \ | ? *).
std::optional<std::filesystem::path> presetPath(const std::filesystem::path& dir, const std::string& folder,
                                                const std::string& name);

// Deletes the preset file `path` (UTF-8), which must be a preset somewhere inside `dir` (Manage Presets). False,
// with nothing deleted, for anything else or when the file can't be removed.
bool deletePreset(const std::filesystem::path& dir, const std::string& path);

// Whether `path` is somewhere inside `dir`
bool isInsideDir(const std::filesystem::path& dir, const std::filesystem::path& path);

} // namespace xythos
