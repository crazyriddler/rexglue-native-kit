# conan - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(conan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(CONAN_SOURCES
    src/cpu_check.cpp
    src/main.cpp
    src/hooks.cpp
    src/native/d3d_capture.cpp
    src/native/native_renderer.cpp
    src/native/pm4_mirror.cpp
    src/native/hang_watchdog.cpp
    src/native/native_graphics_system.cpp
    src/native/native_hooks.cpp
    src/native/texture_decode.cpp
    src/native/shader_registry.cpp
    src/native/pipeline_cache.cpp
    src/launcher_dialog.cpp
    src/settings.cpp
    src/conan.rc
)
# Runs before the x86-64-v3 code: must build for the baseline ISA.
set_source_files_properties(src/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64")

# Native renderer shaders (artifacts/shaders/dxil, tools/shaders/build_corpus.sh)
# packed into one blob embedded in conan.exe (RCDATA 2), so a release needs no
# shader files next to the executable.
if(WIN32)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(CONAN_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts/shaders/dxil")
    set(CONAN_SHADER_PAK "${CMAKE_CURRENT_BINARY_DIR}/conan_shaders.pak")
    file(GLOB CONAN_SHADER_FILES CONFIGURE_DEPENDS "${CONAN_SHADER_DIR}/*.dxil")
    add_custom_command(
        OUTPUT "${CONAN_SHADER_PAK}"
        COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/../tools/shaders/pack_shaders.py"
                "${CONAN_SHADER_DIR}" "${CONAN_SHADER_PAK}"
        DEPENDS ${CONAN_SHADER_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/../tools/shaders/pack_shaders.py"
        COMMENT "Packing native renderer shaders")
    set(CONAN_SHADERS_RC "${CMAKE_CURRENT_BINARY_DIR}/conan_shaders.rc")
    # Base pipeline list for startup precompilation (RCDATA 3, EXP-044), captured
    # from test playthroughs with tools/make_pipeline_base.sh.
    set(CONAN_PIPELINE_BASE "${CMAKE_CURRENT_SOURCE_DIR}/../artifacts/shaders/pipelines_base.bin")
    set(CONAN_RC_DEPENDS "${CONAN_SHADER_PAK}")
    if(EXISTS "${CONAN_PIPELINE_BASE}")
        file(WRITE "${CONAN_SHADERS_RC}"
             "2 RCDATA \"${CONAN_SHADER_PAK}\"\n3 RCDATA \"${CONAN_PIPELINE_BASE}\"\n")
        list(APPEND CONAN_RC_DEPENDS "${CONAN_PIPELINE_BASE}")
    else()
        file(WRITE "${CONAN_SHADERS_RC}" "2 RCDATA \"${CONAN_SHADER_PAK}\"\n")
    endif()
    set_source_files_properties("${CONAN_SHADERS_RC}" PROPERTIES OBJECT_DEPENDS "${CONAN_RC_DEPENDS}")
    list(APPEND CONAN_SOURCES "${CONAN_SHADERS_RC}")
endif()

if(WIN32)
    add_executable(conan WIN32 ${CONAN_SOURCES})
else()
    add_executable(conan ${CONAN_SOURCES})
endif()

rexglue_setup_target(conan GPU_PLUGINS xenos)

# rexglue_configure_target() (called above) compiles the SDK's rex_app.cpp
# straight into this target, and rex_app.cpp needs imgui.h (via
# rex/ui/style.h). The installed/find_package SDK consumption path gets this
# for free because everything is flattened into one combined install/include
# directory; REXSDK_DIR's add_subdirectory path uses the SDK's real modular
# target graph instead, where imgui isn't part of rex::runtime's own public
# link interface. Link it explicitly rather than patching the SDK for a
# one-line project-side fix. See docs/error_log.md E008.
target_link_libraries(conan PRIVATE imgui::imgui)

# Native renderer: rex/hash.h pulls in xxhash.h (header-only, vendored by the SDK).
target_include_directories(conan PRIVATE "${REXSDK_DIR}/thirdparty/xxHash")
target_link_libraries(conan PRIVATE d3d12 dxgi dbghelp)
