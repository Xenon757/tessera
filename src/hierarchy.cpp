#include "hierarchy.h"

#include "animation.h"

namespace {

constexpr int kMaxDepth = 1000;  // parents can't loop (validParent), but never trust that blindly

Vec3 multiply(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float safeDivide(float a, float b) { return b != 0 ? a / b : a; }

// `aim`: apply cameras' look-at targets. Finding a target's position passes false,
// so look-at never recurses into another look-at.
const Object* lookAtTarget(const Scene& scene, const Object& o, bool aim) {
    if (!aim || o.type != ObjectType::Camera || o.camera.lookAt == kNoId || o.camera.lookAt == o.id) return nullptr;
    return scene.find(o.camera.lookAt);
}

Transform worldTransformImpl(const Scene& scene, const Object& o, float time, bool aim, int depth);

Mat4 worldMatrixImpl(const Scene& scene, const Object& o, float time, bool aim, int depth) {
    if (lookAtTarget(scene, o, aim)) return worldTransformImpl(scene, o, time, aim, depth).matrix();
    const Mat4 local = animatedObject(o, time).transform.matrix();
    const Object* p = scene.find(o.parent);
    if (!p || depth >= kMaxDepth) return local;
    return worldMatrixImpl(scene, *p, time, aim, depth + 1) * local;  // the parent applies after the child
}

Transform worldTransformImpl(const Scene& scene, const Object& o, float time, bool aim, int depth) {
    const Transform local = animatedObject(o, time).transform;
    const Object* p = depth < kMaxDepth ? scene.find(o.parent) : nullptr;
    Transform w = local;
    if (p) {
        // Position goes through the parent's full matrix (exact); rotations multiply
        // (parent first, then child); scales multiply per axis.
        const Transform parent = worldTransformImpl(scene, *p, time, aim, depth + 1);
        w.position = (worldMatrixImpl(scene, *p, time, aim, depth + 1) * Vec4(local.position, 1.0f)).xyz();
        w.rotation = normalize(parent.rotation * local.rotation);
        w.scale = multiply(parent.scale, local.scale);
    }
    if (const Object* target = lookAtTarget(scene, o, aim)) {
        const Vec3 toTarget = worldTransformImpl(scene, *target, time, false, 0).position - w.position;
        if (length(toTarget) > 1e-6f) w.rotation = Quat::lookRotation(toTarget);  // standing on it: keep the rotation
    }
    return w;
}

}  // namespace

Mat4 worldMatrix(const Scene& scene, const Object& o, float time) { return worldMatrixImpl(scene, o, time, true, 0); }

Transform worldTransform(const Scene& scene, const Object& o, float time) {
    return worldTransformImpl(scene, o, time, true, 0);
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

bool validLookAt(const Scene& scene, ObjectId camera, ObjectId target, std::string& error) {
    if (target == kNoId) return true;
    if (!scene.find(target)) return error = "lookAt: no such object", false;
    if (target == camera) return error = "a camera can't look at itself", false;
    return true;
}

bool validParent(const Scene& scene, ObjectId child, ObjectId parent, std::string& error) {
    if (parent == kNoId) return true;
    if (!scene.find(parent)) return error = "parent: no such object", false;
    if (parent == child) return error = "an object can't be its own parent", false;
    if (isAncestor(scene, child, parent)) return error = "that would make a loop: the parent is a child of this object", false;
    return true;
}
