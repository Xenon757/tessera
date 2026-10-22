# CLAUDE.md

Tessera is a real-time 3D **software rasterizer** in C++17: the whole graphics pipeline runs on the CPU. It's a portfolio project aimed at game engine programming internships (e.g. Insomniac's Engine & Tools intern), built over about a week. The roadmap and perf table are in `README.md`.

## Ground rules

- **No graphics APIs or rendering libraries.** No OpenGL, Metal, Vulkan, or bgfx, and no helper libraries that do rasterization, clipping, or shading. SDL2 is only for the window, input, and blitting the finished framebuffer. Writing the pipeline by hand is the whole point.
- **Aadi has to be able to explain every line in an interview.** Keep each change focused on one concept. Briefly explain non-obvious math such as barycentrics, perspective-correct interpolation, clipping, and fill rules, both in chat and in short code comments. Don't drop in large blocks of unexplained code.
- **Every pipeline behavior is driven by `RenderSettings`** (`src/render_settings.h`). New features add a field there and get read in `renderer.cpp`. Don't hard-code behavior a mode might want to change. PS1 mode, debug views, SIMD, and threading are all just settings values.
- **Keep a scalar code path.** NEON SIMD must sit behind `settings.useSimd` and `#if defined(__ARM_NEON)`, because the WebAssembly build can't use it.
- **Builds must stay warning-free** under `-Wall -Wextra`.
- `TODO(Day N)` comments mark where each day's work goes. Remove a TODO when its work lands, and tick the box in the README roadmap.

## Build and run (macOS)

```sh
brew install cmake sdl2          # once
cmake -B build                   # defaults to RelWithDebInfo
cmake --build build -j
./build/tessera [model.obj]      # windowed; title bar shows frame time
./build/tessera_headless [model.obj] [out.ppm] [frames] [threads] [distance]   # benchmark + image
```

- Run `tessera_headless` after any rasterizer change and check the output image (convert it with `sips -s format png out.ppm --out out.png`).
- WebAssembly: emsdk lives in `~/emsdk` (not on PATH by default). `source ~/emsdk/emsdk_env.sh`, then `emcmake cmake -B build-web && cmake --build build-web`. It defaults to Release (debug info disables Emscripten's optimizer). Serve `build-web/` over HTTP to run `tessera.html`; `node build-web/tessera_headless.js <model> <out.ppm> [frames]` runs the benchmark in WebAssembly.
- Cross-platform check: the WebAssembly headless output must stay byte-identical to the native one (`simd` 0 and 1) for the same arguments. It was for the bunny, the cube, and the liquid demo; only `powf` (a different math library per platform) could ever legitimately differ.
- GIFs: `tools/make_gifs.sh` (needs ffmpeg) renders `docs/*.gif` via `tessera_record`.

## Architecture

```
Renderer::drawMesh
  geometry pass, parallel over mesh chunks (geometryPass / emitTriangle):
    vertex   model/view/projection -> clip space
    clip     trivial frustum reject; Sutherland-Hodgman against the near plane only
    project  perspective divide + viewport -> ScreenVertex
    cull     back-face culling
    bin      ScreenTriangle appended to the chunk; its index added to each overlapped tile's bin
  raster pass, parallel over tiles (rasterizeTile / rasterizeTriangle):
    edge functions + barycentrics with top-left fill rule, z-buffer, perspective-correct
    attributes, flat/Gouraud/Phong (Blinn-Phong lighting), texture sampling.
    Per triangle: setupTriangle (shared, force-inlined), then the scalar loop in
    rasterizeTriangle, or rasterizeTriangleNeon -> rasterizeNeon (raster_neon.cpp) when
    settings.useSimd and the box is >= settings.simdMinWidth pixels wide. Keep the NEON
    dispatch out of line: inlined, it slowed the scalar loop ~7% (register allocation).
Renderer::endFrame -> debug views (depth, overdraw, thread tiles), 15-bit reduction + dither, upscale

With settings.internalScale > 1, beginFrame/drawMesh/endFrame render into the renderer's own
lowRes_ framebuffer (see renderTarget()), and endFrame upscales into the caller's framebuffer.
Vertex snapping, debug views, and dithering all happen at the low resolution.
```

| File | Role |
|---|---|
| `src/render_settings.h` | All knobs + `modern()` / `ps1()` presets |
| `src/renderer.{h,cpp}` | Pipeline stages, `edgeFunction`, `ScreenVertex`, `RenderStats` |
| `src/framebuffer.{h,cpp}` | Color (ARGB8888), depth (float), overdraw (uint16) buffers; `savePPM` |
| `src/mesh.{h,cpp}` | Unrolled triangle list (every 3 vertices = 1 tri), OBJ loader (smooth normals if the file has none), `makeCube()` |
| `src/raster.h`, `src/raster_neon.cpp` | `Edge` + `TriangleSetup` (shared by both pixel loops); the NEON loop, all under `#if defined(__ARM_NEON)` |
| `src/shader.h` | `Shader` (programmable `vertex()` / `pixel()`, `twoSided()`), `ClipVertex`, `PixelInput`, `fixedFunctionVertex` |
| `src/liquid.{h,cpp}`, `src/liquid_scene.h` | `LiquidShader`, `GlassShader`, `LiquidWobble` (slosh spring), the demo scene used by both front ends |
| `src/thread_pool.{h,cpp}` | Persistent workers, `parallelFor(count, fn(index, thread))`; caller thread joins in |
| `src/texture.{h,cpp}` | `Texture` (ARGB texels, row 0 = top), `checker()`, nearest/bilinear sampling. UV (0,0) = bottom-left |
| `src/vecmath.h` | Vec2/3/4, Mat4 |
| `src/camera.h` | Orbit camera |
| `src/app_common.h` | Model loading + `fitToView` shared by both front ends |
| `src/main_sdl.cpp` | Windowed front end; also the Emscripten entry point |
| `src/main_headless.cpp` | Headless benchmark / image output (also builds for WebAssembly + Node with NODERAWFS) |
| `src/main_record.cpp`, `tools/make_gifs.sh` | Scripted sequences for the README GIFs |
| `web/shell.html` | The page around the WebAssembly build (Emscripten `--shell-file`) |

## Conventions (get these wrong and everything renders wrong)

- **Math:** right-handed, column vectors (`v' = M * v`), `Mat4::m[row][col]`, camera looks down −Z.
- **Clip space:** OpenGL style, z in [−w, w].
- **Screen space:** pixels, origin at the top left, **y down**. Sample at pixel centers (`x + 0.5`).
- **Depth:** `ScreenVertex::z` is in [0, 1], 0 = near and 1 = far. The buffer clears to 1, and **smaller is closer**.
- **Winding:** meshes are counter-clockwise when seen from outside. Because y is flipped, **front-facing triangles have NEGATIVE `edgeFunction` area** in screen space. Inside tests must compare signs against `area`; don't assume positive.
- **Colors:** `packColor` / `unpackColor` use `0xAARRGGBB`, which matches `SDL_PIXELFORMAT_ARGB8888`.
- **Normals:** transformed with the model matrix, which is only correct for uniform scale. Switch to an inverse-transpose normal matrix if non-uniform scale is added.
- `ScreenVertex::invW` (1/clip.w) exists for perspective-correct interpolation. Interpolate `attr * invW` and `invW`, then divide.

## Performance work

- Measure frame time with `tessera_headless` in a RelWithDebInfo or Release build, never Debug, on a model with real triangle counts (Stanford bunny, Spot the cow), not the cube.
- Record every optimization as a before/after row in the README's performance table: change, model, frame time, speedup. Those numbers go on the resume.
- Profile before optimizing. On macOS, use Instruments (Time Profiler) or `xctrace`; with only the Command Line Tools installed (no Xcode), use `sample <pid> 5` on a long `tessera_headless` run.

## Shader rules

- A `Shader` passed to `drawMesh` replaces the vertex stage and the pixel stage for that draw. Clipping, culling, binning, the fill rule, and the depth test stay fixed.
- `vertex()` and `pixel()` run on many threads at once: they're `const` and must not write shared state. Per-frame inputs ("uniforms") are set on the shader before `drawMesh`.
- Custom-shaded triangles go through `rasterizeProgrammable` (scalar, out of line). It writes depth *after* `pixel()` keeps the pixel, so a discard doesn't hide what's behind it.
- Anything a shader needs per pixel that isn't position/normal/uv goes in `ClipVertex::custom` (4 floats). It's clipped and perspective-correctly interpolated like every other attribute.

## SIMD rules

- The scalar loop in `rasterizeTriangle` is the reference. The NEON loop must produce **byte-identical** color and depth output. Mirror every scalar operation in the same order (including the order of additions), and call the same scalar functions per lane where there is no vector equivalent (`std::pow`, texture fetches).
- This relies on `-ffp-contract=off` (CMakeLists.txt). Don't remove it: with FMA contraction the two loops round differently.
- Verify any change to either loop by rendering with `useSimd` off and on and comparing buffers, across shading modes, textures, culling, depth modes, thread counts, and a framebuffer width that isn't a multiple of 4.
- NEON groups are 4 pixels starting at a multiple of 4. Tile sizes are rounded up to a multiple of 4 so a group never straddles two tiles (whole-group stores write back masked lanes). At the right edge of a screen whose width isn't a multiple of 4, use the partial load/store helpers.

## Threading rules

- The geometry pass may only write its own `Chunk`. The raster pass may only write pixels inside its tile. Anything shared (settings, eye position, textures) is read-only during `drawMesh`.
- Output must stay byte-identical at every thread count and tile size. Tiles draw chunks in order and bins in order, which reproduces submission order. Check it by rendering at `threads` 1 and 0 and comparing the files.
- Per-pixel debug data (overdraw) is safe because tiles are disjoint. Anything per-frame and global needs a per-thread copy that is merged afterwards (see `Chunk::stats`).

## Git

Commits are authored by **Aadi Belwal** only. Don't add `Co-Authored-By` trailers or any Claude attribution to commits or PRs. Use small commits with imperative messages, one feature or concept each (e.g. "Add z-buffer depth test").
