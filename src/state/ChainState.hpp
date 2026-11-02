// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#pragma once

#include "engine/Params.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>
#include <type_traits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xythos {

// A JSON integer as T. A float that fits is truncated as get<T>() does; one that doesn't (3e31 as an int) throws
// json::type_error instead of being undefined behaviour, as get<T>() would be.
template <typename T>
T jsonInt(const nlohmann::json& value)
{
    static_assert(std::is_integral_v<T>);
    if (value.is_number_float())
    {
        const double v = value.get<double>();
        const double limit = std::ldexp(1.0, std::numeric_limits<T>::digits); // T's max + 1, exactly
        const bool fits = std::is_signed_v<T> ? v >= -limit && v < limit : v > -1.0 && v < limit;
        if (!fits)
            throw nlohmann::json::type_error::create(302, "number out of range", &value);
    }
    return value.get<T>();
}

// The same for an object's member, `fallback` if it has none (json::value() for integers)
template <typename T>
T jsonInt(const nlohmann::json& object, const char* key, T fallback)
{
    const auto it = object.find(key);
    return it == object.end() ? fallback : jsonInt<T>(*it);
}

// A JSON number as a float. Any number reads, clamped to the float range (get<float>() turns 1e39 into infinity,
// which saves as null and then doesn't load); a non-number throws json::type_error as get<float>() does.
inline float jsonFloat(const nlohmann::json& value)
{
    const double v = value.get<double>();
    if (std::isnan(v))
        return 0.0f;
    return static_cast<float>(std::clamp(v, -static_cast<double>(FLT_MAX), static_cast<double>(FLT_MAX)));
}

class BlockRegistry;

enum Branch : int {
    kBranchA = 0,
    kBranchB = 1,
};

struct BlockState {
    std::string typeId;
    int slot = -1;                                // fixed host parameter slot, 0..kMaxBlocks-1
    nlohmann::json config = nlohmann::json::object(); // non-automatable settings, e.g. a file path
    std::array<float, kParamsPerSlot> params {};   // normalized 0..1 values, for presets and undo
    bool bypass = false;
    int branch = kBranchA;                         // which branch, for blocks inside the split; 0 elsewhere
    std::string image {};                          // the user's own image for it (UserImages.hpp), "" for the stock one
    uint32_t latched = 0;                          // latched footswitches (Block::latched()): bit i = parameter i

    bool operator==(const BlockState&) const = default;
};

// A MIDI CC, or a Program Change, mapped to a host parameter (MIDI learn). A CC sets the parameter from its value;
// a Program Change (a footswitch that only sends those; owner, 2026-11-02) flips a switch (a block's bypass, the
// tuner, ...) or presses a footswitch control (the looper's Rec/Dub) each time it comes.
struct MidiMapping {
    int channel = 0; // 0..15
    int cc = 0;      // 0..127: the controller, or the program for a Program Change
    uint32_t param = 0; // host parameter index (Params.hpp)
    bool program = false; // a Program Change, not a CC

    bool operator==(const MidiMapping&) const = default;
};

// The one split the chain may have: blocks [begin, end) run in two parallel branches (each block's `branch` says
// which) that merge back into one. Blocks before `begin` run before the split, blocks from `end` on after the
// merge. A branch may be empty (it then carries the signal unchanged). The merge controls are host parameters.
struct SplitState {
    int begin = 0;
    int end = 0;

    bool operator==(const SplitState&) const = default;
};

// Parts of the chain, left to right. Without a split, the whole chain is Pre.
enum class Section {
    Pre,
    A,
    B,
    Post,
};

// The whole signal chain as plain data. Shared by the DSP (plugin state), the UI, presets and undo.
// The DSP takes parameter values from the host, not from `params`/`bypass`; the UI pushes those to the host.
struct ChainState {
    static constexpr int kVersion = 2;

    std::vector<BlockState> blocks; // left to right; inside the split, each branch in order
    std::optional<SplitState> split;
    std::vector<MidiMapping> midi;  // at most one mapping per CC and channel
    std::string preset;             // file of the preset this came from (UTF-8), empty if none
    // Set when the chain is a preset being loaded: blocks the DSP keeps take its `latched` too, not only new ones.
    // Never saved: the UI sets it on the copy it sends.
    bool restoreLatches = false;

    std::string toJson() const;
    static std::optional<ChainState> fromJson(std::string_view json, std::string* error = nullptr);

    // Checks block count, slot range and uniqueness, that every type exists, and the split
    bool validate(const BlockRegistry& registry, std::string* error = nullptr) const;

    // Lowest slot not used by any block, or -1 if the chain is full
    int freeSlot() const;

    // Maps a CC to a parameter, replacing any mapping of that CC or to that parameter
    void setMidiMapping(MidiMapping mapping);
    void clearMidiMappings(uint32_t param);
    const MidiMapping* midiMappingFor(uint32_t param) const;
    // What a learned Program Change does to `param`: a block's bypass and its switches flip, its footswitch
    // controls (Momentary) are pressed, its knobs and choices take none
    ProgramAction programAction(uint32_t param, const BlockRegistry& registry) const;

    // Editing helpers. Positions are within a section; they keep the split's range and `branch` tags consistent.
    Section sectionOf(int index) const;
    std::vector<int> sectionIndices(Section section) const; // indices into `blocks`, in order
    void insert(Section section, int position, BlockState block);
    BlockState remove(int index);
    void move(int index, Section section, int position);

    // Adds a split over blocks [begin, end), all in branch A. Does nothing if there's already one.
    void addSplit(int begin, int end);
    // Removes the split; the branches become serial, A then B
    void removeSplit();

    bool operator==(const ChainState&) const = default;
};

} // namespace xythos
