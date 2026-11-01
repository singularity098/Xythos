# Fuzz targets (fuzz/README.md). Included from the top-level CMakeLists.txt.

set(XYTHOS_FUZZ_TARGETS
  FuzzAudioFile.cpp
  FuzzChainState.cpp
  FuzzEngine.cpp
  FuzzModelFiles.cpp
  FuzzPresetFile.cpp
  FuzzPresetJson.cpp
  FuzzSettings.cpp
  FuzzStbImage.cpp
  FuzzText.cpp
  FuzzWebp.cpp)
list(TRANSFORM XYTHOS_FUZZ_TARGETS PREPEND ${CMAKE_CURRENT_LIST_DIR}/)
# DPF's stb_image isn't ours to fix warnings in
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/FuzzStbImage.cpp PROPERTIES COMPILE_OPTIONS
  $<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>)
set(XYTHOS_FUZZ_UI_SOURCES ${CMAKE_SOURCE_DIR}/src/ui/TextEdit.cpp ${CMAKE_SOURCE_DIR}/src/ui/WebpImage.cpp)

# Every target in one binary, without libFuzzer: replays the seed corpus and deterministic mutations of it (a test),
# and reproduces a crash file anywhere (`xythos-fuzz-replay --target <name> <file>`)
add_executable(xythos-fuzz-replay ${CMAKE_CURRENT_LIST_DIR}/ReplayMain.cpp ${CMAKE_CURRENT_LIST_DIR}/FuzzSupport.cpp
  ${XYTHOS_FUZZ_TARGETS} ${XYTHOS_FUZZ_UI_SOURCES})
target_include_directories(xythos-fuzz-replay PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(xythos-fuzz-replay PRIVATE xythos-engine webpdecoder)
target_compile_options(xythos-fuzz-replay PRIVATE ${XYTHOS_WARNINGS})
if(APPLE)
  set_target_properties(xythos-fuzz-replay PROPERTIES OSX_ARCHITECTURES ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()
add_test(NAME xythos-fuzz-replay COMMAND xythos-fuzz-replay ${CMAKE_CURRENT_LIST_DIR}/corpus --mutations 300)

# libFuzzer: one binary per target, xythos-fuzz-<name>, with ASan and UBSan (clang only). The whole build should use
# the same sanitizers, so configure a separate build directory with XYTHOS_FUZZ=ON (see fuzz/README.md).
if(XYTHOS_FUZZ)
  foreach(source ${XYTHOS_FUZZ_TARGETS})
    # a file may hold several targets: one binary for each
    file(STRINGS ${source} lines REGEX "^XYTHOS_FUZZ_TARGET\\(")
    foreach(line ${lines})
      string(REGEX REPLACE "^XYTHOS_FUZZ_TARGET\\(([a-z_0-9]+)\\).*" "\\1" name "${line}")
      set(target xythos-fuzz-${name})
      string(REPLACE "_" "-" target ${target})
      add_executable(${target} ${source} ${CMAKE_CURRENT_LIST_DIR}/FuzzSupport.cpp ${XYTHOS_FUZZ_UI_SOURCES})
      target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_LIST_DIR})
      target_link_libraries(${target} PRIVATE xythos-engine webpdecoder)
      # one target per binary: the others in the same file are registered but never run
      target_compile_definitions(${target} PRIVATE XYTHOS_FUZZ_LIBFUZZER XYTHOS_FUZZ_ONLY="${name}")
      target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
      target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    endforeach()
  endforeach()
endif()
