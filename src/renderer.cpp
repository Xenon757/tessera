#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void Renderer::beginFrame(Framebuffer& fb, const Mat4& view, const Mat4& projection) {
    view_ = view;
    projection_ = projection;
    // The view matrix is [R | t] and maps the eye to the origin: R * eye + t = 0.
    // R is a rotation, so its inverse is its transpose: eye = -R^T * t.
    // Row i of R^T is column i of R, so eye_i = -(R[0][i]*t0 + R[1][i]*t1 + R[2][i]*t2).
    const Vec3 t{view.m[0][3], view.m[1][3], view.m[2][3]};
    eye_ = -Vec3{view.m[0][0] * t.x + view.m[1][0] * t.y + view.m[2][0] * t.z,
                 view.m[0][1] * t.x + view.m[1][1] * t.y + view.m[2][1] * t.z,
                 view.m[0][2] * t.x + view.m[1][2] * t.y + view.m[2][2] * t.z};
    // Recover the near/far distances from the perspective matrix (see Mat4::perspective):
    // m22 = (f+n)/(n-f) and m23 = 2fn/(n-f), so m22 - 1 = 2f/(n-f) and m22 + 1 = 2n/(n-f).
    // Dividing m23 by each leaves n and f. Only the depth debug view needs them.
    zNear_ = projection.m[2][3] / (projection.m[2][2] - 1.0f);
    zFar_ = projection.m[2][3] / (projection.m[2][2] + 1.0f);
    stats_ = RenderStats{};
    fb.clear(packColor({0.08f, 0.09f, 0.11f}));
}

namespace {

ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t) {
    return {a.pos + (b.pos - a.pos) * t, a.worldPos + (b.worldPos - a.worldPos) * t,
            a.normal + (b.normal - a.normal) * t, a.uv + (b.uv - a.uv) * t};
}

// Sutherland-Hodgman clipping against the near plane. In clip space "in front of
// the near plane" is z >= -w, i.e. d = z + w >= 0. Walk each edge a->b: keep a if
// it's inside, and if the edge crosses the plane (d changes sign) emit the crossing
// point. d is linear along the edge, so it hits 0 at t = dA / (dA - dB).
//
// Interpolating in clip space (before the divide by w) is exact: projection is a
// linear map on homogeneous coordinates, so straight lines and linear attributes
// survive it. After the divide they wouldn't.
//
// One plane cuts at most one corner off a triangle, so the output has 3 or 4 verts.
int clipNear(const ClipVertex in[3], ClipVertex out[4]) {
    int n = 0;
    for (int i = 0; i < 3; ++i) {
        const ClipVertex& a = in[i];
        const ClipVertex& b = in[(i + 1) % 3];
        const float da = a.pos.z + a.pos.w;
        const float db = b.pos.z + b.pos.w;
        if (da >= 0.0f) out[n++] = a;
        if ((da >= 0.0f) != (db >= 0.0f)) out[n++] = lerp(a, b, da / (da - db));
    }
    return n;
}

}  // namespace

void Renderer::drawMesh(Framebuffer& fb, const Mesh& mesh, const Mat4& model, const Vec3& baseColor,
                        const Texture* texture) {
    const Mat4 mvp = projection_ * view_ * model;

    for (size_t t = 0; t < mesh.triangleCount(); ++t) {
        ++stats_.trianglesSubmitted;
        const Vertex* in = &mesh.vertices[t * 3];

        // ---- Vertex stage: object space -> clip space.
        ClipVertex cv[3];
        for (int i = 0; i < 3; ++i) {
            Vec4 p(in[i].position, 1.0f);
            cv[i].pos = mvp * p;
            cv[i].worldPos = (model * p).xyz();
            // NOTE: correct only for uniform scale. Use the inverse-transpose
            // ("normal matrix") if you add non-uniform scaling.
            cv[i].normal = normalize(model.transformDir(in[i].normal));
            cv[i].uv = in[i].uv;
        }

        // ---- Clip stage.
        // Trivially reject triangles fully outside one plane of the view frustum.
        auto allOutside = [&](auto test) { return test(cv[0].pos) && test(cv[1].pos) && test(cv[2].pos); };
        if (allOutside([](const Vec4& c) { return c.x < -c.w; }) ||
            allOutside([](const Vec4& c) { return c.x > c.w; }) ||
            allOutside([](const Vec4& c) { return c.y < -c.w; }) ||
            allOutside([](const Vec4& c) { return c.y > c.w; }) ||
            allOutside([](const Vec4& c) { return c.z < -c.w; }) ||
            allOutside([](const Vec4& c) { return c.z > c.w; })) {
            ++stats_.trianglesClipped;
            continue;
        }

        // Only the near plane gets real clipping. It must: behind the camera w <= 0
        // and the perspective divide flips or explodes. The other planes don't need
        // it, because the rasterizer clamps its bounding box to the screen (an
        // unlimited "guard band"), and the far plane is rare enough to just accept.
        const bool crossesNear = cv[0].pos.z < -cv[0].pos.w || cv[1].pos.z < -cv[1].pos.w ||
                                 cv[2].pos.z < -cv[2].pos.w;
        if (!crossesNear) {
            drawClippedTriangle(fb, cv[0], cv[1], cv[2], baseColor, texture);
            continue;
        }
        ++stats_.trianglesClipped;
        ClipVertex poly[4];
        const int n = clipNear(cv, poly);
        // Fan-triangulate the clipped polygon. Clipping preserves winding order.
        for (int i = 1; i + 1 < n; ++i)
            drawClippedTriangle(fb, poly[0], poly[i], poly[i + 1], baseColor, texture);
    }
}

void Renderer::drawClippedTriangle(Framebuffer& fb, const ClipVertex& c0, const ClipVertex& c1,
                                   const ClipVertex& c2, const Vec3& baseColor, const Texture* texture) {
    const float w = static_cast<float>(fb.width());
    const float h = static_cast<float>(fb.height());

    // ---- Project stage: perspective divide + viewport transform.
    // w > 0 is guaranteed here: z >= -w works out to "at least zNear in front of the camera".
    const ClipVertex* cv[3] = {&c0, &c1, &c2};
    ScreenVertex sv[3];
    for (int i = 0; i < 3; ++i) {
        const Vec4& clip = cv[i]->pos;
        sv[i].invW = 1.0f / clip.w;
        float ndcX = clip.x * sv[i].invW;
        float ndcY = clip.y * sv[i].invW;
        float ndcZ = clip.z * sv[i].invW;
        sv[i].x = (ndcX * 0.5f + 0.5f) * w;
        sv[i].y = (1.0f - (ndcY * 0.5f + 0.5f)) * h;  // flip: screen y points down
        sv[i].z = ndcZ * 0.5f + 0.5f;
        sv[i].worldPos = cv[i]->worldPos;
        sv[i].normal = cv[i]->normal;
        sv[i].uv = cv[i]->uv;
        // TODO(Day 4): if (settings.vertexSnap) snap x/y to the pixel grid here.
    }

    // ---- Cull stage. Front-facing triangles have negative area in screen space.
    float area = edgeFunction(sv[0].x, sv[0].y, sv[1].x, sv[1].y, sv[2].x, sv[2].y);
    if (settings.backfaceCull && area >= 0.0f) {
        ++stats_.trianglesCulled;
        return;
    }

    // ---- Raster stage.
    // TODO(Day 4): with DepthMode::OrderingTable, don't rasterize immediately —
    // bucket triangles by average depth and draw back-to-front in endFrame().
    rasterizeTriangle(fb, sv[0], sv[1], sv[2], baseColor, texture);
    ++stats_.trianglesDrawn;

    if (settings.debugView == DebugView::Wireframe)
        drawTriangleEdges(fb, sv[0], sv[1], sv[2], packColor({1.0f, 0.55f, 0.1f}));
}

void Renderer::rasterizeTriangle(Framebuffer& fb, const ScreenVertex& v0, const ScreenVertex& v1,
                                 const ScreenVertex& v2, const Vec3& baseColor, const Texture* texture) {
    // Signed area * 2. Zero area = degenerate (a line or point): nothing to fill,
    // and dividing by it below would blow up.
    const float area = edgeFunction(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0.0f) return;
    const float invArea = 1.0f / area;

    // Bounding box in whole pixels, clamped to the screen. The trivial-reject in
    // drawMesh only drops triangles fully off one side, so big ones still overhang.
    const int minX = std::max(0, static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x}))));
    const int minY = std::max(0, static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y}))));
    const int maxX = std::min(fb.width() - 1, static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x}))));
    const int maxY = std::min(fb.height() - 1, static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y}))));

    // Every mode uses the same lighting function; they differ in *where* it runs.
    // Flat: once per triangle. Gouraud: once per vertex, then the result is
    // interpolated (cheap, but highlights smear or vanish inside big triangles).
    // Phong: interpolate the normal and light every pixel (smooth, but costly).
    const Shading shading = settings.shading;
    Lighting flatLight;
    Lighting vertexLight[3];
    if (shading == Shading::Flat) {
        // Meshes wind counter-clockwise seen from outside, so by the right-hand
        // rule this cross product points outward. Lit at the centroid.
        const Vec3 faceNormal = normalize(cross(v1.worldPos - v0.worldPos, v2.worldPos - v0.worldPos));
        flatLight = light(faceNormal, (v0.worldPos + v1.worldPos + v2.worldPos) / 3.0f);
    } else if (shading == Shading::Gouraud) {
        vertexLight[0] = light(v0.normal, v0.worldPos);
        vertexLight[1] = light(v1.normal, v1.worldPos);
        vertexLight[2] = light(v2.normal, v2.worldPos);
    }
    const bool needsAttributes = shading != Shading::Flat || texture;
    const bool bilinear = settings.textureFilter == TextureFilter::Bilinear;

    const bool depthTest = settings.depthMode == DepthMode::ZBuffer;
    const bool countOverdraw = settings.debugView == DebugView::Overdraw;
    uint32_t* colorBuf = fb.colorData();
    float* depthBuf = fb.depthData();
    uint16_t* overdrawBuf = fb.overdrawData();

    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;  // sample at the pixel center
        for (int x = minX; x <= maxX; ++x) {
            const float px = x + 0.5f;

            // Each edge function is p's signed area against the edge opposite one
            // vertex, so dividing by the full area gives that vertex's barycentric
            // weight: b0 + b1 + b2 = 1. Dividing by `area` also folds in its sign,
            // so "inside" is simply all weights >= 0 whichever way the triangle
            // winds (front faces are negative here; back faces when culling is off).
            const float b0 = edgeFunction(v1.x, v1.y, v2.x, v2.y, px, py) * invArea;
            const float b1 = edgeFunction(v2.x, v2.y, v0.x, v0.y, px, py) * invArea;
            const float b2 = edgeFunction(v0.x, v0.y, v1.x, v1.y, px, py) * invArea;
            // TODO(Day 3): top-left fill rule. With >= 0, pixels exactly on an edge
            // shared by two triangles get drawn by both.
            if (b0 < 0.0f || b1 < 0.0f || b2 < 0.0f) continue;

            const size_t idx = static_cast<size_t>(y) * fb.width() + x;
            if (depthTest) {
                // z after the perspective divide is affine in screen space, so plain
                // barycentric interpolation is exact here (unlike the attributes below).
                const float z = b0 * v0.z + b1 * v1.z + b2 * v2.z;
                if (z >= depthBuf[idx]) continue;  // smaller = closer
                depthBuf[idx] = z;
            }
            // Count pixels that get shaded. Depth-rejected ones never reach the
            // shading below, so they're (nearly) free and not counted.
            if (countOverdraw && overdrawBuf[idx] < UINT16_MAX) ++overdrawBuf[idx];

            // Perspective-correct weights. b0..b2 are linear in *screen* space, but
            // attributes (normals, UVs, ...) vary linearly across the triangle in
            // *3D*; the divide by w bends that, so equal steps on screen are unequal
            // steps on the surface. What IS linear on screen is attr/w and 1/w. So
            // interpolate both and divide: attr = sum(b_i * attr_i / w_i) / sum(b_i / w_i).
            // Folding that division into the weights gives p_i, which also sum to 1
            // and work for any attribute.
            float p0 = 0.0f, p1 = 0.0f, p2 = 0.0f;
            if (needsAttributes) {
                const float q0 = b0 * v0.invW, q1 = b1 * v1.invW, q2 = b2 * v2.invW;
                const float invSum = 1.0f / (q0 + q1 + q2);
                p0 = q0 * invSum;
                p1 = q1 * invSum;
                p2 = q2 * invSum;
            }

            Vec3 albedo = baseColor;
            if (texture) {
                // TODO(Day 4): with TextureMapping::Affine, interpolate UVs with the
                // screen-space b0..b2 instead (the PS1's texture wobble).
                const Vec2 uv = v0.uv * p0 + v1.uv * p1 + v2.uv * p2;
                albedo = albedo * (bilinear ? texture->sampleBilinear(uv) : texture->sampleNearest(uv));
            }

            Lighting lit = flatLight;
            if (shading == Shading::Gouraud) {
                lit.diffuse = p0 * vertexLight[0].diffuse + p1 * vertexLight[1].diffuse + p2 * vertexLight[2].diffuse;
                lit.specular = p0 * vertexLight[0].specular + p1 * vertexLight[1].specular + p2 * vertexLight[2].specular;
            } else if (shading == Shading::Phong) {
                // Interpolated unit vectors come out shorter than 1; renormalize.
                const Vec3 n = normalize(v0.normal * p0 + v1.normal * p1 + v2.normal * p2);
                const Vec3 pos = v0.worldPos * p0 + v1.worldPos * p1 + v2.worldPos * p2;
                lit = light(n, pos);
            }
            // Diffuse tints by the surface color; the highlight is the light's own
            // (white) color reflected, so it's added on top untinted.
            colorBuf[idx] = packColor(albedo * lit.diffuse + Vec3{1, 1, 1} * lit.specular);
        }
    }
}

// Blinn-Phong lighting at one surface point.
// Diffuse (Lambert's cosine law): a surface receives light in proportion to
// dot(N, L), clamped at 0 for surfaces facing away; plus an ambient floor.
// Specular: H is the "half vector" halfway between the light and view directions.
// When N lines up with H, the surface mirrors the light straight at the eye.
// pow(dot(N, H), shininess) turns that into a highlight; higher = tighter.
Renderer::Lighting Renderer::light(const Vec3& normal, const Vec3& worldPos) const {
    const float nDotL = dot(normal, lightDir);
    Lighting out;
    out.diffuse = ambient + (1.0f - ambient) * std::max(nDotL, 0.0f);
    if (nDotL > 0.0f) {  // no highlight on the side facing away from the light
        const Vec3 toEye = normalize(eye_ - worldPos);
        const Vec3 halfVec = normalize(lightDir + toEye);
        out.specular = specular * std::pow(std::max(dot(normal, halfVec), 0.0f), shininess);
    }
    return out;
}

void Renderer::endFrame(Framebuffer& fb) {
    switch (settings.debugView) {
        case DebugView::None:
        case DebugView::Wireframe:
            break;  // wireframe is drawn per-triangle in drawMesh
        case DebugView::Depth:
            showDepth(fb);
            break;
        case DebugView::Overdraw:
            showOverdraw(fb);
            break;
        case DebugView::ThreadTiles:
            // TODO(Day 3): tint each tile by the worker thread that rasterized it.
            break;
    }
    // TODO(Day 4): color-depth reduction (Bits15 + dither) and internalScale upscale.
    (void)fb;
}

// Depth buffer as grayscale: near = white, far = dark, background = black.
// Stored depth is ~1/distance, so almost all of its range is used up right in
// front of the camera and a raw view is nearly all white. Undo the projection to
// get linear view-space distance, then stretch the visible range to full contrast.
void Renderer::showDepth(Framebuffer& fb) const {
    const size_t count = static_cast<size_t>(fb.width()) * fb.height();
    const float* depth = fb.depthData();
    uint32_t* color = fb.colorData();
    const float n = zNear_, f = zFar_;
    const bool perspective = n > 0.0f && f > n;  // otherwise just use raw depth

    // Inverse of the projection's depth mapping: NDC z -> distance in front of the eye.
    auto linearize = [&](float d) {
        if (!perspective) return d;
        const float ndcZ = d * 2.0f - 1.0f;
        return 2.0f * n * f / (f + n - ndcZ * (f - n));
    };

    float lo = INFINITY, hi = -INFINITY;
    for (size_t i = 0; i < count; ++i) {
        if (depth[i] >= 1.0f) continue;  // background: nothing was drawn here
        const float d = linearize(depth[i]);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
    }
    const float scale = hi > lo ? 1.0f / (hi - lo) : 0.0f;
    for (size_t i = 0; i < count; ++i) {
        if (depth[i] >= 1.0f) {
            color[i] = packColor({0, 0, 0});
            continue;
        }
        const float g = 1.0f - 0.85f * (linearize(depth[i]) - lo) * scale;  // farthest stays visible
        color[i] = packColor({g, g, g});
    }
}

// How many times each pixel was shaded, as a heat ramp. 1 is ideal; anything
// above that is shading work thrown away when a closer surface overwrote it.
void Renderer::showOverdraw(Framebuffer& fb) const {
    static const Vec3 ramp[] = {
        {0.0f, 0.0f, 0.0f},   // 0: nothing drawn
        {0.1f, 0.2f, 0.6f},   // 1: blue
        {0.1f, 0.7f, 0.3f},   // 2: green
        {0.95f, 0.85f, 0.1f}, // 3: yellow
        {1.0f, 0.5f, 0.0f},   // 4: orange
        {0.9f, 0.1f, 0.1f},   // 5+: red
    };
    const int last = static_cast<int>(sizeof(ramp) / sizeof(ramp[0])) - 1;
    const size_t count = static_cast<size_t>(fb.width()) * fb.height();
    const uint16_t* overdraw = fb.overdrawData();
    uint32_t* color = fb.colorData();
    for (size_t i = 0; i < count; ++i) color[i] = packColor(ramp[std::min<int>(overdraw[i], last)]);
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
