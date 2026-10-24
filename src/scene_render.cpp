#include "scene_render.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <set>

#include "animation.h"
#include "gizmo.h"
#include "hierarchy.h"

namespace {

Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }  // a unit quaternion's inverse

const uint32_t kSelected = 0xFFFF9A1Au;  // orange, like Blender's selection: the active object
const uint32_t kAlsoSelected = 0xFFB85A14u;  // darker orange: the rest of the selection

void configureLiquid(LiquidShader& s, const Json& params) {
    s.fillLevel = 0.3f;
    s.foamWidth = 0.035f;
    s.layers = {{0.93f, 0.33f, 0.36f}, {0.26f, 0.72f, 0.42f}, {0.30f, 0.50f, 0.95f}, {0.98f, 0.78f, 0.22f}};
    if (const Json* f = params.find("fillLevel")) s.fillLevel = static_cast<float>(f->asNumber());
    if (const Json* f = params.find("foamWidth")) s.foamWidth = static_cast<float>(f->asNumber());
    if (const Json* layers = params.find("layers")) {
        s.layers.clear();
        for (size_t i = 0; i < layers->size(); ++i) {
            Vec3 c;
            std::string error;
            if (vec3FromJson(&(*layers)[i], c, error, "layer")) s.layers.push_back(c);
        }
    }
}

}  // namespace

void SceneRenderer::drawEditOverlay(Renderer& r, Framebuffer& fb, const Scene& scene, const Overlays& overlays,
                                    const std::vector<Mat4>& models) {
    const int index = scene.indexOf(overlays.editObject);
    if (index < 0) return;
    const EditMesh& m = *overlays.editMesh;
    const Mat4& model = models[index];
    std::vector<Vec3> world;
    world.reserve(m.verts().size());
    for (const auto& v : m.verts()) world.push_back((model * Vec4(v.position, 1.0f)).xyz());
    const MeshSelection empty;
    const MeshSelection& sel = overlays.editSelection ? *overlays.editSelection : empty;
    std::vector<bool> vertOn(m.verts().size(), false), faceOn(m.faces().size(), false);
    for (int v : sel.verts)
        if (v >= 0 && v < static_cast<int>(vertOn.size())) vertOn[v] = true;
    for (int f : sel.faces)
        if (f >= 0 && f < static_cast<int>(faceOn.size())) faceOn[f] = true;
    std::set<std::pair<int, int>> edgeOn(sel.edges.begin(), sel.edges.end());
    const uint32_t dark = 0xFF16181Du, light = 0xFFE8EAF0;
    // Edges: all of them, selected ones in orange (drawn second, so they win).
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& e : m.edges()) {
            const bool on = edgeOn.count({std::min(e.v[0], e.v[1]), std::max(e.v[0], e.v[1])}) > 0;
            if (on == (pass == 1)) r.drawLine3DDepthTested(fb, world[e.v[0]], world[e.v[1]], on ? kSelected : dark);
        }
    if (overlays.selectMode == 0) {  // vertices
        for (size_t v = 0; v < world.size(); ++v)
            if (!vertOn[v]) r.drawPoint3D(fb, world[v], dark, 4);
        for (size_t v = 0; v < world.size(); ++v)
            if (vertOn[v]) r.drawPoint3D(fb, world[v], kSelected, 5);
    } else if (overlays.selectMode == 2) {  // a dot at each face's center
        for (size_t f = 0; f < m.faces().size(); ++f) {
            Vec3 c{0, 0, 0};
            const int n = m.faces()[f].count;
            for (int l = m.faces()[f].loop, k = 0; k < n; ++k, l = m.loops()[l].next) c = c + world[m.loops()[l].vert] * (1.0f / n);
            r.drawPoint3D(fb, c, faceOn[f] ? kSelected : light, faceOn[f] ? 5 : 3);
        }
    }
}

bool SceneRenderer::cameraView(const Scene& scene, ObjectId camera, float time, float aspect, Mat4& view,
                               Mat4& projection) {
    const Object* found = scene.find(camera);
    if (!found || found->type != ObjectType::Camera) return false;
    const Object c = animatedObject(*found, time);
    const Transform w = worldTransform(scene, *found, time);  // a parented camera rides along
    // Inverse of (translate * rotate): un-rotate after un-translating.
    view = conjugate(w.rotation).toMat4() * Mat4::translate(-w.position);
    projection = Mat4::perspective(radians(c.camera.fovY), aspect, c.camera.zNear, c.camera.zFar);
    return true;
}

void SceneRenderer::localBounds(const Scene& scene, MeshId mesh, Vec3& lo, Vec3& hi) {
    auto cached = bounds_.find(mesh);
    if (cached == bounds_.end()) {
        Vec3 a{FLT_MAX, FLT_MAX, FLT_MAX}, b{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        if (const MeshEntry* m = scene.mesh(mesh))
            for (const auto& v : m->edit->verts()) {
                a = {std::min(a.x, v.position.x), std::min(a.y, v.position.y), std::min(a.z, v.position.z)};
                b = {std::max(b.x, v.position.x), std::max(b.y, v.position.y), std::max(b.z, v.position.z)};
            }
        cached = bounds_.emplace(mesh, std::make_pair(a, b)).first;
    }
    lo = cached->second.first;
    hi = cached->second.second;
}

bool SceneRenderer::worldBounds(const Scene& scene, const Object& o, const Mat4& m, Vec3& lo, Vec3& hi) {
    if (o.type != ObjectType::Mesh) return false;
    Vec3 a, b;
    localBounds(scene, o.mesh, a, b);
    if (a.x > b.x) return false;  // empty mesh
    // The world box encloses all 8 transformed corners of the local box.
    lo = {FLT_MAX, FLT_MAX, FLT_MAX};
    hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int i = 0; i < 8; ++i) {
        const Vec3 p = (m * Vec4(Vec3{i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z}, 1.0f)).xyz();
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return true;
}

void SceneRenderer::update(const Scene& scene, float time, float dt) {
    for (const Object& base : scene.objects()) {
        if (base.type != ObjectType::Mesh || base.shader.type != "liquid") continue;
        // Probe the top of the object (local +Y), so moving and tilting both slosh,
        // whether the object moves itself or rides on an animated parent.
        liquids_[base.id].wobble.update((worldMatrix(scene, base, time) * Vec4(0, 1, 0, 1)).xyz(), dt);
    }
}

void SceneRenderer::drawGizmo(Renderer& r, Framebuffer& fb, const Scene& scene, const Object& o, uint32_t color,
                              float aspect) {
    const Mat4 m = Mat4::translate(o.transform.position) * o.transform.rotation.toMat4();  // gizmos ignore scale
    auto at = [&](const Vec3& local) { return (m * Vec4(local, 1.0f)).xyz(); };
    if (o.type == ObjectType::Light) {
        // A small circle facing the light's direction, and a line along the way it shines (-Z).
        const int n = 12;
        for (int i = 0; i < n; ++i) {
            const float a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n;
            r.drawLine3D(fb, at({0.25f * std::cos(a0), 0.25f * std::sin(a0), 0}), at({0.25f * std::cos(a1), 0.25f * std::sin(a1), 0}), color);
        }
        r.drawLine3D(fb, at({0, 0, 0}), at({0, 0, -1.5f}), color);
    } else if (o.type == ObjectType::Camera) {
        // The view pyramid: the camera's position to a rectangle one unit ahead,
        // shaped by its field of view; a triangle marks "up" on the active camera.
        const float h = std::tan(radians(o.camera.fovY) * 0.5f), w = h * aspect;
        const Vec3 corners[4] = {at({-w, -h, -1}), at({w, -h, -1}), at({w, h, -1}), at({-w, h, -1})};
        for (int i = 0; i < 4; ++i) {
            r.drawLine3D(fb, at({0, 0, 0}), corners[i], color);
            r.drawLine3D(fb, corners[i], corners[(i + 1) % 4], color);
        }
        if (scene.activeCamera() == o.id) {
            const Vec3 t0 = at({-w * 0.5f, h * 1.1f, -1}), t1 = at({w * 0.5f, h * 1.1f, -1}), t2 = at({0, h * 1.6f, -1});
            r.drawLine3D(fb, t0, t1, color);
            r.drawLine3D(fb, t1, t2, color);
            r.drawLine3D(fb, t2, t0, color);
        }
    }
}

void SceneRenderer::render(Renderer& r, Framebuffer& fb, const Scene& scene, float time, const Mat4& view,
                           const Mat4& projection, const Overlays& overlays) {
    // Every object as it is at `time`, placed in the world (through its parents):
    // `posed` holds world transforms (for gizmos, lights, pivots), `models` the exact
    // world matrices drawing uses. Copies are cheap next to drawing.
    std::vector<Object> posed;
    std::vector<Mat4> models;
    posed.reserve(scene.objects().size());
    models.reserve(scene.objects().size());
    for (const Object& o : scene.objects()) {
        posed.push_back(animatedObject(o, time));
        posed.back().transform = worldTransform(scene, o, time);
        models.push_back(worldMatrix(scene, o, time));
    }

    // World settings and the light are per-frame inputs the renderer already has.
    r.clearColor = scene.world().background;
    r.ambient = scene.world().ambient;
    for (const Object& o : posed)
        if (o.type == ObjectType::Light && o.visible) {
            // The sun shines along its -Z, so the direction toward it is its +Z. The
            // renderer has one directional light: the first visible one drives it.
            r.lightDir = normalize(o.transform.rotation.rotate({0, 0, 1}));
            r.lightColor = o.light.color;
            r.lightIntensity = o.light.intensity;
            break;
        }
    r.beginFrame(fb, view, projection);
    const float aspect = static_cast<float>(fb.width()) / fb.height();

    // The grid goes down first, with no depth test, so objects simply cover it.
    if (overlays.grid) {
        const uint32_t gridColor = packColor({0.22f, 0.23f, 0.26f});
        for (int i = -10; i <= 10; ++i) {
            if (i == 0) continue;
            r.drawLine3D(fb, {float(i), 0, -10}, {float(i), 0, 10}, gridColor);
            r.drawLine3D(fb, {-10, 0, float(i)}, {10, 0, float(i)}, gridColor);
        }
        r.drawLine3D(fb, {-10, 0, 0}, {10, 0, 0}, packColor({0.6f, 0.22f, 0.22f}));  // X axis
        r.drawLine3D(fb, {0, 0, -10}, {0, 0, 10}, packColor({0.22f, 0.32f, 0.65f}));  // Z axis
    }

    for (size_t i = 0; i < posed.size(); ++i) {
        const Object& o = posed[i];
        if (o.type != ObjectType::Mesh || !o.visible) continue;
        const MeshEntry* mesh = scene.mesh(o.mesh);
        if (!mesh) continue;
        const Mat4& model = models[i];
        const Mesh& drawn = o.id == overlays.editObject && overlays.editRender ? *overlays.editRender : *mesh->render;
        r.specular = o.material.specular;
        r.shininess = o.material.shininess;
        const Texture* texture = o.material.checker && mesh->render->hasTexCoords ? &checker_ : nullptr;
        const Shader* shader = nullptr;
        GlassShader glass;
        if (o.shader.type == "liquid") {
            Liquid& liquid = liquids_[o.id];
            configureLiquid(liquid.shader, o.shader.params);
            liquid.shader.pivot = o.transform.position;
            liquid.shader.surfaceUp = liquid.wobble.surfaceUp();
            Vec3 lo, hi;
            if (worldBounds(scene, o, model, lo, hi)) liquid.shader.fitLayers(lo.y - o.transform.position.y);
            shader = &liquid.shader;
        } else if (o.shader.type == "glass") {
            if (const Json* t = o.shader.params.find("tint")) {
                std::string error;
                vec3FromJson(t, glass.tint, error, "tint");
            }
            if (const Json* f = o.shader.params.find("rimStart")) glass.rimStart = static_cast<float>(f->asNumber());
            shader = &glass;
        }
        r.drawMesh(fb, drawn, model, o.material.color, texture, shader);
    }

    auto selectionColor = [&](ObjectId id) -> uint32_t {
        if (id == overlays.active) return kSelected;
        for (ObjectId s : overlays.selection)
            if (s == id) return kAlsoSelected;
        return 0;
    };
    if (overlays.gizmos)
        for (const Object& o : posed)
            if (o.type != ObjectType::Mesh && o.visible) {
                const uint32_t c = selectionColor(o.id);
                drawGizmo(r, fb, scene, o, c ? c : packColor({0.7f, 0.7f, 0.72f}), aspect);
            }

    // The selection: gizmos in orange (drawn above), meshes get their bounding box.
    // The active object is drawn last, so its brighter outline wins where they overlap.
    std::vector<ObjectId> outlined = overlays.selection;
    std::stable_partition(outlined.begin(), outlined.end(), [&](ObjectId id) { return id != overlays.active; });
    for (ObjectId id : outlined) {
        const int index = scene.indexOf(id);
        if (index < 0 || id == overlays.editObject) continue;  // in edit mode its elements show instead
        const Object& sel = posed[index];
        Vec3 lo, hi;
        if (sel.type != ObjectType::Mesh) {
            if (!overlays.gizmos) drawGizmo(r, fb, scene, sel, selectionColor(id), aspect);
        } else if (worldBounds(scene, sel, models[index], lo, hi)) {
            auto corner = [&](int i) { return Vec3{i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z}; };
            for (int i = 0; i < 8; ++i)
                for (int bit : {1, 2, 4})  // each corner to its neighbor along x, y, and z
                    if (!(i & bit)) r.drawLine3D(fb, corner(i), corner(i | bit), selectionColor(id));
        }
    }
    if (overlays.editMesh) drawEditOverlay(r, fb, scene, overlays, models);
    if (overlays.moveGizmo)  // last, so the arrows stay on top of everything
        MoveGizmo::at(overlays.gizmoPosition, view, projection, fb.width(), fb.height()).draw(r, fb, overlays.gizmoHighlight);

    // The box-select rectangle: its pixel corners un-projected to points just past
    // the near plane, then drawn with the same 3D lines as everything else.
    if (overlays.box) {
        Mat4 inv;
        if (invert(projection * view, inv)) {
            const float* b = overlays.boxCorners;
            auto at = [&](float x, float y) {
                const Vec4 p = inv * Vec4((x + 0.5f) / fb.width() * 2 - 1, 1 - (y + 0.5f) / fb.height() * 2, -0.999f, 1);
                return Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
            };
            const Vec3 c[4] = {at(b[0], b[1]), at(b[2], b[1]), at(b[2], b[3]), at(b[0], b[3])};
            for (int i = 0; i < 4; ++i) r.drawLine3D(fb, c[i], c[(i + 1) % 4], packColor({0.85f, 0.87f, 0.92f}));
        }
    }
    r.endFrame(fb);
}
