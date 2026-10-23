#include "hierarchy.h"

#include "animation.h"

namespace {

constexpr int kMaxDepth = 1000;  // parents can't loop (validParent), but never trust that blindly

Vec3 multiply(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float safeDivide(float a, float b) { return b != 0 ? a / b : a; }

}  // namespace

Mat4 worldMatrix(const Scene& scene, const Object& o, float time) {
    Mat4 m = animatedObject(o, time).transform.matrix();
    const Object* p = scene.find(o.parent);
    for (int depth = 0; p && depth < kMaxDepth; ++depth, p = scene.find(p->parent))
        m = animatedObject(*p, time).transform.matrix() * m;  // each parent applies after its child
    return m;
}

Transform worldTransform(const Scene& scene, const Object& o, float time) {
    const Transform local = animatedObject(o, time).transform;
    const Object* p = scene.find(o.parent);
    if (!p) return local;
    // Position goes through the parent's full matrix (exact); rotations multiply
    // (parent first, then child); scales multiply per axis.
    const Transform parent = worldTransform(scene, *p, time);
    Transform w;
    w.position = (worldMatrix(scene, *p, time) * Vec4(local.position, 1.0f)).xyz();
    w.rotation = normalize(parent.rotation * local.rotation);
    w.scale = multiply(parent.scale, local.scale);
    return w;
}

Transform localFromWorld(const Scene& scene, ObjectId parentId, const Transform& world, float time) {
    const Object* p = scene.find(parentId);
    if (!p) return world;
    // Undo each step of worldTransform: the inverse matrix for position, the
    // inverse (conjugate) rotation on the left, and divided scales.
    const Transform parent = worldTransform(scene, *p, time);
    Mat4 inv;
    Transform local;
    local.position = invert(worldMatrix(scene, *p, time), inv) ? (inv * Vec4(world.position, 1.0f)).xyz() : world.position;
    const Quat& q = parent.rotation;
    local.rotation = normalize(Quat{-q.x, -q.y, -q.z, q.w} * world.rotation);
    local.scale = {safeDivide(world.scale.x, parent.scale.x), safeDivide(world.scale.y, parent.scale.y),
                   safeDivide(world.scale.z, parent.scale.z)};
    return local;
}

bool isAncestor(const Scene& scene, ObjectId ancestor, ObjectId id) {
    const Object* o = scene.find(id);
    for (int depth = 0; o && o->parent != kNoId && depth < kMaxDepth; ++depth) {
        if (o->parent == ancestor) return true;
        o = scene.find(o->parent);
    }
    return false;
}

bool validParent(const Scene& scene, ObjectId child, ObjectId parent, std::string& error) {
    if (parent == kNoId) return true;
    if (!scene.find(parent)) return error = "parent: no such object", false;
    if (parent == child) return error = "an object can't be its own parent", false;
    if (isAncestor(scene, child, parent)) return error = "that would make a loop: the parent is a child of this object", false;
    return true;
}
