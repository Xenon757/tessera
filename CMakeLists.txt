cmake_minimum_required(VERSION 3.16)
project(Tessera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / editor tooling

# A software renderer is unusable in Debug; default to an optimized build.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- core library
# Everything platform-independent. Both front ends link this.
add_library(tessera_core STATIC
  src/framebuffer.cpp
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
endif()

# ---------------------------------------------------------------- windowed (SDL2)
if(EMSCRIPTEN)
  # Day 7: emcmake cmake -B build-web && cmake --build build-web
  add_executable(tessera src/main_sdl.cpp)
  target_link_libraries(tessera PRIVATE tessera_core)
  target_compile_options(tessera PRIVATE -sUSE_SDL=2)
  target_link_options(tessera PRIVATE -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1
                      "--preload-file=${CMAKE_SOURCE_DIR}/assets@/assets")
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
