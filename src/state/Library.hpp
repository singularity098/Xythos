// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xythos {

// The NAM and IR library: scanning folders (also inside .zip files), content hashes.
//
// A file inside a zip is addressed as "<archive path>!/<path inside the archive>", e.g.
// "/home/me/NAM/Pack.zip!/Amps/Clean.nam". Blocks can use such paths directly: they're extracted to the cache
// directory when loaded.

enum class LibraryKind { Nam, Ir };

// What a NAM model captures, from its metadata's gear_type: NAM's trainer writes amp, pedal, pedal_amp, amp_cab,
// amp_pedal_cab, preamp and studio; TONE3000 amp, amp-cab, pedal, outboard, cab, space and experimental (and, in older
// files, full-rig and ir). Unknown when the file doesn't say.
enum class NamGear { Unknown, AmpCab, Amp, Cab, Pedal, Outboard, Space, Experimental };

NamGear namGear(std::string_view gearType);
// "Amp + Cab", "Amp Head", "Cabinet", "Pedal", "Outboard", "Spaces", "Experimental"; "" for Unknown
const char* namGearName(NamGear gear);

// What a .nam file says about itself
struct NamFileInfo {
    std::string info;     // "make model", or the model's name
    std::string gearType; // metadata gear_type as written (namGear() reads it)
    std::string arch;     // "A1 Standard" (Lite, Feather, Nano; "A1" for other sizes), "A2", "LSTM", ...; "" if unreadable
};

NamFileInfo describeNamFile(std::string_view bytes);

constexpr std::string_view kArchiveSeparator = "!/";

struct ArchivePath {
    std::string archive; // the .zip file
    std::string entry;   // path inside it, '/'-separated
};

// Splits "a.zip!/b/c.nam"; nullopt for a plain path
std::optional<ArchivePath> splitArchivePath(const std::string& path);

struct LibraryEntry {
    std::string path;   // absolute (UTF-8); see the zip form above
    std::string name;   // file name without extension
    std::string folder; // folder relative to the library root ('/'-separated, a zip counts as a folder), "" at the top
    std::string hash;   // content hash: XXH3-128 of the file's bytes, 32 hex digits
    std::string info;   // short description from the file's metadata, e.g. "Fender Deluxe"
    std::string gearType; // NAM: metadata gear_type, as written (namGear())
    std::string arch;     // NAM: the architecture, e.g. "A1 Standard" or "A2" (describeNamFile())
    uint64_t size = 0;  // bytes
    int64_t mtime = 0;  // modification time (of the archive, for zip entries)

    bool operator==(const LibraryEntry&) const = default;
};

// XXH3-128 of `bytes`, as 32 lowercase hex digits
std::string hashBytes(std::string_view bytes);

// Reads a whole library file, also from inside a zip
std::optional<std::string> readLibraryFile(const std::string& path);

// The content hash of a library file
std::optional<std::string> hashLibraryFile(const std::string& path);

// A path that can be opened directly: a plain path is returned as is; a file inside a zip is extracted to the
// cache directory first (and reused while it's there). Non-RT; may take a while for large files.
std::optional<std::string> materializeLibraryFile(const std::string& path);

// Cache of hashes and metadata from the last scan, so a rescan only reads new or changed files.
// One index per library kind, stored as JSON in the cache directory.
class LibraryIndex {
public:
    static std::filesystem::path defaultPath(LibraryKind kind);

    void load(const std::filesystem::path& path);
    bool save(const std::filesystem::path& path) const;

    // The cached entry for `path`, if its size and modification time still match
    const LibraryEntry* find(const std::string& path, uint64_t size, int64_t mtime) const;
    // A file with this content hash from the last scan, if any (it may have gone since)
    const LibraryEntry* findByHash(const std::string& hash) const;
    void put(const LibraryEntry& entry) { fEntries[entry.path] = entry; }
    void clear() { fEntries.clear(); }
    size_t size() const { return fEntries.size(); }

private:
    std::unordered_map<std::string, LibraryEntry> fEntries;
};

// Scans `root` recursively, also inside .zip files, for one kind of file (.nam or .wav). Unchanged files take
// their hash and metadata from `index`, which is then replaced by exactly the files found. Sorted by folder, then
// name. Stops early (returning what it has) when `cancel` becomes true.
std::vector<LibraryEntry> scanLibrary(const std::filesystem::path& root, LibraryKind kind, LibraryIndex& index,
                                      const std::atomic<bool>* cancel = nullptr);

} // namespace xythos
