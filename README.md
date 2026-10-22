# Tessera

A real-time 3D software rasterizer written from scratch in C++17. No OpenGL, Vulkan, or Metal: every stage of the pipeline runs on the CPU. SDL2 is used only to show the finished framebuffer and read input.

## Build (macOS)

```sh
brew install cmake sdl2
cmake -B build
cmake --build build -j
./build/tessera                    # windowed, built-in cube
./build/tessera path/to/model.obj  # any OBJ, auto-centered and scaled
./build/tessera_headless           # no window: benchmark + writes frame.ppm
```

`tessera_headless [model.obj] [out.ppm] [frames]` prints average frame time and triangle stats. Use it for before/after performance numbers.

## Controls

| Input | Action |
|---|---|
| Left-drag / wheel | Orbit / zoom |
| `1`–`5` | Shaded, wireframe, depth, overdraw, thread-tile views |
| `B` | Toggle back-face culling |
| `P` | Toggle PS1 / modern preset |
| `Esc` | Quit |

## Architecture

```
drawMesh ─► vertex ─► clip ─► project ─► cull ─► rasterize ─► endFrame (debug views, color reduction)
                          everything reads RenderSettings
```

| File | Role |
|---|---|
| `src/render_settings.h` | Every pipeline knob, plus the `modern()` and `ps1()` presets |
| `src/renderer.{h,cpp}` | The pipeline stages |
| `src/framebuffer.{h,cpp}` | Color, depth, and overdraw buffers (ARGB8888, uploads to SDL with no conversion) |
| `src/mesh.{h,cpp}` | OBJ loader (v/vt/vn, n-gons, negative indices) and procedural cube |
| `src/vecmath.h` | Vec2/3/4, Mat4, perspective, lookAt |
| `src/camera.h` | Orbit camera |
| `src/main_sdl.cpp` | Windowed front end (also the WebAssembly entry point) |
| `src/main_headless.cpp` | Benchmark / image-output front end |

## Roadmap

- [ ] **Day 1:** Filled-triangle rasterizer (barycentric/edge functions), z-buffer, flat Lambert shading
- [ ] **Day 2:** Near-plane clipping, perspective-correct UVs, texture sampling, Gouraud + Phong
- [ ] **Day 3:** Depth/overdraw/thread-tile debug views; top-left fill rule; tiled, multithreaded rasterization
- [ ] **Day 4:** PS1 mode: affine texturing, vertex snapping, ordering tables, 15-bit color + dithering, low-res upscale
- [ ] **Day 5:** ARM NEON SIMD inner loop (scalar fallback kept for WASM)
- [ ] **Day 6:** Programmable vertex/pixel stages; Liquid Sort–style liquid shader
- [ ] **Day 7:** WebAssembly build (`emcmake cmake -B build-web && cmake --build build-web`), GIFs, perf table

## Performance

| Change | Model | Frame time | Speedup |
|---|---|---|---|
| _baseline_ | | | |
