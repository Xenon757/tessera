// shader.h — programmable vertex and pixel stages, like GPU shaders.
//
// Passing a Shader to drawMesh replaces the fixed-function pipeline for that draw:
//   vertex()  turns each mesh vertex into a clip-space position plus the
//             attributes to interpolate across the triangle.
//   pixel()   turns the interpolated attributes into a color, or discards the pixel.
// Clipping, culling, rasterization, and the depth test stay fixed, as on a GPU.
//
// Shaders run on several threads at once, so vertex() and pixel() are const and
// must not modify shared state. Values a shader reads ("uniforms", like a fill
// level or the time) are set on the shader object before calling drawMesh.
#pragma once

#include "mesh.h"
#include "vecmath.h"

// A vertex in clip space, plus every attribute interpolated across the triangle.
// This is the vertex stage's output. Clipping creates new vertices partway along
// edges, so it interpolates all of these too.
struct ClipVertex {
    Vec4 pos;  // clip space, before the perspective divide
    Vec3 worldPos;
    Vec3 normal;
    Vec2 uv;
    Vec4 custom;  // free for shaders: 4 extra floats, interpolated like the rest
};

// The matrices a vertex stage needs, computed once per draw.
struct DrawTransforms {
    Mat4 model;         // object -> world
    Mat4 mvp;           // object -> clip (projection * view * model)
    Mat4 normalMatrix;  // object -> world for normals (see normalMatrix in vecmath.h)
};

// The fixed-function vertex stage: transform to clip and world space.
inline void fixedFunctionVertex(const Vertex& in, const DrawTransforms& t, ClipVertex& out) {
    const Vec4 p(in.position, 1.0f);
    out.pos = t.mvp * p;
    out.worldPos = (t.model * p).xyz();
    out.normal = normalize(t.normalMatrix.transformDir(in.normal));
    out.uv = in.uv;
    out.custom = Vec4(0, 0, 0, 0);
}

// What the pixel stage sees for one pixel.
struct PixelInput {
    Vec3 worldPos;     // perspective-correct, like every attribute here
    Vec3 normal;       // interpolated, then renormalized
    Vec2 uv;
    Vec4 custom;       // the vertex stage's custom attributes
    bool frontFacing;  // false: we're seeing the triangle's back (only with twoSided())
    Vec3 eye;          // camera position, world space
    Vec3 lightDir;     // direction toward the light, world space
};

class Shader {
public:
    virtual ~Shader() = default;

    // Default: the fixed-function transform.
    virtual void vertex(const Vertex& in, const DrawTransforms& t, ClipVertex& out) const {
        fixedFunctionVertex(in, t, out);
    }

    // Writes the pixel's color. Returning false discards the pixel: it keeps its
    // old color and depth, so whatever is behind it shows through.
    virtual bool pixel(const PixelInput& in, Vec3& color) const = 0;

    // Draw back faces too, even when settings.backfaceCull is on (to see inside a mesh).
    virtual bool twoSided() const { return false; }
};
