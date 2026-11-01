// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/LooperBlock.hpp"

#include <memory>
#include <vector>

namespace xythos {

namespace {

const char* const kModeChoices[] = { "Count In", "Free" };
const char* const kBeatChoices[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12" };
const char* const kCountInChoices[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8",
                                        "9", "10", "11", "12", "13", "14", "15", "16" };
const char* const kMeasureChoices[] = { "1", "2", "3", "4", "5", "6", "7", "8",
                                        "9", "10", "11", "12", "13", "14", "15", "16" };
const char* const kLoopChoices[] = { "1", "2", "3", "4", "5", "6", "7", "8" };

// The old Looper block: a Looper in the chain. Retired (owner, 2026-11-01: the looper is global now); kept so chains
// saved with one still build, until the UI moves its settings to the global looper and takes it out.
class LooperBlock : public Block {
public:
    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        // the block has its loop's memory from the start, as it always did: one lane, a minute long (nothing
        // services it later, so its overdubs all go into that lane, as they always did)
        fMemory.service();
        fLooper.attach(&fMemory);
        fLooper.prepare(sampleRate);
    }

    bool configure(const nlohmann::json& config) override
    {
        fLooper.setHasCountIn(config.is_object() && config.value(kLooperCountInKey, false));
        return true;
    }

    void setParam(int index, float value) override { fLooper.setParam(index, value); }
    void setClickOutput(float* click) override { fClick = click; }
    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        fLooper.process(io, numChannels, frames, fClick);
    }
    float meter() const override { return fLooper.meter(); }
    void reset() override { fLooper.reset(); }

private:
    LooperMemory fMemory;
    Looper fLooper;
    float* fClick = nullptr;
};

} // namespace

const BlockDescriptor kLooperBlockDescriptor = {
    "looper",
    "Looper",
    Category::Delay,
    kLooperParamCount,
    {
        ParamSpec { "Rec/Dub", "", 0.0f, 1.0f, 0.0f, ParamScale::Momentary },
        ParamSpec { "Play/Stop", "", 0.0f, 1.0f, 0.0f, ParamScale::Momentary },
        ParamSpec { "Clear", "", 0.0f, 1.0f, 0.0f, ParamScale::Momentary },
        ParamSpec { "Level", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Feedback", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Reverse", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Half Speed", "", 0.0f, 1.0f, 0.0f, ParamScale::Toggle },
        ParamSpec { "Mode", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kModeChoices },
        ParamSpec { "BPM", "BPM", 40.0f, 240.0f, 120.0f, ParamScale::Linear },
        ParamSpec { "Beats", "", 1.0f, 12.0f, 4.0f, ParamScale::Choice, kBeatChoices },
        ParamSpec { "Count-In", "", 0.0f, 16.0f, 8.0f, ParamScale::Choice, kCountInChoices },
        ParamSpec { "Measures", "", 1.0f, 16.0f, 4.0f, ParamScale::Choice, kMeasureChoices },
        ParamSpec { "Loops", "", 1.0f, 8.0f, 1.0f, ParamScale::Choice, kLoopChoices },
        ParamSpec { "Click", "%", 0.0f, 200.0f, 150.0f, ParamScale::Linear },
        ParamSpec { "Tap", "", 0.0f, 1.0f, 0.0f, ParamScale::Momentary },
        ParamSpec { "Latency", "ms", 0.0f, 200.0f, 10.0f, ParamScale::Linear },
    },
    [] { return std::unique_ptr<Block>(new LooperBlock()); },
    -1,
    -1,
    false,
    "Loop",
    0.0f,
    MeterStyle::Looper,
};

} // namespace xythos
