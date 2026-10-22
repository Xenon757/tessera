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

enum class TextureMapping {
    PerspectiveCorrect,  // [Day 2] interpolate attr/w and 1/w, divide per pixel
    Affine,              // [Day 4] PS1 style: interpolate attrs linearly in screen space (wobbly)
};

enum class DepthMode {
    ZBuffer,        // [Day 1] per-pixel depth test
    OrderingTable,  // [Day 4] PS1 style: no depth buffer, sort triangles back-to-front by average depth
};

enum class ColorDepth {
    Bits24,  // 8 bits per channel
    Bits15,  // [Day 4] PS1 style: 5 bits per channel (optionally dithered)
};

enum class Shading {
    Flat,     // [Day 2] one normal per triangle
    Gouraud,  // [Day 2] light per vertex, interpolate color
    Phong,    // [Day 2] interpolate normal, light per pixel
};

enum class DebugView {
    None,
    Wireframe,    // triangle edges drawn over the image
    Depth,        // [Day 3] visualize the depth buffer as grayscale
    Overdraw,     // [Day 3] heatmap of how many times each pixel was shaded
    ThreadTiles,  // [Day 3] color each screen tile by the thread that rasterized it
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
    bool vertexSnap = false;  // [Day 4] PS1: snap screen-space vertices to an integer grid

    // Rasterization
    DepthMode depthMode = DepthMode::ZBuffer;
    TextureMapping textureMapping = TextureMapping::PerspectiveCorrect;
    Shading shading = Shading::Phong;

    // Output
    ColorDepth colorDepth = ColorDepth::Bits24;
    bool dither = false;    // [Day 4] ordered (Bayer) dithering when reducing color depth
    int internalScale = 1;  // [Day 4] render at 1/N resolution, then upscale (PS1: ~2-3)

    // Performance
    int threadCount = 1;      // [Day 3] rasterizer worker threads
    int tileSize = 64;        // [Day 3] screen tile size in pixels for binning
    bool useSimd = false;     // [Day 5] NEON inner loop (scalar path stays as fallback for WASM)

    // Debug
    DebugView debugView = DebugView::None;

    // ------------------------------------------------------------ presets
    static RenderSettings modern() { return RenderSettings{}; }

    static RenderSettings ps1() {
        RenderSettings s;
        s.vertexSnap = true;
        s.depthMode = DepthMode::OrderingTable;
        s.textureMapping = TextureMapping::Affine;
        s.shading = Shading::Gouraud;
        s.colorDepth = ColorDepth::Bits15;
        s.dither = true;
        s.internalScale = 3;
        return s;
    }
};
