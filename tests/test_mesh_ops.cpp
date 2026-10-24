// test_mesh_ops.cpp — the modeling operations. Every result must be a valid mesh
// that is still a closed surface wound consistently outward (each directed edge used
// once, its reverse by exactly one other face, positive enclosed volume), with the
// exact element counts each operation should produce, and the right geometry.
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>
#include <string>

#include "check.h"
#include "mesh_ops.h"
#include "primitives.h"

namespace {

int edgeCount(const EditMesh& m) { return static_cast<int>(m.edges().size()); }

// A closed surface, wound consistently: every directed edge (a -> b) appears in
// exactly one face, and so does its reverse. Volume by the divergence theorem (sum of
// signed tetrahedra from the origin) is positive when the faces point outward.
std::string diagnose(const EditMesh& m) {
    std::map<std::pair<int, int>, int> directed;
    double volume = 0;
    for (const auto& f : m.faces()) {
        std::vector<int> v;
        for (int l = f.loop, k = 0; k < f.count; ++k, l = m.loops()[l].next) v.push_back(m.loops()[l].vert);
        for (size_t i = 0; i < v.size(); ++i) ++directed[{v[i], v[(i + 1) % v.size()]}];
        const Vec3 p0 = m.verts()[v[0]].position;
        for (size_t i = 1; i + 1 < v.size(); ++i)
            volume += dot(p0, cross(m.verts()[v[i]].position, m.verts()[v[i + 1]].position)) / 6.0;
    }
    int doubled = 0, unmatched = 0;
    for (const auto& [e, n] : directed) {
        doubled += n != 1;
        unmatched += !directed.count({e.second, e.first});
    }
    return "doubled " + std::to_string(doubled) + ", unmatched " + std::to_string(unmatched) + ", volume " + std::to_string(volume);
}

bool closedAndOutward(const EditMesh& m, float* volumeOut = nullptr) {
    std::map<std::pair<int, int>, int> directed;
    double volume = 0;
    for (const auto& f : m.faces()) {
        std::vector<int> v;
        for (int l = f.loop, k = 0; k < f.count; ++k, l = m.loops()[l].next) v.push_back(m.loops()[l].vert);
        for (size_t i = 0; i < v.size(); ++i) ++directed[{v[i], v[(i + 1) % v.size()]}];
        const Vec3 p0 = m.verts()[v[0]].position;
        for (size_t i = 1; i + 1 < v.size(); ++i)
            volume += dot(p0, cross(m.verts()[v[i]].position, m.verts()[v[i + 1]].position)) / 6.0;
    }
    for (const auto& [e, n] : directed) {
        auto reverse = directed.find({e.second, e.first});
        if (n != 1 || reverse == directed.end() || reverse->second != 1) return false;
    }
    if (volumeOut) *volumeOut = static_cast<float>(volume);
    return volume > 0;
}

EditMesh cube() { return makePrimitive({PrimitiveType::Cube, 32, 16}); }

// Runs an operation that must succeed, checking the result's basic soundness.
EditMesh run(const EditMesh& in, const std::string& op, const MeshSelection& sel, MeshOpParams params,
             MeshSelection* selection = nullptr, bool closed = true) {
    EditMesh out;
    MeshSelection s;
    std::string error;
    const bool ok = runMeshOp(in, op, sel, params, out, s, error);
    CHECK_MSG(ok, "%s: %s", op.c_str(), error.c_str());
    std::string invalid;
    CHECK_MSG(!ok || out.validate(invalid), "%s: %s", op.c_str(), invalid.c_str());
    if (closed) CHECK_MSG(!ok || closedAndOutward(out), "%s: not a closed outward surface", op.c_str());
    if (selection) *selection = s;
    return out;
}

// The face of the cube whose center is at +Y (its top).
int topFace(const EditMesh& m) {
    for (size_t f = 0; f < m.faces().size(); ++f) {
        float y = 0;
        for (int l = m.faces()[f].loop, k = 0; k < m.faces()[f].count; ++k, l = m.loops()[l].next)
            y += m.verts()[m.loops()[l].vert].position.y;
        if (y / m.faces()[f].count > 0.99f) return static_cast<int>(f);
    }
    return -1;
}

std::pair<int, int> anyEdge(const EditMesh& m, int i) {
    const auto& e = m.edges()[i];
    return {std::min(e.v[0], e.v[1]), std::max(e.v[0], e.v[1])};
}

void testExtrudeAndInset() {
    const EditMesh c = cube();
    CHECK(closedAndOutward(c));
    MeshSelection top;
    top.faces = {topFace(c)};
    MeshSelection result;
    EditMesh e = run(c, "extrude", top, {}, &result);
    // One face up: 4 new vertices, 4 wall quads.
    CHECK(e.verts().size() == 12 && e.faces().size() == 10 && edgeCount(e) == 20);
    CHECK(result.faces.size() == 1 && result.verts.size() == 4);
    // Moving the extruded verts up keeps it closed (a taller box) and grows the volume.
    MeshOpParams lift;
    for (int v : result.verts) lift.positions.push_back(e.verts()[v].position + Vec3{0, 1, 0});
    MeshSelection moved;
    moved.verts = result.verts;
    float before = 0, after = 0;
    closedAndOutward(c, &before);
    closedAndOutward(run(e, "move", moved, lift), &after);
    CHECK(std::fabs(before - 8) < 1e-4f && std::fabs(after - 12) < 1e-4f);  // 2x2x2 -> 2x3x2

    // Two neighboring faces (sharing an edge) extruded together are one region: 6 walls, not 8.
    MeshSelection two;
    two.faces = {0, 2};
    EditMesh r = run(c, "extrude", two, {});
    CHECK(r.faces().size() == 6 + 6);

    // Inset: the top face shrinks toward its center; 4 ring quads.
    MeshOpParams by;
    by.amount = 0.25f;
    EditMesh in = run(c, "inset", top, by, &result);
    CHECK(in.faces().size() == 10 && in.verts().size() == 12);
    for (int v : result.verts) {
        const Vec3 p = in.verts()[v].position;
        CHECK(std::fabs(p.y - 1) < 1e-6f && std::fabs(std::hypot(p.x, p.z) - (std::sqrt(2.0f) - 0.25f)) < 1e-4f);
    }
}

void testBevel() {
    const EditMesh c = cube();
    MeshSelection one;
    one.edges = {anyEdge(c, 0)};
    MeshSelection result;
    MeshOpParams by;
    by.amount = 0.2f;
    // One edge: its 2 vertices split in 2 each, 1 strip face. V-E+F stays 2.
    EditMesh b = run(c, "bevel", one, by, &result);
    CHECK(b.verts().size() == 10 && b.faces().size() == 7 && edgeCount(b) == 15);
    CHECK(result.faces.size() == 1);
    float volume = 0;
    closedAndOutward(b, &volume);  // a 0.2 x 0.2 triangular prism (2 long) is shaved off
    CHECK_MSG(std::fabs(volume - (8 - 0.5f * 0.2f * 0.2f * 2)) < 1e-4f, "volume %f", volume);

    // Every edge: the classic chamfered cube, 24 vertices, 6 + 12 + 8 faces.
    MeshSelection all;
    for (int i = 0; i < edgeCount(c); ++i) all.edges.push_back(anyEdge(c, i));
    EditMesh chamfer = run(c, "bevel", all, by, &result);
    CHECK(chamfer.verts().size() == 24 && chamfer.faces().size() == 26 && edgeCount(chamfer) == 48);
    CHECK(result.faces.size() == 20);
    // Two edges meeting at a corner (and a third, separate one).
    MeshSelection some;
    const EditMesh::Face& f0 = c.faces()[0];
    const int l0 = f0.loop, l1 = c.loops()[l0].next, l2 = c.loops()[l1].next;
    some.edges = {{std::min(c.loops()[l0].vert, c.loops()[l1].vert), std::max(c.loops()[l0].vert, c.loops()[l1].vert)},
                  {std::min(c.loops()[l1].vert, c.loops()[l2].vert), std::max(c.loops()[l1].vert, c.loops()[l2].vert)}};
    run(c, "bevel", some, by);
    // Two beveled edges that aren't neighbors around a vertex (a cone's tip): the tip
    // stays between them on both sides, and each side gets its own corner face.
    EditMesh cone = makePrimitive({PrimitiveType::Cone, 8, 2});
    int tip = 0;
    for (size_t v = 0; v < cone.verts().size(); ++v)
        if (cone.verts()[v].position.y > cone.verts()[tip].position.y) tip = static_cast<int>(v);
    std::vector<int> around;  // the base vertices joined to the tip
    for (const auto& e : cone.edges())
        if (e.v[0] == tip || e.v[1] == tip) around.push_back(e.v[0] == tip ? e.v[1] : e.v[0]);
    std::sort(around.begin(), around.end());
    CHECK(around.size() == 8);
    MeshSelection apart;  // edges to base vertices 0 and 4 of 8: opposite sides of the tip
    for (int k : {0, 4}) apart.edges.push_back({std::min(tip, around[k]), std::max(tip, around[k])});
    run(cone, "bevel", apart, by, &result);
    CHECK(result.faces.size() == 2 + 2);  // two strips, and a corner face on each side
    // An open edge (a plane's border) can't be beveled.
    EditMesh plane = makePrimitive({PrimitiveType::Plane, 32, 16}), out;
    MeshSelection border;
    border.edges = {anyEdge(plane, 0)};
    std::string error;
    CHECK(!runMeshOp(plane, "bevel", border, by, out, result, error) && error.find("closed") != std::string::npos);
}

void testLoopCutAndSubdivide() {
    const EditMesh c = cube();
    MeshSelection edge;
    edge.edges = {anyEdge(c, 0)};
    MeshSelection result;
    MeshOpParams one;
    one.cuts = 1;
    // A cube's edge ring goes all the way round: 4 quads split, 4 new vertices.
    EditMesh cut = run(c, "loopcut", edge, one, &result);
    CHECK(cut.verts().size() == 12 && cut.faces().size() == 10);
    CHECK(result.edges.size() == 4);
    for (int v : result.verts) {  // each at the middle of its edge: a coordinate is 0
        const Vec3 p = cut.verts()[v].position;
        CHECK(std::fabs(p.x) < 1e-6f || std::fabs(p.y) < 1e-6f || std::fabs(p.z) < 1e-6f);
    }
    MeshOpParams three;
    three.cuts = 3;
    EditMesh cut3 = run(c, "loopcut", edge, three, &result);
    CHECK(cut3.verts().size() == 8 + 12 && cut3.faces().size() == 6 + 4 * 3 && result.edges.size() == 12);

    // Subdividing every face of the cube: 6 * 4 faces, 8 + 12 + 6 vertices.
    MeshSelection faces;
    for (int f = 0; f < 6; ++f) faces.faces.push_back(f);
    EditMesh s = run(c, "subdivide", faces, one, &result);
    CHECK(s.faces().size() == 24 && s.verts().size() == 26 && result.faces.size() == 24);
    // Just the top face: its neighbors gain midpoints, so no cracks.
    MeshSelection top;
    top.faces = {topFace(c)};
    EditMesh partial = run(c, "subdivide", top, one);
    CHECK(partial.faces().size() == 9 && partial.verts().size() == 13);
    // A triangle splits into four.
    EditMesh cone = makePrimitive({PrimitiveType::Cone, 8, 2});
    MeshSelection tri;
    for (size_t f = 0; f < cone.faces().size(); ++f)
        if (cone.faces()[f].count == 3) tri.faces = {static_cast<int>(f)};
    run(cone, "subdivide", tri, one);
}

void testMergeDeleteAndErrors() {
    const EditMesh c = cube();
    // Merging the top four vertices: the top face disappears, the sides become
    // triangles meeting at a point: a pyramid (5 vertices, 5 faces).
    const int top = topFace(c);
    MeshSelection four;
    for (int l = c.faces()[top].loop, k = 0; k < 4; ++k, l = c.loops()[l].next) four.verts.push_back(c.loops()[l].vert);
    MeshSelection result;
    EditMesh pyramid = run(c, "merge", four, {}, &result);
    CHECK(pyramid.verts().size() == 5 && pyramid.faces().size() == 5);
    CHECK(result.verts.size() == 1 && std::fabs(pyramid.verts()[result.verts[0]].position.y - 1) < 1e-6f);

    // Deleting a face leaves an open box; deleting a vertex takes its 3 faces.
    MeshOpParams faces, verts;
    faces.what = "faces";
    verts.what = "verts";
    MeshSelection f;
    f.faces = {top};
    EditMesh open = run(c, "delete", f, faces, nullptr, false);
    CHECK(open.faces().size() == 5 && open.verts().size() == 8 && !closedAndOutward(open));
    MeshSelection v;
    v.verts = {0};
    EditMesh corner = run(c, "delete", v, verts, nullptr, false);
    CHECK(corner.faces().size() == 3 && corner.verts().size() == 7);
    // ...and the open edges of the open box extrude into a rim.
    MeshSelection rim;
    for (int i = 0; i < edgeCount(open); ++i) {
        const auto e = anyEdge(open, i);
        int faceCount = 0;
        for (int l = open.edges()[i].loop, k = 0; l != EditMesh::kNone && k < 10; ++k) {
            ++faceCount;
            l = open.loops()[l].radialNext;
            if (l == open.edges()[i].loop) break;
        }
        if (faceCount == 1) rim.edges.push_back(e);
    }
    CHECK(rim.edges.size() == 4);
    EditMesh lip = run(open, "extrude", rim, {}, &result, false);
    CHECK(lip.faces().size() == 9 && result.edges.size() == 4);

    // Mistakes are refused, and the input is never changed.
    EditMesh out;
    std::string error;
    MeshSelection none, bad;
    bad.verts = {99};
    CHECK(!runMeshOp(c, "extrude", none, {}, out, result, error));
    CHECK(!runMeshOp(c, "merge", bad, {}, out, result, error) && error.find("99") != std::string::npos);
    CHECK(!runMeshOp(c, "teleport", four, {}, out, result, error));
    MeshSelection everything;
    for (int i = 0; i < 6; ++i) everything.faces.push_back(i);
    CHECK(!runMeshOp(c, "delete", everything, faces, out, result, error));  // would be empty
    CHECK(c.verts().size() == 8 && c.faces().size() == 6);

    // UVs survive: a UV-mapped sphere stays UV-mapped through a bevel.
    EditMesh sphere = makePrimitive({PrimitiveType::Sphere, 12, 6});
    MeshSelection ring;
    ring.edges = {anyEdge(sphere, 30)};
    MeshOpParams by;
    by.amount = 0.05f;
    CHECK(run(sphere, "bevel", ring, by).hasUVs);
}

// Random selections on every primitive (and on meshes already edited), every
// operation: results are always valid; operations that keep a surface closed keep
// it closed and outward; refusals leave no partial result. Nothing may crash (this
// also runs under the sanitizers).
void testRandom() {
    std::mt19937 rng(11);
    std::vector<EditMesh> meshes;
    for (PrimitiveType t : {PrimitiveType::Cube, PrimitiveType::Sphere, PrimitiveType::Cylinder, PrimitiveType::Cone})
        meshes.push_back(makePrimitive({t, 6 + int(rng() % 8), 3 + int(rng() % 4)}));
    const char* ops[] = {"extrude", "inset", "bevel", "loopcut", "subdivide", "merge", "delete", "move"};
    const std::set<std::string> keepsClosed = {"extrude", "inset", "bevel", "loopcut", "subdivide", "move"};
    int ran = 0;
    for (int round = 0; round < 600; ++round) {
        const EditMesh& m = meshes[rng() % meshes.size()];
        MeshSelection sel;
        const int nv = int(m.verts().size()), nf = int(m.faces().size()), ne = int(m.edges().size());
        for (int k = 0, n = 1 + rng() % 3; k < n; ++k) {
            sel.verts.push_back(rng() % nv);
            sel.faces.push_back(rng() % nf);
            sel.edges.push_back(anyEdge(m, rng() % ne));
        }
        std::sort(sel.verts.begin(), sel.verts.end());
        sel.verts.erase(std::unique(sel.verts.begin(), sel.verts.end()), sel.verts.end());
        std::sort(sel.faces.begin(), sel.faces.end());
        sel.faces.erase(std::unique(sel.faces.begin(), sel.faces.end()), sel.faces.end());
        std::sort(sel.edges.begin(), sel.edges.end());
        sel.edges.erase(std::unique(sel.edges.begin(), sel.edges.end()), sel.edges.end());
        const std::string op = ops[rng() % 8];
        MeshOpParams params;
        params.amount = 0.02f + (rng() % 100) / 200.0f;
        params.cuts = 1 + rng() % 3;
        params.what = std::vector<std::string>{"verts", "edges", "faces"}[rng() % 3];
        for (int v : sel.verts) params.positions.push_back(m.verts()[v].position + Vec3{0.1f, 0.2f, 0});
        EditMesh out;
        MeshSelection result;
        std::string error;
        const bool closedBefore = closedAndOutward(m);
        if (!runMeshOp(m, op, sel, params, out, result, error)) continue;
        ++ran;
        std::string invalid;
        CHECK_MSG(out.validate(invalid), "round %d %s: %s", round, op.c_str(), invalid.c_str());
        if (closedBefore && keepsClosed.count(op))
            CHECK_MSG(closedAndOutward(out), "round %d %s: no longer closed (%s; before: %s; sel %zu verts %zu edges %zu faces)",
                      round, op.c_str(), diagnose(out).c_str(), diagnose(m).c_str(), sel.verts.size(), sel.edges.size(), sel.faces.size());
        for (int v : result.verts) CHECK(v >= 0 && v < int(out.verts().size()));
        for (int f : result.faces) CHECK(f >= 0 && f < int(out.faces().size()));
        if (meshes.size() < 40 && out.faces().size() < 2000) meshes.push_back(out);  // edit the edits too
    }
    CHECK_MSG(ran > 300, "only %d operations ran", ran);
}

}  // namespace

int main() {
    testExtrudeAndInset();
    testBevel();
    testLoopCutAndSubdivide();
    testMergeDeleteAndErrors();
    testRandom();
    return testResult("test_mesh_ops");
}
