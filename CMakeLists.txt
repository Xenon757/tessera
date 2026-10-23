cmake_minimum_required(VERSION 3.16)
project(Tessera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / editor tooling

# A software renderer is unusable in Debug; default to an optimized build. Native
# keeps debug info for profiling. The web build defaults to Release: with debug info,
# Emscripten skips most of its WebAssembly optimizations (and warns about it).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  if(EMSCRIPTEN)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  else()
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  endif()
endif()

# ---------------------------------------------------------------- core library
# Everything platform-independent. Both front ends link this.
add_library(tessera_core STATIC
  src/framebuffer.cpp
  src/inflate.cpp
  src/liquid.cpp
  src/mesh.cpp
  src/renderer.cpp
  src/raster_neon.cpp
  src/texture.cpp
  src/thread_pool.cpp
)
target_include_directories(tessera_core PUBLIC src)
if(NOT MSVC)
  # -ffp-contract=off: don't let the compiler fuse a*b+c into one FMA instruction.
  # Fusing rounds once instead of twice, so results would depend on what the
  # optimizer chose. With it off, the scalar, NEON, and WebAssembly paths all do the
  # same IEEE operations and produce bit-identical images, which is how we test them.
  target_compile_options(tessera_core PUBLIC -Wall -Wextra -ffp-contract=off)
  # Start every function on a 64-byte boundary. Without this, editing one function
  # shifts where the others land in memory, and a hot loop that happens to straddle
  # a different boundary can run ~10% faster or slower. That noise swamped real
  # before/after differences in the perf table, so we pin it.
  target_compile_options(tessera_core PRIVATE -falign-functions=64)
endif()

find_package(Threads REQUIRED)  # for the thread pool
target_link_libraries(tessera_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------- headless
# Renders to a .ppm with no window: benchmarking, regression images, CI.
if(NOT EMSCRIPTEN)
  add_executable(tessera_headless src/main_headless.cpp)
  target_link_libraries(tessera_headless PRIVATE tessera_core)
  target_compile_definitions(tessera_headless PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")

  # Renders the scripted sequences behind the README GIFs (see tools/make_gifs.sh).
  add_executable(tessera_record src/main_record.cpp)
  target_link_libraries(tessera_record PRIVATE tessera_core)
  target_compile_definitions(tessera_record PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
endif()

# ---------------------------------------------------------------- windowed (SDL2)
if(EMSCRIPTEN)
  # The headless benchmark also builds for WebAssembly, run with Node:
  #   node build-web/tessera_headless.js assets/bunny.obj out.ppm
  # NODERAWFS gives it direct access to the real filesystem (models in, images out).
  add_executable(tessera_headless src/main_headless.cpp)
  target_link_libraries(tessera_headless PRIVATE tessera_core)
  target_link_options(tessera_headless PRIVATE -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node)
  target_compile_definitions(tessera_headless PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")

  # emcmake cmake -B build-web && cmake --build build-web
  add_executable(tessera src/main_sdl.cpp)
  target_link_libraries(tessera PRIVATE tessera_core)
  target_compile_options(tessera PRIVATE -sUSE_SDL=2)
  target_link_options(tessera PRIVATE -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1
                      "--preload-file=${CMAKE_SOURCE_DIR}/assets@/assets"
                      "--shell-file=${CMAKE_SOURCE_DIR}/web/shell.html"
                      # The page writes uploaded models into the virtual filesystem (FS)
                      # and calls tessera_load_model through ccall.
                      -sEXPORTED_RUNTIME_METHODS=ccall,FS)
  set_target_properties(tessera PROPERTIES LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html")
  target_compile_definitions(tessera PRIVATE TESSERA_ASSET_DIR="/assets")
  set_target_properties(tessera PROPERTIES SUFFIX ".html")
else()
  find_package(SDL2 CONFIG QUIET)
  if(SDL2_FOUND)
    add_executable(tessera src/main_sdl.cpp)
    target_link_libraries(tessera PRIVATE tessera_core SDL2::SDL2)
    if(TARGET SDL2::SDL2main)
      target_link_libraries(tessera PRIVATE SDL2::SDL2main)
    endif()
    target_compile_definitions(tessera PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
  else()
    message(WARNING "SDL2 not found: only tessera_headless will be built. "
                    "On macOS: brew install sdl2")
  endif()
endif()
