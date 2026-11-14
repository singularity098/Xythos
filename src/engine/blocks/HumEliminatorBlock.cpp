// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/blocks/HumEliminatorBlock.hpp"

#include "engine/dsp/HumCanceller.hpp"

#include <algorithm>

namespace xythos {

namespace {

constexpr const char* kMainsNames[] = { "60 Hz", "50 Hz" };

class HumEliminatorBlock : public Block {
public:
    void prepare(double sampleRate, uint32_t /*maxFrames*/) override
    {
        for (HumCanceller& canceller : fCancellers)
        {
            canceller.prepare(sampleRate);
            apply(canceller);
        }
    }

    void setParam(int index, float value) override
    {
        switch (index)
        {
        case kHumParamAmount:
            fAmount = std::clamp(value / 100.0f, 0.0f, 1.0f);
            break;
        case kHumParamAdapt:
            fAdapt = value;
            break;
        case kHumParamMains:
            fMains = value >= 0.5f ? 50.0 : 60.0;
            break;
        default:
            return;
        }
        for (HumCanceller& canceller : fCancellers)
            apply(canceller);
    }

    void process(float* const* io, uint32_t numChannels, uint32_t frames) override
    {
        // each channel has its own hum (a stereo block before this one may have changed it)
        for (uint32_t ch = 0; ch < numChannels && ch < 2; ++ch)
        {
            HumCanceller& canceller = fCancellers[ch];
            float* x = io[ch];
            for (uint32_t i = 0; i < frames; ++i)
                x[i] = canceller.process(x[i]);
        }
    }

    void reset() override
    {
        for (HumCanceller& canceller : fCancellers)
            canceller.reset();
    }

private:
    void apply(HumCanceller& canceller) const
    {
        canceller.setMains(fMains);
        canceller.setLearnTime(fAdapt);
        canceller.setAmount(fAmount);
    }

    HumCanceller fCancellers[2];
    float fAmount = 1.0f;
    double fAdapt = 0.5;
    double fMains = 60.0;
};

} // namespace

const BlockDescriptor kHumEliminatorBlockDescriptor = {
    "humeliminator",
    "Hum Eliminator",
    Category::Dynamics,
    kHumParamCount,
    {
        ParamSpec { "Amount", "%", 0.0f, 100.0f, 100.0f, ParamScale::Linear },
        ParamSpec { "Adapt", "s", 0.1f, 3.0f, 0.5f, ParamScale::Log },
        ParamSpec { "Mains", "", 0.0f, 1.0f, 0.0f, ParamScale::Choice, kMainsNames },
    },
    [] { return std::unique_ptr<Block>(new HumEliminatorBlock()); },
};

} // namespace xythos
