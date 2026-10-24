// scene_render.h — draws a Scene with the Renderer: one drawMesh per visible mesh
// object, plus editor overlays (ground grid, light and camera gizmos, selection).
// Only reads the scene. The renderer itself (and its pixel loops) doesn't change.
// Everything is drawn as it is at a given time: animated objects are posed first.
#pragma once

#include <map>
#include <vector>

#include "liquid.h"
#include "mesh_ops.h"
#include "renderer.h"
#include "scene.h"
#include "texture.h"

class SceneRenderer {
public:
    // The view and projection of a scene camera. A camera looks down its local -Z
    // with +Y up, so the view matrix is its transform inverted (scale ignored).
    static bool cameraView(const Scene& scene, ObjectId camera, float time, float aspect, Mat4& view, Mat4& projection);

    // Advances per-object simulation by dt (liquid sloshing follows each object's
    // motion, including animated motion at `time`).
    void update(const Scene& scene, float time, float dt);

    struct Overlays {
        bool grid = true;
        bool gizmos = true;  // lights and cameras, which have no geometry of their own
        std::vector<ObjectId> selection;  // outlined; the active one brighter
        ObjectId active = kNoId;
        bool moveGizmo = false;           // arrows (see gizmo.h) at gizmoPosition
        Vec3 gizmoPosition;
        int gizmoHighlight = -1;          // the arrow under the mouse or being dragged
        bool box = false;                 // a box-select rectangle being dragged
        float boxCorners[4] = {};         // x0, y0, x1, y1 in pixels
        // Edit mode: the edited object's edges and vertices (or face centers) on top,
        // hidden where surfaces cover them. `editRender` replaces its drawn mesh (a live
        // preview while vertices move); `editSelection` lists verts, edges, and faces.
        ObjectId editObject = kNoId;
        const EditMesh* editMesh = nullptr;
        const Mesh* editRender = nullptr;
        const MeshSelection* editSelection = nullptr;
        int selectMode = 0;               // 0 vertices, 1 edges, 2 faces
    };

    // A whole frame at `time`: beginFrame, overlays, every visible object, endFrame.
    void render(Renderer& renderer, Framebuffer& fb, const Scene& scene, float time, const Mat4& view,
                const Mat4& projection, const Overlays& overlays);

    // World-space bounding box of an object's mesh placed by `model` (its world
    // matrix, see hierarchy.h). False for lights and cameras.
    bool worldBounds(const Scene& scene, const Object& o, const Mat4& model, Vec3& lo, Vec3& hi);

private:
    struct Liquid {
        LiquidShader shader;
        LiquidWobble wobble;
    };
    void localBounds(const Scene& scene, MeshId mesh, Vec3& lo, Vec3& hi);
    void drawGizmo(Renderer& renderer, Framebuffer& fb, const Scene& scene, const Object& o, uint32_t color, float aspect);
    void drawEditOverlay(Renderer& renderer, Framebuffer& fb, const Scene& scene, const Overlays& overlays,
                         const std::vector<Mat4>& models);

    std::map<ObjectId, Liquid> liquids_;
    std::map<MeshId, std::pair<Vec3, Vec3>> bounds_;  // meshes never change, so this cache never goes stale
    Texture checker_ = Texture::checker(256, 8, {0.95f, 0.95f, 0.95f}, {0.2f, 0.45f, 0.9f});
};
