#include "editor.h"

#include "picking.h"

Editor::Editor() : scene_(Scene::makeDefault()) {}

Json Editor::command(const Json& cmd) {
    Json result, out = Json::object();
    std::string error;
    const bool ok = stack_.execute(cmd, result, error);
    out.set("ok", ok);
    if (ok) out.set("result", result);
    else out.set("error", error);
    return out;
}

bool Editor::undo() { return stack_.undo(); }
bool Editor::redo() { return stack_.redo(); }

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
    select(pickObject(scene_, rayThroughPixel(view, projection, x, y, width, height)));
    return selected_;
}

std::string Editor::save() const { return scene_.toJson().dump(2); }

void Editor::replaceScene(Scene scene) {
    scene_ = std::move(scene);  // stack_ refers to scene_, which stays the same object
    stack_.reset();
    selected_ = kNoId;
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

bool Editor::loadExample(const std::string& name, std::string& error) {
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

void Editor::setViewThroughCamera(bool on) {
    if (on != viewThroughCamera_) {
        viewThroughCamera_ = on;
        ++uiChanges_;
    }
}

void Editor::viewMatrices(float aspect, Mat4& view, Mat4& projection) const {
    if (viewThroughCamera_ && SceneRenderer::cameraView(scene_, scene_.activeCamera(), aspect, view, projection)) return;
    view = orbit_.view();
    projection = orbit_.projection(aspect);
}

void Editor::render(Renderer& renderer, Framebuffer& fb, float dt) {
    sceneRenderer_.update(scene_, dt);
    Mat4 view, projection;
    viewMatrices(static_cast<float>(fb.width()) / fb.height(), view, projection);
    SceneRenderer::Overlays overlays;
    overlays.selected = scene_.find(selected_) ? selected_ : kNoId;  // undo may have removed it
    overlays.grid = overlays.gizmos = !viewThroughCamera_;          // the camera view is the final shot
    sceneRenderer_.render(renderer, fb, scene_, view, projection, overlays);
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
    Json world = Json::object();
    world.set("ambient", Json::fromFloat(scene_.world().ambient));
    world.set("background", vec3ToJson(scene_.world().background));
    s.set("world", world);
    Json objects = Json::array();
    for (const Object& o : scene_.objects()) {
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
