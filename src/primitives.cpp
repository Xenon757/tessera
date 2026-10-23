#include "primitives.h"

#include <algorithm>
#include <cmath>

namespace {

// Every face is listed counter-clockwise seen from outside, so face normals (and
// the renderer's back-face culling) point the right way.

EditMesh cube() {
    EditMesh m;
    for (Vec3 p : {Vec3{-1, -1, -1}, Vec3{1, -1, -1}, Vec3{1, 1, -1}, Vec3{-1, 1, -1}, Vec3{-1, -1, 1}, Vec3{1, -1, 1},
                   Vec3{1, 1, 1}, Vec3{-1, 1, 1}})
        m.addVert(p);
    const std::vector<Vec2> uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};  // each face gets the whole texture
    for (const std::vector<int>& q : {std::vector<int>{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {1, 2, 6, 5}, {0, 4, 7, 3}})
        m.addFace(q, false, &uv);
    m.hasUVs = true;
    return m;
}

EditMesh plane() {
    EditMesh m;
    for (Vec3 p : {Vec3{-1, 0, 1}, Vec3{1, 0, 1}, Vec3{1, 0, -1}, Vec3{-1, 0, -1}}) m.addVert(p);
    const std::vector<Vec2> uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    m.addFace({0, 1, 2, 3}, false, &uv);  // in the XZ plane, facing +Y
    m.hasUVs = true;
    return m;
}

// Point on a ring around Y: angle 0 faces +Z, increasing toward +X.
Vec3 ringPoint(float radius, float y, int j, int segments) {
    const float a = 2.0f * kPi * j / segments;
    return {radius * std::sin(a), y, radius * std::cos(a)};
}

EditMesh sphere(int segments, int rings) {
    EditMesh m;
    const int top = m.addVert({0, 1, 0});
    // Rings 1..rings-1 between the poles; row r has `segments` vertices.
    for (int r = 1; r < rings; ++r) {
        const float polar = kPi * r / rings;  // 0 at the top pole
        for (int j = 0; j < segments; ++j) m.addVert(ringPoint(std::sin(polar), std::cos(polar), j, segments));
    }
    const int bottom = m.addVert({0, -1, 0});
    auto at = [&](int r, int j) { return 1 + (r - 1) * segments + (j % segments); };
    // UVs wrap around once: u from 0 to 1 needs both ends at the seam, so UVs are
    // per corner (j and j + 1), never shared across the seam.
    auto uv = [&](int r, int j) { return Vec2{float(j) / segments, 1.0f - float(r) / rings}; };
    for (int j = 0; j < segments; ++j) {
        const std::vector<Vec2> capTop = {uv(0, j), uv(1, j), uv(1, j + 1)};
        m.addFace({top, at(1, j), at(1, j + 1)}, true, &capTop);
        for (int r = 1; r + 1 < rings; ++r) {
            const std::vector<Vec2> q = {uv(r, j), uv(r + 1, j), uv(r + 1, j + 1), uv(r, j + 1)};
            m.addFace({at(r, j), at(r + 1, j), at(r + 1, j + 1), at(r, j + 1)}, true, &q);
        }
        const std::vector<Vec2> capBottom = {uv(rings - 1, j), uv(rings, j), uv(rings - 1, j + 1)};
        m.addFace({at(rings - 1, j), bottom, at(rings - 1, j + 1)}, true, &capBottom);
    }
    m.hasUVs = true;
    return m;
}

// Cylinder (topRadius 1) or cone (topRadius 0, which makes the top a single apex).
EditMesh lathe(int segments, bool cone) {
    EditMesh m;
    std::vector<int> bottom(segments), topRing(segments);
    for (int j = 0; j < segments; ++j) bottom[j] = m.addVert(ringPoint(1, -1, j, segments));
    const int apex = cone ? m.addVert({0, 1, 0}) : EditMesh::kNone;
    if (!cone)
        for (int j = 0; j < segments; ++j) topRing[j] = m.addVert(ringPoint(1, 1, j, segments));
    auto sideUv = [&](int j, float v) { return Vec2{float(j) / segments, v}; };
    for (int j = 0; j < segments; ++j) {
        const int k = (j + 1) % segments;
        if (cone) {
            const std::vector<Vec2> t = {sideUv(j, 0), sideUv(j + 1, 0), Vec2{(j + 0.5f) / segments, 1}};
            m.addFace({bottom[j], bottom[k], apex}, true, &t);
        } else {
            const std::vector<Vec2> q = {sideUv(j, 0), sideUv(j + 1, 0), sideUv(j + 1, 1), sideUv(j, 1)};
            m.addFace({bottom[j], bottom[k], topRing[k], topRing[j]}, true, &q);
        }
    }
    // Caps: one n-gon each, flat, with planar UVs. Reversed order on the bottom so
    // it faces down.
    auto capUv = [&](int j, float flip) {
        const Vec3 p = ringPoint(1, 0, j, segments);
        return Vec2{0.5f + 0.5f * p.x * flip, 0.5f + 0.5f * p.z};
    };
    std::vector<int> cap;
    std::vector<Vec2> capUvs;
    for (int j = segments - 1; j >= 0; --j) cap.push_back(bottom[j]), capUvs.push_back(capUv(j, -1));
    m.addFace(cap, false, &capUvs);
    if (!cone) {
        cap.clear();
        capUvs.clear();
        for (int j = 0; j < segments; ++j) cap.push_back(topRing[j]), capUvs.push_back(capUv(j, 1));
        m.addFace(cap, false, &capUvs);
    }
    m.hasUVs = true;
    return m;
}

}  // namespace

EditMesh makePrimitive(Primitive p) {
    p.segments = std::clamp(p.segments, 3, 256);
    p.rings = std::clamp(p.rings, 2, 256);
    switch (p.type) {
        case PrimitiveType::Sphere: return sphere(p.segments, p.rings);
        case PrimitiveType::Cylinder: return lathe(p.segments, false);
        case PrimitiveType::Cone: return lathe(p.segments, true);
        case PrimitiveType::Plane: return plane();
        case PrimitiveType::Cube: break;
    }
    return cube();
}

const char* primitiveName(PrimitiveType type) {
    switch (type) {
        case PrimitiveType::Cube: return "cube";
        case PrimitiveType::Sphere: return "sphere";
        case PrimitiveType::Cylinder: return "cylinder";
        case PrimitiveType::Cone: return "cone";
        case PrimitiveType::Plane: return "plane";
    }
    return "cube";
}

bool primitiveFromName(const std::string& name, PrimitiveType& out) {
    for (PrimitiveType t : {PrimitiveType::Cube, PrimitiveType::Sphere, PrimitiveType::Cylinder, PrimitiveType::Cone,
                            PrimitiveType::Plane})
        if (name == primitiveName(t)) return out = t, true;
    return false;
}
