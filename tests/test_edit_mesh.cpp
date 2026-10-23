// test_edit_mesh.cpp — the BMesh-style editable mesh: adjacency invariants,
// non-manifold edges, triangulation, normals, and lossless import.
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>

#include "check.h"
#include "edit_mesh.h"

namespace {

// Unit cube from 6 quads, counter-clockwise from outside.
EditMesh makeQuadCube() {
    EditMesh m;
    const Vec3 p[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    for (const Vec3& v : p) m.addVert(v);
    const std::vector<std::vector<int>> quads = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {0, 4, 7, 3}};
    for (const auto& q : quads) CHECK(m.addFace(q) != EditMesh::kNone);
    return m;
}

int radialLength(const EditMesh& m, int edge) {
    const int first = m.edges()[edge].loop;
    if (first == EditMesh::kNone) return 0;
    int n = 0, l = first;
    do {
        ++n;
        l = m.loops()[l].radialNext;
    } while (l != first);
    return n;
}

void testCube() {
    const EditMesh m = makeQuadCube();
    std::string error;
    CHECK_MSG(m.validate(error), "%s", error.c_str());
    CHECK(m.verts().size() == 8 && m.edges().size() == 12 && m.faces().size() == 6);
    CHECK(int(m.verts().size()) - int(m.edges().size()) + int(m.faces().size()) == 2);  // Euler characteristic
    for (int e = 0; e < int(m.edges().size()); ++e) CHECK(radialLength(m, e) == 2);    // closed, manifold
    CHECK(m.findEdge(0, 1) != EditMesh::kNone && m.findEdge(0, 6) == EditMesh::kNone);

    const Mesh tris = m.triangulate();
    CHECK(tris.triangleCount() == 12);
    for (size_t t = 0; t < tris.vertices.size(); t += 3) {
        const Vertex* v = &tris.vertices[t];
        const Vec3 face = normalize(cross(v[1].position - v[0].position, v[2].position - v[0].position));
        const Vec3 center = (v[0].position + v[1].position + v[2].position) / 3.0f;
        CHECK(dot(face, center) > 0);                     // wound counter-clockwise from outside
        CHECK(length(v[0].normal - face) < 1e-6f);        // flat faces: one normal, the face's
    }
}

void testNonManifoldAndDegenerate() {
    EditMesh m;
    for (Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0}, Vec3{0, 0, 1}}) m.addVert(p);
    m.addFace({0, 1, 2});
    m.addFace({1, 0, 3});
    m.addFace({0, 1, 4});  // a third face on edge 0-1: impossible in a half-edge mesh
    std::string error;
    CHECK_MSG(m.validate(error), "%s", error.c_str());
    CHECK(radialLength(m, m.findEdge(0, 1)) == 3);
    CHECK(m.addFace({0, 1}) == EditMesh::kNone);     // too few corners
    CHECK(m.addFace({0, 1, 0}) == EditMesh::kNone);  // a vertex twice
}

void testSmoothMeetsFlat() {
    // Two faces at a right angle along x: one smooth, one flat. The smooth face's
    // normals average only over smooth faces (just itself), so the crease stays sharp.
    EditMesh m;
    for (Vec3 p : {Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{1, 1, 0}, Vec3{0, 1, 0}, Vec3{1, 0, -1}, Vec3{0, 0, -1}}) m.addVert(p);
    m.addFace({0, 1, 2, 3}, true);   // facing +z, smooth
    m.addFace({0, 5, 4, 1}, false);  // facing -y, flat
    const Mesh tris = m.triangulate();
    for (size_t i = 0; i < 6; ++i) CHECK(length(tris.vertices[i].normal - Vec3{0, 0, 1}) < 1e-6f);
    for (size_t i = 6; i < 12; ++i) CHECK(length(tris.vertices[i].normal - Vec3{0, -1, 0}) < 1e-6f);
}

void testImport() {
    Mesh bunny;
    std::string error;
    CHECK_MSG(loadModel(std::string(TESSERA_ASSET_DIR) + "/bunny.obj", bunny, error), "%s", error.c_str());
    const EditMesh m = EditMesh::fromTriangles(bunny);
    CHECK_MSG(m.validate(error), "%s", error.c_str());
    std::set<std::tuple<float, float, float>> unique;
    for (const Vertex& v : bunny.vertices) unique.insert({v.position.x, v.position.y, v.position.z});
    CHECK(m.verts().size() == unique.size());  // merged exactly the shared corners
    // Triangulating gives back the input byte for byte: imported models render
    // exactly as they did before they went through the edit mesh.
    const Mesh back = m.triangulate();
    CHECK(back.vertices.size() == bunny.vertices.size());
    CHECK(back.vertices.size() == bunny.vertices.size() &&
          std::memcmp(back.vertices.data(), bunny.vertices.data(), bunny.vertices.size() * sizeof(Vertex)) == 0);

    // The textured cube: UV seams split corners, but positions still merge to 8.
    Mesh cube;
    CHECK(loadModel(std::string(TESSERA_ASSET_DIR) + "/cube.obj", cube, error));
    const EditMesh mc = EditMesh::fromTriangles(cube);
    CHECK(mc.verts().size() == 8 && mc.hasUVs);
    CHECK(mc.validate(error));
}

}  // namespace

int main() {
    testCube();
    testNonManifoldAndDegenerate();
    testSmoothMeetsFlat();
    testImport();
    return testResult("test_edit_mesh");
}
