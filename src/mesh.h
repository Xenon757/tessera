// mesh.h — triangle mesh + a minimal Wavefront OBJ loader.
// Vertices are stored "unrolled": every 3 consecutive vertices form one triangle.
// That wastes a little memory but keeps the pipeline simple; an index buffer
// with a post-transform vertex cache is a good later optimization.
#pragma once

#include <string>
#include <vector>

#include "vecmath.h"

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

struct Mesh {
    std::vector<Vertex> vertices;  // size is a multiple of 3
    bool hasTexCoords = false;     // false: every uv is (0, 0), so don't texture it

    size_t triangleCount() const { return vertices.size() / 3; }

    // Bounding box helpers, used to center/scale arbitrary models into view.
    void bounds(Vec3& outMin, Vec3& outMax) const;
};

// Loads positions, UVs, and normals. Polygons are fan-triangulated; negative
// (relative) indices are supported. Vertices with no normal get a smooth one,
// averaged from the faces that share their position.
// Returns false and fills `error` on failure.
bool loadOBJ(const std::string& path, Mesh& out, std::string& error);

// Unit cube (-1..1) with per-face normals and UVs, wound counter-clockwise
// when viewed from outside. Useful when no model file is available.
Mesh makeCube();
