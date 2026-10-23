// test_editor.cpp — the editor session the front ends drive: state for the panels,
// import, examples, save/load, selection staying valid across undo, and G/R/S.
#include <cmath>
#include <string>

#include "check.h"
#include "editor.h"

namespace {

Json parse(const std::string& text) {
    Json j;
    std::string error;
    Json::parse(text, j, error);
    return j;
}

void testStateAndSelection() {
    Editor e;
    const Json s = e.state();
    CHECK(s.find("objects")->size() == 3 && !s.find("canUndo")->asBool());
    CHECK((*s.find("objects"))[0].find("meshInfo")->find("primitive")->asString() == "cube");
    CHECK((*s.find("objects"))[0].find("rotationEuler"));

    const uint64_t before = e.uiRevision();
    const Json r = e.command(parse(R"({"op":"addObject","type":"mesh","primitive":{"type":"cone"}})"));
    CHECK(r.find("ok")->asBool());
    CHECK(e.uiRevision() > before);
    const ObjectId cone = static_cast<ObjectId>(r.find("result")->find("id")->asNumber());
    e.select(cone);
    CHECK(e.state().find("selected")->asNumber() == cone);
    CHECK(e.undo());
    CHECK(e.state().find("selected")->asNumber() == kNoId);  // undo removed it: no stale selection
    CHECK(!e.command(parse(R"({"op":"nope"})")).find("ok")->asBool());
    CHECK(e.command(parse(R"({"op":"nope"})")).find("error")->asString().find("unknown op") != std::string::npos);

    // Clicking the middle of the screen through the scene camera picks the cube.
    e.setViewThroughCamera(true);
    CHECK(e.pick(479.5f, 269.5f, 960, 540) == 1);
    CHECK(e.pick(2, 2, 960, 540) == kNoId);
}

void testImportExampleSaveLoad() {
    Editor e;
    const Json r = e.importModel(std::string(TESSERA_ASSET_DIR) + "/bunny.obj");
    CHECK_MSG(r.find("ok")->asBool(), "%s", r.dump().c_str());
    CHECK(e.scene().objects().size() == 4 && e.selected() == 4);
    CHECK(e.scene().find(4)->name == "bunny");
    CHECK(!e.importModel("/no/such/file.obj").find("ok")->asBool());

    // Save, start over, load: the same scene, byte for byte, with an empty history.
    const std::string saved = e.save();
    e.newScene();
    CHECK(e.scene().objects().size() == 3);
    std::string error;
    CHECK_MSG(e.load(saved, error), "%s", error.c_str());
    CHECK(e.save() == saved && !e.state().find("canUndo")->asBool());
    CHECK(!e.load("{\"format\":\"nope\"}", error) && e.save() == saved);  // a bad file changes nothing
    CHECK(e.undo() == false);

    CHECK_MSG(e.loadExample("liquid", error), "%s", error.c_str());
    int liquids = 0, glasses = 0;
    for (const Object& o : e.scene().objects()) liquids += o.shader.type == "liquid", glasses += o.shader.type == "glass";
    CHECK(liquids == 1 && glasses == 1 && !e.scene().find(1));
    CHECK(!e.loadExample("teapot", error));

    // The example renders, and edits show up in the next frame. (Sloshing itself is
    // covered by the wobble spring's own checks; here the move alone changes pixels.)
    Renderer renderer;
    Framebuffer fb(320, 180);
    e.setViewThroughCamera(true);
    for (int i = 0; i < 5; ++i) e.render(renderer, fb, 1.0f / 60);
    const std::vector<uint32_t> settled(fb.colorData(), fb.colorData() + 320 * 180);
    ObjectId liquid = kNoId;
    for (const Object& o : e.scene().objects())
        if (o.shader.type == "liquid") liquid = o.id;
    e.command(parse(R"({"op":"setTransform","id":)" + std::to_string(liquid) + R"(,"position":[0.2,0.03,0]})"));
    for (int i = 0; i < 3; ++i) e.render(renderer, fb, 1.0f / 60);
    CHECK(std::vector<uint32_t>(fb.colorData(), fb.colorData() + 320 * 180) != settled);
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

// The pixel a world point lands on in the editor's view (y down, pixel centers).
void project(const Editor& e, const Vec3& p, float& x, float& y) {
    Mat4 view, projection;
    e.viewMatrices(960.0f / 540.0f, view, projection);
    const Vec4 c = projection * view * Vec4(p, 1.0f);
    x = (c.x / c.w * 0.5f + 0.5f) * 960 - 0.5f;
    y = (0.5f - c.y / c.w * 0.5f) * 540 - 0.5f;
}

// The on-screen direction (degrees, counterclockwise) of the cube's local X axis.
float screenAngleOfX(const Editor& e) {
    const Transform& t = e.scene().find(1)->transform;
    float px, py, ax, ay;
    project(e, t.position, px, py);
    project(e, t.position + t.rotation.rotate({0.3f, 0, 0}), ax, ay);
    return std::atan2(-(ay - py), ax - px) * 180.0f / kPi;
}

void testTransform() {
    Editor e;
    auto cube = [&]() -> const Transform& { return e.scene().find(1)->transform; };
    auto canUndo = [&] { return e.state().find("canUndo")->asBool(); };
    CHECK(!e.beginTransform(TransformMode::Grab, 480, 270, 960, 540));  // nothing selected
    e.select(1);

    // Confirming without moving records nothing.
    CHECK(e.beginTransform(TransformMode::Rotate, 480, 270, 960, 540));
    CHECK(e.transformKey("Enter"));
    CHECK(!canUndo());

    // Typed: G X 2 Enter moves exactly 2 along X, as one undo step.
    CHECK(e.beginTransform(TransformMode::Grab, 480, 270, 960, 540));
    CHECK(e.state().find("transform")->asString() == "Move 0.000, 0.000, 0.000");
    for (const char* k : {"x", "2", "Enter"}) CHECK(e.transformKey(k));
    CHECK(!e.transforming() && !e.state().find("transform"));
    CHECK(near(cube().position, {2, 0, 0}));
    CHECK(e.state().find("undoLabel")->asString() == "Move Cube");
    CHECK(e.undo());
    CHECK(near(cube().position, {0, 0, 0}) && !canUndo());

    // Locked to X: dragging from the cube's center to where (1.5, 0, 0) is on
    // screen puts it there (the nearest point of the axis to the mouse ray).
    float x0, y0, x1, y1;
    project(e, {0, 0, 0}, x0, y0);
    project(e, {1.5f, 0, 0}, x1, y1);
    CHECK(e.beginTransform(TransformMode::Grab, x0, y0, 960, 540));
    CHECK(e.transformKey("X"));
    e.transformMouse(x1, y1);
    CHECK(near(cube().position, {1.5f, 0, 0}, 1e-3f));
    // Unlocked: it slides across the plane facing the camera and stays under the mouse.
    CHECK(e.transformKey("x"));
    float px, py;
    project(e, cube().position, px, py);
    CHECK(std::fabs(px - x1) < 0.01f && std::fabs(py - y1) < 0.01f);
    CHECK(!near(cube().position, {1.5f, 0, 0}, 1e-2f));  // a different point than the axis gave
    // Escape puts it back and records nothing.
    CHECK(e.transformKey("Escape"));
    CHECK(near(cube().position, {0, 0, 0}) && !canUndo());

    // R Z 90: a quarter turn around world Z.
    CHECK(e.beginTransform(TransformMode::Rotate, 480, 270, 960, 540));
    for (const char* k : {"z", "9", "0", "Enter"}) CHECK(e.transformKey(k));
    CHECK(near(cube().rotation.rotate({1, 0, 0}), {0, 1, 0}));
    CHECK(e.state().find("undoLabel")->asString() == "Rotate Cube");
    CHECK(e.undo());

    // S 2, then S Y 3 ("-" twice cancels out; Backspace drops the 4).
    CHECK(e.beginTransform(TransformMode::Scale, 480, 270, 960, 540));
    for (const char* k : {"2", "Enter"}) CHECK(e.transformKey(k));
    CHECK(e.beginTransform(TransformMode::Scale, 480, 270, 960, 540));
    for (const char* k : {"y", "-", "-", "3", "4", "Backspace", "Enter"}) CHECK(e.transformKey(k));
    CHECK(near(cube().scale, {2, 6, 2}));
    CHECK(!e.transformKey("q"));  // not transforming: ignored
    CHECK(e.undo() && e.undo() && near(cube().scale, {1, 1, 1}));

    // Rotating with the mouse: circling the cube counterclockwise on screen turns
    // it counterclockwise, and going past 180 degrees keeps counting. Off center,
    // so the pivot's screen position matters.
    CHECK(e.command(parse(R"({"op":"setTransform","id":1,"position":[1.5,0.8,-0.5]})")).find("ok")->asBool());
    project(e, cube().position, px, py);
    const float before = screenAngleOfX(e);
    CHECK(e.beginTransform(TransformMode::Rotate, px + 100, py, 960, 540));
    e.transformMouse(px, py - 100);  // up on screen: a quarter turn counterclockwise
    float turned = screenAngleOfX(e) - before;
    turned -= 360.0f * std::round(turned / 360.0f);
    CHECK_MSG(std::fabs(turned - 90.0f) < 10.0f, "turned %.1f degrees on screen", turned);
    e.transformMouse(px - 100, py);
    e.transformMouse(px, py + 100);
    CHECK(e.state().find("transform")->asString() == "Rotate 270.0°");
    e.transformMouse(px - 100, py - 100);  // back clockwise across the left side, to upper left
    CHECK(e.state().find("transform")->asString() == "Rotate 135.0°");
    CHECK(e.transformKey("Escape"));
    // Locked to world Y (seen from above here) it still follows the mouse around.
    CHECK(e.beginTransform(TransformMode::Rotate, px + 100, py, 960, 540));
    CHECK(e.transformKey("y"));
    e.transformMouse(px, py - 100);
    turned = screenAngleOfX(e) - before;
    turned -= 360.0f * std::round(turned / 360.0f);
    CHECK_MSG(turned > 20.0f, "turned %.1f degrees on screen around Y", turned);
    CHECK(e.transformKey("Escape"));
    CHECK(e.undo());

    // Anything else finishes a transform first; loading a scene ends it.
    CHECK(e.beginTransform(TransformMode::Grab, 480, 270, 960, 540));
    CHECK(e.transformKey("1"));
    CHECK(e.command(parse(R"({"op":"setName","id":1,"name":"Box"})")).find("ok")->asBool());
    CHECK(!e.transforming() && near(cube().position, {1, 0, 0}));
    CHECK(e.beginTransform(TransformMode::Grab, 480, 270, 960, 540));
    e.newScene();
    CHECK(!e.transforming());
}

}  // namespace

int main() {
    testStateAndSelection();
    testImportExampleSaveLoad();
    testTransform();
    return testResult("test_editor");
}
