// test_render.cpp — rasterizer invariants and output stability.
//
//   fill rule      a tiling of triangles shades every interior pixel exactly once
//   simd           the NEON pixel loop is byte-identical to the scalar loop
//   light color    a colored, brighter light tints and scales diffuse + specular only
//   shadow map     the light's depth image holds the nearest caster's depth, from either
//                  side of an open mesh, identically at any thread count
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
                        for (DepthMode dm : {DepthMode::ZBuffer, DepthMode::OrderingTable})
                            for (int colored = 0; colored < 2; ++colored) {
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
                                    r.settings.backfaceCull = ((configs >> 1) & 1) == 0;  // >> 1: the same mix with and without color
                                    r.settings.textureFilter = ((configs >> 1) & 2) ? TextureFilter::Nearest : TextureFilter::Bilinear;
                                    r.settings.textureMapping = ((configs >> 1) & 4) ? TextureMapping::Affine : TextureMapping::PerspectiveCorrect;
                                    if (colored) {
                                        r.lightColor = {1.0f, 0.55f, 0.2f};
                                        r.lightIntensity = 1.7f;
                                    }
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

// ---- Light color and intensity. The bunny lit by a red light at intensity 2:
// green and blue get no light but ambient (the light has none of them, and
// ambient isn't tinted), and red gets twice the light's share. Setting the
// defaults explicitly must change nothing (the goldens pin the defaults).
void testLightColor(const Mesh& bunny) {
    const Mat4 model = fitToView(bunny);
    const Vec3 base{0.8f, 0.8f, 0.85f};
    auto render = [&](bool setLight, Vec3 color, float intensity, Framebuffer& fb) {
        Renderer r;
        if (setLight) {
            r.lightColor = color;
            r.lightIntensity = intensity;
        }
        OrbitCamera cam;
        r.beginFrame(fb, cam.view(), cam.projection(float(fb.width()) / fb.height()));
        r.drawMesh(fb, bunny, model, base);
        r.endFrame(fb);
    };
    const size_t n = 960 * 540;
    Framebuffer untouched(960, 540), white(960, 540), red(960, 540);
    render(false, {}, 0, untouched);
    render(true, {1, 1, 1}, 1.0f, white);
    render(true, {1, 0, 0}, 2.0f, red);
    CHECK(std::memcmp(untouched.colorData(), white.colorData(), n * 4) == 0);

    // Green and blue: base * ambient + 0, exactly, wherever the bunny is.
    const Renderer defaults;
    const uint32_t ambientOnly = packColor(base * defaults.ambient) & 0x0000FFFFu;
    int covered = 0, wrongGB = 0, wrongR = 0, brighter = 0;
    for (size_t i = 0; i < n; ++i) {
        if (red.depthData()[i] >= 1.0f) continue;  // background
        ++covered;
        const uint32_t c = red.colorData()[i];
        if ((c & 0x0000FFFFu) != ambientOnly) ++wrongGB;
        // Red: base*(ambient + 2*direct) + 2*spec = 2*(white's red) - base*ambient,
        // up to 8-bit rounding, until it saturates.
        const int whiteR = (white.colorData()[i] >> 16) & 0xFF, redR = (c >> 16) & 0xFF;
        const float expected = 2.0f * whiteR - base.x * defaults.ambient * 255.0f;
        if (expected < 254.0f ? std::fabs(redR - expected) > 1.5f : redR < 254) ++wrongR;
        if (redR > whiteR) ++brighter;
    }
    CHECK(covered > 10000);
    CHECK_MSG(wrongGB == 0, "%d of %d bunny pixels have light in green or blue", wrongGB, covered);
    CHECK_MSG(wrongR == 0, "%d of %d bunny pixels have the wrong red", wrongR, covered);
    CHECK_MSG(brighter > covered / 2, "only %d of %d bunny pixels got brighter red", brighter, covered);
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

// ---- Shadows: a 4 x 4 floor at y = 0 and a cube (2 units, scaled to 1) hovering at
// y = 1.5, with the sun straight overhead.
Mesh floorQuad(float half) {
    Mesh m;
    const Vec3 n{0, 1, 0};
    const Vec3 a{-half, 0, -half}, b{-half, 0, half}, c{half, 0, half}, d{half, 0, -half};
    for (const Vec3& p : {a, b, c, a, c, d}) m.vertices.push_back({p, n, {0, 0}});  // counter-clockwise from above
    return m;
}

void testShadowMap() {
    const Mesh floor = floorQuad(2.0f), cube = makeCube();
    const Mat4 cubeModel = Mat4::translate({0, 1.5f, 0}) * Mat4::scale({0.5f, 0.5f, 0.5f});
    auto render = [&](const Vec3& lightDir, int threads) {
        Renderer r;
        r.settings.threadCount = threads;
        r.settings.shadowMapSize = 64;
        r.lightDir = lightDir;
        r.beginShadowMap({0, 0, 0}, 3.0f);
        r.drawShadowCaster(floor, Mat4::identity());
        r.drawShadowCaster(cube, cubeModel);
        return std::vector<float>(r.shadowMap().depthData(), r.shadowMap().depthData() + 64 * 64);
    };
    // The light's camera is 2r = 6 above the center, and depth runs 0..4r = 12: the
    // cube's top (y = 2) is 4 away, depth 1/3; the floor (y = 0) is 6 away, depth 1/2.
    const std::vector<float> map = render({0, 1, 0}, 1);
    auto at = [&](int x, int y) { return map[static_cast<size_t>(y) * 64 + x]; };
    CHECK_MSG(std::fabs(at(32, 32) - 1.0f / 3.0f) < 1e-5f, "%f", at(32, 32));  // under the cube: its top
    CHECK_MSG(std::fabs(at(32, 16) - 0.5f) < 1e-5f, "%f", at(32, 16));        // the floor, 1.5 units out
    CHECK(at(1, 1) == 1.0f);  // beyond the floor's corner: nothing, still cleared
    // The same at any thread count, byte for byte.
    CHECK(render({0, 1, 0}, 4) == map);
    // From below, the floor still casts (no back-face culling in this pass): it's the
    // nearest thing to a light at y = -6, at depth 1/2.
    const std::vector<float> below = render({0, -1, 0}, 1);
    CHECK_MSG(std::fabs(below[32 * 64 + 32] - 0.5f) < 1e-5f, "%f", below[32 * 64 + 32]);
}

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
    testLightColor(bunny);
    testShadowMap();
    return testResult("test_render");
}
