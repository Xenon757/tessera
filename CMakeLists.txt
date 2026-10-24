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

# -DTESSERA_SANITIZE=ON builds everything with AddressSanitizer + UBSan: any
# out-of-bounds access or undefined behavior aborts. CI runs the tests this way too.
option(TESSERA_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(TESSERA_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------- core library
# Everything platform-independent. Both front ends link this.
add_library(tessera_core STATIC
  src/animation.cpp
  src/commands.cpp
  src/deflate.cpp
  src/edit_mesh.cpp
  src/editor.cpp
  src/fbx.cpp
  src/framebuffer.cpp
  src/gizmo.cpp
  src/hierarchy.cpp
  src/inflate.cpp
  src/json.cpp
  src/liquid.cpp
  src/mesh.cpp
  src/picking.cpp
  src/png.cpp
  src/primitives.cpp
  src/renderer.cpp
  src/scene.cpp
  src/scene_render.cpp
  src/raster_neon.cpp
  src/texture.cpp
  src/thread_pool.cpp
  src/transform_tool.cpp
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
                      # The editor page (web/editor.js) calls the tessera_* functions
                      # through ccall, passes strings in heap memory (a saved scene can
                      # outgrow the 64 KB stack ccall would copy it to), and writes
                      # imported models into the virtual FS.
                      -sEXPORTED_RUNTIME_METHODS=ccall,FS,stringToNewUTF8,HEAPU8
                      -sEXPORTED_FUNCTIONS=_main,_malloc,_free)
  set_target_properties(tessera PROPERTIES LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html")
  add_custom_command(TARGET tessera POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/web/editor.js" "$<TARGET_FILE_DIR:tessera>/editor.js"
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/web/export.js" "$<TARGET_FILE_DIR:tessera>/export.js"
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/web/chat.js" "$<TARGET_FILE_DIR:tessera>/chat.js")
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

# ---------------------------------------------------------------- tests
# cmake --build build && ctest --test-dir build --output-on-failure
option(TESSERA_BUILD_TESTS "Build the tests" ON)
if(TESSERA_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()
  find_package(Python3 REQUIRED COMPONENTS Interpreter)  # writes test inputs from independent references
  set(TEST_DATA ${CMAKE_BINARY_DIR}/test_data)

  add_executable(tessera_test_render tests/test_render.cpp)
  target_link_libraries(tessera_test_render PRIVATE tessera_core)
  target_compile_definitions(tessera_test_render PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets"
                             TESSERA_TEST_DIR="${CMAKE_SOURCE_DIR}/tests")
  add_test(NAME render COMMAND tessera_test_render)

  add_executable(tessera_test_math tests/test_math.cpp)
  target_link_libraries(tessera_test_math PRIVATE tessera_core)
  add_test(NAME math COMMAND tessera_test_math)

  add_executable(tessera_test_animation tests/test_animation.cpp)
  target_link_libraries(tessera_test_animation PRIVATE tessera_core)
  add_test(NAME animation COMMAND tessera_test_animation)

  add_executable(tessera_test_hierarchy tests/test_hierarchy.cpp)
  target_link_libraries(tessera_test_hierarchy PRIVATE tessera_core)
  add_test(NAME hierarchy COMMAND tessera_test_hierarchy)

  add_executable(tessera_test_edit_mesh tests/test_edit_mesh.cpp)
  target_link_libraries(tessera_test_edit_mesh PRIVATE tessera_core)
  target_compile_definitions(tessera_test_edit_mesh PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
  add_test(NAME edit_mesh COMMAND tessera_test_edit_mesh)

  add_executable(tessera_test_json tests/test_json.cpp)
  target_link_libraries(tessera_test_json PRIVATE tessera_core)
  add_test(NAME json COMMAND tessera_test_json)

  add_executable(tessera_test_scene tests/test_scene.cpp)
  target_link_libraries(tessera_test_scene PRIVATE tessera_core)
  target_compile_definitions(tessera_test_scene PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
  add_test(NAME scene COMMAND tessera_test_scene)

  add_executable(tessera_test_commands tests/test_commands.cpp)
  target_link_libraries(tessera_test_commands PRIVATE tessera_core)
  add_test(NAME commands COMMAND tessera_test_commands)

  add_executable(tessera_test_picking tests/test_picking.cpp)
  target_link_libraries(tessera_test_picking PRIVATE tessera_core)
  add_test(NAME picking COMMAND tessera_test_picking)

  add_executable(tessera_test_editor tests/test_editor.cpp)
  target_link_libraries(tessera_test_editor PRIVATE tessera_core)
  target_compile_definitions(tessera_test_editor PRIVATE TESSERA_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
  add_test(NAME editor COMMAND tessera_test_editor)

  add_executable(tessera_test_zlib tests/test_zlib.cpp)
  target_link_libraries(tessera_test_zlib PRIVATE tessera_core)
  add_test(NAME zlib_corpus COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/zlib_corpus.py
           ${CMAKE_BINARY_DIR}/zlib_corpus.bin)
  set_tests_properties(zlib_corpus PROPERTIES FIXTURES_SETUP zlib_corpus)
  add_test(NAME zlib COMMAND tessera_test_zlib ${CMAKE_BINARY_DIR}/zlib_corpus.bin)
  set_tests_properties(zlib PROPERTIES FIXTURES_REQUIRED zlib_corpus)

  add_executable(tessera_test_png tests/test_png.cpp)
  target_link_libraries(tessera_test_png PRIVATE tessera_core)
  add_test(NAME png COMMAND tessera_test_png ${CMAKE_BINARY_DIR})
  set_tests_properties(png PROPERTIES FIXTURES_SETUP png_file)
  add_test(NAME png_check COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/png_check.py ${CMAKE_BINARY_DIR})
  set_tests_properties(png_check PROPERTIES FIXTURES_REQUIRED png_file)

  add_executable(tessera_test_loaders tests/test_loaders.cpp)
  target_link_libraries(tessera_test_loaders PRIVATE tessera_core)
  add_test(NAME fbx_files COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tests/make_fbx.py ${TEST_DATA})
  set_tests_properties(fbx_files PROPERTIES FIXTURES_SETUP fbx_files)
  add_test(NAME loaders COMMAND tessera_test_loaders ${TEST_DATA})
  set_tests_properties(loaders PROPERTIES FIXTURES_REQUIRED fbx_files)
endif()
