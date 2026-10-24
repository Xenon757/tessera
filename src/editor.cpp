#include "editor.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>

#include "animation.h"
#include "gizmo.h"
#include "hierarchy.h"
#include "picking.h"

Editor::Editor() : scene_(Scene::makeDefault()) {}

Json Editor::command(const Json& cmd) {
    if (transforming()) confirmTransform();  // anything else finishes a grab first
    Json result, out = Json::object();
    std::string error;
    const bool ok = executeEdit(cmd, result, error);
    out.set("ok", ok);
    if (ok) out.set("result", result);
    else out.set("error", error);
    return out;
}

bool Editor::undo() {
    if (transforming()) cancelTransform();
    return stack_.undo();
}
bool Editor::redo() {
    if (transforming()) cancelTransform();
    return stack_.redo();
}

void Editor::setSelection(std::vector<ObjectId> ids, ObjectId active) {
    if (ids != selection_ || active != selected_) {
        selection_ = std::move(ids);
        selected_ = active;
        ++uiChanges_;
    }
}

void Editor::select(ObjectId id) {
    if (id != kNoId && !scene_.find(id)) id = kNoId;
    setSelection(id == kNoId ? std::vector<ObjectId>{} : std::vector<ObjectId>{id}, id);
}

void Editor::toggleSelect(ObjectId id) {
    if (!scene_.find(id)) return;
    std::vector<ObjectId> ids = selection();
    const auto at = std::find(ids.begin(), ids.end(), id);
    if (at == ids.end()) {
        ids.push_back(id);  // add it; the newest is active
        setSelection(ids, id);
    } else if (id == selected()) {
        ids.erase(at);  // drop it; the one selected before it becomes active
        setSelection(ids, ids.empty() ? kNoId : ids.back());
    } else {
        ids.erase(at);  // selected but not active: make it active
        ids.push_back(id);
        setSelection(ids, id);
    }
}

void Editor::selectAll(bool all) {
    std::vector<ObjectId> ids;
    if (all)
        for (const Object& o : scene_.objects()) ids.push_back(o.id);
    const ObjectId active = !all ? kNoId : scene_.find(selected()) ? selected() : ids.empty() ? kNoId : ids.front();
    setSelection(ids, active);
}

ObjectId Editor::selected() const {
    const std::vector<ObjectId> ids = selection();
    return std::find(ids.begin(), ids.end(), selected_) != ids.end() ? selected_ : kNoId;
}

std::vector<ObjectId> Editor::selection() const {
    std::vector<ObjectId> live;  // undo or delete may have removed some
    for (ObjectId id : selection_)
        if (scene_.find(id)) live.push_back(id);
    return live;
}

ObjectId Editor::pick(float x, float y, int width, int height, bool extend) {
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    const ObjectId hit = pickObject(scene_, rayThroughPixel(view, projection, x, y, width, height), time_);
    if (!extend) select(hit);
    else if (hit != kNoId) toggleSelect(hit);  // shift-clicking empty space keeps the selection
    return hit;
}

void Editor::boxSelect(float x0, float y0, float x1, float y1, int width, int height, bool extend) {
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    const Mat4 viewProj = projection * view;
    const float left = std::min(x0, x1), right = std::max(x0, x1), top = std::min(y0, y1), bottom = std::max(y0, y1);
    // A world point's pixel, in the mouse's convention (pixel centers at +0.5); false behind the camera.
    auto toScreen = [&](const Vec3& p, float& x, float& y) {
        const Vec4 c = viewProj * Vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * width - 0.5f;
        y = (0.5f - c.y / c.w * 0.5f) * height - 0.5f;
        return true;
    };
    std::vector<ObjectId> hits;
    for (const Object& o : scene_.objects()) {
        if (!o.visible) continue;
        const Mat4 model = worldMatrix(scene_, o, time_);
        // The screen rectangle around the object: its mesh's 8 bounding-box corners,
        // or for lights and cameras just their origin.
        float rl = FLT_MAX, rr = -FLT_MAX, rt = FLT_MAX, rb = -FLT_MAX;
        auto include = [&](const Vec3& p) {
            float x, y;
            if (!toScreen((model * Vec4(p, 1.0f)).xyz(), x, y)) return;
            rl = std::min(rl, x), rr = std::max(rr, x), rt = std::min(rt, y), rb = std::max(rb, y);
        };
        const MeshEntry* mesh = o.type == ObjectType::Mesh ? scene_.mesh(o.mesh) : nullptr;
        if (mesh && !mesh->edit->verts().empty()) {
            Vec3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (const auto& v : mesh->edit->verts()) {
                lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
                hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
            }
            for (int i = 0; i < 8; ++i) include({i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z});
        } else {
            include({0, 0, 0});
        }
        if (rl <= right && rr >= left && rt <= bottom && rb >= top) hits.push_back(o.id);  // rectangles overlap
    }
    std::vector<ObjectId> ids = extend ? selection() : std::vector<ObjectId>{};
    for (ObjectId id : hits)
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    // Keep the active object if it's still selected; otherwise the first one boxed.
    const bool keep = std::find(ids.begin(), ids.end(), selected()) != ids.end() && selected() != kNoId;
    setSelection(ids, keep ? selected() : hits.empty() ? kNoId : hits.front());
}

bool Editor::groundPoint(float x, float y, int width, int height, Vec3& out) const {
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    const Ray ray = rayThroughPixel(view, projection, x, y, width, height);
    // origin + t * direction has y = 0 at t = -origin.y / direction.y; only ahead of
    // the camera (t > 0), and not so close to the horizon that it lands miles away.
    if (std::fabs(ray.direction.y) < 1e-6f) return false;
    const float t = -ray.origin.y / ray.direction.y;
    if (t <= 0 || t > 100) return false;
    out = ray.origin + ray.direction * t;
    return true;
}

void Editor::setSelectBox(bool shown, float x0, float y0, float x1, float y1) {
    boxShown_ = shown;
    box_[0] = x0, box_[1] = y0, box_[2] = x1, box_[3] = y1;
}

std::vector<ObjectId> Editor::transformRoots() const {
    const std::vector<ObjectId> ids = selection();
    std::vector<ObjectId> roots;
    for (ObjectId id : ids) {
        bool ancestorSelected = false;
        for (ObjectId other : ids) ancestorSelected = ancestorSelected || isAncestor(scene_, other, id);
        if (!ancestorSelected) roots.push_back(id);
    }
    return roots;
}

Vec3 Editor::selectionPivot() const {
    const std::vector<ObjectId> roots = transformRoots();
    Vec3 sum{0, 0, 0};
    for (ObjectId id : roots) sum = sum + worldTransform(scene_, *scene_.find(id), time_).position;
    return roots.empty() ? sum : sum * (1.0f / static_cast<float>(roots.size()));
}

bool Editor::deleteSelected() {
    if (transforming()) confirmTransform();
    std::vector<ObjectId> doomed = selection();
    if (doomed.empty()) return false;
    auto isDoomed = [&](ObjectId id) { return std::find(doomed.begin(), doomed.end(), id) != doomed.end(); };
    const std::string label = doomed.size() == 1 ? "Delete " + scene_.find(doomed[0])->name
                                                 : "Delete " + std::to_string(doomed.size()) + " objects";
    stack_.beginGroup(label);
    bool ok = true;
    Json result;
    std::string error;
    // Children that stay move up to their nearest surviving ancestor, in place.
    std::vector<std::pair<ObjectId, ObjectId>> moves;
    for (const Object& o : scene_.objects()) {
        if (isDoomed(o.id) || !isDoomed(o.parent)) continue;
        ObjectId up = o.parent;
        while (up != kNoId && isDoomed(up)) up = scene_.find(up)->parent;
        moves.push_back({o.id, up});
    }
    for (const auto& [id, up] : moves) {
        Json c = Json::object();
        c.set("op", "setParent");
        c.set("id", Json(id));
        c.set("parent", Json(up));
        ok = stack_.execute(c, result, error) && ok;
    }
    // Deepest first, so nothing is removed while it still has children.
    auto depth = [&](ObjectId id) {
        int d = 0;
        for (const Object* o = scene_.find(id); o && o->parent != kNoId; o = scene_.find(o->parent)) ++d;
        return d;
    };
    std::stable_sort(doomed.begin(), doomed.end(), [&](ObjectId a, ObjectId b) { return depth(a) > depth(b); });
    for (ObjectId id : doomed) {
        Json c = Json::object();
        c.set("op", "removeObject");
        c.set("id", Json(id));
        ok = stack_.execute(c, result, error) && ok;
    }
    stack_.endGroup();
    select(kNoId);
    return ok;
}

int Editor::parentSelection() {
    if (transforming()) confirmTransform();
    const ObjectId parent = selected();
    const Object* p = scene_.find(parent);
    if (!p) return 0;
    int changed = 0;
    stack_.beginGroup("Parent to " + p->name);
    for (ObjectId id : selection()) {
        std::string error;
        if (id == parent || scene_.find(id)->parent == parent || !validParent(scene_, id, parent, error)) continue;
        Json c = Json::object(), result;
        c.set("op", "setParent");
        c.set("id", Json(id));
        c.set("parent", Json(parent));
        if (stack_.execute(c, result, error)) ++changed;
    }
    stack_.endGroup();
    return changed;
}

int Editor::clearParents() {
    if (transforming()) confirmTransform();
    int changed = 0;
    stack_.beginGroup("Clear parent");
    for (ObjectId id : selection()) {
        if (scene_.find(id)->parent == kNoId) continue;
        Json c = Json::object(), result;
        std::string error;
        c.set("op", "setParent");
        c.set("id", Json(id));
        c.set("parent", Json(kNoId));
        if (stack_.execute(c, result, error)) ++changed;
    }
    stack_.endGroup();
    return changed;
}

std::string Editor::save() const { return scene_.toJson().dump(2); }

void Editor::replaceScene(Scene scene) {
    tool_.end();
    scene_ = std::move(scene);  // stack_ refers to scene_, which stays the same object
    stack_.reset();
    selection_.clear();
    selected_ = kNoId;
    time_ = scene_.animation().startFrame / scene_.animation().fps;
    playing_ = false;
    ++uiChanges_;
}

bool Editor::load(const std::string& text, std::string& error) {
    Json j;
    Scene loaded;
    if (!Json::parse(text, j, error) || !Scene::fromJson(j, loaded, error)) return false;
    replaceScene(std::move(loaded));
    return true;
}

void Editor::newScene() { replaceScene(Scene::makeDefault()); }

namespace {

bool runAll(CommandStack& stack, const std::vector<std::string>& commands, std::string& error) {
    for (const std::string& text : commands) {
        Json c, r;
        if (!Json::parse(text, c, error) || !stack.execute(c, r, error)) return false;
    }
    return true;
}

}  // namespace

bool Editor::loadExample(const std::string& name, std::string& error) {
    if (name == "bounce") {
        // A ball bouncing on a floor, looping once a second at 24 fps. Falling
        // speeds up (ease in) and rising slows down (ease out), like gravity; the
        // ball squashes for the frame it touches the floor.
        Scene s = Scene::makeDefault();
        CommandStack setup(s);
        auto key = [](const char* property, int frame, const std::string& value, const char* easing = "linear") {
            return R"({"op":"addKeyframe","id":4,"property":")" + std::string(property) + R"(","time":)" +
                   std::to_string(frame / 24.0) + R"(,"value":)" + value + R"(,"easing":")" + easing + R"("})";
        };
        if (!runAll(setup,
                    {
                        R"({"op":"removeObject","id":1})",  // the default cube
                        R"({"op":"addObject","type":"mesh","name":"Ball","primitive":{"type":"sphere"},)"
                        R"("transform":{"position":[0,2.5,0],"scale":[0.5,0.5,0.5]},"material":{"color":[1,0.45,0.15]}})",
                        R"({"op":"addObject","type":"mesh","name":"Floor","primitive":{"type":"plane"},)"
                        R"("transform":{"scale":[3,1,3]},"material":{"checker":true}})",
                        R"({"op":"setTransform","id":3,"position":[0,1.6,6.5],"rotation":[-0.07,0,0,0.9975]})",
                        R"({"op":"setAnimation","fps":24,"start":0,"end":24})",
                        key("position", 0, "[0,2.5,0]", "easeIn"),
                        key("position", 12, "[0,0.4,0]", "easeOut"),
                        key("position", 24, "[0,2.5,0]"),
                        key("scale", 11, "[0.5,0.5,0.5]"),
                        key("scale", 12, "[0.6,0.4,0.6]"),
                        key("scale", 13, "[0.5,0.5,0.5]"),
                    },
                    error))
            return false;
        replaceScene(std::move(s));
        orbit_.target = {0, 1.2f, 0};  // frame the whole bounce
        orbit_.distance = 7.5f;
        select(4);
        setPlaying(true);
        return true;
    }
    if (name != "liquid") return error = "unknown example '" + name + "'", false;
    // The liquid demo as ordinary scene objects: a glass tube, and a slightly
    // smaller tube inside it, parented to it, drawn by the liquid shader. Move the
    // glass and the liquid comes along and sloshes (SceneRenderer feeds each liquid
    // object's world motion to its spring).
    Scene s = Scene::makeDefault();
    CommandStack setup(s);
    Json r;
    const MeshId glassMesh = s.addMesh(EditMesh::fromTriangles(makeTube(0.30f, 1.0f, 64, 12)));
    const MeshId liquidMesh = s.addMesh(EditMesh::fromTriangles(makeTube(0.27f, 0.95f, 64, 12)));
    const std::string commands[] = {
        R"({"op":"removeObject","id":1})",  // the default cube
        R"({"op":"addObject","type":"mesh","name":"Liquid","mesh":)" + std::to_string(liquidMesh) +
            R"(,"transform":{"position":[0,0.03,0]},"shader":{"type":"liquid","params":{"fillLevel":0.3}}})",
        R"({"op":"addObject","type":"mesh","name":"Glass","mesh":)" + std::to_string(glassMesh) +
            R"(,"shader":{"type":"glass"}})",
        R"({"op":"setParent","id":4,"parent":5})",  // the liquid rides in the glass
        R"({"op":"setTransform","id":3,"position":[0,0.6,3.6],"rotation":[-0.08,0,0,0.997]})",
    };
    for (const std::string& text : commands) {
        Json c;
        if (!Json::parse(text, c, error) || !setup.execute(c, r, error)) return false;
    }
    replaceScene(std::move(s));
    select(5);  // the glass: move it and the liquid follows, sloshing
    return true;
}

Json Editor::importModel(const std::string& path) {
    Json out = Json::object();
    Mesh mesh;
    std::string error;
    if (!loadModel(path, mesh, error) || mesh.vertices.empty()) {
        out.set("ok", false);
        out.set("error", error.empty() ? "the model has no triangles" : error);
        return out;
    }
    // Fit it into view like before: center it and scale its largest side to 2.
    Vec3 lo, hi;
    mesh.bounds(lo, hi);
    const Vec3 size = hi - lo;
    const float extent = std::max(size.x, std::max(size.y, size.z));
    const float s = extent > 0 ? 2.0f / extent : 1.0f;
    const Vec3 center = (lo + hi) * 0.5f;
    const MeshId id = scene_.addMesh(EditMesh::fromTriangles(mesh));
    const size_t slash = path.find_last_of('/');
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    name = name.substr(0, name.find_last_of('.'));
    if (name.empty()) name = "Model";
    Json cmd = Json::object();
    cmd.set("op", "addObject");
    cmd.set("type", "mesh");
    cmd.set("name", name.substr(0, 128));
    cmd.set("mesh", Json(id));
    Json t = Json::object();
    t.set("position", vec3ToJson(-center * s));
    t.set("scale", vec3ToJson({s, s, s}));
    cmd.set("transform", t);
    out = command(cmd);
    if (out.find("ok")->asBool()) {
        select(static_cast<ObjectId>(out.find("result")->find("id")->asNumber()));
        out.set("triangles", Json(static_cast<int64_t>(mesh.triangleCount())));
    }
    return out;
}

bool Editor::beginTransform(TransformMode mode, float x, float y, int width, int height) {
    const std::vector<ObjectId> roots = transformRoots();
    if (roots.empty() || transforming() || mode == TransformMode::None) return false;
    setPlaying(false);  // edits land on the current frame, so hold it still
    targets_.clear();
    for (ObjectId id : roots) targets_.push_back({id, worldTransform(scene_, *scene_.find(id), time_)});
    // The tool runs on a pivot at the selection's center (no rotation, unit scale),
    // so its result *is* the change: an offset, a turn, and per-axis factors.
    Transform pivot;
    pivot.position = selectionPivot();
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    tool_.begin(mode, pivot, view, projection, width, height, x, y);
    const char* verb = mode == TransformMode::Grab ? "Move " : mode == TransformMode::Rotate ? "Rotate " : "Scale ";
    transformLabel_ = verb + (roots.size() == 1 ? scene_.find(roots[0])->name : std::to_string(roots.size()) + " objects");
    stack_.beginGroup(transformLabel_);
    ++uiChanges_;
    return true;
}

// Every update first reverts the previous one, so the open group only ever holds
// one setTransform per object: from where it started to where it is now.
void Editor::applyTransform() {
    stack_.cancelGroup();
    stack_.beginGroup(transformLabel_);
    const Transform& pivot = tool_.start();
    const Transform& change = tool_.result();
    const Vec3 offset = change.position - pivot.position;
    for (const Target& t : targets_) {
        const Object* o = scene_.find(t.id);
        if (!o) continue;
        // Scale, then turn, its offset from the pivot; then move. Its own rotation
        // turns and its own scale scales the same way (along world axes).
        const Vec3 rel = t.start.position - pivot.position;
        const Vec3& f = change.scale;
        Transform w;
        w.position = pivot.position + offset + change.rotation.rotate({rel.x * f.x, rel.y * f.y, rel.z * f.z});
        w.rotation = normalize(change.rotation * t.start.rotation);
        w.scale = {t.start.scale.x * f.x, t.start.scale.y * f.y, t.start.scale.z * f.z};
        // The tool works in world space; objects store transforms relative to their parent.
        const Transform local = localFromWorld(scene_, o->parent, w, time_);
        Json cmd = Json::object();
        cmd.set("op", "setTransform");
        cmd.set("id", Json(t.id));
        cmd.set("position", vec3ToJson(local.position));
        cmd.set("rotation", quatToJson(local.rotation));
        cmd.set("scale", vec3ToJson(local.scale));
        Json result;
        std::string error;
        executeEdit(cmd, result, error);  // out of range (beyond +-1e6): stays where it started
    }
    ++uiChanges_;  // the status text changes even when the transform can't
}

void Editor::transformMouse(float x, float y) {
    if (!transforming()) return;
    tool_.setMouse(x, y);
    applyTransform();
}

bool Editor::transformKey(const std::string& key) {
    if (!transforming()) return false;
    if (key == "Enter") return confirmTransform(), true;
    if (key == "Escape") return cancelTransform(), true;
    const char c = key == "Backspace" ? '\b'
                   : key.size() == 1  ? static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])))
                                      : 0;
    if (c == 'x' || c == 'y' || c == 'z') tool_.toggleAxis(c - 'x');
    else if (!tool_.typeKey(c)) return false;
    applyTransform();
    return true;
}

void Editor::confirmTransform() {
    if (!transforming()) return;
    const Transform &a = tool_.start(), &b = tool_.result();
    auto same = [](const Vec3& u, const Vec3& v) { return u.x == v.x && u.y == v.y && u.z == v.z; };
    const bool moved = !same(a.position, b.position) || !same(a.scale, b.scale) || a.rotation.x != b.rotation.x ||
                       a.rotation.y != b.rotation.y || a.rotation.z != b.rotation.z || a.rotation.w != b.rotation.w;
    if (moved) stack_.endGroup();
    else stack_.cancelGroup();  // confirming without a change shouldn't add an undo step that does nothing
    tool_.end();
    gizmoDrag_ = false;
    ++uiChanges_;
}

void Editor::cancelTransform() {
    if (!transforming()) return;
    stack_.cancelGroup();
    tool_.end();
    gizmoDrag_ = false;
    ++uiChanges_;
}

int Editor::gizmoAxisAt(float x, float y, int width, int height) const {
    if (selection().empty() || viewThroughCamera_) return -1;  // the camera view is the final shot: no overlays
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    return MoveGizmo::at(selectionPivot(), view, projection, width, height).axisAt(x, y);
}

void Editor::hoverGizmo(float x, float y, int width, int height) {
    gizmoHover_ = transforming() ? -1 : gizmoAxisAt(x, y, width, height);
}

bool Editor::beginGizmoDrag(float x, float y, int width, int height) {
    if (transforming()) return false;
    const int axis = gizmoAxisAt(x, y, width, height);
    if (axis < 0 || !beginTransform(TransformMode::Grab, x, y, width, height)) return false;
    tool_.toggleAxis(axis);  // the mouse hasn't moved yet, so nothing changes until it does
    gizmoDrag_ = true;
    gizmoHover_ = -1;
    return true;
}

void Editor::endGizmoDrag() {
    if (gizmoDrag_) confirmTransform();
}

void Editor::setViewThroughCamera(bool on) {
    if (on != viewThroughCamera_) {
        viewThroughCamera_ = on;
        ++uiChanges_;
    }
}

void Editor::viewMatrices(float aspect, Mat4& view, Mat4& projection) const {
    if (viewThroughCamera_ && SceneRenderer::cameraView(scene_, scene_.activeCamera(), time_, aspect, view, projection))
        return;
    view = orbit_.view();
    projection = orbit_.projection(aspect);
}

void Editor::render(Renderer& renderer, Framebuffer& fb, float dt) {
    if (playing_) {  // over the frame range: wrap at the end, or stop there
        const AnimationSettings& a = scene_.animation();
        const float start = a.startFrame / a.fps, end = a.endFrame / a.fps;
        float t = time_ + dt;
        if (t >= end && loop_) t = start + std::fmod(t - start, end - start);
        if (t >= end && !loop_) t = end, setPlaying(false);
        setTime(t);
    }
    sceneRenderer_.update(scene_, time_, dt);
    Mat4 view, projection;
    viewMatrices(static_cast<float>(fb.width()) / fb.height(), view, projection);
    SceneRenderer::Overlays overlays;
    overlays.selection = selection();  // undo may have removed some
    overlays.active = selected();
    overlays.grid = overlays.gizmos = !viewThroughCamera_;  // the camera view is the final shot
    // The arrows hide while G/R/S runs from the keyboard (like Blender), but stay
    // up, with the dragged arrow lit, while they're being dragged.
    overlays.moveGizmo = !overlays.selection.empty() && !viewThroughCamera_ && (!transforming() || gizmoDrag_);
    if (overlays.moveGizmo) overlays.gizmoPosition = selectionPivot();
    overlays.gizmoHighlight = gizmoDrag_ ? tool_.axis() : gizmoHover_;
    overlays.box = boxShown_;
    std::copy(box_, box_ + 4, overlays.boxCorners);
    sceneRenderer_.render(renderer, fb, scene_, time_, view, projection, overlays);
}

namespace {

SceneRenderer::Overlays noOverlays() {
    SceneRenderer::Overlays none;
    none.grid = none.gizmos = false;
    return none;
}

}  // namespace

void Editor::renderStill(Renderer& renderer, Framebuffer& fb) {
    const float aspect = static_cast<float>(fb.width()) / fb.height();
    Mat4 view, projection;
    if (!SceneRenderer::cameraView(scene_, scene_.activeCamera(), time_, aspect, view, projection)) {
        view = orbit_.view();
        projection = orbit_.projection(aspect);
    }
    sceneRenderer_.render(renderer, fb, scene_, time_, view, projection, noOverlays());
}

bool Editor::beginExport(std::string& error) {
    const Object* camera = scene_.find(scene_.activeCamera());
    if (!camera || camera->type != ObjectType::Camera)
        return error = "there's no active camera to render through: add one (Add > Camera)", false;
    exportRenderer_ = SceneRenderer();  // the slosh starts from rest
    return true;
}

void Editor::renderExportFrame(Renderer& renderer, Framebuffer& fb, float time, float dt) {
    exportRenderer_.update(scene_, time, dt);
    Mat4 view, projection;
    SceneRenderer::cameraView(scene_, scene_.activeCamera(), time, static_cast<float>(fb.width()) / fb.height(), view,
                              projection);
    exportRenderer_.render(renderer, fb, scene_, time, view, projection, noOverlays());
}

void Editor::setTime(float seconds) {
    seconds = clampf(seconds, 0.0f, 3600.0f);  // keys live in 0..3600 s
    if (seconds != time_) {
        time_ = seconds;
        ++uiChanges_;  // the panels show values at the playhead
    }
}

void Editor::setPlaying(bool on) {
    if (on) {
        const AnimationSettings& a = scene_.animation();
        if (frame() < a.startFrame || frame() >= a.endFrame) setFrame(static_cast<float>(a.startFrame));
    }
    if (on != playing_) {
        playing_ = on;
        ++uiChanges_;
    }
}

void Editor::setLoop(bool on) {
    if (on != loop_) loop_ = on, ++uiChanges_;
}

void Editor::setAutoKey(bool on) {
    if (on != autoKey_) autoKey_ = on, ++uiChanges_;
}

float Editor::keyTime() const {
    const float fps = scene_.animation().fps;
    return std::round(time_ * fps) / fps;
}

bool Editor::keyProperty(const Object& posed, TrackProperty property, std::string& error) {
    if (!trackPropertyAllowed(property, posed.type)) return error = "that property can't be animated here", false;
    float v[4] = {0, 0, 0, 0};
    readProperty(posed, property, v);
    Json value = Json::array();
    for (int i = 0; i < trackPropertyWidth(property); ++i) value.push(Json::fromFloat(v[i]));
    Json cmd = Json::object();
    cmd.set("op", "addKeyframe");
    cmd.set("id", Json(posed.id));
    cmd.set("property", trackPropertyName(property));
    cmd.set("time", Json::fromFloat(keyTime()));
    cmd.set("value", value);
    // Re-keying a frame keeps that key's easing (addKeyframe would reset it).
    for (const Track& t : posed.tracks)
        if (t.property == property)
            for (const Keyframe& k : t.keys)
                if (std::fabs(k.time - keyTime()) < 1e-4f) cmd.set("easing", easingName(k.easing));
    Json result;
    return stack_.execute(cmd, result, error);
}

bool Editor::insertKeyframes() {
    const std::vector<ObjectId> ids = selection();
    if (ids.empty()) return false;
    std::string error;
    stack_.beginGroup("Insert keyframes on " +
                      (ids.size() == 1 ? scene_.find(ids[0])->name : std::to_string(ids.size()) + " objects"));
    bool ok = true;
    for (ObjectId id : ids) {
        const Object posed = animatedObject(*scene_.find(id), time_);
        for (TrackProperty p : {TrackProperty::Position, TrackProperty::Rotation, TrackProperty::Scale})
            ok = keyProperty(posed, p, error) && ok;
    }
    stack_.endGroup();
    return ok;
}

bool Editor::insertKeyframe(const std::string& property) {
    const Object* o = scene_.find(selected());
    TrackProperty p;
    if (!o || !trackPropertyFromName(property, p)) return false;
    std::string error;
    return keyProperty(animatedObject(*o, time_), p, error);
}

bool Editor::jumpToKeyframe(int direction) {
    const float now = keyTime();
    float best = direction > 0 ? 1e30f : -1e30f;
    const std::vector<ObjectId> ids = selection();
    for (const Object& o : scene_.objects()) {
        if (!ids.empty() && std::find(ids.begin(), ids.end(), o.id) == ids.end()) continue;
        for (const Track& t : o.tracks)
            for (const Keyframe& k : t.keys)
                if (direction > 0 ? (k.time > now + 1e-4f && k.time < best) : (k.time < now - 1e-4f && k.time > best))
                    best = k.time;
    }
    if (std::fabs(best) > 1e29f) return false;
    setTime(best);
    return true;
}

bool Editor::executeEdit(const Json& cmd, Json& result, std::string& error) {
    // Which fields of which edits are animatable properties.
    struct Field {
        const char* op;
        const char* field;
        TrackProperty property;
    };
    static const Field kFields[] = {
        {"setTransform", "position", TrackProperty::Position}, {"setTransform", "rotation", TrackProperty::Rotation},
        {"setTransform", "rotationEuler", TrackProperty::Rotation}, {"setTransform", "scale", TrackProperty::Scale},
        {"setMaterial", "color", TrackProperty::Color},           {"setLight", "color", TrackProperty::Color},
        {"setLight", "intensity", TrackProperty::LightIntensity}, {"setCamera", "fov", TrackProperty::CameraFov},
    };
    const Json* op = cmd.find("op");
    const Json* id = cmd.find("id");
    const Object* o = id && id->isNumber() && id->asNumber() >= 0 && id->asNumber() < 4e9
                          ? scene_.find(static_cast<ObjectId>(id->asNumber()))
                          : nullptr;
    if (!o || (o->tracks.empty() && !autoKey_) || !op || !op->isString()) return stack_.execute(cmd, result, error);

    // Split the edit: animated fields become keys, the rest stays a plain edit.
    Json rest = Json::object();
    std::vector<TrackProperty> keyed;
    for (const auto& [key, value] : cmd.members()) {
        const Field* match = nullptr;
        for (const Field& f : kFields)
            if (op->asString() == f.op && key == f.field) match = &f;
        // Keyed: it already has a track, or auto-key is on (and the object can have one).
        const bool animated = match && trackPropertyAllowed(match->property, o->type) &&
                              (autoKey_ || std::any_of(o->tracks.begin(), o->tracks.end(),
                                                       [&](const Track& t) { return t.property == match->property; }));
        if (animated) keyed.push_back(match->property);
        else rest.set(key, value);
    }
    if (keyed.empty()) return stack_.execute(cmd, result, error);

    // Validate the whole edit, and get the new values in stored form (a rotation
    // given as Euler angles becomes a quaternion), by applying it to a copy.
    Scene scratch = scene_;
    Json forward, inverse;
    std::string label;
    if (!applyCommand(scratch, cmd, forward, inverse, label, result, error)) return false;
    Object edited = *scratch.find(o->id);
    edited.tracks = o->tracks;

    stack_.beginGroup(label);
    bool ok = rest.size() <= 2 || stack_.execute(rest, result, error);  // more than op and id: run it
    for (TrackProperty p : keyed) ok = ok && keyProperty(edited, p, error);
    stack_.endGroup();
    return ok;
}

Json Editor::state() const {
    Json s = Json::object();
    s.set("revision", Json(static_cast<int64_t>(uiRevision())));
    s.set("canUndo", stack_.canUndo());
    s.set("undoLabel", stack_.undoLabel());
    s.set("canRedo", stack_.canRedo());
    s.set("redoLabel", stack_.redoLabel());
    s.set("selected", Json(selected()));
    Json ids = Json::array();
    for (ObjectId id : selection()) ids.push(Json(id));
    s.set("selection", ids);
    s.set("activeCamera", Json(scene_.activeCamera()));
    s.set("viewThroughCamera", viewThroughCamera_);
    if (transforming()) s.set("transform", transformStatus());
    s.set("time", Json::fromFloat(time_));
    s.set("frame", Json::fromFloat(frame()));
    s.set("playing", playing_);
    s.set("loop", loop_);
    s.set("autoKey", autoKey_);
    s.set("animation", animationToJson(scene_.animation()));
    Json world = Json::object();
    world.set("ambient", Json::fromFloat(scene_.world().ambient));
    world.set("background", vec3ToJson(scene_.world().background));
    s.set("world", world);
    Json objects = Json::array();
    for (const Object& base : scene_.objects()) {
        const Object o = animatedObject(base, time_);  // the panels show values at the playhead
        Json j = objectToJson(o);
        j.set("rotationEuler", vec3ToJson(o.transform.rotation.toEulerXYZ()));
        if (const MeshEntry* m = o.type == ObjectType::Mesh ? scene_.mesh(o.mesh) : nullptr) {
            Json info = Json::object();
            info.set("triangles", Json(static_cast<int64_t>(m->render->triangleCount())));
            info.set("hasUVs", m->render->hasTexCoords);
            if (m->primitive) info.set("primitive", primitiveName(m->primitive->type));
            j.set("meshInfo", info);
        }
        objects.push(j);
    }
    s.set("objects", objects);
    return s;
}
