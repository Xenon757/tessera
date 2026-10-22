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

Measured with `tessera_headless assets/bunny.obj out.ppm <frames> <threads> [distance]` at 960×540, median of 3–5 runs, RelWithDebInfo, on an Apple M1 Pro (8 performance + 2 efficiency cores). Model: Stanford bunny, 69,451 triangles. Speedup is relative to the row before it, unless noted.

| Change | Frame time | Speedup |
|---|---|---|
| Baseline: scalar edge functions per pixel, z-buffer, flat Lambert, 1 thread | 4.52 ms | 1.00× |
| Day 2 features: per-pixel Phong + Blinn-Phong specular, textures (added cost, not an optimization) | 5.18 ms | 0.87× |
| Split into geometry pass + 64 px tile bins + raster pass, still 1 thread | 5.36 ms | 0.97× |
| Thread pool: both passes in parallel, 8 threads | 1.16 ms | **4.6×** |
| Same, camera close-up (`distance` 2.2, raster-heavy): 1 → 8 threads | 10.33 → 1.78 ms | **5.8×** |

Notes:
- A profile of the single-threaded frame (macOS `sample`) showed ~60% in rasterization and ~37% in the geometry stages, so both passes are parallelized. Threading only the raster pass would cap the speedup under 3× (Amdahl's law).
- 10 threads is no faster than 8: the last two are the M1 Pro's efficiency cores.
- 64 px tiles beat 32 and 16 (binning overhead) and 128 (too few busy tiles to share out).

## Credits

`assets/bunny.obj`: the Stanford Bunny, from the [Stanford 3D Scanning Repository](https://graphics.stanford.edu/data/3Dscanrep/) (OBJ via [alecjacobson/common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models)).
