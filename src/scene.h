// scene.h — the editor's document: objects (meshes, lights, cameras), the meshes
// they use, and world settings.
//
// Only the command layer (commands.h) can change a Scene: its mutators are private
// and SceneEditor is its friend. Everything else (renderer, UI, picking, export)
// reads it. That one rule is what makes undo, saving, and (later) AI edits safe.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "edit_mesh.h"
#include "json.h"
#include "mesh.h"
#include "primitives.h"
#include "vecmath.h"

// IDs start at 1 and are never reused, even after a delete: animation tracks,
// undo history, and AI tool calls can all refer to objects by ID safely.
using ObjectId = uint32_t;
using MeshId = uint32_t;
constexpr uint32_t kNoId = 0;

enum class ObjectType { Mesh, Light, Camera };

struct Transform {
    Vec3 position{0, 0, 0};
    Quat rotation;  // unit quaternion; the UI shows Euler degrees
    Vec3 scale{1, 1, 1};
    Mat4 matrix() const { return Mat4::translate(position) * rotation.toMat4() * Mat4::scale(scale); }
};

struct Material {
    Vec3 color{0.8f, 0.8f, 0.85f};
    float specular = 0.35f;   // highlight strength
    float shininess = 48.0f;  // highlight tightness
    bool checker = false;     // multiply by the checker texture (meshes with UVs)
};

// An optional custom shader, by registered type name plus its settings (JSON), so
// it can be saved and edited. Empty type: the fixed-function pipeline.
struct ShaderRef {
    std::string type;
    Json params = Json::object();
};

struct LightData {  // a sun: parallel light shining along the object's -Z axis
    Vec3 color{1, 1, 1};
    float intensity = 1.0f;
};

struct CameraData {  // looks along the object's -Z axis, +Y up
    float fovY = 50.0f;  // vertical field of view, degrees
    float zNear = 0.1f;
    float zFar = 100.0f;
};

// Keyframes (played back in Phase 2). Values: 3 floats for vectors and colors,
// 4 for rotations (a quaternion), 1 for intensity and field of view.
enum class Easing { Linear, EaseIn, EaseOut, EaseInOut, Step };
enum class TrackProperty { Position, Rotation, Scale, Color, LightIntensity, CameraFov };

struct Keyframe {
    float time = 0;  // seconds
    float value[4] = {0, 0, 0, 0};
    Easing easing = Easing::Linear;  // how to get from this key to the next
};

struct Track {
    TrackProperty property = TrackProperty::Position;
    std::vector<Keyframe> keys;  // sorted by time, no two at the same time
};

struct Object {
    ObjectId id = kNoId;
    std::string name;
    ObjectType type = ObjectType::Mesh;
    ObjectId parent = kNoId;  // `transform` is relative to this object's (see hierarchy.h)
    Transform transform;
    bool visible = true;
    // Mesh objects:
    MeshId mesh = kNoId;
    Material material;
    ShaderRef shader;
    // Light / camera objects:
    LightData light;
    CameraData camera;
    std::vector<Track> tracks;
};

// Geometry is immutable once stored: an edit stores a new mesh and points the
// object at it, so undoing a mesh edit just points back (copy-on-write).
struct MeshEntry {
    std::shared_ptr<const EditMesh> edit;
    std::shared_ptr<const Mesh> render;    // triangulated once; what the renderer draws
    std::optional<Primitive> primitive;    // saved as these settings until edited
};

struct World {
    float ambient = 0.15f;  // light floor so surfaces facing away aren't black
    Vec3 background{0.08f, 0.09f, 0.11f};
};

// Playback rate and range. Keys are stored in seconds; frames are how the timeline
// and export count them (frame f is at f / fps seconds).
struct AnimationSettings {
    float fps = 24;
    int startFrame = 0;
    int endFrame = 120;  // 5 seconds at 24 fps
};

class Scene {
public:
    const std::vector<Object>& objects() const { return objects_; }
    const Object* find(ObjectId id) const;
    int indexOf(ObjectId id) const;  // position in the outliner order, or -1
    const MeshEntry* mesh(MeshId id) const;
    ObjectId activeCamera() const { return activeCamera_; }  // kNoId: none
    const World& world() const { return world_; }
    const AnimationSettings& animation() const { return animation_; }
    ObjectId nextObjectId() const { return nextObjectId_; }

    // Stores a mesh and returns its ID. Not a scene change by itself (no object
    // uses it yet), so it doesn't go through a command: the upload path adds the
    // mesh, then runs an addObject command that references it.
    MeshId addMesh(EditMesh mesh, std::optional<Primitive> primitive = std::nullopt);

    // Blender's default scene: a cube, a sun, and a camera looking at the cube.
    static Scene makeDefault();

    // Save/load. The format is versioned; load validates everything and either
    // fills `out` completely or leaves it untouched and returns false.
    Json toJson() const;
    static bool fromJson(const Json& json, Scene& out, std::string& error);

private:
    friend class SceneEditor;  // commands.cpp: the only code that mutates a Scene

    std::vector<Object> objects_;
    std::vector<std::pair<MeshId, MeshEntry>> meshes_;  // few entries; a vector keeps saves ordered
    ObjectId nextObjectId_ = 1;
    MeshId nextMeshId_ = 1;
    ObjectId activeCamera_ = kNoId;
    World world_;
    AnimationSettings animation_;
};

// ---- JSON pieces, shared by save/load and by commands (which take JSON too).

Json vec3ToJson(const Vec3& v);
Json quatToJson(const Quat& q);
bool vec3FromJson(const Json* j, Vec3& out, std::string& error, const char* what);
bool quatFromJson(const Json* j, Quat& out, std::string& error, const char* what);  // normalizes

// Reads the fields present in `j` ({fps?, start?, end?}) onto `a`, validated as a
// whole (whole frames, end after start). Shared by load and the setAnimation command.
bool animationFromJson(const Json& j, AnimationSettings& a, std::string& error);
Json animationToJson(const AnimationSettings& a);

const char* objectTypeName(ObjectType type);
bool objectTypeFromName(const std::string& name, ObjectType& out);
const char* trackPropertyName(TrackProperty p);
bool trackPropertyFromName(const std::string& name, TrackProperty& out);
int trackPropertyWidth(TrackProperty p);  // floats per value
bool trackPropertyAllowed(TrackProperty p, ObjectType type);
const char* easingName(Easing e);
bool easingFromName(const std::string& name, Easing& out);

Json objectToJson(const Object& o);
// Reads the fields present in `j` onto `o` (missing fields keep o's values), with
// full validation against `scene` (e.g. the mesh ID must exist). Doesn't read "id".
bool objectFieldsFromJson(const Json& j, const Scene& scene, Object& o, std::string& error);

Json meshToJson(const EditMesh& mesh);
bool meshFromJson(const Json& j, EditMesh& out, std::string& error);

// Registered shader types and their settings ("liquid", "glass").
bool validateShader(const std::string& type, const Json& params, std::string& error);
