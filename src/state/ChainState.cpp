// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "state/ChainState.hpp"

#include "engine/BlockRegistry.hpp"

#include <algorithm>

namespace xythos {

using nlohmann::json;

std::string ChainState::toJson() const
{
    json blocksJson = json::array();
    for (const BlockState& block : blocks)
    {
        blocksJson.push_back({
            { "type", block.typeId },
            { "slot", block.slot },
            { "config", block.config },
            { "params", block.params },
            { "bypass", block.bypass },
        });
        if (block.branch != kBranchA)
            blocksJson.back()["branch"] = block.branch;
        if (!block.image.empty())
            blocksJson.back()["image"] = block.image;
        if (block.latched != 0)
            blocksJson.back()["latched"] = block.latched;
    }
    json midiJson = json::array();
    for (const MidiMapping& mapping : midi)
        midiJson.push_back({ { "channel", mapping.channel }, { "cc", mapping.cc }, { "param", mapping.param } });
    json root { { "version", kVersion }, { "blocks", blocksJson } };
    if (!midi.empty())
        root["midi"] = midiJson;
    if (!preset.empty())
        root["preset"] = preset;
    if (restoreLatches)
        root["restoreLatches"] = true;
    if (split)
        root["split"] = { { "begin", split->begin }, { "end", split->end } };
    return root.dump();
}

std::optional<ChainState> ChainState::fromJson(std::string_view text, std::string* error)
{
    // An empty state means an empty chain (e.g. a fresh plugin instance)
    if (text.empty())
        return ChainState {};

    try
    {
        const json root = json::parse(text);
        const int version = root.value("version", 0);
        if (version < 1 || version > kVersion) // version 1 had no split
        {
            if (error)
                *error = "unsupported chain version " + std::to_string(version);
            return std::nullopt;
        }

        ChainState state;
        for (const json& item : root.at("blocks"))
        {
            BlockState block;
            block.typeId = item.at("type").get<std::string>();
            block.slot = item.at("slot").get<int>();
            block.config = item.value("config", json::object());
            block.bypass = item.value("bypass", false);
            block.branch = item.value("branch", 0);
            block.image = item.value("image", std::string());
            block.latched = item.value("latched", 0u);
            if (const auto params = item.find("params"); params != item.end())
            {
                for (size_t i = 0; i < params->size() && i < block.params.size(); ++i)
                    block.params[i] = jsonFloat((*params)[i]);
            }
            state.blocks.push_back(std::move(block));
        }
        if (const auto midi = root.find("midi"); midi != root.end())
        {
            for (const json& item : *midi)
            {
                state.midi.push_back({ item.at("channel").get<int>(), item.at("cc").get<int>(),
                                       item.at("param").get<uint32_t>() });
            }
        }
        state.preset = root.value("preset", std::string());
        state.restoreLatches = root.value("restoreLatches", false);
        if (const auto split = root.find("split"); split != root.end())
            state.split = SplitState { split->at("begin").get<int>(), split->at("end").get<int>() };
        return state;
    }
    catch (const json::exception& e)
    {
        if (error)
            *error = e.what();
        return std::nullopt;
    }
}

bool ChainState::validate(const BlockRegistry& registry, std::string* error) const
{
    auto fail = [error](std::string message) {
        if (error)
            *error = std::move(message);
        return false;
    };

    if (blocks.size() > static_cast<size_t>(kMaxBlocks))
        return fail("too many blocks");

    std::array<bool, kMaxBlocks> used {};
    for (const BlockState& block : blocks)
    {
        if (block.slot < 0 || block.slot >= kMaxBlocks)
            return fail("slot out of range: " + std::to_string(block.slot));
        if (used[block.slot])
            return fail("slot used twice: " + std::to_string(block.slot));
        used[block.slot] = true;
        if (registry.find(block.typeId) == nullptr)
            return fail("unknown block type: " + block.typeId);
        if (block.branch != kBranchA && block.branch != kBranchB)
            return fail("bad branch: " + std::to_string(block.branch));
    }

    const int count = static_cast<int>(blocks.size());
    if (split && (split->begin < 0 || split->begin > split->end || split->end > count))
        return fail("bad split range");
    for (int i = 0; i < count; ++i)
    {
        const bool inSplit = split && i >= split->begin && i < split->end;
        if (!inSplit && blocks[i].branch != kBranchA)
            return fail("branch set on a block outside the split");
    }
    for (const MidiMapping& mapping : midi)
    {
        if (mapping.channel < 0 || mapping.channel > 15 || mapping.cc < 0 || mapping.cc > 127)
            return fail("invalid MIDI mapping");
        if (mapping.param >= kParamCount || !isMidiMappable(mapping.param))
            return fail("MIDI mapping to a parameter that can't be mapped: " + std::to_string(mapping.param));
    }
    return true;
}

void ChainState::setMidiMapping(MidiMapping mapping)
{
    std::erase_if(midi, [&mapping](const MidiMapping& m) {
        return (m.channel == mapping.channel && m.cc == mapping.cc) || m.param == mapping.param;
    });
    midi.push_back(mapping);
}

void ChainState::clearMidiMappings(uint32_t param)
{
    std::erase_if(midi, [param](const MidiMapping& m) { return m.param == param; });
}

const MidiMapping* ChainState::midiMappingFor(uint32_t param) const
{
    for (const MidiMapping& mapping : midi)
    {
        if (mapping.param == param)
            return &mapping;
    }
    return nullptr;
}

int ChainState::freeSlot() const
{
    std::array<bool, kMaxBlocks> used {};
    for (const BlockState& block : blocks)
    {
        if (block.slot >= 0 && block.slot < kMaxBlocks)
            used[block.slot] = true;
    }
    for (int slot = 0; slot < kMaxBlocks; ++slot)
    {
        if (!used[slot])
            return slot;
    }
    return -1;
}

Section ChainState::sectionOf(int index) const
{
    if (!split || index < split->begin)
        return Section::Pre;
    if (index < split->end)
        return blocks[index].branch == kBranchB ? Section::B : Section::A;
    return Section::Post;
}

std::vector<int> ChainState::sectionIndices(Section section) const
{
    std::vector<int> indices;
    for (int i = 0; i < static_cast<int>(blocks.size()); ++i)
    {
        if (sectionOf(i) == section)
            indices.push_back(i);
    }
    return indices;
}

void ChainState::insert(Section section, int position, BlockState block)
{
    const int count = static_cast<int>(blocks.size());
    block.branch = kBranchA;
    if (!split)
    {
        blocks.insert(blocks.begin() + std::clamp(position, 0, count), std::move(block));
        return;
    }

    int index = 0;
    switch (section)
    {
    case Section::Pre:
        index = std::clamp(position, 0, split->begin);
        ++split->begin;
        break;
    case Section::A:
    case Section::B: {
        const std::vector<int> branch = sectionIndices(section);
        index = position >= 0 && position < static_cast<int>(branch.size()) ? branch[position] : split->end;
        block.branch = section == Section::B ? kBranchB : kBranchA;
        break;
    }
    case Section::Post:
        index = split->end + std::clamp(position, 0, count - split->end);
        break;
    }
    if (section != Section::Post)
        ++split->end;
    blocks.insert(blocks.begin() + index, std::move(block));
}

BlockState ChainState::remove(int index)
{
    BlockState block = std::move(blocks[index]);
    blocks.erase(blocks.begin() + index);
    if (split)
    {
        if (index < split->begin)
            --split->begin;
        if (index < split->end)
            --split->end;
    }
    block.branch = kBranchA;
    return block;
}

void ChainState::move(int index, Section section, int position)
{
    const Section from = sectionOf(index);
    const std::vector<int> fromIndices = sectionIndices(from);
    const int current = static_cast<int>(std::find(fromIndices.begin(), fromIndices.end(), index) - fromIndices.begin());
    BlockState block = remove(index);
    if (from == section && position > current)
        --position;
    insert(section, position, std::move(block));
}

void ChainState::addSplit(int begin, int end)
{
    if (split)
        return;
    const int count = static_cast<int>(blocks.size());
    begin = std::clamp(begin, 0, count);
    end = std::clamp(end, begin, count);
    split = SplitState { begin, end };
    for (BlockState& block : blocks)
        block.branch = kBranchA;
}

void ChainState::removeSplit()
{
    if (!split)
        return;
    std::stable_partition(blocks.begin() + split->begin, blocks.begin() + split->end,
                          [](const BlockState& block) { return block.branch == kBranchA; });
    for (BlockState& block : blocks)
        block.branch = kBranchA;
    split.reset();
}

} // namespace xythos
