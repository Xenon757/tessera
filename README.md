# Tessera

A real-time 3D software rasterizer written from scratch in C++17. No OpenGL, Vulkan, or Metal: every stage of the pipeline runs on the CPU. SDL2 is used only to show the finished framebuffer and read input. It also runs in the browser via WebAssembly.

**▶ [Try it in your browser](https://xenon757.github.io/tessera/)** (press `L` for the liquid demo, `P` for PS1 mode)

| | |
|---|---|
| ![Stanford bunny, Phong shading](docs/orbit.gif) | ![Liquid Sort-style liquid demo](docs/liquid.gif) |
| Stanford bunny (69k triangles), per-pixel Blinn-Phong | Custom shaders: liquid that stays level and sloshes |
| ![PS1 mode](docs/ps1.gif) | ![Debug views](docs/views.gif) |
| PS1 mode: affine textures, vertex snapping, 15-bit dither | Debug views: wireframe, depth, overdraw, thread tiles |

**Highlights**

- The whole pipeline by hand: near-plane clipping, perspective-correct interpolation, a top-left fill rule (exactly one triangle owns every pixel, verified on 460k pixels), z-buffer, flat/Gouraud/Phong, bilinear textures.
- Tiled, multithreaded rasterization on a custom thread pool: **4.6×** faster on 8 cores (**6.1×** when pixel-bound), with output byte-identical at any thread count.
- An ARM NEON SIMD pixel loop, **1.6×** faster on large triangles, that produces byte-identical images to the scalar loop. The WebAssembly build matches both, byte for byte.
- A PS1 mode where every quirk (affine texturing, vertex snapping, ordering tables, 15-bit dithered color, low-res upscale) is a separate setting.
- Programmable vertex and pixel shader stages, and a Liquid Sort–style liquid demo built on them.

## Build

Native (macOS):

```sh
brew install cmake sdl2
cmake -B build
cmake --build build -j
./build/tessera                    # windowed, built-in cube
./build/tessera path/to/model.obj  # any OBJ, auto-centered and scaled
./build/tessera_headless           # no window: benchmark + writes frame.ppm
./build/tessera_headless liquid    # the liquid demo, headless
```

`tessera_headless [model.obj] [out.ppm] [frames] [threads] [distance] [simd]` prints average frame time and triangle stats. `threads` 0 (the default) means one per core; `distance` moves the camera (default 5; smaller fills more of the screen); `simd` 0 forces the scalar pixel loop. Use it for before/after performance numbers.

WebAssembly (needs [emsdk](https://emscripten.org/docs/getting_started/downloads.html)):

```sh
source ~/emsdk/emsdk_env.sh
emcmake cmake -B build-web && cmake --build build-web
python3 -m http.server 8000 --directory build-web   # open http://localhost:8000/tessera.html
node build-web/tessera_headless.js assets/bunny.obj out.ppm   # the benchmark, in WebAssembly
```

The web build is single-threaded and uses the scalar pixel loop (no NEON in WebAssembly).

GIFs: `tools/make_gifs.sh` renders `docs/*.gif` with `tessera_record` and ffmpeg.

## Controls

| Input | Action |
|---|---|
| Left-drag / wheel | Orbit / zoom |
| `1`–`5` | Shaded, wireframe, depth, overdraw, thread-tile views |
| `S` | Cycle shading: flat, Gouraud, Phong |
| `F` | Toggle texture filter: bilinear / nearest |
| `B` | Toggle back-face culling |
| `P` | Toggle PS1 / modern preset |
| `L` | Toggle the liquid demo (arrows: ←/→ move the tube, ↑/↓ tilt it) |
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

A draw can also pass a `Shader` (`src/shader.h`) that replaces the fixed-function vertex and pixel stages, like GPU shaders: `vertex()` outputs a clip-space position plus attributes (including 4 free "custom" floats), and `pixel()` returns a color or discards the pixel. Clipping, culling, the fill rule, and the depth test stay fixed. Custom shaders run on the scalar pixel loop.

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
| `src/shader.h` | The programmable `Shader` interface, `ClipVertex`, `PixelInput` |
| `src/liquid.{h,cpp}`, `src/liquid_scene.h` | Liquid and glass shaders, slosh simulation, and the demo scene |
| `src/vecmath.h` | Vec2/3/4, Mat4, perspective, lookAt |
| `src/camera.h` | Orbit camera |
| `src/main_sdl.cpp` | Windowed front end (also the WebAssembly entry point) |
| `src/main_headless.cpp` | Benchmark / image-output front end (also builds for WebAssembly + Node) |
| `src/main_record.cpp`, `tools/make_gifs.sh` | Renders the README GIFs |
| `web/shell.html` | The web page around the WebAssembly build |

## Liquid demo

Press `L` (or run `tessera_headless liquid`) for a Liquid Sort–style test tube. It's two custom shaders on two procedural tubes, with no liquid geometry:

- **Liquid** (`LiquidShader`): the vertex stage stores each vertex's height above the tube's center along the surface normal; the pixel stage discards everything above the fill level. Because that plane is in world space, the surface stays level however the tube tilts. Front faces get colored layers and a foam line. Back faces show only where the front was cut away, so they're drawn flat as the liquid's top surface. That is what makes a hollow tube look filled.
- **Slosh** (`LiquidWobble`): in an accelerating container the surface settles perpendicular to effective gravity, so its tilt targets acceleration / g. A damped spring per axis chases that target, so the liquid overshoots and settles. Acceleration comes from finite differences of a point at the top of the tube, so tilting sloshes it too.
- **Glass** (`GlassShader`): there's no alpha blending, so the glass discards itself wherever it faces the camera and draws only the grazing-angle rim (the Fresnel effect) and a specular streak.

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
- [x] **Day 6:** Programmable vertex/pixel stages; Liquid Sort–style liquid shader
- [x] **Day 7:** WebAssembly build (`emcmake cmake -B build-web && cmake --build build-web`), GIFs, perf table

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
| Day 6: programmable shaders; 4 custom floats per vertex (a cost: bigger vertices to bin), 1 thread | 5.77 → 5.96 ms | 0.97× |
| Day 7: WebAssembly (Node 24 / V8) vs. native, scalar, 1 thread, bunny | 6.19 → 8.90 ms | 0.70× |
| Day 7: WebAssembly vs. native, scalar, 1 thread, cube close-up | 12.04 → 16.68 ms | 0.72× |
| **Today vs. Day 1:** bunny, 8 threads, NEON, with all Day 2–5 features | 4.44 → 1.21 ms | **3.7×** |

Notes:
- A profile of the single-threaded frame (macOS `sample`) showed ~60% in rasterization and ~37% in the geometry stages, so both passes run in parallel. Threading only the raster pass would cap the speedup under 3× (Amdahl's law).
- 10 threads is no faster than 8: the last two are the M1 Pro's efficiency cores. 64 px tiles beat 16, 32 (binning overhead) and 128 (too few busy tiles to share out).
- The NEON loop gets 1.6×, not 4×, because `pow` (specular) and texture fetches still run one lane at a time. The bunny gains nothing: its triangles are ~3 px, so its time goes to geometry and per-triangle setup, which SIMD here doesn't touch.
- WebAssembly runs at ~70% of native single-thread speed with no SIMD or threads, and its frames are byte-identical to the native ones (bunny, cube, and liquid demo checked).
- A Day 6 refactor slowed the scalar loop 19% on pixel-heavy scenes: copying `baseColor` into a local `Vec3` pinned three more registers for the whole loop, and the Phong + texture path started spilling. My checks at the time only timed the bunny (tiny triangles) and the NEON path, so it slipped through until the Day 7 re-measure; reading it through a reference fixed it.
- Code layout skews results. Before `-falign-functions=64`, editing one function shifted where untouched hot functions landed, moving their speed by ±10% (one profile showed `Texture::sampleBilinear`, unchanged, running 38% slower). Every row here uses the flag.

## Credits

`assets/bunny.obj`: the Stanford Bunny, from the [Stanford 3D Scanning Repository](https://graphics.stanford.edu/data/3Dscanrep/) (OBJ via [alecjacobson/common-3d-test-models](https://github.com/alecjacobson/common-3d-test-models)).
