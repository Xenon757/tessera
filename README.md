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

`tessera_headless [model.obj] [out.ppm] [frames] [threads] [distance] [simd]` prints average frame time and triangle stats. `threads` 0 (the default) means one per core; `distance` moves the camera (default 5; smaller fills more of the screen); `simd` 0 forces the scalar pixel loop. Use it for before/after performance numbers.

## Controls

| Input | Action |
|---|---|
| Left-drag / wheel | Orbit / zoom |
| `1`–`5` | Shaded, wireframe, depth, overdraw, thread-tile views |
| `S` | Cycle shading: flat, Gouraud, Phong |
| `F` | Toggle texture filter: bilinear / nearest |
| `B` | Toggle back-face culling |
| `P` | Toggle PS1 / modern preset |
| `Esc` | Quit |

## Architecture

```
drawMesh
  geometry pass (parallel over mesh chunks):  vertex ─► clip ─► project ─► cull ─► bin into screen tiles
  raster pass   (parallel over tiles):        edge functions ─► depth test ─► shade
                                              (scalar, or ARM NEON 4 pixels at a time)
endFrame: debug views, color reduction
                          everything reads RenderSettings
```

The pixel loop has two implementations: a scalar reference, and an ARM NEON version that processes 4 pixels per instruction. Floating-point contraction is disabled (`-ffp-contract=off`) so both do identical IEEE operations, and the NEON loop is tested to produce byte-identical images. That also makes it safe to pick a loop per triangle: triangles narrower than 8 pixels stay scalar, where 4-wide groups would mostly waste lanes. The WebAssembly build uses the scalar loop.

Both passes run on a small thread pool. Tiles never overlap, so raster threads need no locks, and each tile draws its triangles in submission order, which keeps the output byte-identical at any thread count.

| File | Role |
|---|---|
| `src/render_settings.h` | Every pipeline knob, plus the `modern()` and `ps1()` presets |
| `src/renderer.{h,cpp}` | The pipeline stages, including the scalar pixel loop |
| `src/raster_neon.cpp`, `src/raster.h` | The NEON pixel loop, and the edge/setup types it shares with the scalar loop |
| `src/framebuffer.{h,cpp}` | Color, depth, and overdraw buffers (ARGB8888, uploads to SDL with no conversion) |
| `src/mesh.{h,cpp}` | OBJ loader (v/vt/vn, n-gons, negative indices, smooth normals) and procedural cube |
| `src/texture.{h,cpp}` | Textures: procedural checkerboard, nearest and bilinear sampling |
| `src/thread_pool.{h,cpp}` | Persistent worker threads with a `parallelFor` |
| `src/vecmath.h` | Vec2/3/4, Mat4, perspective, lookAt |
| `src/camera.h` | Orbit camera |
| `src/main_sdl.cpp` | Windowed front end (also the WebAssembly entry point) |
| `src/main_headless.cpp` | Benchmark / image-output front end |

## PS1 mode

Press `P` to switch to `RenderSettings::ps1()`. Every PS1 quirk is a separate setting, so each can be turned on alone.

| Setting | What the PS1 did | What you see |
|---|---|---|
| `textureMapping = Affine` | No per-pixel divide: UVs interpolated linearly in screen space | Textures kink along polygon diagonals and swim as the camera moves |
| `vertexSnap` | The GTE output whole-pixel screen coordinates | Vertices jitter a pixel at a time; slivers collapse |
| `depthMode = OrderingTable` | No depth buffer: triangles bucketed by average depth, drawn far to near | Overlapping polygons at similar depths pop through each other |
| `colorDepth = Bits15`, `dither` | 5 bits per channel, hidden with the GPU's 4×4 ordered-dither matrix | Fine crosshatch texture instead of banding |
| `internalScale = 3` | ~320×240 output, blown up by the TV | Chunky pixels (here 320×180, upscaled 3× with nearest neighbor) |
| `textureFilter = Nearest`, `shading = Gouraud` | No texture filtering; lighting per vertex | Blocky texels; soft, low-detail lighting |

Simplification: the dither is applied to the whole frame, including the background; the PS1 dithered only shaded polygons.

## Roadmap

- [x] **Day 1:** Filled-triangle rasterizer (barycentric/edge functions), z-buffer, flat Lambert shading
- [x] **Day 2:** Near-plane clipping, perspective-correct UVs, texture sampling, Gouraud + Phong
- [x] **Day 3:** Depth/overdraw/thread-tile debug views; top-left fill rule; tiled, multithreaded rasterization
- [x] **Day 4:** PS1 mode: affine texturing, vertex snapping, ordering tables, 15-bit color + dithering, low-res upscale
- [x] **Day 5:** ARM NEON SIMD inner loop (scalar fallback kept for WASM)
- [ ] **Day 6:** Programmable vertex/pixel stages; Liquid Sort–style liquid shader
- [ ] **Day 7:** WebAssembly build (`emcmake cmake -B build-web && cmake --build build-web`), GIFs, perf table

## Performance

Every row was measured in one session with `tessera_headless <model> out.ppm <frames> <threads> [distance] [simd]` at 960×540: 5 interleaved rounds, median, RelWithDebInfo, Apple M1 Pro (8 performance + 2 efficiency cores). Older rows come from rebuilding that commit with the same flags (including `-falign-functions=64`, see notes). Single-thread rows vary by under 4% between runs; 8-thread rows by 5–10%, because other apps were using some of the cores.

Model: Stanford bunny (69,451 triangles) unless noted. The "cube close-up" is the textured, Phong-lit cube at camera distance 2.4: a few big triangles, so nearly all of its time is per-pixel work.

| Change | Frame time | Speedup |
|---|---|---|
| Day 1 baseline: scalar edge functions, z-buffer, flat Lambert, 1 thread | 4.44 ms | 1.00× |
| Day 2 features: per-pixel Phong + Blinn-Phong specular, textures (a cost, not an optimization) | 5.18 ms | 0.86× |
| Geometry pass + 64 px tile bins + raster pass, still 1 thread | 5.34 ms | 0.97× |
| Thread pool: both passes in parallel, 1 → 8 threads | 5.38 → 1.16 ms | **4.6×** |
| Same, camera close-up (`distance` 2.2, raster-heavy), 1 → 8 threads | 10.26 → 1.68 ms | **6.1×** |
| `-ffp-contract=off` for bit-identical scalar/SIMD output (a cost), 1 thread | 5.44 → 5.69 ms | 0.96× |
| NEON pixel loop, cube close-up, 1 thread | 11.98 → 7.61 ms | **1.57×** |
| NEON pixel loop, cube close-up, 8 threads | 1.74 → 1.15 ms | **1.51×** |
| NEON pixel loop, bunny (triangles under 8 px wide stay scalar), 1 thread | 5.84 → 5.85 ms | 1.00× |
| **Today vs. Day 1:** bunny, 8 threads, NEON, with all Day 2–5 features | 4.44 → 1.21 ms | **3.7×** |

Notes:
- A profile of the single-threaded frame (macOS `sample`) showed ~60% in rasterization and ~37% in the geometry stages, so both passes run in parallel. Threading only the raster pass would cap the speedup under 3× (Amdahl's law).
- 10 threads is no faster than 8: the last two are the M1 Pro's efficiency cores. 64 px tiles beat 16, 32 (binning overhead) and 128 (too few busy tiles to share out).
- The NEON loop gets 1.6×, not 4×, because `pow` (specular) and texture fetches still run one lane at a time. The bunny gains nothing: its triangles are ~3 px, so its time goes to geometry and per-triangle setup, which SIMD here doesn't touch.
- Code layout skews results. Before `-falign-functions=64`, editing one function shifted where untouched hot functions landed, moving their speed by ±10% (one profile showed `Texture::sampleBilinear`, unchanged, running 38% slower). Every row here uses the flag.

## Credits

`assets/bunny.obj`: the Stanford Bunny, from the [Stanford 3D Scanning Repository](https://graphics.stanford.edu/data/3Dscanrep/) (OBJ via [alecjacobson/common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models)).
