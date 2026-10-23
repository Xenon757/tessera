// edit_mesh.h — editable polygon mesh with full adjacency, BMesh-style (as in Blender).
//
// The renderer draws a flat triangle list (Mesh). Editing needs more: which edges
// meet at a vertex, which faces share an edge, faces with any number of corners.
// So every element knows its neighbors through linked cycles:
//
//   Vert  one of its edges. All edges around a vertex form its "disk" cycle.
//   Edge  its two vertices, the disk-cycle links at each end, and one loop.
//   Loop  one corner of one face: its vertex, the edge to the next corner, its face,
//         the next/previous corner around the face, and the next/previous loop on
//         the same edge (the "radial" cycle: one loop per face using that edge).
//   Face  its first loop, its corner count, and whether it's shaded smooth.
//
// Unlike a half-edge mesh, an edge may have any number of faces (its radial cycle
// is just longer), so imported non-manifold models load fine. Everything lives in
// arrays and refers to other elements by index, which makes copies cheap and
// deterministic. Per-corner data (UVs, imported normals) lives on loops, because a
// vertex can have different values on different faces (a cube's sharp corners).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mesh.h"
#include "vecmath.h"

class EditMesh {
public:
    static constexpr int kNone = -1;

    struct Vert {
        Vec3 position;
        int edge = kNone;  // any edge in this vertex's disk cycle
    };
    struct DiskLink {
        int next = kNone, prev = kNone;  // neighbors in one endpoint's disk cycle
    };
    struct Edge {
        int v[2] = {kNone, kNone};
        DiskLink disk[2];  // disk[i] links this edge into v[i]'s disk cycle
        int loop = kNone;  // any loop in this edge's radial cycle
    };
    struct Loop {
        int vert = kNone, edge = kNone, face = kNone;
        int next = kNone, prev = kNone;              // around the face
        int radialNext = kNone, radialPrev = kNone;  // around the edge
        Vec2 uv;
        Vec3 normal;  // imported per-corner normal (only used if the mesh has custom normals)
    };
    struct Face {
        int loop = kNone;  // first corner
        int count = 0;
        bool smooth = false;  // smooth: share normals with smooth neighbors; flat: one normal
    };

    // Adds a vertex; returns its index.
    int addVert(const Vec3& position);

    // Adds a face through `verts` (in counter-clockwise order seen from outside).
    // Edges are created or reused as needed. `uvs` and `normals` (optional) give
    // per-corner values. Returns the face index, or kNone for a degenerate face
    // (fewer than 3 corners, or a vertex used twice).
    int addFace(const std::vector<int>& verts, bool smooth = false, const std::vector<Vec2>* uvs = nullptr,
                const std::vector<Vec3>* normals = nullptr);

    // The edge between two vertices, or kNone.
    int findEdge(int a, int b) const;

    const std::vector<Vert>& verts() const { return verts_; }
    const std::vector<Edge>& edges() const { return edges_; }
    const std::vector<Loop>& loops() const { return loops_; }
    const std::vector<Face>& faces() const { return faces_; }

    bool hasUVs = false;            // loops carry meaningful UVs
    bool hasCustomNormals = false;  // loops carry imported normals; else they're computed

    // Checks every adjacency invariant (cycles closed and consistent, indices in
    // range). For tests and for anything that builds meshes; false + `error` if broken.
    bool validate(std::string& error) const;

    // The renderer's triangle list: each face fan-triangulated, with normals either
    // imported or computed (face normal for flat faces; for smooth faces, the
    // area-weighted average over the smooth faces around each vertex).
    Mesh triangulate() const;

    // Face normal by Newell's method (robust for non-planar polygons). Its length is
    // twice the polygon's area, so summing unnormalized ones area-weights them.
    Vec3 faceNormalUnnormalized(int face) const;

    // Builds an EditMesh from a triangle list, merging vertices whose positions
    // are exactly equal (loaders output triangles with duplicated vertices). UVs
    // and normals stay per corner. Triangles that collapse after merging are dropped.
    static EditMesh fromTriangles(const Mesh& mesh);

private:
    int addEdge(int a, int b);
    DiskLink& diskOf(int edge, int vert);
    const DiskLink& diskOf(int edge, int vert) const;

    std::vector<Vert> verts_;
    std::vector<Edge> edges_;
    std::vector<Loop> loops_;
    std::vector<Face> faces_;
};
