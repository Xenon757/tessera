// hierarchy.h — parenting. An object's transform is relative to its parent's, so
// its place in the world is its parent's world transform followed by its own:
//   world(child) = world(parent) * local(child)
// all the way up. Everything is evaluated at a time, since any level can be animated.
#pragma once

#include <string>

#include "scene.h"

// The matrix that places the object in the world (what drawing and picking use).
Mat4 worldMatrix(const Scene& scene, const Object& o, float time);

// The same placement as position / rotation / scale. Rotations and scales compose
// exactly as long as no parent has a non-uniform scale with a rotation below it
// (that would shear, which a Transform can't hold; Blender has the same limit).
Transform worldTransform(const Scene& scene, const Object& o, float time);

// The local transform that puts a child of `parent` (kNoId: none) at `world`.
Transform localFromWorld(const Scene& scene, ObjectId parent, const Transform& world, float time);

// True if `ancestor` is `id`'s parent, grandparent, and so on.
bool isAncestor(const Scene& scene, ObjectId ancestor, ObjectId id);

// Whether `child` may have `parent` (kNoId always may): it must exist, and not be
// the child itself or one of its descendants (that would make a loop).
bool validParent(const Scene& scene, ObjectId child, ObjectId parent, std::string& error);

// Whether camera `camera` may look at `target` (kNoId: no target): another object
// that exists. A camera with a target faces it: its world rotation becomes the one
// that points its -Z at the target's origin (with +Y up), whatever its own rotation
// or animated rotation says. Where the target is, is worked out ignoring look-at
// targets entirely, so no chain of cameras aiming at each other can loop.
bool validLookAt(const Scene& scene, ObjectId camera, ObjectId target, std::string& error);
