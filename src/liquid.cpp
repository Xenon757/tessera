#include "liquid.h"

#include <algorithm>
#include <cmath>

void LiquidShader::vertex(const Vertex& in, const DrawTransforms& t, ClipVertex& out) const {
    fixedFunctionVertex(in, t, out);
    // Height above the pivot, measured along the surface normal. It's linear in
    // world position, so interpolating it per pixel is exact.
    out.custom = Vec4(dot(out.worldPos - pivot, surfaceUp), 0, 0, 0);
}

bool LiquidShader::pixel(const PixelInput& in, Vec3& color) const {
    const float height = in.custom.x;
    if (height > fillLevel || layers.empty()) return false;  // above the surface: empty tube
    const Vec3& top = layers.back();

    if (!in.frontFacing) {
        // We can see this back face only because the front of the tube was cut
        // away above the surface, so draw it as the flat liquid surface: the top
        // layer's color, a bit lighter, lit with the surface's normal.
        const float lit = 0.75f + 0.25f * std::max(dot(surfaceUp, in.lightDir), 0.0f);
        color = (top * 1.15f + Vec3{0.05f, 0.05f, 0.05f}) * lit;
        return true;
    }

    // The sides: which layer is this, counting down from the surface?
    const float depth = fillLevel - height;
    const int fromTop = static_cast<int>(depth / layerThickness);
    const int count = static_cast<int>(layers.size());
    Vec3 base = layers[std::max(0, count - 1 - fromTop)];
    if (depth < foamWidth) base = top * 0.6f + Vec3{0.4f, 0.4f, 0.4f};  // foam line at the surface

    // Lambert + ambient, plus a rim term: brightest where the surface turns away
    // from the eye, which reads as light catching the curved glass and liquid.
    const Vec3 toEye = normalize(in.eye - in.worldPos);
    const float diffuse = 0.35f + 0.65f * std::max(dot(in.normal, in.lightDir), 0.0f);
    const float rim = std::pow(1.0f - std::fabs(dot(in.normal, toEye)), 3.0f);
    color = base * diffuse + Vec3{1, 1, 1} * (0.35f * rim);
    return true;
}

bool GlassShader::pixel(const PixelInput& in, Vec3& color) const {
    const Vec3 toEye = normalize(in.eye - in.worldPos);
    // |N.V|: both sides of the glass count, so it doesn't matter which way N faces.
    const float facing = std::fabs(dot(in.normal, toEye));
    const float rim = 1.0f - facing;
    const Vec3 halfVec = normalize(in.lightDir + toEye);
    const float highlight = std::pow(std::fabs(dot(in.normal, halfVec)), 80.0f);
    if (rim < rimStart && highlight < 0.3f) return false;  // see-through
    color = tint * (0.35f + 0.65f * rim) + Vec3{1, 1, 1} * highlight;
    return true;
}

void LiquidShader::fitLayers(float bottomHeight) {
    if (!layers.empty()) layerThickness = (fillLevel - bottomHeight) / static_cast<float>(layers.size());
}

void LiquidWobble::update(const Vec3& probe, float dt) {
    if (dt <= 0.0f) return;
    // Finite differences: velocity from two positions, acceleration from two velocities.
    const Vec3 velocity = started_ ? (probe - lastProbe_) / dt : Vec3{};
    const Vec3 accel = started_ ? (velocity - lastVelocity_) / dt : Vec3{};
    started_ = true;
    lastProbe_ = probe;
    lastVelocity_ = velocity;

    // Where the surface would settle if this acceleration held (see the header).
    // Not clamped: a sudden stop is a one-frame spike, and clamping it would throw
    // away most of the kick. The resulting tilt is clamped instead.
    const float targetX = accel.x / gravity;
    const float targetZ = accel.z / gravity;

    // Damped spring toward the target, semi-implicit Euler: update the speed
    // first, then the position with the new speed. That ordering keeps a spring
    // stable at frame-sized time steps, where plain Euler gains energy and blows up.
    speedX_ += (stiffness * (targetX - tiltX_) - damping * speedX_) * dt;
    speedZ_ += (stiffness * (targetZ - tiltZ_) - damping * speedZ_) * dt;
    tiltX_ = clampf(tiltX_ + speedX_ * dt, -maxTilt, maxTilt);
    tiltZ_ = clampf(tiltZ_ + speedZ_ * dt, -maxTilt, maxTilt);
}
