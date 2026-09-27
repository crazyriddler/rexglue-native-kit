# Out-of-tree build of the XenosRecomp HLSL emitter for the game's shader corpus.
# Source: tools/xenosrecomp/src, vendored = pinned reblue-XenosRecomp + patches/ (fetch_source.sh re-vendors).
# Deps come from the SDK's thirdparty (fmt, xxHash); no DXC/zstd/smol-v needed.
cmake_minimum_required(VERSION 3.20)
project(XenosRecompCorpus CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk")
set(XR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/XenosRecomp")
if (NOT EXISTS "${XR_SRC}/shader_recompiler.cpp")
    message(FATAL_ERROR "tools/xenosrecomp/src is missing: run tools/xenosrecomp/fetch_source.sh")
endif()

# Game-specific semantics switch (see patches/). OFF = upstream generic behavior.
# CONAN_RECOMP = the native-renderer feature set developed on Conan (shared constants,
# TEX_SCALE, PWL gamma, spec constants, loops/bools fixes...). Mostly generic;
# see docs/SHADER_PIPELINE.md for the few Conan-specific heuristics.
option(CONAN_RECOMP "Enable the native-port XenosRecomp feature set" ON)

add_executable(XenosRecompCorpus
    corpus_main.cpp
    "${XR_SRC}/shader_recompiler.cpp")
target_include_directories(XenosRecompCorpus PRIVATE
    "${XR_SRC}"
    "${SDK_ROOT}/thirdparty/fmt/include"
    "${SDK_ROOT}/thirdparty/xxHash")
target_compile_definitions(XenosRecompCorpus PRIVATE FMT_HEADER_ONLY _CRT_SECURE_NO_WARNINGS)
if (CONAN_RECOMP)
    target_compile_definitions(XenosRecompCorpus PRIVATE CONAN_RECOMP)
endif()
# Keep asserts enabled even in Release: assertion text is the failure reason
# recorded in the catalog.
target_compile_options(XenosRecompCorpus PRIVATE -UNDEBUG -Wno-switch -Wno-unused-variable
    -Wno-null-arithmetic -fms-extensions -Wno-deprecated-declarations)
target_precompile_headers(XenosRecompCorpus PRIVATE pch_corpus.h)
