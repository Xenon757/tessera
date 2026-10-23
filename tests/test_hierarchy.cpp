// test_hierarchy.cpp — parenting: world transforms through parent chains (animated
// too), converting back to local, loop prevention, setParent keeping objects (and
// their keys) in place with exact undo, and save/load of parents.
#include <cmath>
#include <string>

#include "animation.h"
#include "check.h"
#include "commands.h"
#include "hierarchy.h"

namespace {

Json parse(const std::string& text) {
    Json j;
    std::string error;
    Json::parse(text, j, error);
    return j;
}

bool run(CommandStack& stack, const std::string& text, std::string* errorOut = nullptr) {
    Json r;
    std::string error;
    const bool ok = stack.execute(parse(text), r, error);
    if (errorOut) *errorOut = error;
    return ok;
}

bool near(const Vec3& a, const Vec3& b, float eps = 1e-4f) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}
bool sameRotation(const Quat& a, const Quat& b) { return std::fabs(std::fabs(dot(a, b)) - 1.0f) < 1e-5f; }


void testWorldTransforms() {
    Scene s = Scene::makeDefault();
    CommandStack stack(s);
    // Parent: moved to (0, 2, 0), turned 90 degrees about Y, scaled 2. Child: one unit along its parent's X.
    CHECK(run(stack, R"({"op":"setTransform","id":1,"position":[0,2,0],"rotationEuler":[0,90,0],"scale":[2,2,2]})"));
    CHECK(run(stack, R"({"op":"addObject","type":"mesh","primitive":{"type":"cone"},"parent":1,"transform":{"position":[1,0,0]}})"));
    CHECK(run(stack, R"({"op":"setTransform","id":4,"rotationEuler":[0,0,30]})"));
    const Object& child = *s.find(4);
    const Transform w = worldTransform(s, child, 0);
    // The parent's X axis points along world -Z after its turn, and its scale doubles the offset.
    CHECK(near(w.position, {0, 2, -2}));
    CHECK(near(w.scale, {2, 2, 2}));
    CHECK(sameRotation(w.rotation, Quat::fromEulerXYZ({0, 90, 0}) * Quat::fromEulerXYZ({0, 0, 30})));
    // The matrix form agrees with the decomposed one.
    const Mat4 m = worldMatrix(s, child, 0), fromTransform = w.matrix();
    float worst = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(m.m[r][c] - fromTransform.m[r][c]));
    CHECK_MSG(worst < 1e-5f, "matrix differs by %g", worst);
    // localFromWorld undoes it.
    const Transform back = localFromWorld(s, 1, w, 0);
    CHECK(near(back.position, {1, 0, 0}) && near(back.scale, {1, 1, 1}) && sameRotation(back.rotation, child.transform.rotation));

    // An animated parent carries its child along.
    CHECK(run(stack, R"({"op":"addKeyframe","id":1,"property":"position","time":0,"value":[0,2,0]})"));
    CHECK(run(stack, R"({"op":"addKeyframe","id":1,"property":"position","time":1,"value":[5,2,0]})"));
    CHECK(near(worldTransform(s, *s.find(4), 1).position, {5, 2, -2}));
    CHECK(near(worldTransform(s, *s.find(4), 0.5f).position, {2.5f, 2, -2}));
}

void testLoopsAndChildren() {
    Scene s = Scene::makeDefault();
    CommandStack stack(s);
    std::string error;
    CHECK(run(stack, R"({"op":"setParent","id":2,"parent":1})"));
    CHECK(run(stack, R"({"op":"setParent","id":3,"parent":2})"));  // 1 > 2 > 3
    CHECK(isAncestor(s, 1, 3) && !isAncestor(s, 3, 1));
    CHECK(!run(stack, R"({"op":"setParent","id":1,"parent":3})", &error) && error.find("loop") != std::string::npos);
    CHECK(!run(stack, R"({"op":"setParent","id":1,"parent":1})"));
    CHECK(!run(stack, R"({"op":"setParent","id":1,"parent":99})"));
    CHECK(!run(stack, R"({"op":"addObject","type":"light","parent":99})"));
    CHECK(!run(stack, R"({"op":"removeObject","id":2})", &error) && error.find("children") != std::string::npos);
    CHECK(run(stack, R"({"op":"removeObject","id":3})"));  // a leaf goes
}

void testSetParentKeepsPlace() {
    Scene s = Scene::makeDefault();
    CommandStack stack(s);
    CHECK(run(stack, R"({"op":"setTransform","id":1,"position":[1,1,0],"rotationEuler":[0,45,0],"scale":[2,2,2]})"));
    CHECK(run(stack, R"({"op":"addObject","type":"mesh","primitive":{"type":"sphere"},"transform":{"position":[3,0,0]}})"));
    CHECK(run(stack, R"({"op":"setTransform","id":4,"rotationEuler":[10,0,0]})"));
    // Animate the sphere's position, so its keys have to be converted too.
    CHECK(run(stack, R"({"op":"addKeyframe","id":4,"property":"position","time":0,"value":[3,0,0]})"));
    CHECK(run(stack, R"({"op":"addKeyframe","id":4,"property":"position","time":2,"value":[3,4,0]})"));
    const std::string before = s.toJson().dump();
    const Transform at0 = worldTransform(s, *s.find(4), 0), at1 = worldTransform(s, *s.find(4), 1), at2 = worldTransform(s, *s.find(4), 2);

    CHECK(run(stack, R"({"op":"setParent","id":4,"parent":1})"));
    const Object& sphere = *s.find(4);
    CHECK(sphere.parent == 1);
    CHECK(!near(sphere.transform.position, {3, 0, 0}));  // its local values changed...
    CHECK(near(worldTransform(s, sphere, 0).position, at0.position));  // ...so it stayed put
    CHECK(sameRotation(worldTransform(s, sphere, 0).rotation, at0.rotation));
    CHECK(near(worldTransform(s, sphere, 2).position, at2.position));  // keys too
    CHECK(near(worldTransform(s, sphere, 1).position, at1.position));  // and between them (a static parent keeps lerps straight)

    CHECK(stack.undo() && s.toJson().dump() == before);  // exactly as it was
    CHECK(stack.redo() && s.find(4)->parent == 1);

    // Clearing the parent keeps it in place as well; keepTransform false doesn't.
    CHECK(run(stack, R"({"op":"setParent","id":4,"parent":0})"));
    CHECK(s.find(4)->parent == kNoId && near(worldTransform(s, *s.find(4), 0).position, at0.position));
    CHECK(run(stack, R"({"op":"setTransform","id":1,"position":[0,0,0]})"));
    CHECK(run(stack, R"({"op":"setParent","id":4,"parent":1,"keepTransform":false})"));
    const Transform moved = worldTransform(s, *s.find(4), 0);
    CHECK(!near(moved.position, at0.position));
}

void testSaveLoad() {
    Scene s = Scene::makeDefault();
    CommandStack stack(s);
    CHECK(run(stack, R"({"op":"setParent","id":2,"parent":1})"));
    Json j = s.toJson();
    Scene loaded;
    std::string error;
    CHECK_MSG(Scene::fromJson(j, loaded, error), "%s", error.c_str());
    CHECK(loaded.find(2)->parent == 1 && loaded.toJson().dump() == j.dump());

    // A parent that's missing, or a loop, is rejected.
    Json objects = *j.find("objects");
    Json bad = j;
    Json cube = objects[0];
    cube.set("parent", Json(2));  // 1 > 2 > 1
    Json loop = Json::array();
    loop.push(cube);
    for (size_t i = 1; i < objects.size(); ++i) loop.push(objects[i]);
    bad.set("objects", loop);
    CHECK(!Scene::fromJson(bad, loaded, error) && error.find("loop") != std::string::npos);
    cube.set("parent", Json(42));
    loop = Json::array();
    loop.push(cube);
    for (size_t i = 1; i < objects.size(); ++i) loop.push(objects[i]);
    bad.set("objects", loop);
    CHECK(!Scene::fromJson(bad, loaded, error));
}

}  // namespace

int main() {
    testWorldTransforms();
    testLoopsAndChildren();
    testSetParentKeepsPlace();
    testSaveLoad();
    return testResult("test_hierarchy");
}
