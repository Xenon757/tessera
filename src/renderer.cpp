#include "renderer.h"

#include "raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>

namespace {

// Every Renderer with the same thread count draws through one pool. Starting threads
// is slow, and a threaded WebAssembly build can only use the threads the page started
// up front: a second Renderer (an export, a still) starting its own would wait forever
// for threads that can't start while the main thread waits. Renderers take turns on a
// shared pool, so drawMesh must not run on two threads at once (the editor never does).
std::shared_ptr<ThreadPool> sharedPool(int threads) {
    static std::mutex mutex;
    static std::map<int, std::weak_ptr<ThreadPool>> pools;  // weak: a pool ends with its last Renderer
    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<ThreadPool> pool = pools[threads].lock();
    if (!pool) pools[threads] = pool = std::make_shared<ThreadPool>(threads);
    return pool;
}

}  // namespace

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
    fb.clear(packColor(clearColor));
}

namespace {

ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t) {
    return {a.pos + (b.pos - a.pos) * t, a.worldPos + (b.worldPos - a.worldPos) * t,
            a.normal + (b.normal - a.normal) * t, a.uv + (b.uv - a.uv) * t, a.custom + (b.custom - a.custom) * t};
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
                        const Texture* texture, const Shader* shader) {
    // Per-draw matrices for the vertex stage; the normal matrix keeps normals
    // perpendicular to surfaces even when the model is scaled unevenly.
    const DrawTransforms transforms{model, projection_ * view_ * model, normalMatrix(model)};
    draw(renderTarget(output), mesh, transforms, DrawState{baseColor, texture, shader});
}

void Renderer::beginShadowMap(const Vec3& center, float radius) {
    const int size = std::max(16, settings.shadowMapSize);
    if (shadowMap_.width() != size || shadowMap_.height() != size) shadowMap_.resizeDepthOnly(size, size);
    shadowMap_.clear(0, 1.0f);
    radius = std::max(radius, 1e-3f);
    // The light's camera sits 2 radii out along lightDir (toward the light), looking
    // back at the center, so the whole sphere lies between depth r and 3r. Its "up"
    // only orients the image; any vector not parallel to lightDir works.
    const Vec3 up = std::fabs(lightDir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Mat4 view = Mat4::lookAt(center + lightDir * (2.0f * radius), center, up);
    // A box just around the sphere: 2r across, depth 0 to 4r.
    shadowViewProj_ = Mat4::orthographic(-radius, radius, -radius, radius, 0.0f, 4.0f * radius) * view;
    // A texel is 2r / size world units wide, and depth 0..1 spans 4r: one texel's
    // width is 0.5 / size in depth units. Biases are measured in it (setupTriangle).
    shadowTexelDepth_ = 0.5f / static_cast<float>(size);
}

void Renderer::drawShadowCaster(const Mesh& mesh, const Mat4& model) {
    const DrawTransforms transforms{model, shadowViewProj_ * model, normalMatrix(model)};
    DrawState state;
    state.depthOnly = true;
    draw(shadowMap_, mesh, transforms, state);
}

void Renderer::draw(Framebuffer& fb, const Mesh& mesh, const DrawTransforms& transforms, const DrawState& draw) {
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
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    threads = 1;  // a WebAssembly build without -pthread can't start threads
#elif defined(__EMSCRIPTEN_PTHREADS__)
    threads = std::min(threads, TESSERA_WASM_THREADS);  // only as many as the page started (CMakeLists.txt)
#endif
    threads = std::max(1, threads);
    if (!pool_ || pool_->size() != threads) pool_ = sharedPool(threads);

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
        geometryPass(mesh, transforms, draw, triCount * c / chunkCount, triCount * (c + 1) / chunkCount,
                     chunks_[c]);
    });

    // ---- Raster pass: each tile draws the triangles binned to it. Tiles don't
    // overlap, so no two threads ever touch the same pixel: no locks needed.
    tileThread_.assign(tileCount, -1);
    sortScratch_.resize(pool_->size());
    pool_->parallelFor(tileCount, [&](int tile, int thread) {
        if (rasterizeTile(fb, tile, thread, draw)) tileThread_[tile] = thread;
    });

    if (draw.depthOnly) return;  // the stats and the wireframe are about what the camera sees
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

void Renderer::geometryPass(const Mesh& mesh, const DrawTransforms& transforms, const DrawState& draw,
                            size_t begin, size_t end, Chunk& out) const {
    const Shader* shader = draw.depthOnly ? nullptr : draw.shader;
    // A two-sided shader keeps back faces for this draw even when culling is on. The
    // shadow pass keeps every face: an open mesh (a plane) must still cast from either side.
    const bool cull = settings.backfaceCull && !draw.depthOnly && !(shader && shader->twoSided());
    for (size_t t = begin; t < end; ++t) {
        ++out.stats.trianglesSubmitted;
        const Vertex* in = &mesh.vertices[t * 3];

        // ---- Vertex stage: object space -> clip space (fixed-function, or the shader's).
        ClipVertex cv[3];
        for (int i = 0; i < 3; ++i) {
            if (shader)
                shader->vertex(in[i], transforms, cv[i]);
            else
                fixedFunctionVertex(in[i], transforms, cv[i]);
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
            emitTriangle(cv[0], cv[1], cv[2], cull, draw.depthOnly, out);
            continue;
        }
        ++out.stats.trianglesClipped;
        ClipVertex poly[4];
        const int n = clipNear(cv, poly);
        // Fan-triangulate the clipped polygon. Clipping preserves winding order.
        for (int i = 1; i + 1 < n; ++i) emitTriangle(poly[0], poly[i], poly[i + 1], cull, draw.depthOnly, out);
    }
}

void Renderer::emitTriangle(const ClipVertex& c0, const ClipVertex& c1, const ClipVertex& c2, bool cull,
                            bool depthOnly, Chunk& out) const {
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
        sv[i].custom = cv[i]->custom;
        if (settings.vertexSnap && !depthOnly) {  // (PS1 mode has no shadows; keep the map exact)
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
    if (cull && area >= 0.0f) {
        ++out.stats.trianglesCulled;
        return;
    }

    // ---- Bin stage. Bounding box in whole pixels, clamped to the screen. The
    // trivial reject only drops triangles fully off one side, so big ones overhang.
    // (clampToInt: huge or NaN coordinates from extreme transforms must not overflow
    // the int conversion; a box entirely off screen ends up with min > max.)
    tri.minX = clampToInt(std::floor(std::min({sv[0].x, sv[1].x, sv[2].x})), 0, viewportW_);
    tri.minY = clampToInt(std::floor(std::min({sv[0].y, sv[1].y, sv[2].y})), 0, viewportH_);
    tri.maxX = clampToInt(std::ceil(std::max({sv[0].x, sv[1].x, sv[2].x})), -1, viewportW_ - 1);
    tri.maxY = clampToInt(std::ceil(std::max({sv[0].y, sv[1].y, sv[2].y})), -1, viewportH_ - 1);
    if (tri.minX > tri.maxX || tri.minY > tri.maxY) return;  // nothing on screen

    if (settings.depthMode == DepthMode::OrderingTable && !depthOnly) {
        // PS1 ordering table: one depth per triangle, the average of its vertices'
        // view-space depths (clip w is exactly that), quantized into a bucket. The
        // raster pass draws buckets far to near: the painter's algorithm.
        const float depth = (c0.pos.w + c1.pos.w + c2.pos.w) / 3.0f;
        const int size = std::max(1, settings.orderingTableSize);
        const float t = (depth - zNear_) / (zFar_ - zNear_);
        // t is NaN when the projection isn't a perspective one (near/far can't be
        // recovered from it); clampToInt sends that to bucket 0.
        tri.otBucket = clampToInt(t * size, 0, size - 1);
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
bool Renderer::rasterizeTile(Framebuffer& fb, int tile, int thread, const DrawState& draw) {
    const int tx = tile % tilesX_, ty = tile / tilesX_;
    const TileRect rect{tx * tileSize_, ty * tileSize_, std::min((tx + 1) * tileSize_, viewportW_) - 1,
                        std::min((ty + 1) * tileSize_, viewportH_) - 1};

    if (settings.depthMode == DepthMode::OrderingTable && !draw.depthOnly) {
        // Gather this tile's triangles in submission order, then stable-sort far to
        // near. Stable keeps submission order within a bucket, so the result matches
        // one global sort and doesn't depend on thread count.
        std::vector<TriRef>& refs = sortScratch_[thread];
        refs.clear();
        for (int c = 0; c < static_cast<int>(chunks_.size()); ++c)
            for (uint32_t index : chunks_[c].bins[tile]) refs.push_back({chunks_[c].tris[index].otBucket, c, index});
        std::stable_sort(refs.begin(), refs.end(), [](const TriRef& a, const TriRef& b) { return a.bucket > b.bucket; });
        for (const TriRef& r : refs) rasterizeTriangle(fb, chunks_[r.chunk].tris[r.index], rect, draw);
        return !refs.empty();
    }
    // Chunks in order, and each bin in order, reproduces the original submission
    // order. That matters: with equal depths the first triangle drawn wins.
    bool any = false;
    for (const Chunk& c : chunks_) {
        for (uint32_t index : c.bins[tile]) rasterizeTriangle(fb, c.tris[index], rect, draw);
        any = any || !c.bins[tile].empty();
    }
    return any;
}

// The per-triangle work before a pixel loop runs. Returns false if there's nothing
// to draw. Force-inlined and filled into a local in each caller, so the compiler
// can keep every field in registers instead of in memory.
TESSERA_FORCE_INLINE bool Renderer::setupTriangle(const ScreenTriangle& tri, const TileRect& rect,
                                                  const DrawState& draw, TriangleSetup& s) const {
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
    s.baseColor = draw.baseColor;
    s.texture = draw.texture;
    s.affineUV = settings.textureMapping == TextureMapping::Affine;
    s.needsAttributes = s.shading != Shading::Flat || (draw.texture && !s.affineUV);
    s.bilinear = settings.textureFilter == TextureFilter::Bilinear;
    s.depthTest = settings.depthMode == DepthMode::ZBuffer;
    s.countOverdraw = settings.debugView == DebugView::Overdraw;
    s.shadows = settings.shadows;
    if (s.shadows) {
        s.needsAttributes = true;  // the test needs each pixel's world position
        // Shadow acne: a texel stores one depth, but a surface tilted by angle t to the
        // light changes depth by tan(t) texel-widths across it, so it would shadow
        // itself in stripes. Offset the test by that slope plus a texel, scaled by
        // the setting, and once more per texel a filter reaches out. Per triangle,
        // from its face normal: no per-pixel square root.
        const Vec3 faceNormal = normalize(cross(v1.worldPos - v0.worldPos, v2.worldPos - v0.worldPos));
        const float c = std::max(std::fabs(dot(faceNormal, lightDir)), 0.1f);  // cos(t), capped so tan(t) <= ~10
        const float tanT = std::sqrt(1.0f - c * c) / c;
        const float reach = settings.shadowFilter >= 3 ? 2.0f : 1.0f;
        s.shadowBias = settings.shadowBias * shadowTexelDepth_ * (1.0f + tanT) * reach;
        // The light's projection is orthographic, so world -> shadow map is affine, and
        // an affine map commutes with weighted averages whose weights sum to 1. So the
        // vertices go into the map once here, and each pixel interpolates them with its
        // perspective-correct weights, instead of a 4x4 transform per pixel.
        s.shadowCoord[0] = shadowCoord(v0.worldPos);
        s.shadowCoord[1] = shadowCoord(v1.worldPos);
        s.shadowCoord[2] = shadowCoord(v2.worldPos);
    }
    return true;
}

#if defined(TESSERA_NEON)
// Kept out of line on purpose: if this code sat inside rasterizeTriangle, it would
// change how the compiler allocates registers for the scalar loop (measured: ~7% slower).
TESSERA_NO_INLINE void Renderer::rasterizeTriangleNeon(Framebuffer& fb, const ScreenTriangle& tri,
                                                       const TileRect& rect, const DrawState& draw) {
    TriangleSetup s;
    if (setupTriangle(tri, rect, draw, s)) rasterizeNeon(fb, s);
}
#endif

void Renderer::rasterizeTriangle(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect,
                                 const DrawState& draw) {
    if (draw.depthOnly) {
        rasterizeDepthOnly(fb, tri, rect);
        return;
    }
    if (draw.shader) {
        rasterizeProgrammable(fb, tri, rect, draw);
        return;
    }
#if defined(TESSERA_NEON)
    // 4-wide groups waste lanes on tiny triangles, so those stay scalar. Mixing the
    // two loops per triangle is safe only because they compute bit-identical edge
    // values, so the fill rule still holds where a scalar and a SIMD triangle meet.
    const int width = std::min(tri.maxX, rect.x1) - std::max(tri.minX, rect.x0) + 1;
    if (settings.useSimd && !settings.shadows && width >= settings.simdMinWidth) {  // (NEON has no shadows yet)
        rasterizeTriangleNeon(fb, tri, rect, draw);
        return;
    }
#endif

    // The scalar pixel loop: the reference implementation.
    TriangleSetup s;
    if (!setupTriangle(tri, rect, draw, s)) return;
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
    const bool depthTest = s.depthTest, countOverdraw = s.countOverdraw, shadows = s.shadows;
    const float shadowBias = s.shadowBias;
    const Vec3* shadowCoord = s.shadowCoord;
    // Read these from `draw`, not as copies: a local Vec3 pins three more registers
    // for the whole loop, and the Phong + texture path then spills (~19% slower on
    // a pixel-heavy scene). Loading baseColor from memory when needed is cheaper.
    const Vec3& baseColor = draw.baseColor;
    const Texture* texture = draw.texture;

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
                lit.diffuse = vertexLight[0].diffuse * p0 + vertexLight[1].diffuse * p1 + vertexLight[2].diffuse * p2;
                lit.specular = vertexLight[0].specular * p0 + vertexLight[1].specular * p1 + vertexLight[2].specular * p2;
            } else if (shading == Shading::Phong) {
                // Interpolated unit vectors come out shorter than 1; renormalize.
                const Vec3 n = normalize(v0.normal * p0 + v1.normal * p1 + v2.normal * p2);
                const Vec3 pos = v0.worldPos * p0 + v1.worldPos * p1 + v2.worldPos * p2;
                lit = light(n, pos);
            }
            if (shadows) applyShadow(lit, shadowCoord[0] * p0 + shadowCoord[1] * p1 + shadowCoord[2] * p2, shadowBias);
            // Diffuse tints by the surface color; the highlight is the light's own
            // color reflected, so it's added on top without the surface color.
            colorBuf[idx] = packColor(albedo * lit.diffuse + lit.specular);
        }
    }
}

// The shadow pass: coverage and depth only, with the same edges and fill rule as the
// other loops. Under an orthographic projection depth is affine in screen space, so
// barycentric interpolation of z is exact. Each texel keeps the smallest depth, and a
// minimum doesn't depend on drawing order: the map is the same at any thread count.
TESSERA_NO_INLINE void Renderer::rasterizeDepthOnly(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect) {
    const ScreenVertex& v0 = tri.v[0];
    const ScreenVertex& v1 = tri.v[1];
    const ScreenVertex& v2 = tri.v[2];
    const float area = edgeFunction(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0.0f) return;
    const float invArea = 1.0f / std::fabs(area);
    const Edge e0(v1, v2, area), e1(v2, v0, area), e2(v0, v1, area);
    const int minX = std::max(tri.minX, rect.x0), maxX = std::min(tri.maxX, rect.x1);
    const int minY = std::max(tri.minY, rect.y0), maxY = std::min(tri.maxY, rect.y1);
    float* depthBuf = fb.depthData();
    for (int y = minY; y <= maxY; ++y) {
        const float py = y + 0.5f;
        for (int x = minX; x <= maxX; ++x) {
            const float px = x + 0.5f;
            const float w0 = e0.value(px, py), w1 = e1.value(px, py), w2 = e2.value(px, py);
            if (!e0.inside(w0) || !e1.inside(w1) || !e2.inside(w2)) continue;
            const float z = (w0 * invArea) * v0.z + (w1 * invArea) * v1.z + (w2 * invArea) * v2.z;
            float& d = depthBuf[static_cast<size_t>(y) * fb.width() + x];
            if (z < d) d = z;
        }
    }
}

// The pixel loop for a custom Shader. Same coverage, fill rule, and depth test as
// the scalar loop, but the color comes from shader->pixel(), which may discard.
// Out of line so it can't disturb the fixed-function loop's code generation.
TESSERA_NO_INLINE void Renderer::rasterizeProgrammable(Framebuffer& fb, const ScreenTriangle& tri,
                                                       const TileRect& rect, const DrawState& draw) {
    TriangleSetup s;
    if (!setupTriangle(tri, rect, draw, s)) return;
    const ScreenVertex* v = tri.v;
    const Shader& shader = *draw.shader;
    // Edge values are sign-corrected per triangle, so recover the winding from the
    // raw area: negative = counter-clockwise on our y-down screen = front-facing.
    const bool frontFacing = edgeFunction(v[0].x, v[0].y, v[1].x, v[1].y, v[2].x, v[2].y) < 0.0f;
    uint32_t* colorBuf = fb.colorData();
    float* depthBuf = fb.depthData();
    uint16_t* overdrawBuf = fb.overdrawData();

    for (int y = s.minY; y <= s.maxY; ++y) {
        const float py = y + 0.5f;
        for (int x = s.minX; x <= s.maxX; ++x) {
            const float px = x + 0.5f;
            const float w0 = s.e[0].value(px, py), w1 = s.e[1].value(px, py), w2 = s.e[2].value(px, py);
            if (!s.e[0].inside(w0) || !s.e[1].inside(w1) || !s.e[2].inside(w2)) continue;
            const float b0 = w0 * s.invArea, b1 = w1 * s.invArea, b2 = w2 * s.invArea;

            const size_t idx = static_cast<size_t>(y) * fb.width() + x;
            // Test depth early (skip hidden pixels before running the shader), but
            // write it only after the shader keeps the pixel: a discarded pixel must
            // not hide what's behind it.
            const float z = b0 * v[0].z + b1 * v[1].z + b2 * v[2].z;
            if (s.depthTest && z >= depthBuf[idx]) continue;

            // Perspective-correct weights (see the scalar loop).
            const float q0 = b0 * v[0].invW, q1 = b1 * v[1].invW, q2 = b2 * v[2].invW;
            const float invSum = 1.0f / (q0 + q1 + q2);
            const float p0 = q0 * invSum, p1 = q1 * invSum, p2 = q2 * invSum;

            PixelInput in;
            in.worldPos = v[0].worldPos * p0 + v[1].worldPos * p1 + v[2].worldPos * p2;
            in.normal = normalize(v[0].normal * p0 + v[1].normal * p1 + v[2].normal * p2);
            in.uv = v[0].uv * p0 + v[1].uv * p1 + v[2].uv * p2;
            in.custom = v[0].custom * p0 + v[1].custom * p1 + v[2].custom * p2;
            in.frontFacing = frontFacing;
            in.eye = eye_;
            in.lightDir = lightDir;

            Vec3 color;
            if (!shader.pixel(in, color)) continue;  // discarded
            if (s.depthTest) depthBuf[idx] = z;
            if (s.countOverdraw && overdrawBuf[idx] < UINT16_MAX) ++overdrawBuf[idx];
            colorBuf[idx] = packColor(color);
        }
    }
}

// Blinn-Phong lighting at one surface point.
// Diffuse (Lambert's cosine law): a surface receives light in proportion to
// dot(N, L), clamped at 0 for surfaces facing away; plus an ambient floor.
// Specular: H is the "half vector" halfway between the light and view directions.
// When N lines up with H, the surface mirrors the light straight at the eye.
// pow(dot(N, H), shininess) turns that into a highlight; higher = tighter.
// Both come from the light, so both are scaled by its color * intensity ("radiance");
// ambient is the world's, so it isn't. With the default white light at intensity 1
// every radiance multiply is by exactly 1.0, which leaves the result bit for bit as
// it was before lights had a color.
Lighting Renderer::light(const Vec3& normal, const Vec3& worldPos) const {
    const Vec3 radiance = lightColor * lightIntensity;
    const float nDotL = dot(normal, lightDir);
    const float direct = (1.0f - ambient) * std::max(nDotL, 0.0f);
    Lighting out;
    out.diffuse = Vec3{ambient, ambient, ambient} + radiance * direct;
    if (nDotL > 0.0f) {  // no highlight on the side facing away from the light
        const Vec3 toEye = normalize(eye_ - worldPos);
        const Vec3 halfVec = normalize(lightDir + toEye);
        out.specular = radiance * (specular * std::pow(std::max(dot(normal, halfVec), 0.0f), shininess));
    }
    return out;
}

// Where world point p lands in the shadow map: into the light's clip space
// (orthographic, so w = 1 and that's already NDC), then to texels exactly as the
// shadow pass mapped vertices to pixels, and the depth the light would see p at.
Vec3 Renderer::shadowCoord(const Vec3& p) const {
    const Vec4 c = shadowViewProj_ * Vec4(p, 1.0f);
    const float n = static_cast<float>(shadowMap_.width());
    return {(c.x * 0.5f + 0.5f) * n, (1.0f - (c.y * 0.5f + 0.5f)) * n, c.z * 0.5f + 0.5f};
}

// Shadow mapping's test: if the map holds something nearer the light at the texel
// under c, the point is in shadow. With PCF the 3x3 texels around it are tested too,
// and the passes averaged, so edges soften over ~3 texels.
float Renderer::shadowVisibility(const Vec3& c, float bias) const {
    const int n = shadowMap_.width();
    const float depth = c.z - bias;
    const int cx = clampToInt(std::floor(c.x), -2, n + 1), cy = clampToInt(std::floor(c.y), -2, n + 1);
    const int r = settings.shadowFilter >= 3 ? 1 : 0;
    const float* map = shadowMap_.depthData();
    int lit = 0;
    if (cx - r >= 0 && cy - r >= 0 && cx + r < n && cy + r < n) {
        // The usual case, the whole block on the map: no bounds checks, and the
        // comparisons summed without branches.
        for (int y = cy - r; y <= cy + r; ++y) {
            const float* row = map + static_cast<size_t>(y) * n;
            for (int x = cx - r; x <= cx + r; ++x) lit += int(depth <= row[x]);
        }
    } else {
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x)  // off the map: nothing there casts
                lit += x < 0 || y < 0 || x >= n || y >= n || depth <= map[static_cast<size_t>(y) * n + x];
    }
    const int tests = (2 * r + 1) * (2 * r + 1);
    return lit == tests ? 1.0f : static_cast<float>(lit) / static_cast<float>(tests);
}

TESSERA_NO_INLINE void Renderer::applyShadow(Lighting& lit, const Vec3& c, float bias) const {
    const float visible = shadowVisibility(c, bias);
    if (visible == 1.0f) return;  // fully lit: exactly as without shadows
    // Ambient stays; only the light's own share fades.
    const Vec3 amb{ambient, ambient, ambient};
    lit.diffuse = amb + (lit.diffuse - amb) * visible;
    lit.specular = lit.specular * visible;
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

void Renderer::drawLine3D(Framebuffer& output, const Vec3& a, const Vec3& b, uint32_t color) {
    Framebuffer& fb = renderTarget(output);
    const Mat4 viewProj = projection_ * view_;
    Vec4 ca = viewProj * Vec4(a, 1.0f), cb = viewProj * Vec4(b, 1.0f);
    // Clip against the near plane in clip space (z >= -w), as triangles are: d is
    // linear along the segment, so it crosses zero at t = da / (da - db).
    const float da = ca.z + ca.w, db = cb.z + cb.w;
    if (da < 0 && db < 0) return;  // entirely behind the camera
    if (da < 0) ca = ca + (cb - ca) * (da / (da - db));
    if (db < 0) cb = cb + (ca - cb) * (db / (db - da));
    auto toScreen = [&](const Vec4& c, float& x, float& y) {
        x = (c.x / c.w * 0.5f + 0.5f) * fb.width();
        y = (1.0f - (c.y / c.w * 0.5f + 0.5f)) * fb.height();
    };
    float x0, y0, x1, y1;
    toScreen(ca, x0, y0);
    toScreen(cb, x1, y1);
    auto ix = [](float v) { return clampToInt(std::floor(v), -(1 << 16), 1 << 16); };
    drawLine(fb, ix(x0), ix(y0), ix(x1), ix(y1), color);
}

bool Renderer::projectToTarget(Framebuffer& fb, const Vec3& p, float& x, float& y, float& z) const {
    const Vec4 c = projection_ * view_ * Vec4(p, 1.0f);
    if (c.z + c.w < 0 || c.w <= 0) return false;
    x = (c.x / c.w * 0.5f + 0.5f) * fb.width();
    y = (1.0f - (c.y / c.w * 0.5f + 0.5f)) * fb.height();
    z = c.z / c.w * 0.5f + 0.5f;  // the same depth the rasterizer writes
    return true;
}

// Overlay depth test: a little slack, since an edge or vertex lies exactly on the
// surface it belongs to and would otherwise fight it for the pixel.
constexpr float kOverlayDepthBias = 0.0008f;

void Renderer::drawLine3DDepthTested(Framebuffer& output, const Vec3& a, const Vec3& b, uint32_t color) {
    Framebuffer& fb = renderTarget(output);
    float x0, y0, z0, x1, y1, z1;
    if (!projectToTarget(fb, a, x0, y0, z0) || !projectToTarget(fb, b, x1, y1, z1)) return;  // overlays skip the near plane
    // Bresenham, carrying depth along: NDC depth is linear across the screen, so it
    // can be interpolated by how far along the line each pixel is.
    auto ix = [](float v) { return clampToInt(std::floor(v), -(1 << 16), 1 << 16); };
    int px = ix(x0), py = ix(y0);
    const int ex = ix(x1), ey = ix(y1);
    const int dx = std::abs(ex - px), sx = px < ex ? 1 : -1, dy = -std::abs(ey - py), sy = py < ey ? 1 : -1;
    const int steps = std::max(dx, -dy);
    int err = dx + dy;
    for (int step = 0;; ++step) {
        const float z = steps > 0 ? z0 + (z1 - z0) * (static_cast<float>(step) / steps) : z0;
        if (px >= 0 && py >= 0 && px < fb.width() && py < fb.height() &&
            z - kOverlayDepthBias <= fb.depthData()[static_cast<size_t>(py) * fb.width() + px])
            fb.setPixel(px, py, color);
        if (px == ex && py == ey) break;
        const int e2 = 2 * err;
        if (e2 >= dy) err += dy, px += sx;
        if (e2 <= dx) err += dx, py += sy;
    }
}

void Renderer::drawPoint3D(Framebuffer& output, const Vec3& p, uint32_t color, int size) {
    Framebuffer& fb = renderTarget(output);
    float x, y, z;
    if (!projectToTarget(fb, p, x, y, z)) return;
    const int cx = clampToInt(std::floor(x), -(1 << 16), 1 << 16), cy = clampToInt(std::floor(y), -(1 << 16), 1 << 16);
    if (cx < 0 || cy < 0 || cx >= fb.width() || cy >= fb.height()) return;
    if (z - kOverlayDepthBias > fb.depthData()[static_cast<size_t>(cy) * fb.width() + cx]) return;  // behind a surface
    const int lo = -(size / 2), hi = lo + size;
    for (int dy = lo; dy < hi; ++dy)
        for (int dx = lo; dx < hi; ++dx) fb.setPixel(cx + dx, cy + dy, color);  // setPixel clips
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
    // Clamped to a generous band around the screen, so a near-plane-clipped vertex
    // far off screen can't make Bresenham walk billions of pixels.
    auto ix = [](float v) { return clampToInt(std::floor(v), -(1 << 16), 1 << 16); };
    drawLine(fb, ix(v0.x), ix(v0.y), ix(v1.x), ix(v1.y), color);
    drawLine(fb, ix(v1.x), ix(v1.y), ix(v2.x), ix(v2.y), color);
    drawLine(fb, ix(v2.x), ix(v2.y), ix(v0.x), ix(v0.y), color);
}
