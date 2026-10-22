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
    void vertex(const Vertex& in, const Mat4& model, const Mat4& mvp, ClipVertex& out) const override;
    bool pixel(const PixelInput& in, Vec3& color) const override;

    // Fills `layers` levels of liquid of thickness layerThickness so the top sits
    // at fillLevel: helper for laying out a tube whose bottom is at `bottomHeight`
    // (relative to the pivot).
    void fitLayers(float bottomHeight);

    // NOTE: the fill height is fixed relative to the pivot, so tilting the tube
    // doesn't conserve the liquid's volume. Fine for moderate tilts.
};
