// test_scene.cpp — the scene model and its save format: exact round trips,
// validation of malformed files, and fuzzing the loader.
#include <cstring>
#include <random>
#include <string>

#include "check.h"
#include "scene.h"
#include "scene_render.h"
#include "app_common.h"
#include "camera.h"
#include "commands.h"

namespace {

Json parse(const std::string& text) {
    Json j;
    std::string error;
    Json::parse(text, j, error);
    return j;
}

void testDefaultRoundTrip() {
    const Scene s = Scene::makeDefault();
    CHECK(s.objects().size() == 3 && s.activeCamera() != kNoId);
    const std::string saved = s.toJson().dump(2);
    Json parsed;
    Scene loaded;
    std::string error;
    CHECK_MSG(Json::parse(saved, parsed, error) && Scene::fromJson(parsed, loaded, error), "%s", error.c_str());
    CHECK(loaded.toJson().dump(2) == saved);  // save -> load -> save is byte-identical
    CHECK(saved.size() < 4000);               // the cube is saved as primitive settings, not geometry

    // Files from before animation settings existed load with the defaults.
    Json old = Json::object();
    for (const auto& [key, value] : parsed.members())
        if (key != "animation") old.set(key, value);
    Scene fromOld;
    CHECK_MSG(Scene::fromJson(old, fromOld, error), "%s", error.c_str());
    CHECK(fromOld.animation().fps == 24 && fromOld.animation().startFrame == 0 && fromOld.animation().endFrame == 120);
    // And settings round-trip.
    Json custom = old;
    custom.set("animation", parse(R"({"fps":30,"start":10,"end":250})"));
    Scene withCustom;
    CHECK_MSG(Scene::fromJson(custom, withCustom, error), "%s", error.c_str());
    CHECK(withCustom.animation().fps == 30 && withCustom.animation().startFrame == 10 && withCustom.animation().endFrame == 250);
    custom.set("animation", parse(R"({"fps":30,"start":10,"end":5})"));
    CHECK(!Scene::fromJson(custom, withCustom, error));
}

void testImportedMeshRoundTrip() {
    Mesh bunny;
    std::string error;
    CHECK(loadModel(std::string(TESSERA_ASSET_DIR) + "/bunny.obj", bunny, error));
    const EditMesh edit = EditMesh::fromTriangles(bunny);
    const Json data = meshToJson(edit);
    Json text;
    CHECK(Json::parse(data.dump(), text, error));
    EditMesh back;
    CHECK_MSG(meshFromJson(text, back, error), "%s", error.c_str());
    CHECK(back.validate(error));
    // Geometry survives exactly: the triangulated meshes match byte for byte.
    const Mesh a = edit.triangulate(), b = back.triangulate();
    CHECK(a.vertices.size() == b.vertices.size() &&
          std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) == 0);
}

void testMalformed() {
    const Json good = Scene::makeDefault().toJson();
    const std::string goodText = good.dump();
    auto rejects = [&](const std::string& from, const std::string& to, const char* why) {
        std::string text = goodText;
        const size_t at = text.find(from);
        CHECK_MSG(at != std::string::npos, "test setup: '%s' not found", from.c_str());
        if (at == std::string::npos) return;
        text.replace(at, from.size(), to);
        Json j;
        std::string error;
        Scene target = Scene::makeDefault();
        const std::string before = target.toJson().dump();
        const bool ok = Json::parse(text, j, error) && Scene::fromJson(j, target, error);
        CHECK_MSG(!ok, "accepted a file with %s", why);
        CHECK(target.toJson().dump() == before);  // a failed load leaves the scene untouched
    };
    rejects("\"tessera-scene\"", "\"other\"", "the wrong format");
    rejects("\"version\":1", "\"version\":99", "an unknown version");
    rejects("\"type\":\"cube\"", "\"type\":\"teapot\"", "an unknown primitive");
    rejects("\"mesh\":1", "\"mesh\":7", "a missing mesh");
    rejects("\"activeCamera\":3", "\"activeCamera\":2", "a light as the camera");
    rejects("\"rotation\":[0,0,0,1]", "\"rotation\":[0,0,0,0]", "a zero quaternion");
    rejects("\"fov\":50", "\"fov\":500", "an out-of-range fov");
    rejects("\"name\":\"Cube\"", "\"name\":\"\"", "an empty name");
    rejects("\"id\":2", "\"id\":1", "duplicate object ids");
    rejects("\"nextObjectId\":4", "\"nextObjectId\":2", "ids beyond nextObjectId");
}

void testFuzzLoader() {
    // Mutate a saved scene with an imported mesh in it; the loader must never crash.
    Scene s = Scene::makeDefault();
    EditMesh tri;
    tri.addVert({0, 0, 0}), tri.addVert({1, 0, 0}), tri.addVert({0, 1, 0});
    tri.addFace({0, 1, 2});
    s.addMesh(std::move(tri));
    Json j = s.toJson();
    const std::string seed = j.dump();
    std::mt19937 rng(17);
    const char alphabet[] = "{}[]\",:0123456789-.e ";
    for (int t = 0; t < 5000; ++t) {
        std::string text = seed;
        for (int k = 0, n = 1 + rng() % 3; k < n; ++k) {
            const size_t at = rng() % text.size();
            if (rng() % 2) text[at] = alphabet[rng() % (sizeof(alphabet) - 1)];
            else text.erase(at, 1);
        }
        Json parsed;
        Scene out;
        std::string error;
        if (Json::parse(text, parsed, error)) Scene::fromJson(parsed, out, error);
    }
}

// The scene path (Scene -> SceneRenderer -> drawMesh) must draw a mesh object
// exactly like a direct drawMesh call: same pixels, byte for byte.
void testSceneRenderMatchesDirect() {
    Mesh bunny;
    std::string error;
    CHECK(loadModel(std::string(TESSERA_ASSET_DIR) + "/bunny.obj", bunny, error));
    const Mat4 fit = fitToView(bunny);
    OrbitCamera cam;
    const Mat4 view = cam.view(), proj = cam.projection(960.0f / 540.0f);

    Framebuffer direct(960, 540);
    Renderer r1;
    r1.beginFrame(direct, view, proj);
    r1.drawMesh(direct, bunny, fit, {0.8f, 0.8f, 0.85f});
    r1.endFrame(direct);

    Scene scene;  // no lights: the renderer keeps its default light, like the direct call
    CommandStack stack(scene);
    const MeshId mesh = scene.addMesh(EditMesh::fromTriangles(bunny));
    Json add = Json::object(), result;
    add.set("op", "addObject");
    add.set("type", "mesh");
    add.set("mesh", Json(mesh));
    Json t = Json::object();
    t.set("position", vec3ToJson({fit.m[0][3], fit.m[1][3], fit.m[2][3]}));
    t.set("scale", vec3ToJson({fit.m[0][0], fit.m[1][1], fit.m[2][2]}));
    add.set("transform", t);
    CHECK_MSG(stack.execute(add, result, error), "%s", error.c_str());
    Framebuffer viaScene(960, 540);
    Renderer r2;
    SceneRenderer sr;
    SceneRenderer::Overlays none;
    none.grid = false;
    none.gizmos = false;
    sr.render(r2, viaScene, scene, view, proj, none);
    CHECK(std::memcmp(direct.colorData(), viaScene.colorData(), 960 * 540 * 4) == 0);
}

}  // namespace

int main() {
    testSceneRenderMatchesDirect();
    testDefaultRoundTrip();
    testImportedMeshRoundTrip();
    testMalformed();
    testFuzzLoader();
    return testResult("test_scene");
}
