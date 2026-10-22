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
    Bits15,  // [Day 4] PS1 style: 5 bits per channel (optionally dithered)
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
    bool dither = false;    // [Day 4] ordered (Bayer) dithering when reducing color depth
    int internalScale = 1;  // [Day 4] render at 1/N resolution, then upscale (PS1: ~2-3)

    // Performance
    int threadCount = 0;      // threads for the geometry and raster passes; 0 = one per CPU core
    int tileSize = 64;        // screen tile size in pixels for binning
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
        s.textureFilter = TextureFilter::Nearest;
        s.shading = Shading::Gouraud;
        s.colorDepth = ColorDepth::Bits15;
        s.dither = true;
        s.internalScale = 3;
        return s;
    }
};
