#include "transform_tool.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "picking.h"

namespace {

constexpr float kMinScale = 0.001f;  // a zero scale would collapse the object (and its normals)

// Where the ray meets the plane through `point` with normal `n`; false if parallel.
bool rayPlane(const Ray& r, const Vec3& point, const Vec3& n, Vec3& hit) {
    const float denom = dot(r.direction, n);
    if (std::fabs(denom) < 1e-6f) return false;
    hit = r.origin + r.direction * (dot(point - r.origin, n) / denom);
    return true;
}

// The point on the line p + a*s closest to the ray o + d*u (a and d unit length).
// At the closest pair, the gap w0 + a*s - d*u (w0 = p - o) is perpendicular to
// both lines. Dotting with a and with d gives two equations; eliminating u leaves
//   s * (1 - b^2) = b * (d.w0) - (a.w0),   with b = a.d.
// 1 - b^2 is 0 when the line points straight at the camera: no useful answer then.
bool rayLine(const Ray& r, const Vec3& p, const Vec3& a, float& s) {
    const Vec3 w0 = p - r.origin;
    const float b = dot(a, r.direction);
    const float denom = 1.0f - b * b;
    if (denom < 1e-6f) return false;
    s = (b * dot(r.direction, w0) - dot(a, w0)) / denom;
    return true;
}

float& component(Vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

}  // namespace

void TransformTool::begin(TransformMode mode, const Transform& start, const Mat4& view, const Mat4& projection,
                          int width, int height, float x, float y) {
    mode_ = mode;
    start_ = result_ = start;
    view_ = view;
    projection_ = projection;
    width_ = width;
    height_ = height;
    Mat4 inv;
    eye_ = invert(view, inv) ? Vec3{inv.m[0][3], inv.m[1][3], inv.m[2][3]} : Vec3{0, 0, 0};
    // The object's origin on screen, projected the way the rasterizer does it
    // (clip -> NDC -> pixels, y down, pixel centers at +0.5).
    const Vec4 c = projection * view * Vec4(start.position, 1.0f);
    if (c.w > 1e-6f) {
        pivotX_ = (c.x / c.w * 0.5f + 0.5f) * width - 0.5f;
        pivotY_ = (0.5f - c.y / c.w * 0.5f) * height - 0.5f;
    } else {
        pivotX_ = width * 0.5f;  // behind the camera: turn and scale around the screen center
        pivotY_ = height * 0.5f;
    }
    startX_ = mouseX_ = x;
    startY_ = mouseY_ = y;
    lastAngle_ = std::atan2(-(y - pivotY_), x - pivotX_);
    turned_ = 0;
    axis_ = -1;
    typed_.clear();
    shown_ = 0;
    shownOffset_ = {};
}

void TransformTool::setMouse(float x, float y) {
    // Rotation follows the mouse's angle around the pivot. atan2 jumps by 360° when
    // the mouse crosses the left side, so add up wrapped increments instead.
    const float angle = std::atan2(-(y - pivotY_), x - pivotX_);  // y flipped: counterclockwise is positive
    float d = angle - lastAngle_;
    if (d > kPi) d -= 2 * kPi;
    if (d < -kPi) d += 2 * kPi;
    turned_ += d;
    lastAngle_ = angle;
    mouseX_ = x;
    mouseY_ = y;
    update();
}

void TransformTool::toggleAxis(int axis) {
    axis_ = axis == axis_ ? -1 : axis;
    update();
}

bool TransformTool::typeKey(char c) {
    if ((c >= '0' && c <= '9') || c == '.') {
        if ((c == '.' && typed_.find('.') != std::string::npos) || typed_.size() >= 12) return true;
        typed_ += c;
    } else if (c == '-') {
        if (!typed_.empty() && typed_[0] == '-') typed_.erase(0, 1);
        else typed_.insert(0, "-");
    } else if (c == '\b') {
        if (!typed_.empty()) typed_.pop_back();
    } else {
        return false;
    }
    update();
    return true;
}

bool TransformTool::typedValue(float& v) const {
    if (typed_.find_first_of("0123456789") == std::string::npos) return false;
    v = std::strtof(typed_.c_str(), nullptr);
    return std::isfinite(v);
}

void TransformTool::setDirection(const Vec3& direction) {
    if (length(direction) < 1e-9f) return;
    direction_ = normalize(direction);
    axis_ = 3;
    update();
}

Vec3 TransformTool::axisVector() const {
    if (axis_ == 3) return direction_;
    return axis_ == 0 ? Vec3{1, 0, 0} : axis_ == 1 ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
}

void TransformTool::update() {
    const Vec3 toEye = eye_ - start_.position;
    const Vec3 toViewer = length(toEye) > 1e-6f ? normalize(toEye) : Vec3{0, 0, 1};
    float v = 0;
    const bool typed = typedValue(v);
    result_ = start_;

    if (mode_ == TransformMode::Grab) {
        if (typed) {
            const Vec3 a = axis_ < 0 ? Vec3{1, 0, 0} : axisVector();
            shown_ = v;
            shownOffset_ = a * v;
        } else {
            // Free: drag the object across the plane through it that faces the camera,
            // by as much as the mouse ray's hit point moved. On an axis: by how far
            // the point of the axis nearest the mouse ray moved along it. If the
            // geometry degenerates (ray parallel), keep the last good offset.
            const Ray r0 = rayThroughPixel(view_, projection_, startX_, startY_, width_, height_);
            const Ray r1 = rayThroughPixel(view_, projection_, mouseX_, mouseY_, width_, height_);
            if (axis_ < 0) {
                Vec3 h0, h1;
                if (rayPlane(r0, start_.position, toViewer, h0) && rayPlane(r1, start_.position, toViewer, h1))
                    shownOffset_ = h1 - h0;
            } else {
                float s0, s1;
                if (rayLine(r0, start_.position, axisVector(), s0) && rayLine(r1, start_.position, axisVector(), s1)) {
                    shown_ = s1 - s0;
                    shownOffset_ = axisVector() * shown_;
                }
            }
        }
        result_.position = start_.position + shownOffset_;
    } else if (mode_ == TransformMode::Rotate) {
        // Around the view axis (pointing at the camera), or a world axis. Turning
        // the mouse counterclockwise turns the object counterclockwise on screen;
        // an axis pointing away from the camera needs the opposite sign for that.
        const Vec3 a = axis_ < 0 ? toViewer : axisVector();
        float degrees = v;
        if (!typed) {
            degrees = turned_ * 180.0f / kPi;
            if (axis_ >= 0 && dot(a, toViewer) < 0) degrees = -degrees;
        }
        shown_ = degrees;
        // Applied after the existing rotation, so it turns about the world axis.
        result_.rotation = normalize(Quat::axisAngle(a, radians(degrees)) * start_.rotation);
    } else if (mode_ == TransformMode::Scale) {
        // The mouse's distance from the pivot, relative to where it started.
        float f = v;
        if (!typed) {
            const float d0 = std::max(std::hypot(startX_ - pivotX_, startY_ - pivotY_), 5.0f);
            f = std::hypot(mouseX_ - pivotX_, mouseY_ - pivotY_) / d0;
        }
        f = std::max(f, kMinScale);
        shown_ = f;
        if (axis_ < 0 || axis_ == 3) result_.scale = start_.scale * f;  // (a custom direction scales evenly)
        else component(result_.scale, axis_) *= f;
    }
}

std::string TransformTool::describe() const {
    if (!active()) return "";
    // Grab with no axis shows a vector; everything else one number on an axis (grab
    // falls back to X when typing). Typed text shows as typed, like Blender.
    const char* axes = "XYZ";
    char text[128];
    if (mode_ == TransformMode::Grab && axis_ < 0 && typed_.empty()) {
        std::snprintf(text, sizeof(text), "Move %.3f, %.3f, %.3f", shownOffset_.x, shownOffset_.y, shownOffset_.z);
        return text;
    }
    std::string axis = axis_ < 0 ? "" : axis_ == 3 ? " along the normal" : std::string(" ") + axes[axis_];
    if (mode_ == TransformMode::Grab && axis_ < 0) axis = " X";
    char amount[32];
    std::snprintf(amount, sizeof(amount), "%.*f", mode_ == TransformMode::Rotate ? 1 : 3, shown_);
    const char* name = mode_ == TransformMode::Grab ? "Move" : mode_ == TransformMode::Rotate ? "Rotate" : "Scale";
    const char* unit = mode_ == TransformMode::Rotate ? "°" : mode_ == TransformMode::Scale ? "×" : "";
    std::snprintf(text, sizeof(text), "%s%s %s%s", name, axis.c_str(), typed_.empty() ? amount : typed_.c_str(), unit);
    return text;
}
