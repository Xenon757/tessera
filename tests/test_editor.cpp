// test_editor.cpp — the editor session the front ends drive: state for the panels,
// import, examples, save/load, and selection staying valid across undo.
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

}  // namespace

int main() {
    testStateAndSelection();
    testImportExampleSaveLoad();
    return testResult("test_editor");
}
