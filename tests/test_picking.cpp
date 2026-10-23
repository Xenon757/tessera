// test_picking.cpp — click-to-select must agree with what's drawn: render objects in
// distinct flat colors, then check that picking each pixel returns the object whose
// color the renderer put there.
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "camera.h"
#include "check.h"
#include "commands.h"
#include "picking.h"
#include "scene_render.h"

namespace {

Json parse(const std::string& text) {
    Json j;
    std::string error;
    Json::parse(text, j, error);
    return j;
}

void testInverse() {
    const Mat4 m = Mat4::translate({1, -2, 3}) * Quat::fromEulerXYZ({20, -50, 70}).toMat4() * Mat4::scale({2, 0.5f, 3});
    Mat4 inv;
    CHECK(invert(m, inv));
    const Mat4 id = m * inv;
    float worst = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(id.m[r][c] - (r == c ? 1.0f : 0.0f)));
    CHECK_MSG(worst < 1e-5f, "M * M^-1 off identity by %g", worst);
    CHECK(!invert(Mat4::scale({1, 0, 1}), inv));  // singular
}

void testPickingMatchesRendering() {
    Scene scene;
    CommandStack stack(scene);
    Json r;
    std::string error;
    auto run = [&](const std::string& text) {
        const bool ok = stack.execute(parse(text), r, error);
        CHECK_MSG(ok, "%s: %s", text.c_str(), error.c_str());
        return ok ? static_cast<ObjectId>(r.find("id") ? r.find("id")->asNumber() : 0) : kNoId;
    };
    run(R"({"op":"setWorld","ambient":1})");  // flat colors: diffuse = ambient = 1
    std::map<uint32_t, ObjectId> byColor;
    const char* objects[] = {
        R"({"op":"addObject","type":"mesh","primitive":{"type":"cube"},"material":{"color":[0.2,0,0],"specular":0}})",
        R"({"op":"addObject","type":"mesh","primitive":{"type":"sphere"},"transform":{"position":[1.2,0.5,0.8],"scale":[0.7,0.7,0.7]},"material":{"color":[0,0.4,0],"specular":0}})",
        R"({"op":"addObject","type":"mesh","primitive":{"type":"cone","segments":7},"transform":{"position":[-1.5,0.3,0.5],"rotation":[0.3,0.2,0.1,0.9],"scale":[0.6,1.8,0.4]},"material":{"color":[0,0,0.6],"specular":0}})",
        R"({"op":"addObject","type":"mesh","primitive":{"type":"plane"},"transform":{"position":[0,-1.2,0],"scale":[3,1,3]},"material":{"color":[0.8,0.8,0],"specular":0}})",
    };
    const Vec3 colors[] = {{0.2f, 0, 0}, {0, 0.4f, 0}, {0, 0, 0.6f}, {0.8f, 0.8f, 0}};
    for (int i = 0; i < 4; ++i) byColor[packColor(colors[i])] = run(objects[i]);
    const ObjectId hidden = run(R"({"op":"addObject","type":"mesh","primitive":{"type":"sphere"},"transform":{"position":[0,0,1.5]}})");
    run(R"({"op":"setVisible","id":)" + std::to_string(hidden) + R"(,"visible":false})");

    const int W = 480, H = 270;
    OrbitCamera cam;
    cam.distance = 6;
    const Mat4 view = cam.view(), proj = cam.projection(float(W) / H);
    Framebuffer fb(W, H);
    Renderer renderer;
    SceneRenderer sr;
    SceneRenderer::Overlays none;
    none.grid = none.gizmos = false;
    sr.render(renderer, fb, scene, 0, view, proj, none);

    int checked = 0, disagree = 0, hits = 0;
    for (int y = 0; y < H; y += 3)
        for (int x = 0; x < W; x += 3) {
            const uint32_t drawn = fb.getPixel(x, y);
            const auto it = byColor.find(drawn);
            const ObjectId expected = it == byColor.end() ? kNoId : it->second;
            const ObjectId picked = pickObject(scene, rayThroughPixel(view, proj, float(x), float(y), W, H), 0);
            ++checked;
            hits += picked != kNoId;
            if (picked != expected) ++disagree;
        }
    CHECK_MSG(hits > checked / 4, "only %d of %d pixels hit anything: the test scene isn't in view", hits, checked);
    CHECK_MSG(disagree <= checked / 500, "picking disagrees with rendering on %d of %d pixels", disagree, checked);
    std::printf("picking vs rendering: %d of %d sampled pixels disagree\n", disagree, checked);
}

void testGizmosAndSceneCamera() {
    Scene scene = Scene::makeDefault();
    OrbitCamera cam;
    cam.distance = 12;
    const Mat4 view = cam.view(), proj = cam.projection(16.0f / 9.0f);
    // Project the light's position to a pixel and click there.
    const Object* light = scene.find(2);
    const Vec4 c = proj * view * Vec4(light->transform.position, 1.0f);
    const float px = (c.x / c.w * 0.5f + 0.5f) * 960 - 0.5f, py = (1 - (c.y / c.w * 0.5f + 0.5f)) * 540 - 0.5f;
    CHECK(pickObject(scene, rayThroughPixel(view, proj, px, py, 960, 540), 0) == 2);
    // Through the scene camera, the center of the screen is on the cube.
    Mat4 camView, camProj;
    CHECK(SceneRenderer::cameraView(scene, scene.activeCamera(), 0, 16.0f / 9.0f, camView, camProj));
    CHECK(pickObject(scene, rayThroughPixel(camView, camProj, 479.5f, 269.5f, 960, 540), 0) == 1);
    CHECK(pickObject(scene, rayThroughPixel(camView, camProj, 2, 2, 960, 540), 0) == kNoId);  // a corner: nothing
}

}  // namespace

int main() {
    testInverse();
    testPickingMatchesRendering();
    testGizmosAndSceneCamera();
    return testResult("test_picking");
}
