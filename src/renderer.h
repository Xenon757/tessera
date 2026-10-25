// renderer.h — the software rendering pipeline.
//
//   drawMesh()
//     geometry pass (per triangle)
//       -> vertex stage   model/view/projection transform into clip space
//       -> clip stage     near-plane clipping (Sutherland-Hodgman) + trivial reject
//       -> project stage  perspective divide + viewport transform to pixels
//       -> cull stage     back-face culling (settings.backfaceCull)
//       -> bin            add the triangle to every screen tile its bounding box touches
//     raster pass (per tile)
//       -> rasterizeTriangle() for each binned triangle, clipped to the tile:
//          edge functions, z-buffer, shading
//   endFrame()
//     -> debug views / color-depth reduction / upscale (driven by settings)
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "framebuffer.h"
#include "vecmath.h"
#include "mesh.h"
#include "render_settings.h"
#include "shader.h"
#include "texture.h"
#include "thread_pool.h"

struct RenderStats {
    int trianglesSubmitted = 0;
    int trianglesClipped = 0;  // rejected outside the frustum, or split by the near plane
    int trianglesCulled = 0;   // removed by back-face culling (counted after clipping)
    int trianglesDrawn = 0;    // reached the rasterizer (a split triangle can count twice)
};

// A vertex after projection to the screen. Everything the rasterizer needs.
struct ScreenVertex {
    float x = 0, y = 0;  // pixel coordinates, origin top-left, y down
    float z = 0;         // depth in [0, 1]: 0 = near plane, 1 = far plane
    float invW = 0;      // 1 / clip.w — the key to perspective-correct interpolation
    Vec3 worldPos;       // for per-pixel lighting
    Vec3 normal;         // world-space normal
    Vec2 uv;
    Vec4 custom;         // shader-defined attributes (see ClipVertex)
};

// Result of lighting one surface point (see Renderer::light).
// Per color channel, because the light has a color: ambient + the light's tinted share.
struct Lighting {
    Vec3 diffuse;   // includes ambient; multiplies the surface color
    Vec3 specular;  // the light's color reflected; added on top, not tinted by the surface
};

struct TriangleSetup;  // per-triangle raster state for the SIMD loop (raster.h)

// Everything about one drawMesh call that the raster pass needs besides geometry.
struct DrawState {
    Vec3 baseColor;
    const Texture* texture = nullptr;  // optional; multiplied with baseColor
    const Shader* shader = nullptr;    // optional; replaces the fixed-function stages
    bool depthOnly = false;            // the shadow pass: depth only, both sides of every face
};

// A triangle that survived clipping and culling: everything the raster pass needs.
struct ScreenTriangle {
    ScreenVertex v[3];
    int minX, minY, maxX, maxY;  // pixel bounding box, clamped to the screen
    int otBucket = 0;            // ordering-table bucket (OrderingTable mode): higher = farther
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
    Vec3 lightColor{1, 1, 1};   // tints diffuse and specular (ambient stays white)
    float lightIntensity = 1.0f;  // scales diffuse and specular; the defaults change nothing
    float ambient = 0.15f;  // light floor so faces turned away from the light aren't pure black
    float specular = 0.35f;  // highlight strength (Blinn-Phong)
    float shininess = 48.0f; // highlight tightness: higher = smaller, sharper highlight
    Vec3 clearColor{0.08f, 0.09f, 0.11f};  // background, filled by beginFrame

    // All three take the final output framebuffer. With settings.internalScale > 1
    // the renderer draws into its own low-res buffer and endFrame upscales into `output`.
    void beginFrame(Framebuffer& output, const Mat4& view, const Mat4& projection);
    // `texture` (optional) is multiplied with baseColor, sampled at the mesh's UVs.
    // `shader` (optional) replaces the fixed-function vertex and pixel stages; then
    // baseColor and texture are ignored (a shader brings its own inputs).
    void drawMesh(Framebuffer& output, const Mesh& mesh, const Mat4& model, const Vec3& baseColor,
                  const Texture* texture = nullptr, const Shader* shader = nullptr);
    void endFrame(Framebuffer& output);

    // Shadows: before beginFrame, draw every object that casts a shadow into
    // the light's depth image (the shadow map). The sun's rays are parallel, so it's
    // seen through an orthographic camera looking along -lightDir, fitted tightly
    // around the world box [lo, hi]: pass a box around everything that casts or
    // receives. Set lightDir first. With settings.shadows, the frame that
    // follows tests every pixel against the map; endFrame disarms it again, so a
    // frame without a beginShadowMap has no shadows (and pays nothing for them).
    void beginShadowMap(const Vec3& lo, const Vec3& hi);
    void drawShadowCaster(const Mesh& mesh, const Mat4& model);
    const Framebuffer& shadowMap() const { return shadowMap_; }
    const Mat4& shadowViewProjection() const { return shadowViewProj_; }  // world -> the light's clip space

    // Draws a world-space line over the image (no depth test), for editor overlays:
    // selection boxes, light and camera gizmos, the ground grid. Call between
    // beginFrame and endFrame; uses that frame's view and projection.
    void drawLine3D(Framebuffer& output, const Vec3& a, const Vec3& b, uint32_t color);
    // Edit-mode overlays (vertex dots, mesh edges) that hide behind surfaces: tested
    // against the depth buffer the frame's triangles wrote, but never written to it.
    // Separate from the triangle pixel loop, so drawing them costs that loop nothing.
    void drawLine3DDepthTested(Framebuffer& output, const Vec3& a, const Vec3& b, uint32_t color);
    // A square dot `size` pixels across, centered on p; hidden when behind a surface.
    void drawPoint3D(Framebuffer& output, const Vec3& p, uint32_t color, int size);

    const RenderStats& stats() const { return stats_; }
    int threadsUsed() const { return pool_ ? pool_->size() : 1; }

private:
    struct TileRect {
        int x0, y0, x1, y1;  // inclusive pixel bounds
    };
    // The output of the geometry pass for one slice of the mesh: its surviving
    // triangles, and for each screen tile the indices of those that touch it.
    struct Chunk {
        std::vector<ScreenTriangle> tris;
        std::vector<std::vector<uint32_t>> bins;  // bins[tile], in submission order
        RenderStats stats;
    };

    // drawMesh and drawShadowCaster: the geometry pass, then the raster pass, into fb.
    void draw(Framebuffer& fb, const Mesh& mesh, const DrawTransforms& transforms, const DrawState& draw);
    void geometryPass(const Mesh& mesh, const DrawTransforms& transforms, const DrawState& draw, size_t begin,
                      size_t end, Chunk& out) const;
    // Project, cull, and bin one triangle that is already inside the near plane.
    void emitTriangle(const ClipVertex& c0, const ClipVertex& c1, const ClipVertex& c2, bool cull, bool depthOnly,
                      Chunk& out) const;
    bool rasterizeTile(Framebuffer& fb, int tile, int thread, const DrawState& draw);
    void rasterizeTriangle(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect,
                           const DrawState& draw);
    bool setupTriangle(const ScreenTriangle& tri, const TileRect& rect, const DrawState& draw,
                       TriangleSetup& s) const;
    // The shadow pass's pixel loop: depth only (scalar; there's no shading to vectorize).
    void rasterizeDepthOnly(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect);
    // The pixel loop for draws with a custom Shader (scalar only).
    void rasterizeProgrammable(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect,
                               const DrawState& draw);
#if defined(TESSERA_NEON)
    void rasterizeTriangleNeon(Framebuffer& fb, const ScreenTriangle& tri, const TileRect& rect,
                               const DrawState& draw);
    // The pixel loop with NEON SIMD, 4 pixels at a time (raster_neon.cpp). Produces
    // exactly the same pixels as the scalar loop in rasterizeTriangle.
    void rasterizeNeon(Framebuffer& fb, const TriangleSetup& s) const;
#endif
    Lighting light(const Vec3& normal, const Vec3& worldPos) const;
    // World point p in the shadow map: (texel x, texel y, depth as the light sees it).
    Vec3 shadowCoord(const Vec3& p) const;
    // How much of the light reaches a point at shadow-map coordinates c, from 0 (in
    // shadow) to 1 (lit): the fraction of map tests it passes, `bias` being the depth offset.
    float shadowVisibility(const Vec3& c, float bias) const;
    // Dims the light's share of `lit` (diffuse above ambient, and specular) by the
    // shadow at c. Out of line: it only runs with shadows on.
    void applyShadow(Lighting& lit, const Vec3& c, float bias) const;
    void showDepth(Framebuffer& fb) const;
    void showOverdraw(Framebuffer& fb) const;
    void showThreadTiles(Framebuffer& fb) const;
    void reduceColorDepth(Framebuffer& fb);
    // Runs fn(y0, y1) over bands of rows [y0, y1) in parallel (serially before the
    // thread pool exists). For whole-frame per-pixel passes.
    void parallelRows(int height, const std::function<void(int y0, int y1)>& fn);
    // The framebuffer actually drawn into: `output`, or lowRes_ when internalScale > 1.
    Framebuffer& renderTarget(Framebuffer& output);
    void drawLine(Framebuffer& fb, int x0, int y0, int x1, int y1, uint32_t color);
    // The world point as a pixel and a depth in [0, 1]; false when behind the near plane.
    bool projectToTarget(Framebuffer& fb, const Vec3& p, float& x, float& y, float& z) const;
    void drawTriangleEdges(Framebuffer& fb, const ScreenVertex& v0, const ScreenVertex& v1,
                           const ScreenVertex& v2, uint32_t color);

    Mat4 view_ = Mat4::identity();
    Mat4 projection_ = Mat4::identity();
    Vec3 eye_;  // camera position in world space, for specular
    float zNear_ = 0.0f, zFar_ = 0.0f;  // recovered from the projection matrix
    Framebuffer lowRes_{1, 1};          // render target when settings.internalScale > 1
    Framebuffer shadowMap_{0, 0};       // the light's depth image (depth only; see beginShadowMap)
    Mat4 shadowViewProj_ = Mat4::identity();
    float shadowTexelDepth_ = 0.0f;  // one texel's width in the map's depth units (see beginShadowMap)
    bool shadowMapReady_ = false;    // beginShadowMap ran since the last endFrame
    bool shadowsOn_ = false;         // this frame tests pixels against the map (set in beginFrame)

    // Screen tiling for the current drawMesh call.
    int viewportW_ = 0, viewportH_ = 0;
    int tileSize_ = 64, tilesX_ = 0, tilesY_ = 0;
    std::vector<Chunk> chunks_;  // kept between frames so the vectors keep their capacity
    std::shared_ptr<ThreadPool> pool_;  // shared by every Renderer with the same thread count (see sharedPool)
    std::vector<int> tileThread_;       // which thread rasterized each tile (-1 = no triangles)

    // OrderingTable mode: per-thread scratch list of a tile's triangles, to sort.
    struct TriRef {
        int bucket;
        int chunk;
        uint32_t index;
    };
    std::vector<std::vector<TriRef>> sortScratch_;
    RenderStats stats_;
};
