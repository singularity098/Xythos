// SPDX-License-Identifier: MIT
// Copyright (c) 2026 singularity098

#include "engine/BlockRegistry.hpp"

#include "engine/blocks/AmbientBlocks.hpp"
#include "engine/blocks/BarberPoleBlock.hpp"
#include "engine/blocks/BitcrusherBlock.hpp"
#include "engine/blocks/ChorusBlock.hpp"
#include "engine/blocks/CloudBlocks.hpp"
#include "engine/blocks/CompBlocks.hpp"
#include "engine/blocks/CompressorBlock.hpp"
#include "engine/blocks/DelayBlock.hpp"
#include "engine/blocks/DelayFxBlocks.hpp"
#include "engine/blocks/DiBlocks.hpp"
#include "engine/blocks/DoppelgangerBlock.hpp"
#include "engine/blocks/DoublerBlock.hpp"
#include "engine/blocks/DriveBlocks.hpp"
#include "engine/blocks/EchoBlocks.hpp"
#include "engine/blocks/EqBlocks.hpp"
#include "engine/blocks/FilterBlock.hpp"
#include "engine/blocks/FlangerBlock.hpp"
#include "engine/blocks/GateBlock.hpp"
#include "engine/blocks/InstrumentBlocks.hpp"
#include "engine/blocks/IrBlock.hpp"
#include "engine/blocks/LooperBlock.hpp"
#include "engine/blocks/MidiOutBlock.hpp"
#include "engine/blocks/ModSweeperBlock.hpp"
#include "engine/blocks/MoreSynthBlocks.hpp"
#include "engine/blocks/NamBlock.hpp"
#include "engine/blocks/OctaveBlock.hpp"
#include "engine/blocks/PhaserBlock.hpp"
#include "engine/blocks/PitchBlocks.hpp"
#include "engine/blocks/PlayedBlocks.hpp"
#include "engine/blocks/PolyChipBlock.hpp"
#include "engine/blocks/ResonatorBlocks.hpp"
#include "engine/blocks/ReverbBlock.hpp"
#include "engine/blocks/RhythmBlocks.hpp"
#include "engine/blocks/RingModBlock.hpp"
#include "engine/blocks/RotaryBlock.hpp"
#include "engine/blocks/SpringReverbBlock.hpp"
#include "engine/blocks/SynthBlock.hpp"
#include "engine/blocks/SynthBlocks.hpp"
#include "engine/blocks/TalkBoxBlock.hpp"
#include "engine/blocks/TextureBlocks.hpp"
#include "engine/blocks/TimeFxBlocks.hpp"
#include "engine/blocks/TremoloBlock.hpp"
#include "engine/blocks/VolumeBlocks.hpp"
#include "engine/blocks/WahBlocks.hpp"
#include "engine/blocks/WarbleBlock.hpp"

namespace xythos {

const char* categoryName(Category category)
{
    switch (category)
    {
    case Category::Nam:             return "NAM";
    case Category::Ir:              return "IR";
    case Category::Di:              return "DI";
    case Category::Distortion:      return "Distortion";
    case Category::Dynamics:        return "Dynamics";
    case Category::Eq:              return "EQ";
    case Category::Modulation:      return "Modulation";
    case Category::Reverb:          return "Reverb";
    case Category::Delay:           return "Delay";
    case Category::Pitch:           return "Pitch";
    case Category::Synth:           return "Synth";
    case Category::Filter:          return "Filter";
    case Category::Wah:             return "Wah";
    }
    return "";
}

BlockRegistry::BlockRegistry(std::vector<const BlockDescriptor*> types)
    : fTypes(std::move(types))
{
}

const BlockRegistry& BlockRegistry::builtin()
{
    // One line per block type, in Add Block dialog order within each category
    static const BlockRegistry registry({
        &kNamBlockDescriptor,
        &kIrBlockDescriptor,
        &kStudioDiBlockDescriptor,
        &kValveDiBlockDescriptor,
        &kOptoDiBlockDescriptor,
        &kHiFiPreampBlockDescriptor,
        &kTubePreampBlockDescriptor,
        &kBritishPreampBlockDescriptor,
        &kDriverDiBlockDescriptor,
        &kDualDriveBlockDescriptor,
        &kModernDriveBlockDescriptor,
        &kCrossoverDriveBlockDescriptor,
        &kBoostBlockDescriptor,
        &kOverdriveBlockDescriptor,
        &kDistortionBlockDescriptor,
        &kFuzzBlockDescriptor,
        &kGatedFuzzBlockDescriptor,
        &kEnvDriveBlockDescriptor,
        &kBitcrusherBlockDescriptor,
        &kJawariBlockDescriptor,
        &kCompressorBlockDescriptor,
        &kDiodeBridgeBlockDescriptor,
        &kConsoleCompBlockDescriptor,
        &kOptoLevelerBlockDescriptor,
        &kOptoStompBlockDescriptor,
        &kFetLimiterBlockDescriptor,
        &kSquashCompBlockDescriptor,
        &kGateBlockDescriptor,
        &kVolumeBlockDescriptor,
        &kSwellBlockDescriptor,
        &kKillSwitchBlockDescriptor,
        &kGraphicEqBlockDescriptor,
        &kParametricEqBlockDescriptor,
        &kAcousticBlockDescriptor,
        &kChorusBlockDescriptor,
        &kFlangerBlockDescriptor,
        &kPhaserBlockDescriptor,
        &kBarberPoleBlockDescriptor,
        &kModSweeperBlockDescriptor,
        &kTremoloBlockDescriptor,
        &kRotaryBlockDescriptor,
        &kDoublerBlockDescriptor,
        &kRingModBlockDescriptor,
        &kWarbleBlockDescriptor,
        &kSlicerBlockDescriptor,
        &kSwarmBlockDescriptor,
        &kChaosModBlockDescriptor,
        &kReverbBlockDescriptor,
        &kSpringReverbBlockDescriptor,
        &kShimmerBlockDescriptor,
        &kFreezeBlockDescriptor,
        &kBloomReverbBlockDescriptor,
        &kGatedReverbBlockDescriptor,
        &kSympatheticBlockDescriptor,
        &kDelayBlockDescriptor,
        &kDoppelgangerBlockDescriptor,
        &kTapeEchoBlockDescriptor,
        &kDuckingDelayBlockDescriptor,
        &kReverseDelayBlockDescriptor,
        &kPitchDelayBlockDescriptor,
        &kBeatRepeatBlockDescriptor,
        &kGrainCloudBlockDescriptor,
        &kTapeStopBlockDescriptor,
        &kBounceDelayBlockDescriptor,
        &kPatternDelayBlockDescriptor,
        &kLooperBlockDescriptor,
        &kOctaveBlockDescriptor,
        &kPolyOctaveBlockDescriptor,
        &kPitchShifterBlockDescriptor,
        &kWhammyBlockDescriptor,
        &kHarmonizerBlockDescriptor,
        &kStrumBlockDescriptor,
        &kHardTuneBlockDescriptor,
        &kDetuneBlockDescriptor,
        &kCapoBlockDescriptor,
        &kSustainerBlockDescriptor,
        &kArpeggiatorBlockDescriptor,
        &kOrganBlockDescriptor,
        &kSynthBlockDescriptor,
        &kSuperSawBlockDescriptor,
        &kFmSynthBlockDescriptor,
        &kFoldSynthBlockDescriptor,
        &kAcidSynthBlockDescriptor,
        &kStringSynthBlockDescriptor,
        &kChipSynthBlockDescriptor,
        &kPolyChipBlockDescriptor,
        &kThereminBlockDescriptor,
        &kSyncLeadBlockDescriptor,
        &kPdSynthBlockDescriptor,
        &kWavetableBlockDescriptor,
        &kSubBassBlockDescriptor,
        &kLaserBlockDescriptor,
        &kDroneBlockDescriptor,
        &kHarmonicPainterBlockDescriptor,
        &kTrumpetBlockDescriptor,
        &kViolinBlockDescriptor,
        &kPianoBlockDescriptor,
        &kDrumTriggerBlockDescriptor,
        &kFartBlockDescriptor,
        &kMidiOutBlockDescriptor,
        &kFilterBlockDescriptor,
        &kRobotFilterBlockDescriptor,
        &kWahBlockDescriptor,
        &kAutoWahBlockDescriptor,
        &kTalkBoxBlockDescriptor,
    });
    return registry;
}

const BlockDescriptor* BlockRegistry::find(std::string_view typeId) const
{
    for (const BlockDescriptor* type : fTypes)
    {
        if (typeId == type->typeId)
            return type;
    }
    return nullptr;
}

} // namespace xythos
