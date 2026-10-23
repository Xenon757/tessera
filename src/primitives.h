// primitives.h — the editor's Add menu: cube, UV sphere, cylinder, cone, plane.
//
// All are unit-sized like Blender's (cube of side 2, sphere radius 1, cylinder and
// cone radius 1 and height 2, plane 2 x 2), centered on the origin; an object's
// transform sizes them. So a primitive is fully described by its type and segment
// counts, which is how scenes save it until it's edited.
#pragma once

#include <string>

#include "edit_mesh.h"

enum class PrimitiveType { Cube, Sphere, Cylinder, Cone, Plane };

struct Primitive {
    PrimitiveType type = PrimitiveType::Cube;
    int segments = 32;  // around the axis (sphere, cylinder, cone)
    int rings = 16;     // top to bottom (sphere)
};

// Valid segment counts: 3..256 around, 2..256 rings. Out-of-range values clamp.
EditMesh makePrimitive(Primitive p);

const char* primitiveName(PrimitiveType type);              // "cube", "sphere", ...
bool primitiveFromName(const std::string& name, PrimitiveType& out);
