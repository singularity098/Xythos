cmake_minimum_required(VERSION 3.24)

# macOS: a universal binary (Apple Silicon and Intel) by default, for macOS 11 and later (C++20's atomic wait)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures to build for")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS to run on")
endif()

project(Xythos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# DPF uses GNU extensions (typeof) in its UI code
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Linux and Windows: x86-64 only. macOS: arm64 and x86-64 (see docs/Requirements.md).
if(APPLE)
  foreach(arch ${CMAKE_OSX_ARCHITECTURES})
    if(NOT arch MATCHES "^(arm64|x86_64)$")
      message(FATAL_ERROR "Xythos builds for arm64 and x86_64 on macOS (found ${arch})")
    endif()
  endforeach()
elseif(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
  message(FATAL_ERROR "Xythos only supports x86-64 (found ${CMAKE_SYSTEM_PROCESSOR})")
endif()

# AVX2/FMA is a hard requirement on x86-64 (see docs/Requirements.md). No fast-math on any platform: the engine
# relies on IEEE behaviour, and all platforms should compute the same results.
if(MSVC)
  add_compile_options(/arch:AVX2 /utf-8)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
elseif(APPLE)
  # only for the Intel half of a universal binary (SHELL: keeps CMake from merging the repeated -Xarch_x86_64)
  add_compile_options("SHELL:-Xarch_x86_64 -mavx2" "SHELL:-Xarch_x86_64 -mfma")
  add_compile_definitions(GL_SILENCE_DEPRECATION)
else()
  add_compile_options(-mavx2 -mfma)
endif()

# ---------------------------------------------------------------------------------------------------------------------
# Dependencies

# Our patches to submodules (patches/<name>/, see the README there), applied to the submodule's checkout once, in
# name order, skipping ones already applied. --ignore-whitespace lets them apply to a Windows checkout with CRLF line
# endings. NAM Core's (bounds-checked weight loading) is a safety fix and applies everywhere; DPF's only change Linux
# code (guarded, so applying them on macOS is harmless) and are skipped on Windows, as before.
find_package(Git REQUIRED)
function(xythos_apply_patches submodule patch_dir)
  file(GLOB patches ${CMAKE_CURRENT_SOURCE_DIR}/patches/${patch_dir}/*.patch)
  list(SORT patches)
  foreach(patch ${patches})
    execute_process(COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace --reverse --check ${patch}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/${submodule}
                    RESULT_VARIABLE applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT applied EQUAL 0)
      execute_process(COMMAND ${GIT_EXECUTABLE} apply --ignore-whitespace ${patch}
                      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/deps/${submodule}
                      RESULT_VARIABLE failed)
      if(NOT failed EQUAL 0)
        message(FATAL_ERROR "Can't apply ${patch} to deps/${submodule} (was the submodule updated?)")
      endif()
      message(STATUS "Applied ${patch}")
    endif()
  endforeach()
endfunction()
if(NOT WIN32)
  xythos_apply_patches(DPF dpf)
endif()
xythos_apply_patches(NeuralAmpModelerCore nam)

add_subdirectory(deps/DPF)

# NeuralAmpModelerCore is built as a static library from its sources; its own CMake also builds tools we don't need
set(NAM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/NeuralAmpModelerCore)
file(GLOB_RECURSE NAM_SOURCES CONFIGURE_DEPENDS ${NAM_DIR}/NAM/*.cpp)
add_library(nam STATIC ${NAM_SOURCES})
target_include_directories(nam PUBLIC ${NAM_DIR})
target_include_directories(nam SYSTEM PUBLIC
  ${NAM_DIR}/Dependencies             # <nlohmann/json.hpp>
  ${NAM_DIR}/Dependencies/eigen
  ${NAM_DIR}/Dependencies/nlohmann)   # NAM includes "json.hpp"
target_compile_definitions(nam PUBLIC NAM_ENABLE_A2_FAST NAM_SAMPLE_FLOAT)

# FFTConvolver: zero-latency partitioned convolution for the IR block
set(FFTCONVOLVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/FFTConvolver)
add_library(fftconvolver STATIC
  ${FFTCONVOLVER_DIR}/AudioFFT.cpp
  ${FFTCONVOLVER_DIR}/FFTConvolver.cpp
  ${FFTCONVOLVER_DIR}/TwoStageFFTConvolver.cpp
  ${FFTCONVOLVER_DIR}/Utilities.cpp)
target_include_directories(fftconvolver SYSTEM PUBLIC ${FFTCONVOLVER_DIR})

# miniz: reading zip files in the library. Its CMake generates miniz_export.h; we only need an empty export macro.
set(MINIZ_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/miniz)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/miniz/miniz_export.h "#pragma once\n#define MINIZ_EXPORT\n")
add_library(miniz STATIC
  ${MINIZ_DIR}/miniz.c
  ${MINIZ_DIR}/miniz_tdef.c
  ${MINIZ_DIR}/miniz_tinfl.c
  ${MINIZ_DIR}/miniz_zip.c)
target_include_directories(miniz SYSTEM PUBLIC ${MINIZ_DIR} ${CMAKE_CURRENT_BINARY_DIR}/miniz)
# 64-bit file offsets, so large archives work
target_compile_definitions(miniz PUBLIC MINIZ_NO_ZLIB_COMPATIBLE_NAMES _LARGEFILE64_SOURCE)

# xxHash: content hashes (XXH3-128) for favorites and missing-file matching; header-only
add_library(xxhash INTERFACE)
target_include_directories(xxhash SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/deps/xxHash)

# Signalsmith Stretch (+ Signalsmith Linear, its FFTs): spectral pitch shifting for the pitch blocks' HQ mode;
# header-only. Stretch is vendored with our pitch-accuracy changes (deps/signalsmith-stretch/README.md); Linear is a
# submodule. Their own CMake files fetch from the network, so they're not used. On macOS Linear uses Accelerate's FFT,
# as its CMake would.
add_library(signalsmith INTERFACE)
target_include_directories(signalsmith SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/deps/signalsmith-stretch
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/signalsmith-linear/include)
if(APPLE)
  target_compile_definitions(signalsmith INTERFACE SIGNALSMITH_USE_ACCELERATE ACCELERATE_NEW_LAPACK)
  target_link_libraries(signalsmith INTERFACE "-framework Accelerate")
endif()
if(MSVC)
  target_compile_options(signalsmith INTERFACE /bigobj)
endif()

# libwebp: decoding the built-in images (WebP; DPF's stb_image reads only JPEG and PNG). Only the decoder is built, from
# the lists in its src/*/Makefile.am. Without HAVE_CONFIG_H its SSE2/SSE4.1/AVX2/NEON code is chosen by the compiler's
# own macros (src/dsp/cpu.h), so every architecture of a macOS universal build gets its own; the other files compile empty.
set(WEBP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/libwebp)
set(WEBP_DSP_SOURCES alpha_processing cpu dec dec_clip_tables filters lossless rescaler upsampling yuv
  alpha_processing_sse2 dec_sse2 filters_sse2 lossless_sse2 rescaler_sse2 upsampling_sse2 yuv_sse2
  alpha_processing_sse41 dec_sse41 lossless_sse41 upsampling_sse41 yuv_sse41
  lossless_avx2
  alpha_processing_neon dec_neon filters_neon lossless_neon rescaler_neon upsampling_neon yuv_neon)
set(WEBP_UTILS_SOURCES bit_reader_utils color_cache_utils filters_utils huffman_utils palette quant_levels_dec_utils
  rescaler_utils random_utils thread_utils utils)
set(WEBP_DEC_SOURCES alpha_dec buffer_dec frame_dec idec_dec io_dec quant_dec tree_dec vp8_dec vp8l_dec webp_dec)
list(TRANSFORM WEBP_DSP_SOURCES PREPEND ${WEBP_DIR}/src/dsp/)
list(TRANSFORM WEBP_UTILS_SOURCES PREPEND ${WEBP_DIR}/src/utils/)
list(TRANSFORM WEBP_DEC_SOURCES PREPEND ${WEBP_DIR}/src/dec/)
set(WEBP_SOURCES ${WEBP_DSP_SOURCES} ${WEBP_UTILS_SOURCES} ${WEBP_DEC_SOURCES})
list(TRANSFORM WEBP_SOURCES APPEND .c)
add_library(webpdecoder STATIC ${WEBP_SOURCES})
target_include_directories(webpdecoder PRIVATE ${WEBP_DIR})
target_include_directories(webpdecoder SYSTEM PUBLIC ${WEBP_DIR}/src)

# ---------------------------------------------------------------------------------------------------------------------
# Engine (DSP and state, independent of DPF so it can be unit tested)

add_library(xythos-engine STATIC
  src/engine/BlockRegistry.cpp
  src/engine/PhaseCheck.cpp
  src/engine/blocks/AmbientBlocks.cpp
  src/engine/blocks/BarberPoleBlock.cpp
  src/engine/blocks/BitcrusherBlock.cpp
  src/engine/blocks/ChorusBlock.cpp
  src/engine/blocks/CloudBlocks.cpp
  src/engine/blocks/CompBlocks.cpp
  src/engine/blocks/CompressorBlock.cpp
  src/engine/blocks/DelayBlock.cpp
  src/engine/blocks/DelayFxBlocks.cpp
  src/engine/blocks/DoppelgangerBlock.cpp
  src/engine/blocks/DoublerBlock.cpp
  src/engine/blocks/DiBlocks.cpp
  src/engine/blocks/DriveBlocks.cpp
  src/engine/blocks/EchoBlocks.cpp
  src/engine/blocks/EqBlocks.cpp
  src/engine/blocks/FilterBlock.cpp
  src/engine/blocks/FlangerBlock.cpp
  src/engine/blocks/GateBlock.cpp
  src/engine/blocks/InstrumentBlocks.cpp
  src/engine/blocks/IrBlock.cpp
  src/engine/blocks/LooperBlock.cpp
  src/engine/blocks/MidiOutBlock.cpp
  src/engine/blocks/ModSweeperBlock.cpp
  src/engine/blocks/MoreSynthBlocks.cpp
  src/engine/blocks/NamBlock.cpp
  src/engine/blocks/OctaveBlock.cpp
  src/engine/blocks/PhaserBlock.cpp
  src/engine/blocks/PitchBlocks.cpp
  src/engine/blocks/PlayedBlocks.cpp
  src/engine/blocks/PolyChipBlock.cpp
  src/engine/blocks/ResonatorBlocks.cpp
  src/engine/blocks/ReverbBlock.cpp
  src/engine/blocks/RhythmBlocks.cpp
  src/engine/blocks/RingModBlock.cpp
  src/engine/blocks/RotaryBlock.cpp
  src/engine/blocks/SpringReverbBlock.cpp
  src/engine/blocks/SynthBlock.cpp
  src/engine/blocks/SynthBlocks.cpp
  src/engine/blocks/TalkBoxBlock.cpp
  src/engine/blocks/TextureBlocks.cpp
  src/engine/blocks/TimeFxBlocks.cpp
  src/engine/blocks/TremoloBlock.cpp
  src/engine/blocks/VolumeBlocks.cpp
  src/engine/blocks/WahBlocks.cpp
  src/engine/blocks/WarbleBlock.cpp
  src/engine/Engine.cpp
  src/engine/ParallelWorker.cpp
  src/engine/TempoSync.cpp
  src/engine/metronome/Metronome.cpp
  src/engine/tuner/PitchDetector.cpp
  src/engine/tuner/Tuner.cpp
  src/engine/Params.cpp
  src/state/ChainExchange.cpp
  src/state/ChainMigration.cpp
  src/state/ChainState.cpp
  src/state/Ink.cpp
  src/state/Library.cpp
  src/state/LibraryScanner.cpp
  src/state/AudioFile.cpp
  src/state/Paths.cpp
  src/state/Preset.cpp
  src/state/Settings.cpp
  src/state/UpdateCheck.cpp
  src/state/UserImages.cpp)
target_include_directories(xythos-engine PUBLIC src)
# The plugin's version (the update check compares it with the latest release)
target_compile_definitions(xythos-engine PUBLIC XYTHOS_VERSION="${PROJECT_VERSION}"
  XYTHOS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} XYTHOS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  XYTHOS_VERSION_PATCH=${PROJECT_VERSION_PATCH})
# Whole archive: NAM Core registers its model architectures from static initializers, which the linker would
# otherwise drop ("No config parser registered for architecture")
find_package(Threads REQUIRED)
target_link_libraries(xythos-engine PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam>" fftconvolver miniz xxhash signalsmith Threads::Threads)

set(XYTHOS_WARNINGS $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall -Wextra -Wpedantic>)
target_compile_options(xythos-engine PRIVATE ${XYTHOS_WARNINGS})

# ---------------------------------------------------------------------------------------------------------------------
# Plugin

# Audio Units only exist on macOS (Logic and GarageBand load nothing else)
set(XYTHOS_FORMATS lv2 vst3)
if(APPLE)
  list(APPEND XYTHOS_FORMATS au)
endif()

# The built-in block images: the WebPs in resources/blocks/ (made by tools/prepare_block_image.py), embedded as byte
# arrays at build time, so only the WebPs are committed
file(GLOB XYTHOS_BLOCK_IMAGES CONFIGURE_DEPENDS resources/blocks/*.webp resources/blocks/thumbs/*.webp)
set(XYTHOS_BLOCK_IMAGES_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/BlockImages.cpp)
add_custom_command(
  OUTPUT ${XYTHOS_BLOCK_IMAGES_CPP}
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/resources/blocks -DOUT=${XYTHOS_BLOCK_IMAGES_CPP}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  DEPENDS ${XYTHOS_BLOCK_IMAGES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  COMMENT "Embedding the block images"
  VERBATIM)
# The built-in speaker cabinet images for IR blocks: resources/cabs/ (made by tools/prepare_cab_images.py), the same way
file(GLOB XYTHOS_CAB_IMAGES CONFIGURE_DEPENDS resources/cabs/*.webp resources/cabs/thumbs/*.webp)
set(XYTHOS_CAB_IMAGES_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/CabImages.cpp)
add_custom_command(
  OUTPUT ${XYTHOS_CAB_IMAGES_CPP}
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/resources/cabs -DOUT=${XYTHOS_CAB_IMAGES_CPP} -DKIND=Cab
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  DEPENDS ${XYTHOS_CAB_IMAGES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  COMMENT "Embedding the cab images"
  VERBATIM)
# The built-in generic pedal images for NAM pedals and pedals awaiting their own photo: resources/pedals/ (made by
# tools/prepare_pedal_images.py), the same way
file(GLOB XYTHOS_PEDAL_IMAGES CONFIGURE_DEPENDS resources/pedals/*.webp resources/pedals/thumbs/*.webp)
set(XYTHOS_PEDAL_IMAGES_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/PedalImages.cpp)
add_custom_command(
  OUTPUT ${XYTHOS_PEDAL_IMAGES_CPP}
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/resources/pedals -DOUT=${XYTHOS_PEDAL_IMAGES_CPP}
          -DKIND=Pedal -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  DEPENDS ${XYTHOS_PEDAL_IMAGES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  COMMENT "Embedding the generic pedal images"
  VERBATIM)
# The built-in amp head images for NAM amp models: resources/amps/ (made by tools/prepare_amp_images.py), the same way
file(GLOB XYTHOS_AMP_IMAGES CONFIGURE_DEPENDS resources/amps/*.webp resources/amps/thumbs/*.webp)
set(XYTHOS_AMP_IMAGES_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/AmpImages.cpp)
add_custom_command(
  OUTPUT ${XYTHOS_AMP_IMAGES_CPP}
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/resources/amps -DOUT=${XYTHOS_AMP_IMAGES_CPP}
          -DKIND=Amp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  DEPENDS ${XYTHOS_AMP_IMAGES} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedBlockImages.cmake
  COMMENT "Embedding the amp head images"
  VERBATIM)

dpf_add_plugin(xythos
  TARGETS ${XYTHOS_FORMATS}
  USE_FILE_BROWSER
  FILES_DSP
    src/plugin/XythosPlugin.cpp
  FILES_UI
    src/ui/BackgroundImage.cpp
    src/ui/BlockGlyphs.cpp
    ${XYTHOS_BLOCK_IMAGES_CPP}
    ${XYTHOS_CAB_IMAGES_CPP}
    ${XYTHOS_PEDAL_IMAGES_CPP}
    ${XYTHOS_AMP_IMAGES_CPP}
    src/ui/FileLooks.cpp
    src/ui/FolderPicker.cpp
    src/ui/FontData.cpp
    src/ui/HttpGet.cpp
    src/ui/KnobImages.cpp
    src/ui/LedImages.cpp
    src/ui/LogoImage.cpp
    src/ui/OpenUrl.cpp
    src/ui/ParamHelp.cpp
    src/ui/PedalFaces.cpp
    src/ui/TextEdit.cpp
    src/ui/RasterImage.cpp
    src/ui/Vsync.cpp
    src/ui/WebpImage.cpp
    src/ui/XythosUI.cpp)

target_include_directories(xythos PUBLIC src src/plugin)

# MSVC has no glext.h; use our copy instead of the one DPF downloads at configure time
if(MSVC)
  target_include_directories(dgl-system-libs-definitions BEFORE INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/deps/khronos)
endif()
target_link_libraries(xythos PUBLIC xythos-engine webpdecoder)
# stb_image (deps/stb, header only) for PNG and JPEG, in RasterImage.cpp
target_include_directories(xythos SYSTEM PUBLIC deps/stb)
if(WIN32)
  # ShellExecute (Fretbored link); IFileOpenDialog (folder picker); WinHTTP (update check)
  target_link_libraries(xythos PUBLIC shell32 ole32 winhttp)
endif()

# ---------------------------------------------------------------------------------------------------------------------
# Tests

option(XYTHOS_BUILD_TESTS "Build unit tests" ON)
option(XYTHOS_FUZZ "Build the libFuzzer targets (clang; see fuzz/README.md)" OFF)
if(XYTHOS_BUILD_TESTS)
  enable_testing()
  add_executable(xythos-tests
    tests/TestMain.cpp
    tests/BlockGlyphTests.cpp
    tests/BlockSanityTests.cpp
    tests/ChainStateTests.cpp
    tests/CompPedalTests.cpp
    tests/CompressorBlockTests.cpp
    tests/DelayBlockTests.cpp
    tests/DiTests.cpp
    tests/DoppelgangerBlockTests.cpp
    tests/DriveTests.cpp
    tests/DspTests.cpp
    tests/EngineTests.cpp
    tests/FileLookTests.cpp
    tests/LooperTests.cpp
    tests/AudioFileTests.cpp
    tests/InkTests.cpp
    tests/IrBlockTests.cpp
    tests/LibraryTests.cpp
    tests/MetronomeTests.cpp
    tests/MidiOutTests.cpp
    tests/MorePedalTests.cpp
    tests/MoreSynthTests.cpp
    tests/NamBlockTests.cpp
    tests/ParamHelpTests.cpp
    tests/PedalFaceTests.cpp
    tests/PhaseCheckTests.cpp
    tests/PedalTests.cpp
    tests/PolyTrackingTests.cpp
    tests/PresetTests.cpp
    tests/RealTrackingTests.cpp
    tests/RasterImageTests.cpp
    tests/ReverbBlockTests.cpp
    tests/SettingsTests.cpp
    tests/SynthTests.cpp
    tests/TextEditTests.cpp
    tests/ThirdSetTests.cpp
    tests/TrackingTests.cpp
    tests/TunerTests.cpp
    tests/UndoStackTests.cpp
    tests/HqPitchTests.cpp
    tests/UpdateCheckTests.cpp
    tests/UserImageTests.cpp
    tests/WebpImageTests.cpp
    src/ui/BackgroundImage.cpp
    src/ui/BlockGlyphs.cpp
    src/ui/FileLooks.cpp
    src/ui/HttpGet.cpp
    src/ui/KnobImages.cpp
    src/ui/LedImages.cpp
    src/ui/LogoImage.cpp
    src/ui/ParamHelp.cpp
    src/ui/PedalFaces.cpp
    src/ui/RasterImage.cpp
    src/ui/TextEdit.cpp
    src/ui/WebpImage.cpp)
  if(WIN32)
    target_link_libraries(xythos-tests PRIVATE winhttp)
  endif()
  target_include_directories(xythos-tests PRIVATE tests deps/doctest src)
  target_include_directories(xythos-tests SYSTEM PRIVATE deps/stb)
  if(APPLE)
    # the tests run on the machine that builds them: only its own architecture
    set_target_properties(xythos-tests PROPERTIES OSX_ARCHITECTURES ${CMAKE_HOST_SYSTEM_PROCESSOR})
  endif()
  target_link_libraries(xythos-tests PRIVATE xythos-engine webpdecoder)
  target_compile_options(xythos-tests PRIVATE ${XYTHOS_WARNINGS})
  target_compile_definitions(xythos-tests PRIVATE XYTHOS_EXAMPLE_MODELS="${NAM_DIR}/example_models"
    XYTHOS_RECORDINGS="${CMAKE_CURRENT_SOURCE_DIR}/tests/recordings" XYTHOS_RECORDINGS_WAV="${CMAKE_BINARY_DIR}/recordings"
    XYTHOS_BLOCK_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/resources/blocks"
    XYTHOS_CAB_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/resources/cabs"
    XYTHOS_PEDAL_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/resources/pedals"
    XYTHOS_AMP_IMAGES="${CMAKE_CURRENT_SOURCE_DIR}/resources/amps")
  add_test(NAME xythos-tests COMMAND xythos-tests)

  # Fuzz targets: xythos-fuzz-replay (a test) always; libFuzzer binaries with XYTHOS_FUZZ=ON (fuzz/README.md)
  include(fuzz/CMakeLists.txt)
endif()
