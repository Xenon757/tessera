// mesh_ops.h — the modeling operations of edit mode (Phase 5). Each one is a pure
// function: a mesh and a selection in, a new mesh and the new selection out. Meshes
// are never changed in place (the scene stores them copy-on-write), so an operation
// is undone by pointing the object back at its old mesh.
//
// The operations work on a plain polygon list (PolyMesh: positions, plus faces as
// vertex index lists with per-corner UVs) that's easy to rewrite, and use the
// EditMesh's adjacency where they need to walk the surface (around a vertex for
// bevel, along a ring of quads for loop cut). The result is rebuilt into an EditMesh,
// with unused vertices removed, and validated.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "edit_mesh.h"

// A selection of mesh elements, by index into the mesh it belongs to. Edges are
// vertex pairs (smaller index first): they identify an edge independently of how
// the EditMesh happened to number its edges.
struct MeshSelection {
    std::vector<int> verts;
    std::vector<std::pair<int, int>> edges;
    std::vector<int> faces;
    bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }
};

struct MeshOpParams {
    float amount = 0.1f;  // inset and bevel: distance, in the mesh's own units
    int cuts = 1;         // loop cut: number of cuts; subdivide: levels
    std::string what;     // delete: "verts", "edges", or "faces"
    std::vector<Vec3> positions;  // move: one new position per selected vertex, in selection order
};

// Runs `operation` on `in`:
//   extrude    selected faces as one region (or, with no faces, open edges into new
//              faces); the new geometry starts in place, ready to be moved
//   inset      each selected face shrinks inward by `amount`, ringed by new quads
//   bevel      selected edges become flat strips `amount` wide; where several meet,
//              the corner gets its own face (edges must be inside a closed surface)
//   loopcut    `cuts` evenly spaced cuts around the ring of quads through the
//              first selected edge
//   subdivide  each selected face split `cuts` times (quads into 4, triangles into 4)
//   merge      the selected vertices become one, at their center
//   delete     the selected verts, edges, or faces (`what`), and faces using them
//   move       the selected vertices to `positions`
// On success fills `out` and `selection` (the elements the user would want selected
// next: the extruded faces, the inner faces, the new loop...). False + `error` if the
// selection doesn't fit the operation; `in` is never changed.
bool runMeshOp(const EditMesh& in, const std::string& operation, const MeshSelection& sel,
               const MeshOpParams& params, EditMesh& out, MeshSelection& selection, std::string& error);

// The faces whose vertices are all selected, and the edges with both ends selected:
// how a vertex selection extends to edges and faces (as in Blender).
MeshSelection completeSelection(const EditMesh& mesh, const std::vector<int>& verts);
