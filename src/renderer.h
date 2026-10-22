// renderer.h — the software rendering pipeline.
//
//   drawMesh()
//     -> vertex stage     model/view/projection transform into clip space
//     -> clip stage       [Day 2] clip against the near plane (currently: reject)
//     -> project stage    perspective divide + viewport transform to pixels
//     -> cull stage       back-face culling (settings.backfaceCull)
//     -> raster stage     rasterizeTriangle(): edge functions, z-buffer, flat Lambert
//   endFrame()
//     -> debug views / color-depth reduction / upscale (driven by settings)
#pragma once

#include <cstdint>

#include "framebuffer.h"
#include "vecmath.h"
#include "mesh.h"
#include "render_settings.h"

struct RenderStats {
    int trianglesSubmitted = 0;
    int trianglesClipped = 0;  // rejected (or, from Day 2, split) by the near plane
    int trianglesCulled = 0;   // removed by back-face culling
    int trianglesDrawn = 0;    // reached the rasterizer
};

// A vertex after projection to the screen. Everything the rasterizer needs.
struct ScreenVertex {
    float x = 0, y = 0;  // pixel coordinates, origin top-left, y down
    float z = 0;         // depth in [0, 1]: 0 = near plane, 1 = far plane
    float invW = 0;      // 1 / clip.w — the key to perspective-correct interpolation (Day 2)
    Vec3 worldPos;       // for per-pixel lighting
    Vec3 normal;         // world-space normal
    Vec2 uv;
};

// Signed area * 2 of triangle (a, b, c) in screen space. Because screen y points
// down, triangles that are counter-clockwise in world space (front-facing) come
// out NEGATIVE here. Also the core of the barycentric rasterizer: for a pixel p,
// edgeFunction(a, b, p) / edgeFunction(a, b, c) is p's weight for vertex c.
inline float edgeFunction(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

class Renderer {
public:
    RenderSettings settings;
    Vec3 lightDir = normalize(Vec3{0.5f, 1.0f, 0.3f});  // direction TOWARD the light
    float ambient = 0.15f;  // light floor so faces turned away from the light aren't pure black

    void beginFrame(Framebuffer& fb, const Mat4& view, const Mat4& projection);
    void drawMesh(Framebuffer& fb, const Mesh& mesh, const Mat4& model, const Vec3& baseColor);
    void endFrame(Framebuffer& fb);

    const RenderStats& stats() const { return stats_; }

private:
    void rasterizeTriangle(Framebuffer& fb, const ScreenVertex& v0, const ScreenVertex& v1,
                           const ScreenVertex& v2, const Vec3& baseColor);
    void drawLine(Framebuffer& fb, int x0, int y0, int x1, int y1, uint32_t color);
    void drawTriangleEdges(Framebuffer& fb, const ScreenVertex& v0, const ScreenVertex& v1,
                           const ScreenVertex& v2, uint32_t color);

    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
    RenderStats stats_;
};
