#include "scene.h"

#include <cmath>

#include "hierarchy.h"

// ---------------------------------------------------------------- Scene

const Object* Scene::find(ObjectId id) const {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &objects_[i];
}

int Scene::indexOf(ObjectId id) const {
    for (size_t i = 0; i < objects_.size(); ++i)
        if (objects_[i].id == id) return static_cast<int>(i);
    return -1;
}

const MeshEntry* Scene::mesh(MeshId id) const {
    for (const auto& m : meshes_)
        if (m.first == id) return &m.second;
    return nullptr;
}

MeshId Scene::addMesh(EditMesh mesh, std::optional<Primitive> primitive) {
    MeshEntry entry;
    entry.render = std::make_shared<const Mesh>(mesh.triangulate());
    entry.edit = std::make_shared<const EditMesh>(std::move(mesh));
    entry.primitive = primitive;
    const MeshId id = nextMeshId_++;
    meshes_.emplace_back(id, std::move(entry));
    return id;
}

Scene Scene::makeDefault() {
    Scene s;
    Object cube;
    cube.id = s.nextObjectId_++;
    cube.name = "Cube";
    const Primitive prim{PrimitiveType::Cube, 32, 16};
    cube.mesh = s.addMesh(makePrimitive(prim), prim);
    s.objects_.push_back(cube);

    Object sun;
    sun.id = s.nextObjectId_++;
    sun.name = "Light";
    sun.type = ObjectType::Light;
    sun.transform.position = {4, 6, 3};
    sun.transform.rotation = Quat::lookRotation(normalize(Vec3{-0.5f, -1.0f, -0.3f}));  // the old fixed light
    s.objects_.push_back(sun);

    Object camera;
    camera.id = s.nextObjectId_++;
    camera.name = "Camera";
    camera.type = ObjectType::Camera;
    camera.transform.position = {7.4f, 5.3f, 6.9f};
    camera.transform.rotation = Quat::lookRotation(-camera.transform.position);  // at the origin
    s.objects_.push_back(camera);
    s.activeCamera_ = camera.id;
    return s;
}

// ---------------------------------------------------------------- small JSON helpers

Json vec3ToJson(const Vec3& v) {
    Json a = Json::array();
    a.push(Json::fromFloat(v.x));
    a.push(Json::fromFloat(v.y));
    a.push(Json::fromFloat(v.z));
    return a;
}

Json quatToJson(const Quat& q) {
    Json a = Json::array();
    for (float f : {q.x, q.y, q.z, q.w}) a.push(Json::fromFloat(f));
    return a;
}

namespace {

bool floatsFromJson(const Json* j, float* out, size_t n, std::string& error, const char* what) {
    if (!j || !j->isArray() || j->size() != n) return error = std::string(what) + ": expected " + std::to_string(n) + " numbers", false;
    for (size_t i = 0; i < n; ++i) {
        if (!(*j)[i].isNumber()) return error = std::string(what) + ": expected numbers", false;
        const float v = static_cast<float>((*j)[i].asNumber());
        if (!std::isfinite(v)) return error = std::string(what) + ": number out of range", false;
        out[i] = v;
    }
    return true;
}

bool numberFromJson(const Json* j, float lo, float hi, float& out, std::string& error, const char* what) {
    if (!j || !j->isNumber()) return error = std::string(what) + ": expected a number", false;
    const double v = j->asNumber();
    if (!(v >= lo && v <= hi)) return error = std::string(what) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi), false;
    out = static_cast<float>(v);
    return true;
}

bool boolFromJson(const Json* j, bool& out, std::string& error, const char* what) {
    if (!j || !j->isBool()) return error = std::string(what) + ": expected true or false", false;
    out = j->asBool();
    return true;
}

bool colorFromJson(const Json* j, Vec3& out, std::string& error, const char* what) {
    Vec3 c;
    if (!vec3FromJson(j, c, error, what)) return false;
    if (c.x < 0 || c.y < 0 || c.z < 0 || c.x > 100 || c.y > 100 || c.z > 100)
        return error = std::string(what) + ": color components must be between 0 and 100", false;
    out = c;
    return true;
}

}  // namespace

bool vec3FromJson(const Json* j, Vec3& out, std::string& error, const char* what) {
    float f[3];
    if (!floatsFromJson(j, f, 3, error, what)) return false;
    out = {f[0], f[1], f[2]};
    return true;
}

bool quatFromJson(const Json* j, Quat& out, std::string& error, const char* what) {
    float f[4];
    if (!floatsFromJson(j, f, 4, error, what)) return false;
    const Quat q{f[0], f[1], f[2], f[3]};
    if (dot(q, q) < 1e-12f) return error = std::string(what) + ": a zero quaternion isn't a rotation", false;
    out = normalize(q);
    return true;
}

// ---------------------------------------------------------------- names

const char* objectTypeName(ObjectType type) {
    switch (type) {
        case ObjectType::Mesh: return "mesh";
        case ObjectType::Light: return "light";
        case ObjectType::Camera: return "camera";
    }
    return "mesh";
}

bool objectTypeFromName(const std::string& name, ObjectType& out) {
    for (ObjectType t : {ObjectType::Mesh, ObjectType::Light, ObjectType::Camera})
        if (name == objectTypeName(t)) return out = t, true;
    return false;
}

const char* trackPropertyName(TrackProperty p) {
    switch (p) {
        case TrackProperty::Position: return "position";
        case TrackProperty::Rotation: return "rotation";
        case TrackProperty::Scale: return "scale";
        case TrackProperty::Color: return "color";
        case TrackProperty::LightIntensity: return "intensity";
        case TrackProperty::CameraFov: return "fov";
    }
    return "position";
}

bool trackPropertyFromName(const std::string& name, TrackProperty& out) {
    for (TrackProperty p : {TrackProperty::Position, TrackProperty::Rotation, TrackProperty::Scale,
                            TrackProperty::Color, TrackProperty::LightIntensity, TrackProperty::CameraFov})
        if (name == trackPropertyName(p)) return out = p, true;
    return false;
}

int trackPropertyWidth(TrackProperty p) {
    switch (p) {
        case TrackProperty::Rotation: return 4;
        case TrackProperty::LightIntensity:
        case TrackProperty::CameraFov: return 1;
        default: return 3;
    }
}

bool trackPropertyAllowed(TrackProperty p, ObjectType type) {
    switch (p) {
        case TrackProperty::Color: return type != ObjectType::Camera;
        case TrackProperty::LightIntensity: return type == ObjectType::Light;
        case TrackProperty::CameraFov: return type == ObjectType::Camera;
        default: return true;
    }
}

const char* easingName(Easing e) {
    switch (e) {
        case Easing::Linear: return "linear";
        case Easing::EaseIn: return "easeIn";
        case Easing::EaseOut: return "easeOut";
        case Easing::EaseInOut: return "easeInOut";
        case Easing::Step: return "step";
    }
    return "linear";
}

bool easingFromName(const std::string& name, Easing& out) {
    for (Easing e : {Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut, Easing::Step})
        if (name == easingName(e)) return out = e, true;
    return false;
}

// ---------------------------------------------------------------- shaders

bool validateShader(const std::string& type, const Json& params, std::string& error) {
    if (!params.isObject()) return error = "shader params must be an object", false;
    if (type.empty()) return params.size() == 0 ? true : (error = "the fixed-function pipeline takes no params", false);
    float f;
    Vec3 c;
    if (type == "liquid") {
        for (const auto& [key, value] : params.members()) {
            if (key == "fillLevel") {
                if (!numberFromJson(&value, -10, 10, f, error, "fillLevel")) return false;
            } else if (key == "foamWidth") {
                if (!numberFromJson(&value, 0, 1, f, error, "foamWidth")) return false;
            } else if (key == "layers") {
                if (!value.isArray() || value.size() == 0 || value.size() > 16) return error = "layers: 1 to 16 colors", false;
                for (size_t i = 0; i < value.size(); ++i)
                    if (!colorFromJson(&value[i], c, error, "layer color")) return false;
            } else {
                return error = "unknown liquid setting '" + key + "'", false;
            }
        }
        return true;
    }
    if (type == "glass") {
        for (const auto& [key, value] : params.members()) {
            if (key == "tint") {
                if (!colorFromJson(&value, c, error, "tint")) return false;
            } else if (key == "rimStart") {
                if (!numberFromJson(&value, 0, 1, f, error, "rimStart")) return false;
            } else {
                return error = "unknown glass setting '" + key + "'", false;
            }
        }
        return true;
    }
    return error = "unknown shader type '" + type + "'", false;
}

// ---------------------------------------------------------------- objects

Json objectToJson(const Object& o) {
    Json j = Json::object();
    j.set("id", Json(o.id));
    j.set("name", o.name);
    j.set("type", objectTypeName(o.type));
    if (o.parent != kNoId) j.set("parent", Json(o.parent));
    j.set("visible", o.visible);
    Json t = Json::object();
    t.set("position", vec3ToJson(o.transform.position));
    t.set("rotation", quatToJson(o.transform.rotation));
    t.set("scale", vec3ToJson(o.transform.scale));
    j.set("transform", t);
    if (o.type == ObjectType::Mesh) {
        j.set("mesh", Json(o.mesh));
        Json m = Json::object();
        m.set("color", vec3ToJson(o.material.color));
        m.set("specular", Json::fromFloat(o.material.specular));
        m.set("shininess", Json::fromFloat(o.material.shininess));
        m.set("checker", o.material.checker);
        j.set("material", m);
        if (!o.shader.type.empty()) {
            Json s = Json::object();
            s.set("type", o.shader.type);
            s.set("params", o.shader.params);
            j.set("shader", s);
        }
    } else if (o.type == ObjectType::Light) {
        Json l = Json::object();
        l.set("color", vec3ToJson(o.light.color));
        l.set("intensity", Json::fromFloat(o.light.intensity));
        j.set("light", l);
    } else {
        Json c = Json::object();
        c.set("fov", Json::fromFloat(o.camera.fovY));
        c.set("near", Json::fromFloat(o.camera.zNear));
        c.set("far", Json::fromFloat(o.camera.zFar));
        j.set("camera", c);
    }
    if (!o.tracks.empty()) {
        Json tracks = Json::array();
        for (const Track& track : o.tracks) {
            Json tj = Json::object();
            tj.set("property", trackPropertyName(track.property));
            Json keys = Json::array();
            for (const Keyframe& k : track.keys) {
                Json kj = Json::object();
                kj.set("time", Json::fromFloat(k.time));
                Json v = Json::array();
                for (int i = 0; i < trackPropertyWidth(track.property); ++i) v.push(Json::fromFloat(k.value[i]));
                kj.set("value", v);
                kj.set("easing", easingName(k.easing));
                keys.push(kj);
            }
            tj.set("keys", keys);
            tracks.push(tj);
        }
        j.set("tracks", tracks);
    }
    return j;
}

namespace {

bool tracksFromJson(const Json& j, ObjectType type, std::vector<Track>& out, std::string& error) {
    if (!j.isArray()) return error = "tracks must be an array", false;
    std::vector<Track> tracks;
    for (size_t i = 0; i < j.size(); ++i) {
        const Json& tj = j[i];
        const Json* prop = tj.find("property");
        Track track;
        if (!prop || !prop->isString() || !trackPropertyFromName(prop->asString(), track.property))
            return error = "track: unknown property", false;
        if (!trackPropertyAllowed(track.property, type))
            return error = std::string("a ") + objectTypeName(type) + " can't animate " + trackPropertyName(track.property), false;
        for (const Track& other : tracks)
            if (other.property == track.property) return error = "two tracks for the same property", false;
        const Json* keys = tj.find("keys");
        if (!keys || !keys->isArray() || keys->size() == 0) return error = "track: needs keys", false;
        for (size_t k = 0; k < keys->size(); ++k) {
            const Json& kj = (*keys)[k];
            Keyframe key;
            if (!numberFromJson(kj.find("time"), 0, 3600, key.time, error, "key time")) return false;
            if (!floatsFromJson(kj.find("value"), key.value, trackPropertyWidth(track.property), error, "key value"))
                return false;
            if (track.property == TrackProperty::Rotation) {  // rotations are stored normalized
                const Quat raw{key.value[0], key.value[1], key.value[2], key.value[3]};
                if (dot(raw, raw) < 1e-12f) return error = "key value: a zero quaternion isn't a rotation", false;
                const Quat q = normalize(raw);
                key.value[0] = q.x, key.value[1] = q.y, key.value[2] = q.z, key.value[3] = q.w;
            }
            const Json* easing = kj.find("easing");
            if (easing && (!easing->isString() || !easingFromName(easing->asString(), key.easing)))
                return error = "key: unknown easing", false;
            if (!track.keys.empty() && !(key.time > track.keys.back().time))
                return error = "keys must be in increasing time order", false;
            track.keys.push_back(key);
        }
        tracks.push_back(std::move(track));
    }
    out = std::move(tracks);
    return true;
}

}  // namespace

bool objectFieldsFromJson(const Json& j, const Scene& scene, Object& o, std::string& error) {
    if (!j.isObject()) return error = "object must be a JSON object", false;
    Object r = o;  // work on a copy: nothing changes unless everything is valid
    if (const Json* type = j.find("type"))
        if (!type->isString() || !objectTypeFromName(type->asString(), r.type)) return error = "unknown object type", false;
    if (const Json* name = j.find("name")) {
        if (!name->isString() || name->asString().empty() || name->asString().size() > 128)
            return error = "name must be 1 to 128 characters", false;
        r.name = name->asString();
    }
    if (const Json* v = j.find("visible"))
        if (!boolFromJson(v, r.visible, error, "visible")) return false;
    // Only the shape is checked here: on load the parent may come later in the
    // file, so whether it exists (and makes no loop) is checked by the callers.
    if (const Json* p = j.find("parent")) {
        if (!p->isNumber() || p->asNumber() < 0 || p->asNumber() >= 4e9 || p->asNumber() != std::floor(p->asNumber()))
            return error = "parent must be an object id (0 for none)", false;
        r.parent = static_cast<ObjectId>(p->asNumber());
    }
    if (const Json* t = j.find("transform")) {
        if (!t->isObject()) return error = "transform must be an object", false;
        for (const auto& [key, value] : t->members())
            if (key != "position" && key != "rotation" && key != "scale")
                return error = "transform: unknown field '" + key + "' (rotations here are quaternions)", false;
        if (const Json* p = t->find("position"))
            if (!vec3FromJson(p, r.transform.position, error, "position")) return false;
        if (const Json* q = t->find("rotation"))
            if (!quatFromJson(q, r.transform.rotation, error, "rotation")) return false;
        if (const Json* s = t->find("scale"))
            if (!vec3FromJson(s, r.transform.scale, error, "scale")) return false;
    }
    if (const Json* m = j.find("mesh")) {
        if (!m->isNumber() || !scene.mesh(static_cast<MeshId>(m->asNumber())) || m->asNumber() != std::floor(m->asNumber()))
            return error = "mesh: no such mesh", false;
        r.mesh = static_cast<MeshId>(m->asNumber());
    }
    if (const Json* mat = j.find("material")) {
        if (!mat->isObject()) return error = "material must be an object", false;
        if (const Json* c = mat->find("color"))
            if (!colorFromJson(c, r.material.color, error, "color")) return false;
        if (const Json* s = mat->find("specular"))
            if (!numberFromJson(s, 0, 10, r.material.specular, error, "specular")) return false;
        if (const Json* s = mat->find("shininess"))
            if (!numberFromJson(s, 1, 1024, r.material.shininess, error, "shininess")) return false;
        if (const Json* c = mat->find("checker"))
            if (!boolFromJson(c, r.material.checker, error, "checker")) return false;
    }
    if (const Json* sh = j.find("shader")) {
        const Json* type = sh->isObject() ? sh->find("type") : nullptr;
        if (!type || !type->isString()) return error = "shader needs a type", false;
        const Json* params = sh->find("params");
        const Json p = params ? *params : Json::object();
        if (!validateShader(type->asString(), p, error)) return false;
        r.shader = {type->asString(), p};
    }
    if (const Json* l = j.find("light")) {
        if (!l->isObject()) return error = "light must be an object", false;
        if (const Json* c = l->find("color"))
            if (!colorFromJson(c, r.light.color, error, "light color")) return false;
        if (const Json* i = l->find("intensity"))
            if (!numberFromJson(i, 0, 100, r.light.intensity, error, "intensity")) return false;
    }
    if (const Json* c = j.find("camera")) {
        if (!c->isObject()) return error = "camera must be an object", false;
        if (const Json* f = c->find("fov"))
            if (!numberFromJson(f, 1, 170, r.camera.fovY, error, "fov")) return false;
        if (const Json* n = c->find("near"))
            if (!numberFromJson(n, 0.001f, 1000, r.camera.zNear, error, "near")) return false;
        if (const Json* f = c->find("far"))
            if (!numberFromJson(f, 0.01f, 100000, r.camera.zFar, error, "far")) return false;
        if (!(r.camera.zFar > r.camera.zNear)) return error = "far must be greater than near", false;
    }
    if (const Json* t = j.find("tracks"))
        if (!tracksFromJson(*t, r.type, r.tracks, error)) return false;
    // Each block belongs to one kind of object; on another kind it's a mistake.
    if (r.type != ObjectType::Mesh && (j.find("mesh") || j.find("material") || j.find("shader")))
        return error = std::string("a ") + objectTypeName(r.type) + " has no mesh, material, or shader", false;
    if (r.type != ObjectType::Light && j.find("light")) return error = "only lights have light settings", false;
    if (r.type != ObjectType::Camera && j.find("camera")) return error = "only cameras have camera settings", false;
    if (r.type == ObjectType::Mesh && !scene.mesh(r.mesh)) return error = "a mesh object needs a mesh", false;
    if (r.type != ObjectType::Mesh) r.mesh = kNoId;
    o = std::move(r);
    return true;
}

// ---------------------------------------------------------------- meshes

Json meshToJson(const EditMesh& mesh) {
    // Flat arrays: positions (xyz per vertex), face sizes, face corner vertex
    // indices, smooth flags, and per-corner UVs and normals when present.
    Json positions = Json::array(), sizes = Json::array(), corners = Json::array(), smooth = Json::array();
    Json uvs = Json::array(), normals = Json::array();
    for (const auto& v : mesh.verts())
        for (float f : {v.position.x, v.position.y, v.position.z}) positions.push(Json::fromFloat(f));
    for (const auto& face : mesh.faces()) {
        sizes.push(Json(face.count));
        smooth.push(Json(face.smooth ? 1 : 0));
        int l = face.loop;
        for (int i = 0; i < face.count; ++i, l = mesh.loops()[l].next) {
            const auto& loop = mesh.loops()[l];
            corners.push(Json(loop.vert));
            if (mesh.hasUVs) uvs.push(Json::fromFloat(loop.uv.x)), uvs.push(Json::fromFloat(loop.uv.y));
            if (mesh.hasCustomNormals)
                for (float f : {loop.normal.x, loop.normal.y, loop.normal.z}) normals.push(Json::fromFloat(f));
        }
    }
    Json j = Json::object();
    j.set("positions", positions);
    j.set("faceSizes", sizes);
    j.set("faceVerts", corners);
    j.set("smooth", smooth);
    if (mesh.hasUVs) j.set("uvs", uvs);
    if (mesh.hasCustomNormals) j.set("normals", normals);
    return j;
}

bool meshFromJson(const Json& j, EditMesh& out, std::string& error) {
    const Json* positions = j.find("positions");
    const Json* sizes = j.find("faceSizes");
    const Json* corners = j.find("faceVerts");
    const Json* smooth = j.find("smooth");
    const Json* uvs = j.find("uvs");
    const Json* normals = j.find("normals");
    if (!positions || !positions->isArray() || positions->size() % 3 != 0) return error = "mesh: bad positions", false;
    if (!sizes || !sizes->isArray() || !corners || !corners->isArray()) return error = "mesh: missing faces", false;
    if (smooth && (!smooth->isArray() || smooth->size() != sizes->size())) return error = "mesh: bad smooth flags", false;
    if (uvs && (!uvs->isArray() || uvs->size() != corners->size() * 2)) return error = "mesh: bad uvs", false;
    if (normals && (!normals->isArray() || normals->size() != corners->size() * 3)) return error = "mesh: bad normals", false;
    auto finite = [](const Json& v, float& f) { return v.isNumber() && std::isfinite(f = static_cast<float>(v.asNumber())); };

    EditMesh m;
    const size_t nv = positions->size() / 3;
    for (size_t i = 0; i < nv; ++i) {
        Vec3 p;
        if (!finite((*positions)[i * 3], p.x) || !finite((*positions)[i * 3 + 1], p.y) || !finite((*positions)[i * 3 + 2], p.z))
            return error = "mesh: bad position", false;
        m.addVert(p);
    }
    size_t corner = 0;
    std::vector<int> verts;
    std::vector<Vec2> faceUvs;
    std::vector<Vec3> faceNormals;
    for (size_t f = 0; f < sizes->size(); ++f) {
        const double n = (*sizes)[f].isNumber() ? (*sizes)[f].asNumber() : 0;
        if (!(n >= 3 && n <= 1e6 && n == std::floor(n)) || corner + static_cast<size_t>(n) > corners->size())
            return error = "mesh: bad face size", false;
        verts.clear();
        faceUvs.clear();
        faceNormals.clear();
        for (size_t i = 0; i < static_cast<size_t>(n); ++i, ++corner) {
            const Json& c = (*corners)[corner];
            if (!c.isNumber() || c.asNumber() < 0 || c.asNumber() >= static_cast<double>(nv) || c.asNumber() != std::floor(c.asNumber()))
                return error = "mesh: corner index out of range", false;
            verts.push_back(static_cast<int>(c.asNumber()));
            Vec2 uv;
            Vec3 normal;
            if (uvs && (!finite((*uvs)[corner * 2], uv.x) || !finite((*uvs)[corner * 2 + 1], uv.y))) return error = "mesh: bad uv", false;
            if (normals && (!finite((*normals)[corner * 3], normal.x) || !finite((*normals)[corner * 3 + 1], normal.y) ||
                            !finite((*normals)[corner * 3 + 2], normal.z)))
                return error = "mesh: bad normal", false;
            faceUvs.push_back(uv);
            faceNormals.push_back(normal);
        }
        const bool isSmooth = smooth && (*smooth)[f].isNumber() && (*smooth)[f].asNumber() != 0;
        if (m.addFace(verts, isSmooth, &faceUvs, &faceNormals) == EditMesh::kNone)
            return error = "mesh: face " + std::to_string(f) + " is degenerate", false;
    }
    if (corner != corners->size()) return error = "mesh: corner count doesn't match face sizes", false;
    m.hasUVs = uvs != nullptr;
    m.hasCustomNormals = normals != nullptr;
    out = std::move(m);
    return true;
}

// ---------------------------------------------------------------- scenes

bool animationFromJson(const Json& j, AnimationSettings& a, std::string& error) {
    if (!j.isObject()) return error = "animation must be an object", false;
    AnimationSettings r = a;
    for (const auto& [key, value] : j.members())
        if (key != "fps" && key != "start" && key != "end") return error = "unknown animation setting '" + key + "'", false;
    auto frame = [&](const char* key, int& out) {
        const Json* v = j.find(key);
        if (!v) return true;
        if (!v->isNumber() || v->asNumber() != std::floor(v->asNumber()) || v->asNumber() < 0 || v->asNumber() > 1e6)
            return error = std::string(key) + " must be a whole frame number from 0", false;
        out = static_cast<int>(v->asNumber());
        return true;
    };
    if (const Json* f = j.find("fps"))
        if (!numberFromJson(f, 1, 240, r.fps, error, "fps")) return false;
    if (!frame("start", r.startFrame) || !frame("end", r.endFrame)) return false;
    if (r.endFrame <= r.startFrame) return error = "end must come after start", false;
    if (r.endFrame / r.fps > 3600) return error = "the animation can be at most an hour long", false;  // like key times
    a = r;
    return true;
}

Json animationToJson(const AnimationSettings& a) {
    Json j = Json::object();
    j.set("fps", Json::fromFloat(a.fps));
    j.set("start", Json(a.startFrame));
    j.set("end", Json(a.endFrame));
    return j;
}

Json Scene::toJson() const {
    Json j = Json::object();
    j.set("format", "tessera-scene");
    j.set("version", Json(1));
    j.set("nextObjectId", Json(nextObjectId_));
    j.set("nextMeshId", Json(nextMeshId_));
    j.set("activeCamera", Json(activeCamera_));
    Json world = Json::object();
    world.set("ambient", Json::fromFloat(world_.ambient));
    world.set("background", vec3ToJson(world_.background));
    j.set("world", world);
    j.set("animation", animationToJson(animation_));
    // Only meshes some object uses (the store also keeps ones undo history needs).
    Json meshes = Json::array();
    for (const auto& [id, entry] : meshes_) {
        bool used = false;
        for (const Object& o : objects_) used = used || (o.type == ObjectType::Mesh && o.mesh == id);
        if (!used) continue;
        Json mj = Json::object();
        mj.set("id", Json(id));
        if (entry.primitive) {
            Json p = Json::object();
            p.set("type", primitiveName(entry.primitive->type));
            p.set("segments", Json(entry.primitive->segments));
            p.set("rings", Json(entry.primitive->rings));
            mj.set("primitive", p);
        } else {
            mj.set("data", meshToJson(*entry.edit));
        }
        meshes.push(mj);
    }
    j.set("meshes", meshes);
    Json objects = Json::array();
    for (const Object& o : objects_) objects.push(objectToJson(o));
    j.set("objects", objects);
    return j;
}

bool Scene::fromJson(const Json& j, Scene& out, std::string& error) {
    if (!j.isObject() || !j.find("format") || !j.find("format")->isString() || j.find("format")->asString() != "tessera-scene")
        return error = "not a Tessera scene file", false;
    const Json* version = j.find("version");
    if (!version || !version->isNumber() || version->asNumber() != 1) return error = "unsupported scene version", false;
    auto idFrom = [&](const char* key, uint32_t& v) {
        const Json* n = j.find(key);
        if (!n || !n->isNumber() || n->asNumber() < 0 || n->asNumber() > 4e9 || n->asNumber() != std::floor(n->asNumber()))
            return error = std::string("bad ") + key, false;
        v = static_cast<uint32_t>(n->asNumber());
        return true;
    };
    Scene s;
    if (!idFrom("nextObjectId", s.nextObjectId_) || !idFrom("nextMeshId", s.nextMeshId_) || !idFrom("activeCamera", s.activeCamera_))
        return false;
    if (const Json* world = j.find("world")) {
        if (const Json* a = world->find("ambient"))
            if (!numberFromJson(a, 0, 1, s.world_.ambient, error, "ambient")) return false;
        if (const Json* b = world->find("background"))
            if (!colorFromJson(b, s.world_.background, error, "background")) return false;
    }
    if (const Json* a = j.find("animation"))  // optional: files from before Phase 2 have none
        if (!animationFromJson(*a, s.animation_, error)) return false;
    const Json* meshes = j.find("meshes");
    if (!meshes || !meshes->isArray()) return error = "missing meshes", false;
    for (size_t i = 0; i < meshes->size(); ++i) {
        const Json& mj = (*meshes)[i];
        const Json* id = mj.find("id");
        if (!id || !id->isNumber() || id->asNumber() < 1 || id->asNumber() >= s.nextMeshId_ || id->asNumber() != std::floor(id->asNumber()))
            return error = "mesh with a bad id", false;
        const MeshId meshId = static_cast<MeshId>(id->asNumber());
        if (s.mesh(meshId)) return error = "two meshes with the same id", false;
        MeshEntry entry;
        EditMesh edit;
        if (const Json* p = mj.find("primitive")) {
            Primitive prim;
            const Json* type = p->find("type");
            if (!type || !type->isString() || !primitiveFromName(type->asString(), prim.type)) return error = "unknown primitive", false;
            float segments = 32, rings = 16;
            if (const Json* sg = p->find("segments"))
                if (!numberFromJson(sg, 3, 256, segments, error, "segments")) return false;
            if (const Json* rg = p->find("rings"))
                if (!numberFromJson(rg, 2, 256, rings, error, "rings")) return false;
            prim.segments = static_cast<int>(segments);
            prim.rings = static_cast<int>(rings);
            edit = makePrimitive(prim);
            entry.primitive = prim;
        } else if (const Json* data = mj.find("data")) {
            if (!meshFromJson(*data, edit, error)) return false;
        } else {
            return error = "mesh needs a primitive or data", false;
        }
        entry.render = std::make_shared<const Mesh>(edit.triangulate());
        entry.edit = std::make_shared<const EditMesh>(std::move(edit));
        s.meshes_.emplace_back(meshId, std::move(entry));
    }
    const Json* objects = j.find("objects");
    if (!objects || !objects->isArray()) return error = "missing objects", false;
    for (size_t i = 0; i < objects->size(); ++i) {
        const Json& oj = (*objects)[i];
        Object o;
        const Json* id = oj.find("id");
        if (!id || !id->isNumber() || id->asNumber() < 1 || id->asNumber() >= s.nextObjectId_ || id->asNumber() != std::floor(id->asNumber()))
            return error = "object with a bad id", false;
        o.id = static_cast<ObjectId>(id->asNumber());
        if (s.find(o.id)) return error = "two objects with the same id", false;
        if (!oj.find("name") || !oj.find("type")) return error = "object needs a name and a type", false;
        if (!objectFieldsFromJson(oj, s, o, error)) return false;
        s.objects_.push_back(std::move(o));
    }
    for (const Object& o : s.objects_)  // now that every object is known
        if (!validParent(s, o.id, o.parent, error)) return error = o.name + ": " + error, false;
    if (s.activeCamera_ != kNoId) {
        const Object* cam = s.find(s.activeCamera_);
        if (!cam || cam->type != ObjectType::Camera) return error = "activeCamera isn't a camera", false;
    }
    out = std::move(s);
    return true;
}
