// liquid.h — Liquid Sort–style liquid in a test tube, built on the Shader stages.
//
// The liquid is drawn on the tube mesh itself; there's no liquid geometry:
//   * everything above a world-space fill plane is discarded, so the cut stays
//     level however the tube is tilted;
//   * front faces below the plane are the liquid's sides: colored layers, foam;
//   * back faces show only where the front was discarded (looking down into the
//     tube), and are drawn flat as the liquid's top surface. That's what makes a
//     hollow mesh look filled.
#pragma once

#include <vector>

#include "shader.h"

class LiquidShader : public Shader {
public:
    // ---- Uniforms: set before drawMesh.
    Vec3 pivot;                   // world point the fill level is measured from (the tube's center)
    Vec3 surfaceUp{0, 1, 0};      // unit normal of the liquid surface (world up, tilted by wobble)
    float fillLevel = 0.3f;       // surface height above the pivot, along surfaceUp
    std::vector<Vec3> layers;     // layer colors, bottom to top
    float layerThickness = 0.3f;  // each layer's thickness, measured down from the surface
    float foamWidth = 0.035f;     // band just under the surface drawn as foam

    bool twoSided() const override { return true; }
    void vertex(const Vertex& in, const DrawTransforms& t, ClipVertex& out) const override;
    bool pixel(const PixelInput& in, Vec3& color) const override;

    // Fills `layers` levels of liquid of thickness layerThickness so the top sits
    // at fillLevel: helper for laying out a tube whose bottom is at `bottomHeight`
    // (relative to the pivot).
    void fitLayers(float bottomHeight);

    // NOTE: the fill height is fixed relative to the pivot, so tilting the tube
    // doesn't conserve the liquid's volume. Fine for moderate tilts.
};

// Glass without transparency. Real glass is nearly invisible head-on and bright
// at grazing angles (the Fresnel effect), so this shader discards the glass where
// it faces the camera and draws only the grazing-angle rim plus a specular streak.
// Drawn after the liquid, on a slightly larger tube, so the two never z-fight.
class GlassShader : public Shader {
public:
    Vec3 tint{0.8f, 0.9f, 1.0f};
    float rimStart = 0.6f;  // how edge-on (1 - |N.V|) a pixel must be to show as glass

    bool twoSided() const override { return true; }  // the far wall's outline shows through
    bool pixel(const PixelInput& in, Vec3& color) const override;
};

// Sloshing. In an accelerating container, liquid settles perpendicular to the
// *effective* gravity, g - a = (-a.x, -g, -a.z): accelerate toward +x and the
// liquid lags, piling up on the -x side. The surface normal points the other way,
// (a.x, g, a.z), so its horizontal tilt (normal.x / normal.y) wants to be a / g.
// Liquid doesn't snap there: it overshoots and sloshes. So each tilt axis (X and
// Z) is a damped spring chasing that target. Run update() once per frame on the
// CPU, then hand surfaceUp() to the shader as a uniform.
class LiquidWobble {
public:
    float gravity = 9.8f;     // world units per second^2
    float stiffness = 60.0f;  // spring constant: sqrt(stiffness) is the slosh speed in rad/s
    float damping = 2.5f;     // higher settles sooner
    float maxTilt = 0.7f;     // cap on the surface's tilt (tan of the angle, ~35 degrees)

    // `probe`: a world-space point that moves with the tube. Use the top of the
    // tube, not its center, so rotating the tube sloshes it too. dt in seconds.
    void update(const Vec3& probe, float dt);
    Vec3 surfaceUp() const { return normalize(Vec3{tiltX_, 1.0f, tiltZ_}); }
    float tiltX() const { return tiltX_; }

private:
    bool started_ = false;
    Vec3 lastProbe_, lastVelocity_;
    float tiltX_ = 0, tiltZ_ = 0;      // horizontal components of the surface normal (y = 1)
    float speedX_ = 0, speedZ_ = 0;    // their rates of change
};
