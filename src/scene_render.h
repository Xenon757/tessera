// scene_render.h — draws a Scene with the Renderer: one drawMesh per visible mesh
// object, plus editor overlays (ground grid, light and camera gizmos, selection).
// Only reads the scene. The renderer itself (and its pixel loops) doesn't change.
#pragma once

#include <map>

#include "liquid.h"
#include "renderer.h"
#include "scene.h"
#include "texture.h"

class SceneRenderer {
public:
    // The view and projection of a scene camera. A camera looks down its local -Z
    // with +Y up, so the view matrix is its transform inverted (scale ignored).
    static bool cameraView(const Scene& scene, ObjectId camera, float aspect, Mat4& view, Mat4& projection);

    // Advances per-object simulation (liquid sloshing follows each object's motion).
    void update(const Scene& scene, float dt);

    struct Overlays {
        bool grid = true;
        bool gizmos = true;  // lights and cameras, which have no geometry of their own
        ObjectId selected = kNoId;
        bool moveGizmo = false;  // arrows on the selection (see gizmo.h)
        int gizmoHighlight = -1; // the arrow under the mouse or being dragged
    };

    // A whole frame: beginFrame, overlays, every visible object, endFrame.
    void render(Renderer& renderer, Framebuffer& fb, const Scene& scene, const Mat4& view, const Mat4& projection,
                const Overlays& overlays);

    // World-space bounding box of an object's mesh (false for lights and cameras).
    bool worldBounds(const Scene& scene, const Object& o, Vec3& lo, Vec3& hi);

private:
    struct Liquid {
        LiquidShader shader;
        LiquidWobble wobble;
    };
    void localBounds(const Scene& scene, MeshId mesh, Vec3& lo, Vec3& hi);
    void drawGizmo(Renderer& renderer, Framebuffer& fb, const Scene& scene, const Object& o, uint32_t color, float aspect);

    std::map<ObjectId, Liquid> liquids_;
    std::map<MeshId, std::pair<Vec3, Vec3>> bounds_;  // meshes never change, so this cache never goes stale
    Texture checker_ = Texture::checker(256, 8, {0.95f, 0.95f, 0.95f}, {0.2f, 0.45f, 0.9f});
};
