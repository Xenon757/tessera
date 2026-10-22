// camera.h — orbit camera: rotates around a target point at a fixed distance.
#pragma once

#include <cmath>

#include "vecmath.h"

struct OrbitCamera {
    Vec3 target{0, 0, 0};
    float yaw = 0.6f;     // radians around +Y
    float pitch = 0.45f;  // radians above the XZ plane
    float distance = 5.0f;
    float fovY = radians(60.0f);
    float zNear = 0.1f;
    float zFar = 100.0f;

    Vec3 eye() const {
        float cp = std::cos(pitch);
        return target + Vec3{distance * cp * std::sin(yaw), distance * std::sin(pitch),
                             distance * cp * std::cos(yaw)};
    }

    Mat4 view() const { return Mat4::lookAt(eye(), target, {0, 1, 0}); }

    Mat4 projection(float aspect) const { return Mat4::perspective(fovY, aspect, zNear, zFar); }

    void orbit(float dYaw, float dPitch) {
        yaw += dYaw;
        pitch = clampf(pitch + dPitch, -1.5f, 1.5f);  // stay short of the poles
    }

    void zoom(float factor) { distance = clampf(distance * factor, 0.5f, 50.0f); }
};
