// transform_tool.h — Blender-style modal grab / rotate / scale (G, R, S). Pure math:
// given the object's transform when the tool started, the camera, and where the
// mouse started and is now, compute the new transform. The Editor turns the result
// into setTransform commands; nothing here touches the scene.
#pragma once

#include <string>

#include "scene.h"

enum class TransformMode { None = 0, Grab = 1, Rotate = 2, Scale = 3 };

class TransformTool {
public:
    // Starts transforming `start`, seen through view/projection in a width x height
    // viewport, with the mouse at pixel (x, y).
    void begin(TransformMode mode, const Transform& start, const Mat4& view, const Mat4& projection, int width,
               int height, float x, float y);
    void end() { mode_ = TransformMode::None; }
    bool active() const { return mode_ != TransformMode::None; }
    TransformMode mode() const { return mode_; }

    void setMouse(float x, float y);
    // Locks the transform to world axis 0/1/2 (X/Y/Z); the same axis again unlocks it.
    void toggleAxis(int axis);
    int axis() const { return axis_; }  // -1 when free
    // Typing a value: digits and '.', '-' flips the sign, '\b' deletes. Once the
    // text holds a digit it replaces the mouse: a distance along the axis (X if
    // none) for grab, degrees for rotate, a factor for scale. False for other keys.
    bool typeKey(char c);
    const std::string& typed() const { return typed_; }

    const Transform& start() const { return start_; }
    const Transform& result() const { return result_; }
    std::string describe() const;  // e.g. "Move X 1.25", for the status line

private:
    void update();
    bool typedValue(float& v) const;
    Vec3 axisVector() const;

    TransformMode mode_ = TransformMode::None;
    Transform start_, result_;
    Mat4 view_, projection_;
    int width_ = 1, height_ = 1;
    Vec3 eye_;              // camera position
    float pivotX_ = 0, pivotY_ = 0;  // the object's origin on screen, in pixels
    float startX_ = 0, startY_ = 0, mouseX_ = 0, mouseY_ = 0;
    float lastAngle_ = 0, turned_ = 0;  // rotate: screen angle, unwrapped across ±180°
    int axis_ = -1;
    std::string typed_;
    float shown_ = 0;       // the amount describe() reports
    Vec3 shownOffset_;      // grab without an axis reports a vector
};
