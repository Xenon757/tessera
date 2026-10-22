// liquid_scene.h — the liquid-in-a-tube demo, shared by both front ends.
// Move the tube and the liquid sloshes; tilt it and the surface stays level.
#pragma once

#include "liquid.h"
#include "renderer.h"

struct LiquidScene {
    Mesh liquidMesh = makeTube(0.27f, 0.95f, 64, 12);
    Mesh glassMesh = makeTube(0.30f, 1.0f, 64, 12);
    LiquidShader liquid;
    GlassShader glass;
    LiquidWobble wobble;
    float x = 0.0f;     // tube position along X
    float tilt = 0.0f;  // tube rotation around Z, radians

    LiquidScene() {
        // Liquid Sort-style stacked colors, bottom to top.
        liquid.layers = {{0.93f, 0.33f, 0.36f}, {0.26f, 0.72f, 0.42f}, {0.30f, 0.50f, 0.95f}, {0.98f, 0.78f, 0.22f}};
        liquid.fillLevel = 0.3f;
        liquid.fitLayers(-1.0f + kLiquidLift);  // the liquid tube's lowest point, relative to the pivot
    }

    Mat4 model() const { return Mat4::translate({x, 0, 0}) * Mat4::rotateZ(tilt); }

    // move and tiltInput in -1..1 (e.g. from arrow keys); dt in seconds.
    void update(float dt, float move, float tiltInput) {
        x = clampf(x + move * 1.5f * dt, -1.2f, 1.2f);
        tilt = clampf(tilt + tiltInput * 1.2f * dt, -0.6f, 0.6f);
        // Probe the top of the tube so tilting sloshes the liquid too.
        wobble.update((model() * Vec4(0, 1, 0, 1)).xyz(), dt);
    }

    void draw(Renderer& renderer, Framebuffer& fb) {
        const Mat4 m = model();
        liquid.pivot = (m * Vec4(0, 0, 0, 1)).xyz();
        liquid.surfaceUp = wobble.surfaceUp();
        // Liquid first, then glass over it (the glass discards everything but its rim).
        renderer.drawMesh(fb, liquidMesh, m * Mat4::translate({0, kLiquidLift, 0}), {1, 1, 1}, nullptr, &liquid);
        renderer.drawMesh(fb, glassMesh, m, {1, 1, 1}, nullptr, &glass);
    }

private:
    // The liquid tube sits this far above the glass tube's bottom, inside it.
    static constexpr float kLiquidLift = 0.03f;
};
