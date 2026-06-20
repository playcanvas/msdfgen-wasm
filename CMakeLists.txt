# Emscripten build for @playcanvas/msdfgen-wasm.
#   emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# Emits dist/msdfgen.{mjs,wasm}. Requires the upstream msdfgen submodule at vendor/msdfgen.
#
# STATUS: skeleton (untested without a local emsdk). The main risk is making msdfgen's
# own CMake find FreeType via Emscripten's port instead of vcpkg/system — iterate here
# on the first CI build.

cmake_minimum_required(VERSION 3.16)
project(msdfgen_wasm CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build only what we need from upstream msdfgen (core + font-loading ext; no CLI, no Skia).
set(MSDFGEN_CORE_ONLY      OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA       OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG      OFF CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME OFF CACHE BOOL "" FORCE)
# Drop optional ext deps we don't need (SVG=tinyxml2, PNG=libpng); we load fonts via
# FreeType and emit raw RGBA, leaving FreeType as the only third-party dependency.
set(MSDFGEN_DISABLE_SVG    ON  CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG    ON  CACHE BOOL "" FORCE)
# Emscripten's FreeType port (2.6.0) predates the variable-font API msdfgen v1.13 uses
# (FT_Get_Var_Design_Coordinates / FT_Done_MM_Var); compile that path out (we don't need it).
set(MSDFGEN_DISABLE_VARIABLE_FONTS ON CACHE BOOL "" FORCE)
add_compile_definitions(MSDFGEN_DISABLE_VARIABLE_FONTS)

add_subdirectory(vendor/msdfgen)

add_executable(msdfgen cpp/binding.cpp)
target_link_libraries(msdfgen PRIVATE msdfgen::msdfgen msdfgen::msdfgen-ext)

# FreeType comes from Emscripten's port (no separate build). Applied to msdfgen's ext too.
target_compile_options(msdfgen PRIVATE "-sUSE_FREETYPE=1")
target_link_options(msdfgen PRIVATE
    "-sUSE_FREETYPE=1"
    "--bind"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createMsdfgenModule"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sENVIRONMENT=web,worker,node"
)

set_target_properties(msdfgen PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    OUTPUT_NAME "msdfgen"
    SUFFIX ".mjs"
)
