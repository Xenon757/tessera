#include "renderer.h"

#include <cmath>
#include <cstdlib>

void Renderer::beginFrame(Framebuffer& fb, const Mat4& view, const Mat4& projection) {
    view_ = view;
    projection_ = projection;
    stats_ = RenderStats{};
    fb.clear(packColor({0.08f, 0.09f, 0.11f}));
}

void Renderer::drawMesh(Framebuffer& fb, const Mesh& mesh, const Mat4& model, const Vec3& baseColor) {
    const Mat4 mvp = projection_ * view_ * model;
    const float w = static_cast<float>(fb.width());
    const float h = static_cast<float>(fb.height());

    for (size_t t = 0; t < mesh.triangleCount(); ++t) {
        ++stats_.trianglesSubmitted;
        const Vertex* in = &mesh.vertices[t * 3];

        // ---- Vertex stage: object space -> clip space.
        Vec4 clip[3];
        ScreenVertex sv[3];
        for (int i = 0; i < 3; ++i) {
            Vec4 p(in[i].position, 1.0f);
            clip[i] = mvp * p;
            sv[i].worldPos = (model * p).xyz();
            // NOTE: correct only for uniform scale. Use the inverse-transpose
            // ("normal matrix") if you add non-uniform scaling.
            sv[i].normal = normalize(model.transformDir(in[i].normal));
            sv[i].uv = in[i].uv;
        }

        // ---- Clip stage.
        // TODO(Day 2): clip against the near plane (z >= -w) instead of rejecting.
        // A triangle crossing the near plane becomes 1 or 2 triangles; interpolate
        // every attribute at the intersection points. Without this, geometry pops out
        // of existence as soon as one vertex goes behind the camera.
        bool rejected = false;
        for (const Vec4& c : clip)
            if (c.w <= 1e-6f || c.z < -c.w) rejected = true;
        if (rejected) {
            ++stats_.trianglesClipped;
            continue;
        }
        // Trivially reject triangles fully outside one side of the view frustum.
        auto allOutside = [&](auto test) { return test(clip[0]) && test(clip[1]) && test(clip[2]); };
        if (allOutside([](const Vec4& c) { return c.x < -c.w; }) ||
            allOutside([](const Vec4& c) { return c.x > c.w; }) ||
            allOutside([](const Vec4& c) { return c.y < -c.w; }) ||
            allOutside([](const Vec4& c) { return c.y > c.w; }) ||
            allOutside([](const Vec4& c) { return c.z > c.w; })) {
            ++stats_.trianglesClipped;
            continue;
        }

        // ---- Project stage: perspective divide + viewport transform.
        for (int i = 0; i < 3; ++i) {
            sv[i].invW = 1.0f / clip[i].w;
            float ndcX = clip[i].x * sv[i].invW;
            float ndcY = clip[i].y * sv[i].invW;
            float ndcZ = clip[i].z * sv[i].invW;
            sv[i].x = (ndcX * 0.5f + 0.5f) * w;
            sv[i].y = (1.0f - (ndcY * 0.5f + 0.5f)) * h;  // flip: screen y points down
            sv[i].z = ndcZ * 0.5f + 0.5f;
            // TODO(Day 4): if (settings.vertexSnap) snap x/y to the pixel grid here.
        }

        // ---- Cull stage. Front-facing triangles have negative area in screen space.
        float area = edgeFunction(sv[0].x, sv[0].y, sv[1].x, sv[1].y, sv[2].x, sv[2].y);
        if (settings.backfaceCull && area >= 0.0f) {
            ++stats_.trianglesCulled;
            continue;
        }

        // ---- Raster stage.
        // TODO(Day 4): with DepthMode::OrderingTable, don't rasterize immediately —
        // bucket triangles by average depth and draw back-to-front in endFrame().
        rasterizeTriangle(fb, sv[0], sv[1], sv[2], baseColor);
        ++stats_.trianglesDrawn;

        if (settings.debugView == DebugView::Wireframe)
            drawTriangleEdges(fb, sv[0], sv[1], sv[2], packColor({1.0f, 0.55f, 0.1f}));
    }
}

void Renderer::rasterizeTriangle(Framebuffer& fb, const ScreenVertex& v0, const ScreenVertex& v1,
                                 const ScreenVertex& v2, const Vec3& baseColor) {
    // =========================================================================
    // TODO(Day 1): THIS IS WHERE YOU START.
    //
    // Replace this placeholder with a filled-triangle rasterizer:
    //   1. Compute the triangle's bounding box in pixels; clamp it to the screen.
    //   2. For each pixel center (x + 0.5, y + 0.5) in the box, compute the three
    //      edge functions w0, w1, w2 with edgeFunction() (see renderer.h).
    //   3. The pixel is inside if all three have the same sign as `area`
    //      (front-facing triangles have negative area here). Watch the sign!
    //   4. Barycentrics: b0 = w0 / area, etc. Interpolate z, test against
    //      fb.depthData() (smaller = closer), and write color + depth on pass.
    //   5. Shade: start with flat Lambert: max(dot(faceNormal, lightDir), 0).
    //
    // Day 2 adds perspective-correct interpolation (use invW) and Phong shading.
    // Day 3 adds a top-left fill rule, tiling, and threads.
    // =========================================================================
    (void)baseColor;
    drawTriangleEdges(fb, v0, v1, v2, packColor({0.85f, 0.85f, 0.9f}));
}

void Renderer::endFrame(Framebuffer& fb) {
    switch (settings.debugView) {
        case DebugView::None:
        case DebugView::Wireframe:
            break;  // wireframe is drawn per-triangle in drawMesh
        case DebugView::Depth:
            // TODO(Day 3): map fb.depthData() to grayscale. Raw depth is very
            // non-linear; linearize it with zNear/zFar so the image isn't all white.
            break;
        case DebugView::Overdraw:
            // TODO(Day 3): increment fb.overdrawData() per shaded pixel in the
            // rasterizer, then map counts to a heat ramp here.
            break;
        case DebugView::ThreadTiles:
            // TODO(Day 3): tint each tile by the worker thread that rasterized it.
            break;
    }
    // TODO(Day 4): color-depth reduction (Bits15 + dither) and internalScale upscale.
    (void)fb;
}

void Renderer::drawLine(Framebuffer& fb, int x0, int y0, int x1, int y1, uint32_t color) {
    // Bresenham. setPixel bounds-checks, so off-screen segments are safe (if slow).
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        fb.setPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Renderer::drawTriangleEdges(Framebuffer& fb, const ScreenVertex& v0, const ScreenVertex& v1,
                                 const ScreenVertex& v2, uint32_t color) {
    auto ix = [](float v) { return static_cast<int>(std::floor(v)); };
    drawLine(fb, ix(v0.x), ix(v0.y), ix(v1.x), ix(v1.y), color);
    drawLine(fb, ix(v1.x), ix(v1.y), ix(v2.x), ix(v2.y), color);
    drawLine(fb, ix(v2.x), ix(v2.y), ix(v0.x), ix(v0.y), color);
}
