// animation.h — keyframe evaluation: where everything is at time t. A pure function
// of the object and t, with no clock or state inside, so any frame can be drawn in
// any order and the same t always gives the same image (what export relies on).
#pragma once

#include "scene.h"

// Maps progress between two keys (0..1) to progress of the value (0..1).
//   Linear     u
//   EaseIn     u^2                 starts slow
//   EaseOut    1 - (1-u)^2         ends slow
//   EaseInOut  u^2 * (3 - 2u)      smoothstep: slow at both ends
//   Step       0                   holds the key's value until the next key
float applyEasing(Easing easing, float u);

// The track's value at `time` (trackPropertyWidth floats). Before the first key
// and after the last, it holds that key's value. Between two keys it blends with
// the earlier key's easing: lerp for vectors and numbers, slerp for rotations
// (which turns the short way round, at a constant angular speed).
void sampleTrack(const Track& track, float time, float out[4]);

// `o` with each animated property replaced by its value at `time`.
Object animatedObject(const Object& o, float time);

// Seconds from a frame number and back, at `fps` frames per second.
inline float frameToTime(float frame, float fps) { return frame / fps; }
inline float timeToFrame(float time, float fps) { return time * fps; }
