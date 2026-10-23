#include "picking.h"

#include <cfloat>
#include <cmath>

#include "animation.h"
#include "hierarchy.h"

namespace {

constexpr float kGizmoRadius = 0.3f;  // how close a click must come to a light or camera

// Möller–Trumbore: solve origin + t*dir = (1-u-v)*a + u*b + v*c for t, u, v with
// Cramer's rule. Hits both sides (no culling), so planes can be picked from below.
bool rayTriangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c, float& t) {
    const Vec3 e1 = b - a, e2 = c - a;
    const Vec3 p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-12f) return false;  // ray parallel to the triangle
    const float inv = 1.0f / det;
    const Vec3 s = o - a;
    const float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    const Vec3 q = cross(s, e1);
    const float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    t = dot(e2, q) * inv;
    return t > 0;
}

// Slab test: the ray is inside the box between its entry and exit on each axis;
// it hits if the latest entry comes before the earliest exit.
bool rayBox(const Vec3& o, const Vec3& d, const Vec3& lo, const Vec3& hi, float tMax) {
    float t0 = 0, t1 = tMax;
    const float os[3] = {o.x, o.y, o.z}, ds[3] = {d.x, d.y, d.z}, los[3] = {lo.x, lo.y, lo.z}, his[3] = {hi.x, hi.y, hi.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ds[i]) < 1e-20f) {
            if (os[i] < los[i] || os[i] > his[i]) return false;  // parallel and outside
            continue;
        }
        float near = (los[i] - os[i]) / ds[i], far = (his[i] - os[i]) / ds[i];
        if (near > far) std::swap(near, far);
        t0 = std::max(t0, near);
        t1 = std::min(t1, far);
        if (t0 > t1) return false;
    }
    return true;
}

}  // namespace

Ray rayThroughPixel(const Mat4& view, const Mat4& projection, float x, float y, int width, int height) {
    const float ndcX = (x + 0.5f) / width * 2.0f - 1.0f;
    const float ndcY = 1.0f - (y + 0.5f) / height * 2.0f;  // screen y points down
    Mat4 inv;
    if (!invert(projection * view, inv)) return {{0, 0, 0}, {0, 0, -1}};
    // The same pixel on the near plane (NDC z = -1) and the far plane (z = +1).
    auto unproject = [&](float z) {
        const Vec4 p = inv * Vec4(ndcX, ndcY, z, 1.0f);
        return Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
    };
    const Vec3 nearPoint = unproject(-1.0f), farPoint = unproject(1.0f);
    return {nearPoint, normalize(farPoint - nearPoint)};
}

ObjectId pickObject(const Scene& scene, const Ray& ray, float time, float* distance) {
    ObjectId best = kNoId;
    float bestT = FLT_MAX;
    for (const Object& base : scene.objects()) {
        if (!base.visible) continue;
        const Object o = animatedObject(base, time);
        const Mat4 model = worldMatrix(scene, base, time);  // through its parents
        if (o.type != ObjectType::Mesh) {
            // Ray vs sphere around the gizmo: closest approach of the ray to its center.
            const Vec3 toCenter = Vec3{model.m[0][3], model.m[1][3], model.m[2][3]} - ray.origin;
            const float along = dot(toCenter, ray.direction);
            const float miss2 = dot(toCenter, toCenter) - along * along;
            if (along > 0 && miss2 <= kGizmoRadius * kGizmoRadius && along < bestT) best = o.id, bestT = along;
            continue;
        }
        const MeshEntry* mesh = scene.mesh(o.mesh);
        Mat4 toLocal;
        if (!mesh || !invert(model, toLocal)) continue;  // zero scale: nothing to hit
        // Test in the object's own space: one matrix for the ray instead of one per
        // vertex. The local direction stays unnormalized, so a hit's t is the same
        // distance along the world ray, comparable across objects.
        const Vec3 lo = (toLocal * Vec4(ray.origin, 1.0f)).xyz(), ld = toLocal.transformDir(ray.direction);
        Vec3 bmin{FLT_MAX, FLT_MAX, FLT_MAX}, bmax{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (const auto& v : mesh->edit->verts()) {
            bmin = {std::min(bmin.x, v.position.x), std::min(bmin.y, v.position.y), std::min(bmin.z, v.position.z)};
            bmax = {std::max(bmax.x, v.position.x), std::max(bmax.y, v.position.y), std::max(bmax.z, v.position.z)};
        }
        if (!rayBox(lo, ld, bmin, bmax, bestT)) continue;  // quick reject before testing triangles
        const std::vector<Vertex>& tris = mesh->render->vertices;
        for (size_t i = 0; i + 2 < tris.size(); i += 3) {
            float t;
            if (rayTriangle(lo, ld, tris[i].position, tris[i + 1].position, tris[i + 2].position, t) && t < bestT)
                best = o.id, bestT = t;
        }
    }
    if (distance) *distance = bestT;
    return best;
}
