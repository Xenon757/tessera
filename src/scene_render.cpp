#include "scene_render.h"

#include "gizmo.h"

#include <cfloat>
#include <cmath>

namespace {

Quat conjugate(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }  // a unit quaternion's inverse

const uint32_t kSelected = 0xFFFF9A1Au;  // orange, like Blender's selection

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

bool SceneRenderer::cameraView(const Scene& scene, ObjectId camera, float aspect, Mat4& view, Mat4& projection) {
    const Object* c = scene.find(camera);
    if (!c || c->type != ObjectType::Camera) return false;
    // Inverse of (translate * rotate): un-rotate after un-translating.
    view = conjugate(c->transform.rotation).toMat4() * Mat4::translate(-c->transform.position);
    projection = Mat4::perspective(radians(c->camera.fovY), aspect, c->camera.zNear, c->camera.zFar);
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

bool SceneRenderer::worldBounds(const Scene& scene, const Object& o, Vec3& lo, Vec3& hi) {
    if (o.type != ObjectType::Mesh) return false;
    Vec3 a, b;
    localBounds(scene, o.mesh, a, b);
    if (a.x > b.x) return false;  // empty mesh
    // The world box encloses all 8 transformed corners of the local box.
    const Mat4 m = o.transform.matrix();
    lo = {FLT_MAX, FLT_MAX, FLT_MAX};
    hi = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (int i = 0; i < 8; ++i) {
        const Vec3 p = (m * Vec4(Vec3{i & 1 ? b.x : a.x, i & 2 ? b.y : a.y, i & 4 ? b.z : a.z}, 1.0f)).xyz();
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return true;
}

void SceneRenderer::update(const Scene& scene, float dt) {
    for (const Object& o : scene.objects()) {
        if (o.type != ObjectType::Mesh || o.shader.type != "liquid") continue;
        // Probe the top of the object (local +Y), so moving and tilting both slosh.
        liquids_[o.id].wobble.update((o.transform.matrix() * Vec4(0, 1, 0, 1)).xyz(), dt);
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

void SceneRenderer::render(Renderer& r, Framebuffer& fb, const Scene& scene, const Mat4& view, const Mat4& projection,
                           const Overlays& overlays) {
    // World settings and the light are per-frame inputs the renderer already has.
    r.clearColor = scene.world().background;
    r.ambient = scene.world().ambient;
    for (const Object& o : scene.objects())
        if (o.type == ObjectType::Light && o.visible) {
            // The sun shines along its -Z, so the direction toward it is its +Z. The
            // renderer has one directional light: the first visible one drives it.
            r.lightDir = normalize(o.transform.rotation.rotate({0, 0, 1}));
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

    for (const Object& o : scene.objects()) {
        if (o.type != ObjectType::Mesh || !o.visible) continue;
        const MeshEntry* mesh = scene.mesh(o.mesh);
        if (!mesh) continue;
        const Mat4 model = o.transform.matrix();
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
            if (worldBounds(scene, o, lo, hi)) liquid.shader.fitLayers(lo.y - o.transform.position.y);
            shader = &liquid.shader;
        } else if (o.shader.type == "glass") {
            if (const Json* t = o.shader.params.find("tint")) {
                std::string error;
                vec3FromJson(t, glass.tint, error, "tint");
            }
            if (const Json* f = o.shader.params.find("rimStart")) glass.rimStart = static_cast<float>(f->asNumber());
            shader = &glass;
        }
        r.drawMesh(fb, *mesh->render, model, o.material.color, texture, shader);
    }

    if (overlays.gizmos)
        for (const Object& o : scene.objects())
            if (o.type != ObjectType::Mesh && o.visible)
                drawGizmo(r, fb, scene, o, o.id == overlays.selected ? kSelected : packColor({0.7f, 0.7f, 0.72f}), aspect);

    // The selection: the gizmo in orange (drawn above), or a mesh's bounding box.
    if (const Object* sel = scene.find(overlays.selected)) {
        Vec3 lo, hi;
        if (sel->type != ObjectType::Mesh) {
            if (!overlays.gizmos) drawGizmo(r, fb, scene, *sel, kSelected, aspect);
        } else if (worldBounds(scene, *sel, lo, hi)) {
            auto corner = [&](int i) { return Vec3{i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z}; };
            for (int i = 0; i < 8; ++i)
                for (int bit : {1, 2, 4})  // each corner to its neighbor along x, y, and z
                    if (!(i & bit)) r.drawLine3D(fb, corner(i), corner(i | bit), kSelected);
        }
        if (overlays.moveGizmo)  // last, so the arrows stay on top of everything
            MoveGizmo::at(sel->transform.position, view, projection, fb.width(), fb.height())
                .draw(r, fb, overlays.gizmoHighlight);
    }
    r.endFrame(fb);
}
