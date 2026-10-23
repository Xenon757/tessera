#include "editor.h"

#include <cctype>
#include <cmath>

#include "animation.h"
#include "gizmo.h"
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

void Editor::select(ObjectId id) {
    if (id != kNoId && !scene_.find(id)) id = kNoId;
    if (id != selected_) {
        selected_ = id;
        ++uiChanges_;
    }
}

ObjectId Editor::pick(float x, float y, int width, int height) {
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    select(pickObject(scene_, rayThroughPixel(view, projection, x, y, width, height), time_));
    return selected_;
}

std::string Editor::save() const { return scene_.toJson().dump(2); }

void Editor::replaceScene(Scene scene) {
    tool_.end();
    scene_ = std::move(scene);  // stack_ refers to scene_, which stays the same object
    stack_.reset();
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
    // smaller tube inside it drawn by the liquid shader. Move the tube and the
    // liquid sloshes (SceneRenderer feeds each liquid object's motion to its spring).
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
        R"({"op":"setTransform","id":3,"position":[0,0.6,3.6],"rotation":[-0.08,0,0,0.997]})",
    };
    for (const std::string& text : commands) {
        Json c;
        if (!Json::parse(text, c, error) || !setup.execute(c, r, error)) return false;
    }
    replaceScene(std::move(s));
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
    const Object* o = scene_.find(selected_);
    if (!o || transforming() || mode == TransformMode::None) return false;
    setPlaying(false);  // edits land on the current frame, so hold it still
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    tool_.begin(mode, animatedObject(*o, time_).transform, view, projection, width, height, x, y);
    transformTarget_ = o->id;
    const char* verb = mode == TransformMode::Grab ? "Move " : mode == TransformMode::Rotate ? "Rotate " : "Scale ";
    transformLabel_ = verb + o->name;
    stack_.beginGroup(transformLabel_);
    ++uiChanges_;
    return true;
}

// Every update first reverts the previous one, so the open group only ever holds
// one setTransform: from where the object started to where it is now.
void Editor::applyTransform() {
    stack_.cancelGroup();
    stack_.beginGroup(transformLabel_);
    const Transform& t = tool_.result();
    Json cmd = Json::object();
    cmd.set("op", "setTransform");
    cmd.set("id", Json(transformTarget_));
    cmd.set("position", vec3ToJson(t.position));
    cmd.set("rotation", quatToJson(t.rotation));
    cmd.set("scale", vec3ToJson(t.scale));
    Json result;
    std::string error;
    executeEdit(cmd, result, error);  // out of range (beyond +-1e6): stays where it started
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
    const Object* o = scene_.find(selected_);
    if (!o || viewThroughCamera_) return -1;  // the camera view is the final shot: no overlays
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    return MoveGizmo::at(animatedObject(*o, time_).transform.position, view, projection, width, height).axisAt(x, y);
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
    if (playing_) {  // loop over the frame range
        const AnimationSettings& a = scene_.animation();
        const float start = a.startFrame / a.fps, end = a.endFrame / a.fps;
        float t = time_ + dt;
        if (t >= end) t = start + std::fmod(t - start, end - start);
        setTime(t);
    }
    sceneRenderer_.update(scene_, time_, dt);
    Mat4 view, projection;
    viewMatrices(static_cast<float>(fb.width()) / fb.height(), view, projection);
    SceneRenderer::Overlays overlays;
    overlays.selected = scene_.find(selected_) ? selected_ : kNoId;  // undo may have removed it
    overlays.grid = overlays.gizmos = !viewThroughCamera_;          // the camera view is the final shot
    // The arrows hide while G/R/S runs from the keyboard (like Blender), but stay
    // up, with the dragged arrow lit, while they're being dragged.
    overlays.moveGizmo = overlays.selected != kNoId && !viewThroughCamera_ && (!transforming() || gizmoDrag_);
    overlays.gizmoHighlight = gizmoDrag_ ? tool_.axis() : gizmoHover_;
    sceneRenderer_.render(renderer, fb, scene_, time_, view, projection, overlays);
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
    const Object* o = scene_.find(selected_);
    if (!o) return false;
    const Object posed = animatedObject(*o, time_);
    std::string error;
    stack_.beginGroup("Insert keyframes on " + o->name);
    bool ok = true;
    for (TrackProperty p : {TrackProperty::Position, TrackProperty::Rotation, TrackProperty::Scale})
        ok = keyProperty(posed, p, error) && ok;
    stack_.endGroup();
    return ok;
}

bool Editor::insertKeyframe(const std::string& property) {
    const Object* o = scene_.find(selected_);
    TrackProperty p;
    if (!o || !trackPropertyFromName(property, p)) return false;
    std::string error;
    return keyProperty(animatedObject(*o, time_), p, error);
}

bool Editor::jumpToKeyframe(int direction) {
    const float now = keyTime();
    float best = direction > 0 ? 1e30f : -1e30f;
    for (const Object& o : scene_.objects()) {
        if (scene_.find(selected_) && o.id != selected_) continue;
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
    if (!o || o->tracks.empty() || !op || !op->isString()) return stack_.execute(cmd, result, error);

    // Split the edit: animated fields become keys, the rest stays a plain edit.
    Json rest = Json::object();
    std::vector<TrackProperty> keyed;
    for (const auto& [key, value] : cmd.members()) {
        const Field* match = nullptr;
        for (const Field& f : kFields)
            if (op->asString() == f.op && key == f.field) match = &f;
        const bool animated = match && std::any_of(o->tracks.begin(), o->tracks.end(),
                                                   [&](const Track& t) { return t.property == match->property; });
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
    s.set("selected", Json(scene_.find(selected_) ? selected_ : kNoId));
    s.set("activeCamera", Json(scene_.activeCamera()));
    s.set("viewThroughCamera", viewThroughCamera_);
    if (transforming()) s.set("transform", transformStatus());
    s.set("time", Json::fromFloat(time_));
    s.set("frame", Json::fromFloat(frame()));
    s.set("playing", playing_);
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
