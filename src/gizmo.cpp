#include "gizmo.h"

#include <cmath>

namespace {

const Vec3 kAxes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

// Distance from point p to the segment a-b, all in screen pixels.
float distanceToSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    t = clampf(t, 0.0f, 1.0f);
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

}  // namespace

MoveGizmo MoveGizmo::at(const Vec3& origin, const Mat4& view, const Mat4& projection, int width, int height) {
    MoveGizmo g;
    g.origin = origin;
    const Mat4 viewProj = projection * view;
    // Clip -> pixels, in the convention rayThroughPixel uses for the mouse
    // (pixel centers at +0.5), so hit tests compare like with like.
    auto toScreen = [&](const Vec3& p, float& x, float& y) {
        const Vec4 c = viewProj * Vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;  // behind the camera
        x = (c.x / c.w * 0.5f + 0.5f) * width - 0.5f;
        y = (0.5f - c.y / c.w * 0.5f) * height - 0.5f;
        return true;
    };
    if (!toScreen(origin, g.baseX, g.baseY)) return g;

    // Constant screen size: at depth d, a perspective camera sees 2*d*tan(fovY/2)
    // world units across its height, and projection.m[1][1] is 1/tan(fovY/2). For a
    // perspective projection, clip w is exactly that depth.
    const float depth = (viewProj * Vec4(origin, 1.0f)).w;
    g.pixel = 2.0f * depth / (projection.m[1][1] * height);
    g.length = kLengthPixels * g.pixel;

    Mat4 inv;
    const Vec3 eye = invert(view, inv) ? Vec3{inv.m[0][3], inv.m[1][3], inv.m[2][3]} : origin + Vec3{0, 0, 1};
    const Vec3 toEye = eye - origin;
    for (int a = 0; a < 3; ++a) {
        if (!toScreen(origin + kAxes[a] * g.length, g.tipX[a], g.tipY[a])) continue;
        // Hide an arrow that's foreshortened to a stub: it points (almost) at the
        // camera, so it can't be aimed along and would only be in the way.
        g.shown[a] = std::hypot(g.tipX[a] - g.baseX, g.tipY[a] - g.baseY) > 0.25f * kLengthPixels;
        // Sideways on screen = perpendicular to both the arrow and the view direction.
        const Vec3 across = cross(kAxes[a], toEye);
        if (::length(across) > 1e-6f) g.side[a] = normalize(across) * g.pixel;
    }
    g.valid = true;
    return g;
}

int MoveGizmo::axisAt(float x, float y) const {
    if (!valid) return -1;
    int best = -1;
    float bestDistance = kGrabPixels;
    for (int a = 0; a < 3; ++a) {
        if (!shown[a]) continue;
        // Skip the innermost 15%, where all three arrows meet and the mouse is
        // usually over the object itself (clicking there should select, not drag).
        const float sx = baseX + (tipX[a] - baseX) * 0.15f, sy = baseY + (tipY[a] - baseY) * 0.15f;
        const float d = distanceToSegment(x, y, sx, sy, tipX[a], tipY[a]);
        if (d <= bestDistance) {
            bestDistance = d;
            best = a;
        }
    }
    return best;
}

uint32_t MoveGizmo::axisColor(int axis, bool highlighted) {
    if (highlighted) return packColor({1.0f, 0.85f, 0.25f});
    static const Vec3 colors[3] = {{0.92f, 0.26f, 0.32f}, {0.50f, 0.82f, 0.22f}, {0.26f, 0.52f, 0.96f}};
    return packColor(colors[axis]);
}

void MoveGizmo::draw(Renderer& r, Framebuffer& fb, int highlight) const {
    if (!valid) return;
    auto drawArrow = [&](int a) {
        const uint32_t color = axisColor(a, a == highlight);
        const Vec3 dir = kAxes[a], s = side[a];
        // Shaft: three parallel lines one pixel apart, so it's 3 px thick.
        const Vec3 shaftEnd = origin + dir * (length * 0.8f);
        for (int k = -1; k <= 1; ++k) r.drawLine3D(fb, origin + s * float(k), shaftEnd + s * float(k), color);
        // Head: a triangle 14 px wide, filled with a fan of lines from the tip to
        // points half a pixel apart along its base.
        const Vec3 tip = origin + dir * length, base = origin + dir * (length * 0.78f);
        for (int i = -14; i <= 14; ++i) r.drawLine3D(fb, tip, base + s * (i * 0.5f), color);
    };
    for (int a = 0; a < 3; ++a)
        if (shown[a] && a != highlight) drawArrow(a);
    if (highlight >= 0 && highlight < 3 && shown[highlight]) drawArrow(highlight);  // on top
}
