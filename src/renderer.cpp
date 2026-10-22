#include "renderer.h"

#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void Renderer::beginFrame(Framebuffer& output, const Mat4& view, const Mat4& projection) {
    const int scale = std::max(1, settings.internalScale);
    if (scale > 1) {
        const int w = std::max(1, output.width() / scale), h = std::max(1, output.height() / scale);
        if (lowRes_.width() != w || lowRes_.height() != h) lowRes_.resize(w, h);
    }
    Framebuffer& fb = renderTarget(output);
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

void Renderer::drawMesh(Framebuffer& output, const Mesh& mesh, const Mat4& model, const Vec3& baseColor,
                        const Texture* texture) {
    Framebuffer& fb = renderTarget(output);
    const Mat4 mvp = projection_ * view_ * model;

    // Cut the screen into tileSize x tileSize tiles (partial ones at the edges).
    viewportW_ = fb.width();
    viewportH_ = fb.height();
    // A multiple of 4, so the SIMD loop's aligned 4-pixel groups never straddle two tiles.
    tileSize_ = (std::max(8, settings.tileSize) + 3) & ~3;
    tilesX_ = (viewportW_ + tileSize_ - 1) / tileSize_;
    tilesY_ = (viewportH_ + tileSize_ - 1) / tileSize_;
    const int tileCount = tilesX_ * tilesY_;

    int threads = settings.threadCount > 0 ? settings.threadCount
                                           : static_cast<int>(std::thread::hardware_concurrency());
#ifdef __EMSCRIPTEN__
    threads = 1;  // the default WebAssembly build has no pthreads
#endif
    threads = std::max(1, threads);
    if (!pool_ || pool_->size() != threads) pool_ = std::make_unique<ThreadPool>(threads);

    // Several chunks per thread, so a thread that lands a slow core (or a chunk of
    // near-plane clipping) doesn't hold everyone up: the others take more chunks.
    const int chunkCount = threads == 1 ? 1 : threads * 4;
    chunks_.resize(chunkCount);
    for (Chunk& c : chunks_) {
        c.tris.clear();
        c.bins.resize(tileCount);
        for (auto& bin : c.bins) bin.clear();
        c.stats = RenderStats{};
    }

    // ---- Geometry pass: each chunk transforms, clips, culls, and bins a slice of
    // the mesh. Chunks only write their own output, so they run in parallel freely.
    const size_t triCount = mesh.triangleCount();
    pool_->parallelFor(chunkCount, [&](int c, int) {
        geometryPass(mesh, model, mvp, triCount * c / chunkCount, triCount * (c + 1) / chunkCount, chunks_[c]);
    });

    // ---- Raster pass: each tile draws the triangles binned to it. Tiles don't
    // overlap, so no two threads ever touch the same pixel: no locks needed.
    tileThread_.assign(tileCount, -1);
    sortScratch_.resize(pool_->size());
    pool_->parallelFor(tileCount, [&](int tile, int thread) {
        if (rasterizeTile(fb, tile, thread, baseColor, texture)) tileThread_[tile] = thread;
    });

    for (const Chunk& c : chunks_) {
        stats_.trianglesSubmitted += c.stats.trianglesSubmitted;
        stats_.trianglesClipped += c.stats.trianglesClipped;
        stats_.trianglesCulled += c.stats.trianglesCulled;
        stats_.trianglesDrawn += c.stats.trianglesDrawn;
    }

    // Lines cross tile boundaries, so the wireframe overlay is drawn afterwards.
    if (settings.debugView == DebugView::Wireframe)
        for (const Chunk& c : chunks_)
            for (const ScreenTriangle& t : c.tris)
                drawTriangleEdges(fb, t.v[0], t.v[1], t.v[2], packColor({1.0f, 0.55f, 0.1f}));
}

void Renderer::geometryPass(const Mesh& mesh, const Mat4& model, const Mat4& mvp, size_t begin, size_t end,
                            Chunk& out) const {
    for (size_t t = begin; t < end; ++t) {
        ++out.stats.trianglesSubmitted;
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
            ++out.stats.trianglesClipped;
            continue;
        }

        // Only the near plane gets real clipping. It must: behind the camera w <= 0
        // and the perspective divide flips or explodes. The other planes don't need
        // it, because the rasterizer clamps its bounding box to the screen (an
        // unlimited "guard band"), and the far plane is rare enough to just accept.
        const bool crossesNear = cv[0].pos.z < -cv[0].pos.w || cv[1].pos.z < -cv[1].pos.w ||
                                 cv[2].pos.z < -cv[2].pos.w;
        if (!crossesNear) {
            emitTriangle(cv[0], cv[1], cv[2], out);
            continue;
        }
        ++out.stats.trianglesClipped;
        ClipVertex poly[4];
        const int n = clipNear(cv, poly);
        // Fan-triangulate the clipped polygon. Clipping preserves winding order.
        for (int i = 1; i + 1 < n; ++i) emitTriangle(poly[0], poly[i], poly[i + 1], out);
    }
}

void Renderer::emitTriangle(const ClipVertex& c0, const ClipVertex& c1, const ClipVertex& c2,
                            Chunk& out) const {
    const float w = static_cast<float>(viewportW_);
    const float h = static_cast<float>(viewportH_);

    // ---- Project stage: perspective divide + viewport transform.
    // w > 0 is guaranteed here: z >= -w works out to "at least zNear in front of the camera".
    const ClipVertex* cv[3] = {&c0, &c1, &c2};
    ScreenTriangle tri;
    ScreenVertex* sv = tri.v;
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
        if (settings.vertexSnap) {
            // PS1: the GTE produced whole-pixel screen coordinates, no subpixel bits.
            // Moving geometry then jumps a pixel at a time: the classic PS1 jitter.
            // Pixel centers can now land exactly on edges, which the fill rule handles.
            sv[i].x = std::round(sv[i].x);
            sv[i].y = std::round(sv[i].y);
        }
    }

    // ---- Cull stage. Front-facing triangles have negative area in screen space.
    // (After snapping: a sliver can collapse to zero area or flip, and then it's culled.)
    float area = edgeFunction(sv[0].x, sv[0].y, sv[1].x, sv[1].y, sv[2].x, sv[2].y);
    if (settings.backfaceCull && area >= 0.0f) {
        ++out.stats.trianglesCulled;
        return;
    }

    // ---- Bin stage. Bounding box in whole pixels, clamped to the screen. The
    // trivial reject only drops triangles fully off one side, so big ones overhang.
    tri.minX = std::max(0, static_cast<int>(std::floor(std::min({sv[0].x, sv[1].x, sv[2].x}))));
    tri.minY = std::max(0, static_cast<int>(std::floor(std::min({sv[0].y, sv[1].y, sv[2].y}))));
    tri.maxX = std::min(viewportW_ - 1, static_cast<int>(std::ceil(std::max({sv[0].x, sv[1].x, sv[2].x}))));
    tri.maxY = std::min(viewportH_ - 1, static_cast<int>(std::ceil(std::max({sv[0].y, sv[1].y, sv[2].y}))));
    if (tri.minX > tri.maxX || tri.minY > tri.maxY) return;  // nothing on screen

    if (settings.depthMode == DepthMode::OrderingTable) {
        // PS1 ordering table: one depth per triangle, the average of its vertices'
        // view-space depths (clip w is exactly that), quantized into a bucket. The
        // raster pass draws buckets far to near: the painter's algorithm.
        const float depth = (c0.pos.w + c1.pos.w + c2.pos.w) / 3.0f;
        const int size = std::max(1, settings.orderingTableSize);
        const float t = (depth - zNear_) / (zFar_ - zNear_);
        tri.otBucket = std::clamp(static_cast<int>(t * size), 0, size - 1);
    }

    const uint32_t index = static_cast<uint32_t>(out.tris.size());
    out.tris.push_back(tri);
    ++out.stats.trianglesDrawn;
    // Add it to every tile its bounding box overlaps. Conservative: a thin diagonal
    // triangle lands in tiles it doesn't actually cover; the rasterizer finds no
    // pixels there and moves on.
    for (int ty = tri.minY / tileSize_; ty <= tri.maxY / tileSize_; ++ty)
        for (int tx = tri.minX / tileSize_; tx <= tri.maxX / tileSize_; ++tx)
            out.bins[ty * tilesX_ + tx].push_back(index);
}

// Returns whether the tile had any triangles to draw.
bool Renderer::rasterizeTile(Framebuffer& fb, int tile, int thread, const Vec3& baseColor,
                             const Texture* texture) {
    const int tx = tile % tilesX_, ty = tile / tilesX_;
    const TileRect rect{tx * tileSize_, ty * tileSize_, std::min((tx + 1) * tileSize_, viewportW_) - 1,
                        std::min((ty + 1) * tileSize_, viewportH_) - 1};

    if (settings.depthMode == DepthMode::OrderingTable) {
        // Gather this tile's triangles in submission order, then stable-sort far to
        // near. Stable keeps submission order within a bucket, so the result matches
        // one global sort and doesn't depend on thread count.
        std::vector<TriRef>& refs = sortScratch_[thread];
        refs.clear();
        for (int c = 0; c < static_cast<int>(chunks_.size()); ++c)
            for (uint32_t index : chunks_[c].bins[tile]) refs.push_back({chunks_[c].tris[index].otBucket, c, index});
        std::stable_sort(refs.begin(), refs.end(), [](const TriRef& a, const TriRef& b) { return a.bucket > b.bucket; });
        for (const TriRef& r : refs) rasterizeTriangle(fb, chunks_[r.chunk].tris[r.index], rect, baseColor, texture);
        return !refs.empty();
    }
    // Chunks in order, and each bin in order, reproduces the original submission
    // order. That matters: with equal depths the first triangle drawn wins.
    bool any = false;
    for (const Chunk& c : chunks_) {
        for (uint32_t index : c.bins[tile]) rasterizeTriangle(fb, c.tris[index], rect, baseColor, texture);
        any = any || !c.bins[tile].empty();
    }
    return any;
}

// The per-triangle work before a pixel loop runs. Returns false if there's nothing
// to draw. Force-inlined and filled into a local in each caller, so the compiler
// can keep every field in registers instead of in memory.
TESSERA_FORCE_INLINE bool Renderer::setupTriangle(const ScreenTriangle& tri, const TileRect& rect,
                                                  const Vec3& baseColor, const Texture* texture,
                                                  TriangleSetup& s) const {
    const ScreenVertex& v0 = tri.v[0];
    const ScreenVertex& v1 = tri.v[1];
    const ScreenVertex& v2 = tri.v[2];
    s.tri = &tri;

    // Signed area * 2. Zero area = degenerate (a line or point): nothing to fill,
    // and dividing by it below would blow up.
    const float area = edgeFunction(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0.0f) return false;
    s.invArea = 1.0f / std::fabs(area);  // edge values are already sign-corrected

    // Edge i is the one opposite vertex i, so its value is vertex i's weight.
    s.e[0] = Edge(v1, v2, area);
    s.e[1] = Edge(v2, v0, area);
    s.e[2] = Edge(v0, v1, area);

    // Only the part of the bounding box inside this tile.
    s.minX = std::max(tri.minX, rect.x0);
    s.maxX = std::min(tri.maxX, rect.x1);
    s.minY = std::max(tri.minY, rect.y0);
    s.maxY = std::min(tri.maxY, rect.y1);

    // Every mode uses the same lighting function; they differ in *where* it runs.
    // Flat: once per triangle. Gouraud: once per vertex, then the result is
    // interpolated (cheap, but highlights smear or vanish inside big triangles).
    // Phong: interpolate the normal and light every pixel (smooth, but costly).
    s.shading = settings.shading;
    if (s.shading == Shading::Flat) {
        // Meshes wind counter-clockwise seen from outside, so by the right-hand
        // rule this cross product points outward. Lit at the centroid.
        const Vec3 faceNormal = normalize(cross(v1.worldPos - v0.worldPos, v2.worldPos - v0.worldPos));
        s.flatLight = light(faceNormal, (v0.worldPos + v1.worldPos + v2.worldPos) / 3.0f);
    } else if (s.shading == Shading::Gouraud) {
        s.vertexLight[0] = light(v0.normal, v0.worldPos);
        s.vertexLight[1] = light(v1.normal, v1.worldPos);
        s.vertexLight[2] = light(v2.normal, v2.worldPos);
    }
    s.baseColor = baseColor;
    s.texture = texture;
    s.affineUV = settings.textureMapping == TextureMapping::Affine;
    s.needsAttributes = s.shading != Shading::Flat || (texture && !s.affineUV);
    s.bilinear = settings.textureFilter == TextureFilter::Bilinear;
    s.depthTest = settings.depthMode == DepthMode::ZBuffer;
    s.countOverdraw = settings.debugView == DebugView::Overdraw;
    return true;
}

#if defined(__ARM_NEON)
// Kept out of line on purpose: if this code sat inside rasterizeTriangle, it would
// change how the compiler allocates registers for the scalar loop (measured: ~7% slower).
TESSERA_NO_INLINE void Renderer::rasterizeTriangleNeon(Framebuffer& fb, const ScreenTriangle& tri,
                                                       const TileRect& rect, const Vec3& baseColor,
                                                       const Texture* texture) {
    TriangleSetup s;
    if (setupTriangle(tri, rect, baseColor, texture, s)) rasterizeNeon(fb, s);
}
#endif

void Renderer::rasterizeTriangle(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect,
                                 const Vec3& baseColor, const Texture* texture) {
#if defined(__ARM_NEON)
    // 4-wide groups waste lanes on tiny triangles, so those stay scalar. Mixing the
    // two loops per triangle is safe only because they compute bit-identical edge
    // values, so the fill rule still holds where a scalar and a SIMD triangle meet.
    const int width = std::min(tri.maxX, rect.x1) - std::max(tri.minX, rect.x0) + 1;
    if (settings.useSimd && width >= settings.simdMinWidth) {
        rasterizeTriangleNeon(fb, tri, rect, baseColor, texture);
        return;
    }
#endif

    // The scalar pixel loop: the reference implementation.
    TriangleSetup s;
    if (!setupTriangle(tri, rect, baseColor, texture, s)) return;
    const ScreenVertex& v0 = tri.v[0];
    const ScreenVertex& v1 = tri.v[1];
    const ScreenVertex& v2 = tri.v[2];
    const Edge e0 = s.e[0], e1 = s.e[1], e2 = s.e[2];
    const float invArea = s.invArea;
    const int minX = s.minX, maxX = s.maxX, minY = s.minY, maxY = s.maxY;
    const Shading shading = s.shading;
    const Lighting flatLight = s.flatLight;
    const Lighting vertexLight[3] = {s.vertexLight[0], s.vertexLight[1], s.vertexLight[2]};
    const bool affineUV = s.affineUV, needsAttributes = s.needsAttributes, bilinear = s.bilinear;
    const bool depthTest = s.depthTest, countOverdraw = s.countOverdraw;

    uint32_t* colorBuf = fb.colorData();
    float* depthBuf = fb.depthData();
    uint16_t* overdrawBuf = fb.overdrawData();

    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;  // sample at the pixel center
        for (int x = minX; x <= maxX; ++x) {
            const float px = x + 0.5f;

            // Each edge function is p's signed area against the edge opposite one
            // vertex, so dividing by the full area gives that vertex's barycentric
            // weight: b0 + b1 + b2 = 1. Edge::value() is already sign-corrected to be
            // positive inside, whichever way the triangle winds.
            const float w0 = e0.value(px, py), w1 = e1.value(px, py), w2 = e2.value(px, py);
            if (!e0.inside(w0) || !e1.inside(w1) || !e2.inside(w2)) continue;
            const float b0 = w0 * invArea, b1 = w1 * invArea, b2 = w2 * invArea;

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
                // Affine (PS1): interpolate UVs with the plain screen-space weights,
                // skipping the per-pixel divide the PS1 had no hardware for. Exact at
                // the vertices, wrong in between: textures kink along each quad's
                // diagonal and "swim" as the camera moves.
                const Vec2 uv = affineUV ? v0.uv * b0 + v1.uv * b1 + v2.uv * b2
                                         : v0.uv * p0 + v1.uv * p1 + v2.uv * p2;
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
Lighting Renderer::light(const Vec3& normal, const Vec3& worldPos) const {
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

Framebuffer& Renderer::renderTarget(Framebuffer& output) {
    return settings.internalScale > 1 ? lowRes_ : output;
}

void Renderer::endFrame(Framebuffer& output) {
    Framebuffer& fb = renderTarget(output);
    switch (settings.debugView) {
        case DebugView::None:
        case DebugView::Wireframe:
            break;  // wireframe is drawn at the end of drawMesh
        case DebugView::Depth:
            showDepth(fb);
            break;
        case DebugView::Overdraw:
            showOverdraw(fb);
            break;
        case DebugView::ThreadTiles:
            showThreadTiles(fb);
            break;
    }
    if (settings.colorDepth == ColorDepth::Bits15) reduceColorDepth(fb);

    // Nearest-neighbor upscale: each low-res pixel becomes a sharp block, like a
    // PS1 picture on a big screen. x * lowW / outW maps an output column to the
    // source column covering it (exact N x N blocks when the sizes divide evenly).
    if (&fb != &output) {
        const uint32_t* src = fb.colorData();
        uint32_t* dst = output.colorData();
        parallelRows(output.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const int sy = y * fb.height() / output.height();
                for (int x = 0; x < output.width(); ++x) {
                    const int sx = x * fb.width() / output.width();
                    dst[static_cast<size_t>(y) * output.width() + x] = src[static_cast<size_t>(sy) * fb.width() + sx];
                }
            }
        });
    }
}

void Renderer::parallelRows(int height, const std::function<void(int, int)>& fn) {
    const int band = 16;  // rows per job: enough work to be worth handing out
    const int count = (height + band - 1) / band;
    auto job = [&](int i, int) { fn(i * band, std::min(height, (i + 1) * band)); };
    if (pool_) {
        pool_->parallelFor(count, job);
    } else {
        for (int i = 0; i < count; ++i) job(i, 0);
    }
}

// PS1 framebuffer: 5 bits per channel, 32 levels instead of 256. Plain truncation
// leaves visible bands in smooth gradients. Ordered dithering adds a small offset
// from a fixed 4x4 pattern before truncating, so neighboring pixels round in
// different directions and the eye averages them back into the in-between shade.
void Renderer::reduceColorDepth(Framebuffer& fb) {
    // The PS1 GPU's dither matrix, in 8-bit units: one 5-bit step is 8, so the
    // offsets span half a step either way.
    static const int kDither[4][4] = {
        {-4, 0, -3, 1},
        {2, -2, 3, -1},
        {-3, 1, -4, 0},
        {3, -1, 2, -2},
    };
    uint32_t* color = fb.colorData();
    parallelRows(fb.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < fb.width(); ++x) {
                const int offset = settings.dither ? kDither[y & 3][x & 3] : 0;
                uint32_t& c = color[static_cast<size_t>(y) * fb.width() + x];
                uint32_t out = 0xFF000000u;
                for (int shift = 0; shift <= 16; shift += 8) {
                    const int c8 = static_cast<int>((c >> shift) & 0xFF);
                    const int c5 = std::clamp(c8 + offset, 0, 255) >> 3;  // keep the top 5 bits
                    // Back to 8 bits: copy the top bits into the bottom so 31 -> 255, not 248.
                    out |= static_cast<uint32_t>((c5 << 3) | (c5 >> 2)) << shift;
                }
                c = out;
            }
        }
    });
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

// Tints each tile by the thread that rasterized it, to show how work was shared.
void Renderer::showThreadTiles(Framebuffer& fb) const {
    static const Vec3 palette[] = {
        {0.90f, 0.30f, 0.30f}, {0.30f, 0.75f, 0.35f}, {0.30f, 0.50f, 0.95f}, {0.95f, 0.80f, 0.20f},
        {0.75f, 0.35f, 0.90f}, {0.20f, 0.85f, 0.85f}, {0.95f, 0.55f, 0.15f}, {0.95f, 0.45f, 0.75f},
        {0.55f, 0.85f, 0.25f}, {0.55f, 0.55f, 0.55f},
    };
    const int paletteSize = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    uint32_t* color = fb.colorData();
    for (int tile = 0; tile < static_cast<int>(tileThread_.size()); ++tile) {
        const int thread = tileThread_[tile];
        if (thread < 0) continue;
        const Vec3 tint = palette[thread % paletteSize];
        const int x0 = (tile % tilesX_) * tileSize_, y0 = (tile / tilesX_) * tileSize_;
        const int x1 = std::min(x0 + tileSize_, viewportW_), y1 = std::min(y0 + tileSize_, viewportH_);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) {
                const size_t i = static_cast<size_t>(y) * fb.width() + x;
                const bool border = x == x0 || y == y0;  // outline tiles so neighbors with the same color separate
                const float mix = border ? 0.9f : 0.5f;
                color[i] = packColor(unpackColor(color[i]) * (1.0f - mix) + tint * mix);
            }
    }
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
