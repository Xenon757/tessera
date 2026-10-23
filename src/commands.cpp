#include "commands.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <initializer_list>

// The command implementations. SceneEditor is Scene's friend: the one place that
// touches a Scene's internals.
class SceneEditor {
public:
    static bool apply(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json& result,
                      std::string& error);

private:
    using Handler = bool (*)(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static Object* object(Scene& s, const Json& cmd, std::string& error, ObjectType* requireType = nullptr);
    static std::string uniqueName(const Scene& s, const std::string& base, ObjectId except);
    static bool addObject(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool removeObject(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setTransform(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setName(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setMaterial(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setShader(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setLight(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setCamera(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setActiveCamera(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setVisible(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool setWorld(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool addKeyframe(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool removeKeyframe(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
    static bool moveKeyframe(Scene&, const Json&, Json&, Json&, std::string&, Json&, std::string&);
};

namespace {

// Rejects fields a command doesn't know: a typo (or an AI's guess) should be an
// error, not silently ignored.
bool onlyFields(const Json& cmd, std::initializer_list<const char*> allowed, std::string& error) {
    for (const auto& member : cmd.members()) {
        bool known = member.first == "op";
        for (const char* a : allowed) known = known || member.first == a;
        if (!known) return error = "unknown field '" + member.first + "'", false;
    }
    return true;
}

Json op(const char* name) {
    Json j = Json::object();
    j.set("op", name);
    return j;
}

bool idOf(const Json& cmd, ObjectId& id, std::string& error) {
    const Json* j = cmd.find("id");
    if (!j || !j->isNumber() || j->asNumber() < 1 || j->asNumber() > 4e9 || j->asNumber() != std::floor(j->asNumber()))
        return error = "needs an object id", false;
    id = static_cast<ObjectId>(j->asNumber());
    return true;
}

Json transformToJson(const Transform& t) {
    Json j = Json::object();
    j.set("position", vec3ToJson(t.position));
    j.set("rotation", quatToJson(t.rotation));
    j.set("scale", vec3ToJson(t.scale));
    return j;
}

// Keyframe times match within a tenth of a millisecond: times arrive as JSON
// numbers and floats, so exact equality would be fragile.
constexpr float kTimeEpsilon = 1e-4f;

Track* findTrack(Object& o, TrackProperty p) {
    for (Track& t : o.tracks)
        if (t.property == p) return &t;
    return nullptr;
}

int findKey(const Track& t, float time) {
    for (size_t i = 0; i < t.keys.size(); ++i)
        if (std::fabs(t.keys[i].time - time) <= kTimeEpsilon) return static_cast<int>(i);
    return -1;
}

Json keyCommand(const char* name, ObjectId id, TrackProperty p, const Keyframe& k) {
    Json j = op(name);
    j.set("id", Json(id));
    j.set("property", trackPropertyName(p));
    j.set("time", Json::fromFloat(k.time));
    Json v = Json::array();
    for (int i = 0; i < trackPropertyWidth(p); ++i) v.push(Json::fromFloat(k.value[i]));
    j.set("value", v);
    j.set("easing", easingName(k.easing));
    return j;
}

}  // namespace

Object* SceneEditor::object(Scene& s, const Json& cmd, std::string& error, ObjectType* requireType) {
    ObjectId id;
    if (!idOf(cmd, id, error)) return nullptr;
    const int i = s.indexOf(id);
    if (i < 0) return error = "no object with id " + std::to_string(id), nullptr;
    Object& o = s.objects_[i];
    if (requireType && o.type != *requireType)
        return error = "object " + std::to_string(id) + " is a " + objectTypeName(o.type) + ", not a " + objectTypeName(*requireType), nullptr;
    return &o;
}

// Names stay unique, Blender-style: "Cube", then "Cube.001", "Cube.002", ...
std::string SceneEditor::uniqueName(const Scene& s, const std::string& base, ObjectId except) {
    auto taken = [&](const std::string& n) {
        for (const Object& o : s.objects_)
            if (o.id != except && o.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 1;; ++i) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), ".%03d", i);
        if (!taken(base + suffix)) return base + suffix;
    }
}

// addObject: {type, name?, primitive?: {type, segments?, rings?} | mesh?: meshId,
//             transform?, material?, shader?, light?, camera?, visible?, tracks?,
//             activeCamera?: bool, index?: outliner position, id?: (restores only)}
bool SceneEditor::addObject(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json& result,
                            std::string& error) {
    if (!onlyFields(cmd, {"type", "name", "primitive", "mesh", "transform", "material", "shader", "light", "camera",
                          "visible", "tracks", "activeCamera", "index", "id"},
                    error))
        return false;
    Object o;
    const Json* type = cmd.find("type");
    if (!type || !type->isString() || !objectTypeFromName(type->asString(), o.type))
        return error = "type must be mesh, light, or camera", false;
    // The ID: the next free one, or (when undo restores a deleted object) its old one.
    if (cmd.find("id")) {
        if (!idOf(cmd, o.id, error)) return false;
        if (o.id >= s.nextObjectId_ || s.find(o.id)) return error = "id " + std::to_string(o.id) + " isn't free", false;
    }
    bool makeActive = false;
    if (const Json* a = cmd.find("activeCamera")) {
        if (!a->isBool() || (a->asBool() && o.type != ObjectType::Camera)) return error = "activeCamera: true only for cameras", false;
        makeActive = a->asBool();
    }
    int index = static_cast<int>(s.objects_.size());
    if (const Json* i = cmd.find("index")) {
        if (!i->isNumber() || i->asNumber() < 0 || i->asNumber() > s.objects_.size() || i->asNumber() != std::floor(i->asNumber()))
            return error = "index out of range", false;
        index = static_cast<int>(i->asNumber());
    }

    // A primitive builds a new mesh. Storing a mesh isn't a scene change (nothing
    // uses it until the object is inserted below), so it's safe to build it before
    // the remaining fields are checked: if they fail, the unused mesh is never saved.
    std::string defaultName = o.type == ObjectType::Light ? "Light" : o.type == ObjectType::Camera ? "Camera" : "Mesh";
    if (const Json* p = cmd.find("primitive")) {
        if (o.type != ObjectType::Mesh || cmd.find("mesh")) return error = "a primitive is only for mesh objects without a mesh", false;
        Primitive prim;
        const Json* ptype = p->isObject() ? p->find("type") : nullptr;
        if (!ptype || !ptype->isString() || !primitiveFromName(ptype->asString(), prim.type))
            return error = "primitive type must be cube, sphere, cylinder, cone, or plane", false;
        if (const Json* n = p->find("segments")) {
            if (!n->isNumber() || n->asNumber() < 3 || n->asNumber() > 256) return error = "segments must be 3 to 256", false;
            prim.segments = static_cast<int>(n->asNumber());
        }
        if (const Json* n = p->find("rings")) {
            if (!n->isNumber() || n->asNumber() < 2 || n->asNumber() > 256) return error = "rings must be 2 to 256", false;
            prim.rings = static_cast<int>(n->asNumber());
        }
        o.mesh = s.addMesh(makePrimitive(prim), prim);
        defaultName = primitiveName(prim.type);
        defaultName[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(defaultName[0])));
    }
    // Everything else (name, transform, mesh ID, material, ...) through the same
    // validated reader the save format uses.
    if (!objectFieldsFromJson(cmd, s, o, error)) return false;

    // All checked: change the scene.
    if (o.id == kNoId) o.id = s.nextObjectId_++;
    o.name = uniqueName(s, cmd.find("name") ? o.name : defaultName, o.id);
    s.objects_.insert(s.objects_.begin() + index, o);
    const ObjectId previousCamera = s.activeCamera_;
    // A new camera in a scene without one becomes active, so the scene always renders
    // from somewhere. Only when the command doesn't say: undo's restores always do.
    const bool autoActive = !cmd.find("activeCamera") && o.type == ObjectType::Camera && s.activeCamera_ == kNoId;
    if (makeActive || autoActive) s.activeCamera_ = o.id;

    forward = objectToJson(o);  // canonical: the assigned ID and mesh, so redo recreates exactly this
    forward.set("op", "addObject");
    forward.set("index", Json(index));
    forward.set("activeCamera", s.activeCamera_ == o.id);
    inverse = op("removeObject");
    inverse.set("id", Json(o.id));
    if (s.activeCamera_ != previousCamera) inverse.set("activeCamera", Json(previousCamera));  // hand it back
    label = "Add " + o.name;
    result.set("id", Json(o.id));
    result.set("name", o.name);
    return true;
}

// removeObject: {id, activeCamera?}. Removing the active camera leaves none active,
// unless activeCamera names the camera that takes over (only allowed then: that keeps
// every case exactly undoable, and it's how undoing "add camera as active" restores
// the previous one).
bool SceneEditor::removeObject(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                               std::string& error) {
    if (!onlyFields(cmd, {"id", "activeCamera"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    const bool wasActive = s.activeCamera_ == o->id;
    ObjectId takeover = kNoId;
    if (const Json* a = cmd.find("activeCamera")) {
        if (!wasActive) return error = "activeCamera: only when removing the active camera", false;
        if (!a->isNumber() || a->asNumber() < 0 || a->asNumber() != std::floor(a->asNumber())) return error = "activeCamera: an id", false;
        takeover = static_cast<ObjectId>(a->asNumber());
        const Object* t = takeover == kNoId ? nullptr : s.find(takeover);
        if (takeover != kNoId && (!t || t->type != ObjectType::Camera || takeover == o->id))
            return error = "activeCamera: must be another camera, or 0", false;
    }
    const int index = s.indexOf(o->id);
    // The inverse re-adds the whole object, with the same id, at the same position.
    inverse = objectToJson(*o);
    inverse.set("op", "addObject");
    inverse.set("index", Json(index));
    inverse.set("activeCamera", wasActive);
    forward = op("removeObject");
    forward.set("id", Json(o->id));
    if (cmd.find("activeCamera")) forward.set("activeCamera", Json(takeover));
    label = "Delete " + o->name;
    if (wasActive) s.activeCamera_ = takeover;
    s.objects_.erase(s.objects_.begin() + index);
    return true;
}

// setTransform: {id, position?, rotation?: quaternion [x,y,z,w], rotationEuler?: degrees
//                XYZ, scale?}. Fields left out keep their value.
bool SceneEditor::setTransform(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                               std::string& error) {
    if (!onlyFields(cmd, {"id", "position", "rotation", "rotationEuler", "scale"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    Transform t = o->transform;
    if (const Json* p = cmd.find("position"))
        if (!vec3FromJson(p, t.position, error, "position")) return false;
    if (cmd.find("rotation") && cmd.find("rotationEuler")) return error = "give rotation or rotationEuler, not both", false;
    if (const Json* r = cmd.find("rotation"))
        if (!quatFromJson(r, t.rotation, error, "rotation")) return false;
    if (const Json* r = cmd.find("rotationEuler")) {
        Vec3 degrees;
        if (!vec3FromJson(r, degrees, error, "rotationEuler")) return false;
        t.rotation = normalize(Quat::fromEulerXYZ(degrees));
    }
    if (const Json* sc = cmd.find("scale"))
        if (!vec3FromJson(sc, t.scale, error, "scale")) return false;
    for (float v : {t.position.x, t.position.y, t.position.z, t.scale.x, t.scale.y, t.scale.z})
        if (std::fabs(v) > 1e6f) return error = "position and scale must stay within +-1e6", false;
    inverse = transformToJson(o->transform);
    inverse.set("op", "setTransform");
    inverse.set("id", Json(o->id));
    forward = transformToJson(t);
    forward.set("op", "setTransform");
    forward.set("id", Json(o->id));
    label = "Transform " + o->name;
    o->transform = t;
    return true;
}

// setName: {id, name}. A taken name gets a .001 suffix; the result has the final name.
bool SceneEditor::setName(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json& result,
                          std::string& error) {
    if (!onlyFields(cmd, {"id", "name"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    const Json* name = cmd.find("name");
    if (!name || !name->isString() || name->asString().empty() || name->asString().size() > 128)
        return error = "name must be 1 to 128 characters", false;
    const std::string final = uniqueName(s, name->asString(), o->id);
    inverse = op("setName");
    inverse.set("id", Json(o->id));
    inverse.set("name", o->name);
    forward = op("setName");
    forward.set("id", Json(o->id));
    forward.set("name", final);
    label = "Rename " + o->name;
    result.set("name", final);
    o->name = final;
    return true;
}

namespace {

// For the "set a few fields of one block" commands (material, light, camera):
// apply the given fields to a copy through the shared validated reader.
template <typename Get>
bool setBlock(Scene& s, Object& o, const Json& cmd, const char* block, std::initializer_list<const char*> fields,
              Json& forward, Json& inverse, std::string& error, Get get) {
    Json partial = Json::object();
    for (const char* f : fields)
        if (const Json* v = cmd.find(f)) partial.set(f, *v);
    Json wrapper = Json::object();
    wrapper.set(block, partial);
    Object copy = o;
    if (!objectFieldsFromJson(wrapper, s, copy, error)) return false;
    const Json before = objectToJson(o), after = objectToJson(copy);
    inverse = *before.find(block);
    inverse.set("op", cmd.find("op")->asString());
    inverse.set("id", Json(o.id));
    forward = *after.find(block);
    forward.set("op", cmd.find("op")->asString());
    forward.set("id", Json(o.id));
    get(o) = get(copy);
    return true;
}

}  // namespace

// setMaterial: {id, color?, specular?, shininess?, checker?}
bool SceneEditor::setMaterial(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                              std::string& error) {
    if (!onlyFields(cmd, {"id", "color", "specular", "shininess", "checker"}, error)) return false;
    ObjectType mesh = ObjectType::Mesh;
    Object* o = object(s, cmd, error, &mesh);
    if (!o) return false;
    label = "Material of " + o->name;
    return setBlock(s, *o, cmd, "material", {"color", "specular", "shininess", "checker"}, forward, inverse, error,
                    [](Object& x) -> Material& { return x.material; });
}

// setShader: {id, type: "" | "liquid" | "glass", params?}
bool SceneEditor::setShader(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                            std::string& error) {
    if (!onlyFields(cmd, {"id", "type", "params"}, error)) return false;
    ObjectType mesh = ObjectType::Mesh;
    Object* o = object(s, cmd, error, &mesh);
    if (!o) return false;
    const Json* type = cmd.find("type");
    if (!type || !type->isString()) return error = "needs a shader type (\"\" for none)", false;
    const Json* params = cmd.find("params");
    const Json p = params ? *params : Json::object();
    if (!validateShader(type->asString(), p, error)) return false;
    auto shaderCommand = [&](const ShaderRef& ref) {
        Json j = op("setShader");
        j.set("id", Json(o->id));
        j.set("type", ref.type);
        j.set("params", ref.params);
        return j;
    };
    inverse = shaderCommand(o->shader);
    o->shader = {type->asString(), p};
    forward = shaderCommand(o->shader);
    label = "Shader of " + o->name;
    return true;
}

// setLight: {id, color?, intensity?}
bool SceneEditor::setLight(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                           std::string& error) {
    if (!onlyFields(cmd, {"id", "color", "intensity"}, error)) return false;
    ObjectType light = ObjectType::Light;
    Object* o = object(s, cmd, error, &light);
    if (!o) return false;
    label = "Light " + o->name;
    return setBlock(s, *o, cmd, "light", {"color", "intensity"}, forward, inverse, error,
                    [](Object& x) -> LightData& { return x.light; });
}

// setCamera: {id, fov?, near?, far?}
bool SceneEditor::setCamera(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                            std::string& error) {
    if (!onlyFields(cmd, {"id", "fov", "near", "far"}, error)) return false;
    ObjectType camera = ObjectType::Camera;
    Object* o = object(s, cmd, error, &camera);
    if (!o) return false;
    label = "Camera " + o->name;
    return setBlock(s, *o, cmd, "camera", {"fov", "near", "far"}, forward, inverse, error,
                    [](Object& x) -> CameraData& { return x.camera; });
}

// setActiveCamera: {id} (0 for none). The scene renders and exports through it.
bool SceneEditor::setActiveCamera(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                                  std::string& error) {
    if (!onlyFields(cmd, {"id"}, error)) return false;
    const Json* id = cmd.find("id");
    ObjectId target = kNoId;
    if (id && id->isNumber() && id->asNumber() == 0) {
        target = kNoId;
    } else {
        ObjectType camera = ObjectType::Camera;
        Object* o = object(s, cmd, error, &camera);
        if (!o) return false;
        target = o->id;
    }
    inverse = op("setActiveCamera");
    inverse.set("id", Json(s.activeCamera_));
    forward = op("setActiveCamera");
    forward.set("id", Json(target));
    label = "Set active camera";
    s.activeCamera_ = target;
    return true;
}

// setVisible: {id, visible}
bool SceneEditor::setVisible(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                             std::string& error) {
    if (!onlyFields(cmd, {"id", "visible"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    const Json* v = cmd.find("visible");
    if (!v || !v->isBool()) return error = "visible must be true or false", false;
    inverse = op("setVisible");
    inverse.set("id", Json(o->id));
    inverse.set("visible", o->visible);
    forward = op("setVisible");
    forward.set("id", Json(o->id));
    forward.set("visible", v->asBool());
    label = (v->asBool() ? "Show " : "Hide ") + o->name;
    o->visible = v->asBool();
    return true;
}

// setWorld: {ambient?, background?}
bool SceneEditor::setWorld(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                           std::string& error) {
    if (!onlyFields(cmd, {"ambient", "background"}, error)) return false;
    World w = s.world_;
    if (const Json* a = cmd.find("ambient")) {
        if (!a->isNumber() || !(a->asNumber() >= 0 && a->asNumber() <= 1)) return error = "ambient must be between 0 and 1", false;
        w.ambient = static_cast<float>(a->asNumber());
    }
    if (const Json* b = cmd.find("background")) {
        Vec3 c;
        if (!vec3FromJson(b, c, error, "background")) return false;
        if (c.x < 0 || c.y < 0 || c.z < 0 || c.x > 1 || c.y > 1 || c.z > 1) return error = "background must be between 0 and 1", false;
        w.background = c;
    }
    auto worldCommand = [](const World& x) {
        Json j = op("setWorld");
        j.set("ambient", Json::fromFloat(x.ambient));
        j.set("background", vec3ToJson(x.background));
        return j;
    };
    inverse = worldCommand(s.world_);
    forward = worldCommand(w);
    label = "World settings";
    s.world_ = w;
    return true;
}

// addKeyframe: {id, property, time, value, easing?}. A key already at that time is replaced.
bool SceneEditor::addKeyframe(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                              std::string& error) {
    if (!onlyFields(cmd, {"id", "property", "time", "value", "easing"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    // Reuse the save format's validated reader on a one-key track.
    Json track = Json::object(), keys = Json::array(), key = Json::object();
    for (const char* f : {"time", "value", "easing"})
        if (const Json* v = cmd.find(f)) key.set(f, *v);
    keys.push(key);
    track.set("property", cmd.find("property") ? *cmd.find("property") : Json());
    track.set("keys", keys);
    Json tracks = Json::array();
    tracks.push(track);
    Json wrapper = Json::object();
    wrapper.set("tracks", tracks);
    Object parsed = *o;
    if (!objectFieldsFromJson(wrapper, s, parsed, error)) return false;
    const TrackProperty property = parsed.tracks[0].property;
    const Keyframe k = parsed.tracks[0].keys[0];

    Track* t = findTrack(*o, property);
    const int existing = t ? findKey(*t, k.time) : -1;
    if (existing >= 0) {  // replacing a key: undo puts the old one back
        inverse = keyCommand("addKeyframe", o->id, property, t->keys[existing]);
    } else {  // a new key: undo removes it
        inverse = op("removeKeyframe");
        inverse.set("id", Json(o->id));
        inverse.set("property", trackPropertyName(property));
        inverse.set("time", Json::fromFloat(k.time));
    }
    forward = keyCommand("addKeyframe", o->id, property, k);
    label = std::string("Keyframe ") + trackPropertyName(property);
    if (!t) {  // tracks stay sorted by property, so undo restores them exactly
        Track fresh;
        fresh.property = property;
        auto at = std::find_if(o->tracks.begin(), o->tracks.end(), [&](const Track& x) { return x.property > property; });
        t = &*o->tracks.insert(at, fresh);
    }
    if (existing >= 0) {
        t->keys[existing] = k;
    } else {
        auto at = std::find_if(t->keys.begin(), t->keys.end(), [&](const Keyframe& x) { return x.time > k.time; });
        t->keys.insert(at, k);
    }
    return true;
}

// removeKeyframe: {id, property, time}
bool SceneEditor::removeKeyframe(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                                 std::string& error) {
    if (!onlyFields(cmd, {"id", "property", "time"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    TrackProperty property;
    const Json* p = cmd.find("property");
    const Json* time = cmd.find("time");
    if (!p || !p->isString() || !trackPropertyFromName(p->asString(), property)) return error = "unknown property", false;
    if (!time || !time->isNumber()) return error = "needs a time", false;
    Track* t = findTrack(*o, property);
    const int k = t ? findKey(*t, static_cast<float>(time->asNumber())) : -1;
    if (k < 0) return error = "no keyframe there", false;
    inverse = keyCommand("addKeyframe", o->id, property, t->keys[k]);
    forward = op("removeKeyframe");
    forward.set("id", Json(o->id));
    forward.set("property", trackPropertyName(property));
    forward.set("time", Json::fromFloat(t->keys[k].time));
    label = std::string("Delete keyframe ") + trackPropertyName(property);
    t->keys.erase(t->keys.begin() + k);
    if (t->keys.empty()) o->tracks.erase(o->tracks.begin() + (t - o->tracks.data()));
    return true;
}

// moveKeyframe: {id, property, time, newTime}. Refuses to land on another key.
bool SceneEditor::moveKeyframe(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json&,
                               std::string& error) {
    if (!onlyFields(cmd, {"id", "property", "time", "newTime"}, error)) return false;
    Object* o = object(s, cmd, error);
    if (!o) return false;
    TrackProperty property;
    const Json* p = cmd.find("property");
    const Json* time = cmd.find("time");
    const Json* newTime = cmd.find("newTime");
    if (!p || !p->isString() || !trackPropertyFromName(p->asString(), property)) return error = "unknown property", false;
    if (!time || !time->isNumber() || !newTime || !newTime->isNumber()) return error = "needs time and newTime", false;
    const float to = static_cast<float>(newTime->asNumber());
    if (!(to >= 0 && to <= 3600)) return error = "newTime must be between 0 and 3600", false;
    Track* t = findTrack(*o, property);
    const int k = t ? findKey(*t, static_cast<float>(time->asNumber())) : -1;
    if (k < 0) return error = "no keyframe there", false;
    const int clash = findKey(*t, to);
    if (clash >= 0 && clash != k) return error = "another keyframe is already at that time", false;
    const float from = t->keys[k].time;
    auto moveCommand = [&](float a, float b) {
        Json j = op("moveKeyframe");
        j.set("id", Json(o->id));
        j.set("property", trackPropertyName(property));
        j.set("time", Json::fromFloat(a));
        j.set("newTime", Json::fromFloat(b));
        return j;
    };
    forward = moveCommand(from, to);
    inverse = moveCommand(to, from);
    label = std::string("Move keyframe ") + trackPropertyName(property);
    Keyframe key = t->keys[k];
    key.time = to;
    t->keys.erase(t->keys.begin() + k);
    auto at = std::find_if(t->keys.begin(), t->keys.end(), [&](const Keyframe& x) { return x.time > to; });
    t->keys.insert(at, key);
    return true;
}

bool SceneEditor::apply(Scene& s, const Json& cmd, Json& forward, Json& inverse, std::string& label, Json& result,
                        std::string& error) {
    static const std::pair<const char*, Handler> kHandlers[] = {
        {"addObject", addObject},       {"removeObject", removeObject},       {"setTransform", setTransform},
        {"setName", setName},           {"setMaterial", setMaterial},         {"setShader", setShader},
        {"setLight", setLight},         {"setCamera", setCamera},             {"setActiveCamera", setActiveCamera},
        {"setVisible", setVisible},     {"setWorld", setWorld},               {"addKeyframe", addKeyframe},
        {"removeKeyframe", removeKeyframe}, {"moveKeyframe", moveKeyframe},
    };
    if (!cmd.isObject()) return error = "a command must be a JSON object", false;
    const Json* name = cmd.find("op");
    if (!name || !name->isString()) return error = "a command needs an \"op\"", false;
    for (const auto& [opName, handler] : kHandlers)
        if (name->asString() == opName) {
            result = Json::object();
            return handler(s, cmd, forward, inverse, label, result, error);
        }
    return error = "unknown op '" + name->asString() + "'", false;
}

bool applyCommand(Scene& scene, const Json& command, Json& forward, Json& inverse, std::string& label, Json& result,
                  std::string& error) {
    return SceneEditor::apply(scene, command, forward, inverse, label, result, error);
}

// ---------------------------------------------------------------- CommandStack

bool CommandStack::execute(const Json& command, Json& result, std::string& error) {
    Json forward, inverse;
    std::string label;
    if (!applyCommand(scene_, command, forward, inverse, label, result, error)) return false;
    redo_.clear();  // a new change ends the redo branch
    if (depth_ > 0) {
        open_.steps.push_back({std::move(forward), std::move(inverse)});
    } else {
        push({label, {{std::move(forward), std::move(inverse)}}});
    }
    ++revision_;
    return true;
}

void CommandStack::push(Group group) {
    undo_.push_back(std::move(group));
    if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
}

void CommandStack::beginGroup(const std::string& label) {
    if (depth_++ == 0) open_ = {label, {}};
}

void CommandStack::endGroup() {
    if (depth_ == 0) return;
    if (--depth_ != 0 || open_.steps.empty()) return;
    push(std::move(open_));
    ++revision_;  // undo just became available, which the UI shows
}

void CommandStack::cancelGroup() {
    if (depth_ == 0) return;
    for (auto step = open_.steps.rbegin(); step != open_.steps.rend(); ++step) {
        Json f, i, r;
        std::string label, error;
        applyCommand(scene_, step->inverse, f, i, label, r, error);  // built from the scene as it was, so it applies
    }
    open_ = {};
    depth_ = 0;
    ++revision_;
}

bool CommandStack::undo() {
    if (!canUndo()) return false;
    Group g = std::move(undo_.back());
    undo_.pop_back();
    for (auto step = g.steps.rbegin(); step != g.steps.rend(); ++step) {
        Json f, i, r;
        std::string label, error;
        // An inverse is built from the scene as it was, so it always applies.
        if (!applyCommand(scene_, step->inverse, f, i, label, r, error))
            std::fprintf(stderr, "undo failed (a bug): %s\n", error.c_str());
    }
    redo_.push_back(std::move(g));
    ++revision_;
    return true;
}

bool CommandStack::redo() {
    if (!canRedo()) return false;
    Group g = std::move(redo_.back());
    redo_.pop_back();
    for (const Step& step : g.steps) {
        Json f, i, r;
        std::string label, error;
        if (!applyCommand(scene_, step.forward, f, i, label, r, error))
            std::fprintf(stderr, "redo failed (a bug): %s\n", error.c_str());
    }
    push(std::move(g));
    ++revision_;
    return true;
}

void CommandStack::reset() {
    undo_.clear();
    redo_.clear();
    open_ = {};
    depth_ = 0;
    ++revision_;
}
