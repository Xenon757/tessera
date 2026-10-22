// raster.h — rasterizer internals shared by the scalar pixel loop (renderer.cpp)
// and the NEON pixel loop (raster_neon.cpp). Not part of the public API.
#pragma once

#include "renderer.h"

// One triangle edge, prepared for the per-pixel inside test and the top-left fill rule.
//
// Fill rule: a pixel center exactly ON an edge (edge function == 0) belongs to the
// triangle only if that edge is a "top" edge (horizontal, interior below it) or a
// "left" edge. Two triangles sharing an edge walk it in opposite directions, so
// exactly one of them claims it: no pixel is drawn twice, and none is skipped.
//
// That only works if both triangles compute *exactly* the same value with opposite
// signs. In floating point, edgeFunction(a, b, p) isn't bit-for-bit
// -edgeFunction(b, a, p), because the subtractions happen in a different order. So
// each edge is evaluated with its endpoints in a canonical order (sorted by y, then
// x) and the sign fixed up afterwards. (GPUs get the same guarantee by snapping
// vertices to fixed point, which makes edge functions exact integers.)
struct Edge {
    float ax = 0, ay = 0, bx = 0, by = 0;  // endpoints in canonical order
    float sign = 1;                        // makes value() positive inside the triangle
    bool topLeft = false;

    Edge() = default;

    // `area` is the triangle's signed area; the edge runs a -> b in triangle order.
    Edge(const ScreenVertex& a, const ScreenVertex& b, float area) {
        // Classify as if the triangle had negative area (front-facing, which on our
        // y-down screen is counter-clockwise); for the other winding, flip the edge.
        float dx = b.x - a.x, dy = b.y - a.y;
        if (area > 0.0f) { dx = -dx; dy = -dy; }
        // Walking counter-clockwise, the interior is on the walker's left. So an edge
        // heading down has the interior to its right (a left edge), and a horizontal
        // edge heading left has the interior below it (a top edge).
        topLeft = dy > 0.0f || (dy == 0.0f && dx < 0.0f);

        const bool swap = b.y < a.y || (b.y == a.y && b.x < a.x);
        const ScreenVertex& lo = swap ? b : a;
        const ScreenVertex& hi = swap ? a : b;
        ax = lo.x; ay = lo.y; bx = hi.x; by = hi.y;
        // Inside means "same sign as area"; swapping the endpoints negates the value.
        sign = (area < 0.0f ? -1.0f : 1.0f) * (swap ? -1.0f : 1.0f);
    }

    float value(float px, float py) const { return sign * edgeFunction(ax, ay, bx, by, px, py); }
    bool inside(float w) const { return w > 0.0f || (w == 0.0f && topLeft); }
};


// The per-triangle values rasterizeTriangle computes before its pixel loop,
// bundled up to hand to the SIMD pixel loop.
struct TriangleSetup {
    const ScreenTriangle* tri = nullptr;
    Edge e[3];             // e[i] is opposite vertex i, so its value is vertex i's weight
    float invArea = 0.0f;  // 1 / |area|: turns edge values into barycentric weights
    int minX = 0, maxX = -1, minY = 0, maxY = -1;  // bounding box clipped to the tile
    Shading shading = Shading::Flat;
    Lighting flatLight;       // Flat: the whole triangle's lighting
    Lighting vertexLight[3];  // Gouraud: lighting at each vertex
    Vec3 baseColor;
    bool depthTest = false;
    bool countOverdraw = false;
};
