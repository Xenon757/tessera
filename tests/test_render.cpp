// test_render.cpp — rasterizer invariants and output stability.
//
//   fill rule      a tiling of triangles shades every interior pixel exactly once
//   simd           the NEON pixel loop is byte-identical to the scalar loop
//   golden         reference scenes hash to known values, so any change to rendered
//                  output is caught. Deliberate changes regenerate the hashes:
//                  tessera_test_render --update-golden
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "app_common.h"
#include "camera.h"
#include "check.h"
#include "liquid_scene.h"
#include "renderer.h"

namespace {

// ---- Fill rule: a jittered grid of triangles (shared edges everywhere), drawn
// without a depth test. Every interior pixel must be shaded exactly once.
void testFillRule(bool simd) {
    const int W = 64, H = 64, G = 12;
    const float lo = 8, hi = 56, cell = (hi - lo) / G;
    for (int snap = 0; snap < 3; ++snap) {  // random floats, pixel centers, pixel corners
        long doubles = 0, cracks = 0;
        for (int trial = 0; trial < 100; ++trial) {
            std::mt19937 rng(trial);
            std::uniform_real_distribution<float> jitter(-0.2f * cell, 0.2f * cell);
            Vec2 grid[G + 1][G + 1];
            for (int j = 0; j <= G; ++j)
                for (int i = 0; i <= G; ++i) {
                    float x = lo + i * cell, y = lo + j * cell;
                    if (i > 0 && i < G) x += jitter(rng);
                    if (j > 0 && j < G) y += jitter(rng);
                    if (snap == 1) x = std::floor(x) + 0.5f, y = std::floor(y) + 0.5f;
                    if (snap == 2) x = std::round(x), y = std::round(y);
                    grid[j][i] = {x, y};
                }
            Mesh mesh;
            auto ndc = [&](Vec2 p) { return Vec3{p.x / W * 2 - 1, 1 - p.y / H * 2, 0}; };
            for (int j = 0; j < G; ++j)
                for (int i = 0; i < G; ++i) {
                    const Vec2 a = grid[j][i], b = grid[j][i + 1], c = grid[j + 1][i + 1], d = grid[j + 1][i];
                    const bool flip = rng() & 1;  // vary the diagonal so edges run every direction
                    const Vec2 tris[2][3] = {{a, b, flip ? d : c}, {flip ? b : a, c, d}};
                    for (auto& t : tris)
                        for (Vec2 p : t) mesh.vertices.push_back({ndc(p), {0, 0, 1}, {0, 0}});
                }
            Framebuffer fb(W, H);
            Renderer r;
            r.settings.backfaceCull = false;
            r.settings.depthMode = DepthMode::OrderingTable;  // no depth test: raw coverage
            r.settings.debugView = DebugView::Overdraw;
            r.settings.shading = Shading::Flat;
            r.settings.useSimd = simd;
            r.settings.simdMinWidth = 0;  // send every triangle down the chosen loop
            r.beginFrame(fb, Mat4::identity(), Mat4::identity());
            r.drawMesh(fb, mesh, Mat4::identity(), {1, 1, 1});
            const uint16_t* od = fb.overdrawData();
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) {
                    const int count = od[y * W + x];
                    if (count > 1) ++doubles;
                    const bool interior = x + 0.5f > lo && x + 0.5f < hi && y + 0.5f > lo && y + 0.5f < hi;
                    if (interior && count == 0) ++cracks;
                }
        }
        CHECK_MSG(doubles == 0 && cracks == 0, "simd=%d snap=%d: %ld double-drawn, %ld cracks", simd, snap,
                  doubles, cracks);
    }
}

// ---- Scalar vs NEON: identical color and depth across a spread of configurations.
void testSimdMatchesScalar(const Mesh& bunny) {
    const Mesh cube = makeCube();
    const Texture tex = defaultTexture();
    int configs = 0, mismatches = 0;
    struct Size { int w, h; } sizes[] = {{960, 540}, {957, 533}};  // 957: a row end that isn't a multiple of 4
    for (Size sz : sizes)
        for (int m = 0; m < 2; ++m)
            for (float dist : {5.0f, 1.6f})  // 1.6 crosses the near plane
                for (Shading sh : {Shading::Flat, Shading::Gouraud, Shading::Phong})
                    for (int texOn = 0; texOn < 2; ++texOn)
                        for (DepthMode dm : {DepthMode::ZBuffer, DepthMode::OrderingTable}) {
                            const Mesh& mesh = m ? cube : bunny;
                            if (texOn && !mesh.hasTexCoords) continue;
                            const Mat4 model = fitToView(mesh);
                            OrbitCamera cam;
                            cam.distance = dist;
                            std::vector<uint32_t> color[2];
                            std::vector<float> depth[2];
                            for (int simd = 0; simd < 2; ++simd) {
                                Framebuffer fb(sz.w, sz.h);
                                Renderer r;
                                r.settings.useSimd = simd;
                                r.settings.simdMinWidth = 0;
                                r.settings.shading = sh;
                                r.settings.depthMode = dm;
                                r.settings.backfaceCull = (configs & 1) == 0;
                                r.settings.textureFilter = (configs & 2) ? TextureFilter::Nearest : TextureFilter::Bilinear;
                                r.settings.textureMapping = (configs & 4) ? TextureMapping::Affine : TextureMapping::PerspectiveCorrect;
                                r.beginFrame(fb, cam.view(), cam.projection(float(sz.w) / sz.h));
                                r.drawMesh(fb, mesh, model, {0.8f, 0.8f, 0.85f}, texOn ? &tex : nullptr);
                                r.endFrame(fb);
                                const size_t n = static_cast<size_t>(sz.w) * sz.h;
                                color[simd].assign(fb.colorData(), fb.colorData() + n);
                                depth[simd].assign(fb.depthData(), fb.depthData() + n);
                            }
                            ++configs;
                            if (color[0] != color[1] ||
                                std::memcmp(depth[0].data(), depth[1].data(), depth[0].size() * sizeof(float)) != 0)
                                ++mismatches;
                        }
    CHECK_MSG(mismatches == 0, "%d of %d configurations differ between the scalar and NEON loops", mismatches,
              configs);
}

// ---- Golden hashes of reference scenes.
uint64_t fnv1a(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

struct GoldenScene {
    const char* name;
    std::function<void(Renderer&, OrbitCamera&)> setup;
    bool bunny;
};

uint64_t renderScene(const GoldenScene& s, const Mesh& bunny) {
    const Mesh cube = makeCube();
    const Texture tex = defaultTexture();
    const Mesh& mesh = s.bunny ? bunny : cube;
    Framebuffer fb(960, 540);
    Renderer r;
    OrbitCamera cam;
    s.setup(r, cam);
    r.beginFrame(fb, cam.view(), cam.projection(960.0f / 540.0f));
    r.drawMesh(fb, mesh, fitToView(mesh), {0.8f, 0.8f, 0.85f}, mesh.hasTexCoords ? &tex : nullptr);
    r.endFrame(fb);
    return fnv1a(fb.colorData(), static_cast<size_t>(fb.width()) * fb.height() * 4);
}

uint64_t renderLiquid() {
    Framebuffer fb(960, 540);
    Renderer r;
    OrbitCamera cam;
    cam.distance = 3.5f;
    LiquidScene scene;
    for (int i = 0; i < 40; ++i) scene.update(1.0f / 60.0f, i < 20 ? 1.0f : 0.0f, 0.0f);  // slide, then slosh
    r.beginFrame(fb, cam.view(), cam.projection(960.0f / 540.0f));
    scene.draw(r, fb);
    r.endFrame(fb);
    return fnv1a(fb.colorData(), static_cast<size_t>(fb.width()) * fb.height() * 4);
}

void testGolden(const Mesh& bunny, bool update) {
    const std::vector<GoldenScene> scenes = {
        {"bunny_phong", [](Renderer&, OrbitCamera&) {}, true},
        {"bunny_flat_close", [](Renderer& r, OrbitCamera& c) { r.settings.shading = Shading::Flat; c.distance = 2.6f; }, true},
        {"bunny_gouraud", [](Renderer& r, OrbitCamera&) { r.settings.shading = Shading::Gouraud; }, true},
        {"bunny_nocull", [](Renderer& r, OrbitCamera&) { r.settings.backfaceCull = false; }, true},
        {"bunny_overdraw", [](Renderer& r, OrbitCamera&) { r.settings.debugView = DebugView::Overdraw; r.settings.backfaceCull = false; }, true},
        {"bunny_depth", [](Renderer& r, OrbitCamera&) { r.settings.debugView = DebugView::Depth; }, true},
        {"bunny_wireframe", [](Renderer& r, OrbitCamera&) { r.settings.debugView = DebugView::Wireframe; }, true},
        {"bunny_ps1", [](Renderer& r, OrbitCamera& c) { r.settings = RenderSettings::ps1(); c.distance = 2.6f; }, true},
        {"cube_textured", [](Renderer&, OrbitCamera&) {}, false},
        {"cube_close_nearest", [](Renderer& r, OrbitCamera& c) { r.settings.textureFilter = TextureFilter::Nearest; c.distance = 1.9f; c.yaw = 0.9f; c.pitch = 0.35f; }, false},
        {"cube_near_clip", [](Renderer& r, OrbitCamera& c) { r.settings.backfaceCull = false; c.distance = 3.0f; c.zNear = 2.4f; }, false},
        {"cube_ps1", [](Renderer& r, OrbitCamera& c) { r.settings = RenderSettings::ps1(); c.distance = 2.4f; c.yaw = 0.9f; c.pitch = 0.35f; }, false},
    };
    std::map<std::string, uint64_t> actual;
    for (const GoldenScene& s : scenes) actual[s.name] = renderScene(s, bunny);
    actual["liquid"] = renderLiquid();

    const std::string path = std::string(TESSERA_TEST_DIR) + "/golden_hashes.txt";
    if (update) {
        std::ofstream out(path);
        out << "# FNV-1a hashes of reference renders (tests/test_render.cpp). Regenerate with\n"
               "# tessera_test_render --update-golden, and say why in the commit.\n";
        for (const auto& [name, hash] : actual) out << name << " " << std::hex << hash << "\n";
        std::printf("wrote %s\n", path.c_str());
        return;
    }
    std::ifstream in(path);
    CHECK_MSG(in.good(), "missing %s (run with --update-golden)", path.c_str());
    std::map<std::string, uint64_t> expected;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string name;
        uint64_t hash = 0;
        ss >> name >> std::hex >> hash;
        expected[name] = hash;
    }
    for (const auto& [name, hash] : actual) {
        auto e = expected.find(name);
        CHECK_MSG(e != expected.end() && e->second == hash, "golden scene '%s' rendered differently", name.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool update = argc > 1 && std::strcmp(argv[1], "--update-golden") == 0;
    Mesh bunny;
    std::string error;
    if (!loadModel(std::string(TESSERA_ASSET_DIR) + "/bunny.obj", bunny, error)) {
        std::fprintf(stderr, "can't load the bunny: %s\n", error.c_str());
        return 1;
    }
    testGolden(bunny, update);
    if (update) return 0;
    testFillRule(false);
    testFillRule(true);
    testSimdMatchesScalar(bunny);
    return testResult("test_render");
}
