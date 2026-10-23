// test_commands.cpp — the command layer: validation leaves the scene untouched,
// undo/redo restore every intermediate state exactly, IDs are stable, groups undo
// as one step, and fuzzed commands never crash or half-apply.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "commands.h"

namespace {

// The scene's content, without the ID counters (IDs are never reused, so undoing
// an add deliberately doesn't wind nextObjectId back).
std::string content(const Scene& s) {
    const Json full = s.toJson();
    Json c = Json::object();
    for (const auto& [key, value] : full.members())
        if (key != "nextObjectId" && key != "nextMeshId") c.set(key, value);
    return c.dump();
}

Json cmd(const std::string& text) {
    Json j;
    std::string error;
    if (!Json::parse(text, j, error)) std::fprintf(stderr, "bad test JSON: %s\n", text.c_str());
    return j;
}

bool run(CommandStack& stack, const std::string& text, Json* result = nullptr) {
    Json r;
    std::string error;
    const bool ok = stack.execute(cmd(text), r, error);
    if (result) *result = r;
    return ok;
}

void testBasics() {
    Scene scene = Scene::makeDefault();
    CommandStack stack(scene);
    Json r;
    CHECK(run(stack, R"({"op":"addObject","type":"mesh","primitive":{"type":"sphere"}})", &r));
    const ObjectId sphere = static_cast<ObjectId>(r.find("id")->asNumber());
    CHECK(r.find("name")->asString() == "Sphere");
    CHECK(run(stack, R"({"op":"addObject","type":"mesh","primitive":{"type":"sphere"}})", &r));
    CHECK(r.find("name")->asString() == "Sphere.001");  // names stay unique
    const std::string id = std::to_string(sphere);
    CHECK(run(stack, R"({"op":"setTransform","id":)" + id + R"(,"position":[1,2,3],"rotationEuler":[0,90,0]})"));
    const Object* o = scene.find(sphere);
    CHECK(o && o->transform.position.y == 2 && std::fabs(o->transform.rotation.rotate({1, 0, 0}).z + 1) < 1e-5f);
    CHECK(run(stack, R"({"op":"setMaterial","id":)" + id + R"(,"color":[1,0,0],"checker":true})"));
    CHECK(scene.find(sphere)->material.color.x == 1 && scene.find(sphere)->material.checker);
    CHECK(run(stack, R"({"op":"setShader","id":)" + id + R"(,"type":"glass","params":{"rimStart":0.5}})"));
    CHECK(run(stack, R"({"op":"setName","id":)" + id + R"(,"name":"Ball"})"));
    CHECK(scene.find(sphere)->name == "Ball");
    CHECK(run(stack, R"({"op":"addKeyframe","id":)" + id + R"(,"property":"position","time":1,"value":[0,0,0],"easing":"easeInOut"})"));
    CHECK(run(stack, R"({"op":"addKeyframe","id":)" + id + R"(,"property":"position","time":0,"value":[1,1,1]})"));
    CHECK(scene.find(sphere)->tracks.size() == 1 && scene.find(sphere)->tracks[0].keys[0].time == 0);  // kept sorted
    CHECK(run(stack, R"({"op":"moveKeyframe","id":)" + id + R"(,"property":"position","time":0,"newTime":2})"));
    CHECK(scene.find(sphere)->tracks[0].keys[1].time == 2);
    CHECK(run(stack, R"({"op":"setLight","id":2,"intensity":3})"));
    CHECK(run(stack, R"({"op":"setCamera","id":3,"fov":35})"));
    CHECK(run(stack, R"({"op":"setWorld","ambient":0.3})"));
    CHECK(run(stack, R"({"op":"setVisible","id":1,"visible":false})"));
    CHECK(run(stack, R"({"op":"removeObject","id":)" + id + "}"));
    CHECK(!scene.find(sphere));
}

void testValidation() {
    Scene scene = Scene::makeDefault();
    CommandStack stack(scene);
    const std::string before = scene.toJson().dump();
    const char* bad[] = {
        R"({"op":"explode"})",                                                     // unknown op
        R"({"op":"setTransform","id":1,"positon":[0,0,0]})",                       // misspelled field
        R"({"op":"setTransform","id":99,"position":[0,0,0]})",                     // no such object
        R"({"op":"setTransform","id":1,"position":[0,0]})",                        // wrong shape
        R"({"op":"setTransform","id":1,"rotation":[0,0,0,0]})",                    // not a rotation
        R"({"op":"setTransform","id":1,"rotation":[0,0,0,1],"rotationEuler":[0,0,0]})",
        R"({"op":"setTransform","id":1,"position":[1e30,0,0]})",                   // out of range
        R"({"op":"setLight","id":1,"intensity":2})",                               // the cube isn't a light
        R"({"op":"setMaterial","id":2,"color":[1,0,0]})",                          // the sun has no material
        R"({"op":"setMaterial","id":1,"color":[-1,0,0]})",                         // negative color
        R"({"op":"setShader","id":1,"type":"lava"})",                              // unknown shader
        R"({"op":"setShader","id":1,"type":"liquid","params":{"fill":1}})",        // unknown shader setting
        R"({"op":"addObject","type":"mesh"})",                                     // no geometry
        R"({"op":"addObject","type":"light","material":{"color":[1,1,1]}})",       // material on a light
        R"({"op":"addObject","type":"mesh","primitive":{"type":"cube"},"id":1})",  // id taken
        R"({"op":"addObject","type":"mesh","primitive":{"type":"cube"},"index":9})",
        R"({"op":"addKeyframe","id":1,"property":"fov","time":0,"value":[50]})",   // a cube has no fov
        R"({"op":"addKeyframe","id":1,"property":"position","time":-1,"value":[0,0,0]})",
        R"({"op":"removeKeyframe","id":1,"property":"position","time":0})",        // nothing there
        R"({"op":"setActiveCamera","id":1})",                                      // the cube isn't a camera
        R"({"op":"addObject","type":"light","transform":{"rotationEuler":[0,0,0]}})",  // Euler only in setTransform
        R"({"op":"setAnimation","fps":0})",                                        // no frames per second
        R"({"op":"setAnimation","start":10,"end":5})",                             // ends before it starts
        R"({"op":"setAnimation","end":12.5})",                                     // not a whole frame
        R"({"op":"setAnimation","speed":2})",                                      // unknown setting
    };
    for (const char* text : bad) {
        Json r;
        std::string error;
        CHECK_MSG(!stack.execute(cmd(text), r, error), "accepted: %s", text);
        CHECK_MSG(!error.empty(), "no error message for: %s", text);
    }
    CHECK(content(scene) == content(Scene::makeDefault()));  // nothing changed...
    CHECK(scene.toJson().dump() == before);
    CHECK(!stack.canUndo());  // ...and nothing was recorded
}

// Random edit sessions: snapshot after every command, then undo all the way back
// and redo all the way forward, comparing against every snapshot.
void testUndoRedoRandom() {
    std::mt19937 rng(21);
    for (int session = 0; session < 30; ++session) {
        Scene scene = Scene::makeDefault();
        CommandStack stack(scene);
        std::vector<std::string> snapshots = {content(scene)};
        auto randomId = [&] {
            const auto& objs = scene.objects();
            return objs.empty() ? std::string("1") : std::to_string(objs[rng() % objs.size()].id);
        };
        auto num = [&] { return std::to_string(int(rng() % 200) / 10.0 - 10.0); };
        for (int i = 0; i < 60; ++i) {
            std::string c;
            switch (rng() % 10) {
                case 0: c = R"({"op":"addObject","type":"mesh","primitive":{"type":")" + std::string(primitiveName(PrimitiveType(rng() % 5))) + R"("}})"; break;
                case 1: c = R"({"op":"removeObject","id":)" + randomId() + "}"; break;
                case 2: c = R"({"op":"setTransform","id":)" + randomId() + R"(,"position":[)" + num() + "," + num() + "," + num() + R"(],"rotationEuler":[)" + num() + ",0," + num() + "]}"; break;
                case 3: c = R"({"op":"setMaterial","id":)" + randomId() + R"(,"color":[0.5,0.2,0.9]})"; break;
                case 4: c = R"({"op":"setName","id":)" + randomId() + R"(,"name":"Thing"})"; break;
                case 5: c = R"({"op":"addKeyframe","id":)" + randomId() + R"(,"property":"scale","time":)" + std::to_string(rng() % 4) + R"(,"value":[1,2,1]})"; break;
                case 6: c = R"({"op":"removeKeyframe","id":)" + randomId() + R"(,"property":"scale","time":)" + std::to_string(rng() % 4) + "}"; break;
                case 7: c = R"({"op":"addObject","type":"camera","activeCamera":true})"; break;
                case 8: c = R"({"op":"setAnimation","fps":30,"end":)" + std::to_string(10 + rng() % 200) + "}"; break;
                default: c = R"({"op":"setVisible","id":)" + randomId() + R"(,"visible":false})";
            }
            if (run(stack, c)) snapshots.push_back(content(scene));  // invalid ones (e.g. wrong type) just don't count
        }
        for (size_t k = snapshots.size() - 1; k > 0; --k) {
            CHECK(stack.undo());
            CHECK_MSG(content(scene) == snapshots[k - 1], "session %d: undo to state %zu differs", session, k - 1);
        }
        CHECK(!stack.canUndo());
        for (size_t k = 1; k < snapshots.size(); ++k) {
            CHECK(stack.redo());
            CHECK_MSG(content(scene) == snapshots[k], "session %d: redo to state %zu differs", session, k);
        }
        CHECK(!stack.canRedo());
    }
}

void testIdsAndGroups() {
    Scene scene = Scene::makeDefault();
    CommandStack stack(scene);
    CHECK(run(stack, R"({"op":"removeObject","id":2})"));
    CHECK(stack.undo());
    CHECK(scene.find(2) && scene.indexOf(2) == 1);  // same id, same place
    Json r;
    CHECK(run(stack, R"({"op":"addObject","type":"light"})", &r));
    CHECK(r.find("id")->asNumber() == 4);
    CHECK(stack.undo());
    CHECK(run(stack, R"({"op":"addObject","type":"light"})", &r));
    CHECK(r.find("id")->asNumber() == 5);  // 4 is never handed out again
    CHECK(!stack.canRedo());               // a new command ends the redo branch

    const std::string before = content(scene);
    const uint64_t revision = stack.revision();
    stack.beginGroup("Drag");
    for (int i = 0; i < 5; ++i) CHECK(run(stack, R"({"op":"setTransform","id":1,"position":[)" + std::to_string(i) + ",0,0]}"));
    stack.endGroup();
    CHECK(stack.revision() == revision + 6);  // 5 commands, then closing the group
    CHECK(stack.undoLabel() == "Drag");
    CHECK(stack.undo());
    CHECK(content(scene) == before);  // the whole drag undoes as one step

    // A cancelled group puts everything back and records nothing.
    const std::string undoLabel = stack.undoLabel();
    stack.beginGroup("Cancelled drag");
    CHECK(run(stack, R"({"op":"setTransform","id":1,"position":[7,0,0]})"));
    CHECK(run(stack, R"({"op":"setName","id":1,"name":"Moved"})"));
    stack.cancelGroup();
    CHECK(content(scene) == before);
    CHECK(stack.canUndo() && stack.undoLabel() == undoLabel);
}

void testFuzz() {
    Scene scene = Scene::makeDefault();
    CommandStack stack(scene);
    const std::string seeds[] = {
        R"({"op":"addObject","type":"mesh","primitive":{"type":"cone","segments":12},"transform":{"position":[1,0,0]}})",
        R"({"op":"setTransform","id":1,"position":[1,2,3],"rotation":[0,0,0,1],"scale":[1,1,1]})",
        R"({"op":"addKeyframe","id":1,"property":"rotation","time":1.5,"value":[0,0.7,0,0.7],"easing":"step"})",
        R"({"op":"setShader","id":1,"type":"liquid","params":{"fillLevel":0.2,"layers":[[1,0,0],[0,1,0]]}})",
    };
    std::mt19937 rng(23);
    const char alphabet[] = "{}[]\",:0123456789-.e abcdefghijklmnopqrstuvwxyz";
    for (int t = 0; t < 10000; ++t) {
        std::string text = seeds[rng() % 4];
        for (int k = 0, n = 1 + rng() % 3; k < n; ++k) {
            const size_t at = rng() % text.size();
            if (rng() % 2) text[at] = alphabet[rng() % (sizeof(alphabet) - 1)];
            else text.erase(at, 1);
        }
        Json c, r;
        std::string error;
        if (!Json::parse(text, c, error)) continue;
        const std::string before = content(scene);
        if (!stack.execute(c, r, error)) CHECK_MSG(content(scene) == before, "a failed command changed the scene: %s", text.c_str());
    }
    while (stack.undo()) {
    }
    CHECK(content(scene) == content(Scene::makeDefault()));  // and all of it undoes
}

}  // namespace

int main() {
    testBasics();
    testValidation();
    testUndoRedoRandom();
    testIdsAndGroups();
    testFuzz();
    return testResult("test_commands");
}
