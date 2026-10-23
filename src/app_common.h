// app_common.h — bits shared by the windowed and headless front ends.
#pragma once

#include <cstdio>
#include <string>

#include "vecmath.h"
#include "mesh.h"
#include "texture.h"

#ifndef TESSERA_ASSET_DIR
#define TESSERA_ASSET_DIR "assets"
#endif

// Loads the model at `path` (or the built-in cube if path is empty / fails).
inline Mesh loadModelOrCube(const std::string& path) {
    if (!path.empty()) {
        Mesh mesh;
        std::string error;
        if (loadModel(path, mesh, error)) {
            std::printf("Loaded %s (%zu triangles)\n", path.c_str(), mesh.triangleCount());
            return mesh;
        }
        std::fprintf(stderr, "Model load failed: %s — using built-in cube\n", error.c_str());
    }
    return makeCube();
}

// Texture used for any model that has UVs.
inline Texture defaultTexture() {
    return Texture::checker(256, 8, {0.95f, 0.95f, 0.95f}, {0.2f, 0.45f, 0.9f});
}

// Model matrix that centers the mesh at the origin and scales its largest
// dimension to 2 units, so any OBJ shows up framed by the default camera.
inline Mat4 fitToView(const Mesh& mesh) {
    Vec3 lo, hi;
    mesh.bounds(lo, hi);
    Vec3 size = hi - lo;
    float extent = std::max(size.x, std::max(size.y, size.z));
    float s = extent > 0.0f ? 2.0f / extent : 1.0f;
    Vec3 center = (lo + hi) * 0.5f;
    return Mat4::scale({s, s, s}) * Mat4::translate(-center);
}
