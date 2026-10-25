// render_settings.h — every knob the pipeline reads, in one place.
//
// Design rule: the pipeline never hard-codes a behavior that a mode might want to
// change. PS1 mode, debug views, SIMD, and threading are all just different values
// in this struct. Adding a feature = add a field here + read it in renderer.cpp.
//
// Fields marked [Day N] are declared now but not implemented yet; the renderer
// ignores them until that day's work lands.
#pragma once

#include <cstdint>

// Builds with the NEON pixel loop (raster_neon.cpp): ARM, natively, and WebAssembly,
// where Emscripten translates the NEON intrinsics to WebAssembly SIMD (CMakeLists.txt
// compiles just that file with -msimd128 -mfpu=neon and defines TESSERA_WASM_SIMD).
#if defined(__ARM_NEON) || defined(TESSERA_WASM_SIMD)
#define TESSERA_NEON 1
#endif

enum class TextureMapping {
    PerspectiveCorrect,  // interpolate attr/w and 1/w, divide per pixel
    Affine,              // PS1 style: interpolate UVs linearly in screen space (wobbly)
};

enum class TextureFilter {
    Nearest,   // the single closest texel: blocky up close (the PS1 look)
    Bilinear,  // blend the 4 closest texels by distance: smooth up close
};

enum class DepthMode {
    ZBuffer,        // [Day 1] per-pixel depth test
    OrderingTable,  // PS1 style: no depth buffer, draw triangles back-to-front by average depth
};

enum class ColorDepth {
    Bits24,  // 8 bits per channel
    Bits15,  // PS1 style: 5 bits per channel (optionally dithered)
};

enum class Shading {
    Flat,     // one normal per triangle
    Gouraud,  // light per vertex, interpolate color
    Phong,    // interpolate normal, light per pixel
};

inline const char* shadingName(Shading s) {
    switch (s) {
        case Shading::Flat: return "Flat";
        case Shading::Gouraud: return "Gouraud";
        case Shading::Phong: return "Phong";
    }
    return "?";
}

enum class DebugView {
    None,
    Wireframe,    // triangle edges drawn over the image
    Depth,        // linearized depth buffer as grayscale (near = white)
    Overdraw,     // heatmap of how many times each pixel was shaded
    ThreadTiles,  // color each screen tile by the thread that rasterized it
};

inline const char* debugViewName(DebugView v) {
    switch (v) {
        case DebugView::None: return "Shaded";
        case DebugView::Wireframe: return "Wireframe";
        case DebugView::Depth: return "Depth";
        case DebugView::Overdraw: return "Overdraw";
        case DebugView::ThreadTiles: return "Thread tiles";
    }
    return "?";
}

struct RenderSettings {
    // Geometry
    bool backfaceCull = true;
    bool vertexSnap = false;  // PS1: round screen-space vertices to whole pixels

    // Rasterization
    DepthMode depthMode = DepthMode::ZBuffer;
    int orderingTableSize = 1024;  // depth buckets between zNear and zFar (OrderingTable mode)
    TextureMapping textureMapping = TextureMapping::PerspectiveCorrect;
    TextureFilter textureFilter = TextureFilter::Bilinear;
    Shading shading = Shading::Phong;

    // Output
    ColorDepth colorDepth = ColorDepth::Bits24;
    bool dither = false;    // ordered dithering (the PS1's 4x4 matrix) when reducing color depth
    int internalScale = 1;  // render at 1/N resolution, then upscale with nearest neighbor (PS1: ~2-3)

    // Performance
    int threadCount = 0;      // threads for the geometry and raster passes; 0 = one per CPU core
    int tileSize = 64;        // screen tile size in pixels for binning
    bool useSimd = true;      // NEON pixel loop where available (TESSERA_NEON); the scalar loop is the reference
    int simdMinWidth = 8;     // triangles narrower than this (in pixels) use the scalar loop anyway:
                              // 4-pixel groups waste most lanes on tiny triangles (measured neutral at 8)

    // Debug
    DebugView debugView = DebugView::None;

    // ------------------------------------------------------------ presets
    static RenderSettings modern() { return RenderSettings{}; }

    static RenderSettings ps1() {
        RenderSettings s;
        s.vertexSnap = true;
        s.depthMode = DepthMode::OrderingTable;
        s.textureMapping = TextureMapping::Affine;
        s.textureFilter = TextureFilter::Nearest;
        s.shading = Shading::Gouraud;
        s.colorDepth = ColorDepth::Bits15;
        s.dither = true;
        s.internalScale = 3;
        return s;
    }
};
