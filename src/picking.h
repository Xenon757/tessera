// picking.h — click-to-select: which object is under a pixel? Runs on the CPU, only
// when the user clicks, so it never touches the per-frame rendering path.
#pragma once

#include "scene.h"

struct Ray {
    Vec3 origin;
    Vec3 direction;  // unit length
};

// The ray from the camera through the center of pixel (x, y) in a width x height
// viewport (y down), by un-projecting through the inverse of projection * view.
// Pixel centers are where the rasterizer samples, so picking and drawing agree.
Ray rayThroughPixel(const Mat4& view, const Mat4& projection, float x, float y, int width, int height);

// The nearest visible object the ray hits, or kNoId. Meshes are hit-tested exactly
// (both sides of every triangle); lights and cameras by a small sphere around
// their gizmo. `distance` (optional) receives the hit distance along the ray.
ObjectId pickObject(const Scene& scene, const Ray& ray, float* distance = nullptr);
