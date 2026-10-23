// gizmo.h — the move gizmo: three arrows on the selected object, one per world axis
// (X red, Y green, Z blue), that stay the same size on screen. Dragging one runs the
// axis-locked grab from transform_tool.h. Hit testing is done in screen pixels, so
// "close enough to grab" matches what the user sees, however far away the object is.
#pragma once

#include <cstdint>

#include "renderer.h"

struct MoveGizmo {
    static constexpr float kLengthPixels = 120;  // arrow length on screen (before foreshortening)
    static constexpr float kGrabPixels = 8;      // how close the mouse must come to an arrow

    bool valid = false;  // false when the object is behind the camera
    Vec3 origin;
    float pixel = 0;     // world units per screen pixel at the origin's depth
    float length = 0;    // world length of an arrow (kLengthPixels on screen)
    bool shown[3] = {};  // an arrow pointing almost straight at the camera is hidden
    Vec3 side[3];        // per arrow: one pixel sideways, in world units (for thickness)
    float baseX = 0, baseY = 0;              // the origin on screen
    float tipX[3] = {}, tipY[3] = {};        // each arrow's tip on screen

    // The gizmo for an object at `origin`, seen through view/projection in a
    // width x height viewport. Screen positions use the mouse's pixel convention.
    static MoveGizmo at(const Vec3& origin, const Mat4& view, const Mat4& projection, int width, int height);

    // The arrow (0/1/2 = X/Y/Z) within kGrabPixels of pixel (x, y), nearest first; -1 if none.
    int axisAt(float x, float y) const;

    // Draws the arrows as overlay lines (no depth test, like the other overlays).
    // `highlight` (an axis, or -1) is drawn in the hover color, on top.
    void draw(Renderer& renderer, Framebuffer& fb, int highlight) const;

    static uint32_t axisColor(int axis, bool highlighted);
};
