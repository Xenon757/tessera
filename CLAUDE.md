# CLAUDE.md

Tessera is a real-time 3D **software rasterizer** in C++17: the whole graphics pipeline runs on the CPU. It's a portfolio project aimed at game engine programming internships (e.g. Insomniac's Engine & Tools intern), built over about a week. The roadmap and perf table are in `README.md`.

## Ground rules

- **No graphics APIs or rendering libraries.** No OpenGL, Metal, Vulkan, or bgfx, and no helper libraries that do rasterization, clipping, or shading. SDL2 is only for the window, input, and blitting the finished framebuffer. Writing the pipeline by hand is the whole point.
- **Aadi has to be able to explain every line in an interview.** Keep each change focused on one concept. Briefly explain non-obvious math such as barycentrics, perspective-correct interpolation, clipping, and fill rules, both in chat and in short code comments. Don't drop in large blocks of unexplained code.
- **Every pipeline behavior is driven by `RenderSettings`** (`src/render_settings.h`). New features add a field there and get read in `renderer.cpp`. Don't hard-code behavior a mode might want to change. PS1 mode, debug views, SIMD, and threading are all just settings values.
- **Keep a scalar code path.** NEON SIMD (Day 5) must sit behind `settings.useSimd` and `#if defined(__ARM_NEON)`, because the WebAssembly build (Day 7) can't use it.
- **Builds must stay warning-free** under `-Wall -Wextra`.
- `TODO(Day N)` comments mark where each day's work goes. Remove a TODO when its work lands, and tick the box in the README roadmap.

## Build and run (macOS)

```sh
brew install cmake sdl2          # once
cmake -B build                   # defaults to RelWithDebInfo
cmake --build build -j
./build/tessera [model.obj]      # windowed; title bar shows frame time
./build/tessera_headless [model.obj] [out.ppm] [frames]   # benchmark + image
```

- Run `tessera_headless` after any rasterizer change and check the output image (convert it with `sips -s format png out.ppm --out out.png`).
- WebAssembly (Day 7): `emcmake cmake -B build-web && cmake --build build-web`.

## Architecture

```
Renderer::drawMesh
  vertex   model/view/projection -> clip space
  clip     trivial frustum reject; Sutherland-Hodgman against the near plane only
  project  perspective divide + viewport -> ScreenVertex
  cull     back-face culling
  raster   rasterizeTriangle()  edge functions + barycentrics, z-buffer, flat Lambert
Renderer::endFrame -> debug views, color-depth reduction, upscale
```

| File | Role |
|---|---|
| `src/render_settings.h` | All knobs + `modern()` / `ps1()` presets |
| `src/renderer.{h,cpp}` | Pipeline stages, `edgeFunction`, `ScreenVertex`, `RenderStats` |
| `src/framebuffer.{h,cpp}` | Color (ARGB8888), depth (float), overdraw (uint16) buffers; `savePPM` |
| `src/mesh.{h,cpp}` | Unrolled triangle list (every 3 vertices = 1 tri), OBJ loader, `makeCube()` |
| `src/vecmath.h` | Vec2/3/4, Mat4 |
| `src/camera.h` | Orbit camera |
| `src/app_common.h` | Model loading + `fitToView` shared by both front ends |
| `src/main_sdl.cpp` | Windowed front end; also the Emscripten entry point |
| `src/main_headless.cpp` | Headless benchmark / image output |

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
- Profile before optimizing. On macOS, use Instruments (Time Profiler) or `xctrace`.

## Git

Commits are authored by **Aadi Belwal** only. Don't add `Co-Authored-By` trailers or any Claude attribution to commits or PRs. Use small commits with imperative messages, one feature or concept each (e.g. "Add z-buffer depth test").
