// editor_edit.cpp — the Editor's edit mode (Phase 5): selecting a mesh's vertices,
// edges, and faces, moving them with G/R/S, and running modeling operations on them.
// Every change to the mesh is an editMesh command, so undo, saving, and the AI
// assistant all see the same thing. Selecting is UI state and never a command.
#include <algorithm>
#include <cmath>
#include <set>

#include "editor.h"
#include "hierarchy.h"
#include "picking.h"

namespace {

constexpr float kVertexRadius = 12;  // pixels: how close a click must come to a vertex
constexpr float kEdgeRadius = 10;
// Above this many (candidates x faces) ray tests, box select stops checking which
// elements are hidden (it selects through the mesh instead of stalling the page).
constexpr double kVisibilityBudget = 3e7;

// The edited object as seen in the viewport: projects its points to pixels and
// checks whether a point is hidden behind its own surface.
struct EditView {
    Mat4 view, projection, viewProj, model;
    int width = 1, height = 1;

    // A world point's pixel, in the mouse's convention (pixel centers at +0.5);
    // false behind the camera.
    bool screen(const Vec3& p, float& x, float& y) const {
        const Vec4 c = viewProj * Vec4(p, 1.0f);
        if (c.w <= 1e-4f) return false;
        x = (c.x / c.w * 0.5f + 0.5f) * width - 0.5f;
        y = (0.5f - c.y / c.w * 0.5f) * height - 0.5f;
        return true;
    }
    // Visible if the ray from the camera through the point meets the surface no
    // sooner than the point itself. The faces around a vertex meet the ray right at
    // it, so the comparison allows a little slack (relative, as depth precision is).
    bool visible(const EditMesh& mesh, const Vec3& p) const {
        float x, y;
        if (!screen(p, x, y)) return false;
        const Ray ray = rayThroughPixel(view, projection, x, y, width, height);
        const float along = dot(p - ray.origin, ray.direction);
        return rayMeshDistance(mesh, model, ray) >= along - (2e-3f * along + 1e-4f);
    }
};

// Distance from pixel p to the segment a-b, and where along it (0..1) it's closest.
float segmentDistance(float px, float py, float ax, float ay, float bx, float by, float& t) {
    const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    t = len2 > 1e-9f ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f) : 0.0f;
    const float cx = ax + dx * t - px, cy = ay + dy * t - py;
    return std::sqrt(cx * cx + cy * cy);
}

std::pair<int, int> edgeKey(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

template <typename T>
void sortUnique(std::vector<T>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

std::vector<int> faceVerts(const EditMesh& m, int face) {
    std::vector<int> verts;
    for (int l = m.faces()[face].loop, k = 0; k < m.faces()[face].count; ++k, l = m.loops()[l].next)
        verts.push_back(m.loops()[l].vert);
    return verts;
}

Vec3 faceCenter(const EditMesh& m, int face) {
    Vec3 c{0, 0, 0};
    const std::vector<int> verts = faceVerts(m, face);
    for (int v : verts) c = c + m.verts()[v].position * (1.0f / verts.size());
    return c;
}

Json intArray(const std::vector<int>& v) {
    Json a = Json::array();
    for (int i : v) a.push(Json(i));
    return a;
}

Json selectionJson(const MeshSelection& s) {
    Json j = Json::object(), edges = Json::array();
    for (const auto& [a, b] : s.edges) edges.push(intArray({a, b}));
    j.set("verts", intArray(s.verts));
    j.set("edges", edges);
    j.set("faces", intArray(s.faces));
    return j;
}

// The selection in an editMesh result ({verts, edges, faces}).
MeshSelection selectionFromJson(const Json& j) {
    MeshSelection s;
    auto ints = [&](const char* key, std::vector<int>& out) {
        if (const Json* a = j.find(key); a && a->isArray())
            for (size_t i = 0; i < a->size(); ++i) out.push_back(static_cast<int>((*a)[i].asNumber()));
    };
    ints("verts", s.verts);
    ints("faces", s.faces);
    if (const Json* a = j.find("edges"); a && a->isArray())
        for (size_t i = 0; i < a->size(); ++i)
            s.edges.push_back(edgeKey(static_cast<int>((*a)[i][0].asNumber()), static_cast<int>((*a)[i][1].asNumber())));
    return s;
}

Json failure(const std::string& error) {
    Json out = Json::object();
    out.set("ok", false);
    out.set("error", error);
    return out;
}

Json rounded(float v) { return Json(std::round(static_cast<double>(v) * 1000.0) / 1000.0); }

const char* modeName(Editor::SelectMode mode) {
    return mode == Editor::SelectMode::Vertex ? "vertex" : mode == Editor::SelectMode::Edge ? "edge" : "face";
}

}  // namespace

// ------------------------------------------------------------ entering and leaving

const MeshEntry* Editor::editEntry() const {
    const Object* o = scene_.find(editObject_);
    return o && o->type == ObjectType::Mesh ? scene_.mesh(o->mesh) : nullptr;
}

ObjectId Editor::editObject() const { return editEntry() ? editObject_ : kNoId; }

bool Editor::enterEditMode() {
    const Object* o = scene_.find(selected());
    if (!o || o->type != ObjectType::Mesh || !scene_.mesh(o->mesh)) return false;
    if (transforming()) confirmTransform();
    const ObjectId id = o->id;
    setSelection({id}, id);  // just the edited object (before editObject_, so this doesn't leave edit mode)
    editObject_ = id;
    lastOp_ = {};
    ++uiChanges_;
    return true;
}

void Editor::exitEditMode() {
    if (editObject_ == kNoId) return;
    if (transforming()) confirmTransform();
    editObject_ = kNoId;
    lastOp_ = {};
    ++uiChanges_;
}

// ------------------------------------------------------------ the element selection

const MeshSelection& Editor::elements() const {
    static const MeshSelection kEmpty;
    const Object* o = scene_.find(editObject());
    if (!o) return kEmpty;
    const auto it = elements_.find(o->mesh);
    return it == elements_.end() ? kEmpty : it->second;
}

// Blender's rules for how the kinds of elements follow each other. In vertex mode
// the vertices are what's selected, and an edge or face is selected when all its
// vertices are; in edge mode a face needs all its edges; in face mode the faces'
// vertices and edges are selected along with them.
MeshSelection Editor::completed(MeshSelection sel, SelectMode mode) const {
    const MeshEntry* e = editEntry();
    if (!e) return {};
    const EditMesh& m = *e->edit;
    const int nv = static_cast<int>(m.verts().size()), nf = static_cast<int>(m.faces().size());
    MeshSelection out;
    if (mode == SelectMode::Vertex) {
        for (int v : sel.verts)
            if (v >= 0 && v < nv) out.verts.push_back(v);
        sortUnique(out.verts);
        MeshSelection c = completeSelection(m, out.verts);
        out.edges = std::move(c.edges);
        out.faces = std::move(c.faces);
    } else if (mode == SelectMode::Edge) {
        for (const auto& [a, b] : sel.edges)
            if (a >= 0 && b >= 0 && a < nv && b < nv && m.findEdge(a, b) != EditMesh::kNone) {
                out.edges.push_back(edgeKey(a, b));
                out.verts.push_back(a);
                out.verts.push_back(b);
            }
        sortUnique(out.edges);
        sortUnique(out.verts);
        const std::set<std::pair<int, int>> on(out.edges.begin(), out.edges.end());
        for (int f = 0; f < nf; ++f) {
            const std::vector<int> verts = faceVerts(m, f);
            bool all = true;
            for (size_t k = 0; k < verts.size() && all; ++k) all = on.count(edgeKey(verts[k], verts[(k + 1) % verts.size()])) > 0;
            if (all) out.faces.push_back(f);
        }
    } else {
        for (int f : sel.faces)
            if (f >= 0 && f < nf) out.faces.push_back(f);
        sortUnique(out.faces);
        for (int f : out.faces) {
            const std::vector<int> verts = faceVerts(m, f);
            for (size_t k = 0; k < verts.size(); ++k) {
                out.verts.push_back(verts[k]);
                out.edges.push_back(edgeKey(verts[k], verts[(k + 1) % verts.size()]));
            }
        }
        sortUnique(out.verts);
        sortUnique(out.edges);
    }
    return out;
}

void Editor::setElements(const MeshSelection& sel) {
    const Object* o = scene_.find(editObject());
    if (!o) return;
    elements_[o->mesh] = completed(sel, selectMode_);
    ++uiChanges_;
}

void Editor::setSelectMode(SelectMode mode) {
    if (mode == selectMode_) return;
    selectMode_ = mode;
    if (editing()) setElements(elements());  // every kind is already consistent: re-derive from the new mode's
    ++uiChanges_;
}

void Editor::selectAllElements(bool all) {
    const MeshEntry* e = editEntry();
    if (!e) return;
    MeshSelection sel;
    if (all) {
        const EditMesh& m = *e->edit;
        for (int v = 0; v < static_cast<int>(m.verts().size()); ++v) sel.verts.push_back(v);
        for (const auto& edge : m.edges()) sel.edges.push_back(edgeKey(edge.v[0], edge.v[1]));
        for (int f = 0; f < static_cast<int>(m.faces().size()); ++f) sel.faces.push_back(f);
    }
    setElements(sel);
}

// ------------------------------------------------------------ picking

int Editor::edgeAt(float x, float y, int width, int height) const {
    const MeshEntry* e = editEntry();
    if (!e) return -1;
    const EditMesh& m = *e->edit;
    EditView ev;
    viewMatrices(static_cast<float>(width) / height, ev.view, ev.projection);
    ev.viewProj = ev.projection * ev.view;
    ev.model = worldMatrix(scene_, *scene_.find(editObject_), time_);
    ev.width = width, ev.height = height;
    struct Candidate {
        float distance;
        int edge;
        Vec3 point;  // the edge's closest point, to test for visibility
    };
    std::vector<Candidate> near;
    for (int i = 0; i < static_cast<int>(m.edges().size()); ++i) {
        const Vec3 a = (ev.model * Vec4(m.verts()[m.edges()[i].v[0]].position, 1.0f)).xyz();
        const Vec3 b = (ev.model * Vec4(m.verts()[m.edges()[i].v[1]].position, 1.0f)).xyz();
        float ax, ay, bx, by, t;
        if (!ev.screen(a, ax, ay) || !ev.screen(b, bx, by)) continue;
        const float d = segmentDistance(x, y, ax, ay, bx, by, t);
        if (d <= kEdgeRadius) near.push_back({d, i, a + (b - a) * t});
    }
    std::sort(near.begin(), near.end(), [](const Candidate& p, const Candidate& q) {
        return p.distance < q.distance || (p.distance == q.distance && p.edge < q.edge);
    });
    for (const Candidate& c : near)
        if (ev.visible(m, c.point)) return c.edge;
    return -1;
}

bool Editor::pickElement(float x, float y, int width, int height, bool extend) {
    const MeshEntry* e = editEntry();
    if (!e || transforming()) return false;
    const EditMesh& m = *e->edit;
    EditView ev;
    viewMatrices(static_cast<float>(width) / height, ev.view, ev.projection);
    ev.viewProj = ev.projection * ev.view;
    ev.model = worldMatrix(scene_, *scene_.find(editObject_), time_);
    ev.width = width, ev.height = height;

    // What's under the mouse, as a selection of one element (empty: nothing).
    MeshSelection hit;
    if (selectMode_ == SelectMode::Vertex) {
        std::vector<std::pair<float, int>> near;  // (pixel distance, vertex)
        for (int v = 0; v < static_cast<int>(m.verts().size()); ++v) {
            float sx, sy;
            if (!ev.screen((ev.model * Vec4(m.verts()[v].position, 1.0f)).xyz(), sx, sy)) continue;
            const float d = std::hypot(sx - x, sy - y);
            if (d <= kVertexRadius) near.push_back({d, v});
        }
        std::sort(near.begin(), near.end());
        for (const auto& [d, v] : near)
            if (ev.visible(m, (ev.model * Vec4(m.verts()[v].position, 1.0f)).xyz())) {
                hit.verts.push_back(v);
                break;
            }
    } else if (selectMode_ == SelectMode::Edge) {
        if (const int edge = edgeAt(x, y, width, height); edge >= 0)
            hit.edges.push_back(edgeKey(m.edges()[edge].v[0], m.edges()[edge].v[1]));
    } else {
        int face = -1;
        rayMeshDistance(m, ev.model, rayThroughPixel(ev.view, ev.projection, x, y, width, height), &face);
        if (face >= 0) hit.faces.push_back(face);
    }

    if (!extend) {
        setElements(hit);  // clicking empty space deselects everything
        return !hit.empty();
    }
    if (hit.empty()) return false;  // shift-clicking empty space keeps the selection
    // Shift-click toggles the element.
    MeshSelection sel = elements();
    auto toggle = [](auto& list, const auto& item) {
        const auto at = std::find(list.begin(), list.end(), item);
        if (at == list.end()) list.push_back(item);
        else list.erase(at);
    };
    if (!hit.verts.empty()) toggle(sel.verts, hit.verts[0]);
    if (!hit.edges.empty()) toggle(sel.edges, hit.edges[0]);
    if (!hit.faces.empty()) toggle(sel.faces, hit.faces[0]);
    setElements(sel);
    return true;
}

void Editor::boxSelectElements(float x0, float y0, float x1, float y1, int width, int height, bool extend) {
    const MeshEntry* e = editEntry();
    if (!e) return;
    const EditMesh& m = *e->edit;
    EditView ev;
    viewMatrices(static_cast<float>(width) / height, ev.view, ev.projection);
    ev.viewProj = ev.projection * ev.view;
    ev.model = worldMatrix(scene_, *scene_.find(editObject_), time_);
    ev.width = width, ev.height = height;
    const float left = std::min(x0, x1), right = std::max(x0, x1), top = std::min(y0, y1), bottom = std::max(y0, y1);
    auto inside = [&](const Vec3& world) {
        float x, y;
        return ev.screen(world, x, y) && x >= left && x <= right && y >= top && y <= bottom;
    };
    std::vector<Vec3> world;
    for (const auto& v : m.verts()) world.push_back((ev.model * Vec4(v.position, 1.0f)).xyz());

    // The points that decide each candidate (a vertex; an edge's two ends; a face's
    // center), then which candidates have all their points in the box and in view.
    std::vector<std::vector<Vec3>> points;
    std::vector<int> candidates;
    if (selectMode_ == SelectMode::Vertex) {
        for (int v = 0; v < static_cast<int>(world.size()); ++v)
            if (inside(world[v])) candidates.push_back(v), points.push_back({world[v]});
    } else if (selectMode_ == SelectMode::Edge) {
        for (int i = 0; i < static_cast<int>(m.edges().size()); ++i) {
            const Vec3 &a = world[m.edges()[i].v[0]], &b = world[m.edges()[i].v[1]];
            if (inside(a) && inside(b)) candidates.push_back(i), points.push_back({a, b});
        }
    } else {
        for (int f = 0; f < static_cast<int>(m.faces().size()); ++f) {
            const Vec3 c = (ev.model * Vec4(faceCenter(m, f), 1.0f)).xyz();
            if (inside(c)) candidates.push_back(f), points.push_back({c});
        }
    }
    const bool checkVisibility = static_cast<double>(candidates.size()) * m.faces().size() <= kVisibilityBudget;
    MeshSelection sel = extend ? elements() : MeshSelection{};
    for (size_t i = 0; i < candidates.size(); ++i) {
        bool shown = true;
        for (const Vec3& p : points[i]) shown = shown && (!checkVisibility || ev.visible(m, p));
        if (!shown) continue;
        const int c = candidates[i];
        if (selectMode_ == SelectMode::Vertex) sel.verts.push_back(c);
        else if (selectMode_ == SelectMode::Edge) sel.edges.push_back(edgeKey(m.edges()[c].v[0], m.edges()[c].v[1]));
        else sel.faces.push_back(c);
    }
    setElements(sel);
}

// ------------------------------------------------------------ operations

Json Editor::meshOperation(const std::string& operation, const Json& params) {
    if (transforming()) confirmTransform();
    const Object* o = scene_.find(editObject());
    if (!o) return failure("not in edit mode");
    const MeshSelection& sel = elements();
    Json cmd = Json::object();
    cmd.set("op", "editMesh");
    cmd.set("id", Json(o->id));
    cmd.set("operation", operation);
    const Json s = selectionJson(sel);
    for (const char* key : {"verts", "edges", "faces"})
        if (s.find(key)->size()) cmd.set(key, *s.find(key));
    if (operation == "delete")
        cmd.set("what", selectMode_ == SelectMode::Vertex ? "verts" : selectMode_ == SelectMode::Edge ? "edges" : "faces");
    for (const char* key : {"amount", "cuts"})
        if (const Json* v = params.find(key)) cmd.set(key, *v);
    // Loop cut goes through the edge under the mouse (as in Blender), or else the
    // first selected one.
    if (const Json* at = params.find("at"); operation == "loopcut" && at && at->isArray() && at->size() == 4) {
        const int edge = edgeAt(static_cast<float>((*at)[0].asNumber()), static_cast<float>((*at)[1].asNumber()),
                                static_cast<int>((*at)[2].asNumber()), static_cast<int>((*at)[3].asNumber()));
        if (edge >= 0) {
            const EditMesh& m = *editEntry()->edit;
            Json edges = Json::array();
            edges.push(intArray({std::min(m.edges()[edge].v[0], m.edges()[edge].v[1]),
                                 std::max(m.edges()[edge].v[0], m.edges()[edge].v[1])}));
            cmd.set("edges", edges);
        }
    }
    // The adjustable ones always say their amount or cuts, for the panel to show.
    const bool adjustable = operation == "inset" || operation == "bevel" || operation == "loopcut" || operation == "subdivide";
    if ((operation == "inset" || operation == "bevel") && !cmd.find("amount")) cmd.set("amount", Json::fromFloat(MeshOpParams{}.amount));
    if ((operation == "loopcut" || operation == "subdivide") && !cmd.find("cuts")) cmd.set("cuts", Json(1));
    Json result;
    std::string error;
    if (!executeEdit(cmd, result, error)) return failure(error);
    showResult(result);
    lastOp_ = adjustable ? LastOperation{operation, cmd, stack_.revision()} : LastOperation{};
    ++uiChanges_;
    Json out = Json::object();
    out.set("ok", true);
    out.set("result", result);
    return out;
}

Json Editor::adjustLastOperation(const Json& params) {
    if (lastOp_.operation.empty() || lastOp_.revision != stack_.revision() || !editing())
        return failure("there's no operation to adjust");
    Json cmd = lastOp_.params;
    for (const char* key : {"amount", "cuts"})
        if (const Json* v = params.find(key)) cmd.set(key, *v);
    const LastOperation last = lastOp_;
    undo();  // back to the mesh (and selection) it ran on
    Json result;
    std::string error;
    if (!executeEdit(cmd, result, error)) {
        redo();  // keep the previous result
        lastOp_ = last;
        lastOp_.revision = stack_.revision();
        return failure(error);
    }
    showResult(result);
    lastOp_ = {last.operation, cmd, stack_.revision()};
    ++uiChanges_;
    Json out = Json::object();
    out.set("ok", true);
    out.set("result", result);
    return out;
}

// An operation's result selection is on the new mesh: show it in the current mode
// (faces an operation leaves, seen in vertex mode, select their vertices).
void Editor::showResult(const Json& result) {
    const Object* o = scene_.find(editObject());
    if (!o) return;
    const MeshSelection next = selectionFromJson(result);
    MeshSelection verts;
    for (const SelectMode m : {SelectMode::Vertex, SelectMode::Edge, SelectMode::Face})
        for (int v : completed(next, m).verts) verts.verts.push_back(v);
    MeshSelection shown = completed(verts, SelectMode::Vertex);
    if (selectMode_ == SelectMode::Edge && !next.edges.empty()) shown = completed(next, SelectMode::Edge);
    if (selectMode_ == SelectMode::Face && !next.faces.empty()) shown = completed(next, SelectMode::Face);
    elements_[o->mesh] = completed(shown, selectMode_);
    ++uiChanges_;
}

bool Editor::extrudeAndMove(float x, float y, int width, int height) {
    const Json r = meshOperation("extrude", Json::object());
    if (!r.find("ok")->asBool()) return false;
    // Move along the extruded faces' average normal (area-weighted), in world space.
    const MeshEntry* e = editEntry();
    const Mat4 model = worldMatrix(scene_, *scene_.find(editObject_), time_);
    Vec3 normal{0, 0, 0};
    for (int f : elements().faces) normal = normal + e->edit->faceNormalUnnormalized(f);
    if (!beginTransform(TransformMode::Grab, x, y, width, height)) return false;
    const Vec3 world = (model * Vec4(normal, 0.0f)).xyz();  // a direction: w = 0, no translation
    if (length(world) > 1e-9f) tool_.setDirection(world);
    return true;
}

// ------------------------------------------------------------ vertex G/R/S

bool Editor::elementCenter(Vec3& center) const {
    const Object* o = scene_.find(editObject());
    if (!o) return false;
    const Mat4 model = worldMatrix(scene_, *o, time_);
    std::vector<Vec3> local;
    if (vertexTransform_) local = moveLocal_;  // the gizmo follows a drag
    else
        for (int v : elements().verts) local.push_back(editEntry()->edit->verts()[v].position);
    if (local.empty()) return false;
    Vec3 sum{0, 0, 0};
    for (const Vec3& p : local) sum = sum + p;
    center = (model * Vec4(sum * (1.0f / local.size()), 1.0f)).xyz();
    return true;
}

bool Editor::beginVertexTransform(TransformMode mode, float x, float y, int width, int height) {
    const MeshEntry* e = editEntry();
    Vec3 center;
    if (!e || !elementCenter(center)) return false;
    setPlaying(false);
    const Mat4 model = worldMatrix(scene_, *scene_.find(editObject_), time_);
    moveVerts_ = elements().verts;
    moveLocal_.clear();
    moveStart_.clear();
    for (int v : moveVerts_) {
        moveLocal_.push_back(e->edit->verts()[v].position);
        moveStart_.push_back((model * Vec4(moveLocal_.back(), 1.0f)).xyz());
    }
    Transform pivot;
    pivot.position = center;
    Mat4 view, projection;
    viewMatrices(static_cast<float>(width) / height, view, projection);
    tool_.begin(mode, pivot, view, projection, width, height, x, y);
    vertexTransform_ = true;
    preview_ = *e->edit;
    previewRender_ = *e->render;
    ++uiChanges_;
    return true;
}

// The same math as for objects (applyTransform), on points: scale and turn each
// vertex's offset from the pivot, then move. The preview is rebuilt every time; the
// stored mesh only changes when the move is confirmed.
void Editor::applyVertexTransform() {
    const MeshEntry* e = editEntry();
    if (!e) return;
    Mat4 inverse;
    if (!invert(worldMatrix(scene_, *scene_.find(editObject_), time_), inverse)) return;
    const Transform& pivot = tool_.start();
    const Transform& change = tool_.result();
    const Vec3 offset = change.position - pivot.position;
    const Vec3& f = change.scale;
    for (size_t i = 0; i < moveVerts_.size(); ++i) {
        const Vec3 rel = moveStart_[i] - pivot.position;
        const Vec3 w = pivot.position + offset + change.rotation.rotate({rel.x * f.x, rel.y * f.y, rel.z * f.z});
        moveLocal_[i] = (inverse * Vec4(w, 1.0f)).xyz();
    }
    MeshSelection sel;
    sel.verts = moveVerts_;
    MeshOpParams params;
    params.positions = moveLocal_;
    EditMesh moved;
    MeshSelection unused;
    std::string error;
    if (runMeshOp(*e->edit, "move", sel, params, moved, unused, error)) {
        preview_ = std::move(moved);
        previewRender_ = preview_.triangulate();
    }
    ++uiChanges_;
}

void Editor::confirmVertexTransform() {
    const Transform &a = tool_.start(), &b = tool_.result();
    auto same = [](const Vec3& u, const Vec3& v) { return u.x == v.x && u.y == v.y && u.z == v.z; };
    const bool moved = !same(a.position, b.position) || !same(a.scale, b.scale) || a.rotation.x != b.rotation.x ||
                       a.rotation.y != b.rotation.y || a.rotation.z != b.rotation.z || a.rotation.w != b.rotation.w;
    vertexTransform_ = false;
    tool_.end();
    gizmoDrag_ = false;
    ++uiChanges_;
    const Object* o = scene_.find(editObject());
    if (!moved || !o) return;
    const MeshSelection before = elements();
    const size_t vertCount = editEntry()->edit->verts().size();
    Json cmd = Json::object(), positions = Json::array();
    cmd.set("op", "editMesh");
    cmd.set("id", Json(o->id));
    cmd.set("operation", "move");
    cmd.set("verts", intArray(moveVerts_));
    for (const Vec3& p : moveLocal_) positions.push(vec3ToJson(p));
    cmd.set("positions", positions);
    Json result;
    std::string error;
    if (!executeEdit(cmd, result, error)) return;  // beyond +-1e6: it stays where it was
    o = scene_.find(editObject_);
    // Moving keeps every index (unless the mesh had unused vertices, which the
    // rebuild drops): the same elements stay selected.
    elements_[o->mesh] = editEntry()->edit->verts().size() == vertCount ? before : completed(selectionFromJson(result), SelectMode::Vertex);
    lastOp_ = {};
}

// ------------------------------------------------------------ for the AI

Json Editor::meshSummary(ObjectId id) const {
    const Object* o = scene_.find(id);
    const MeshEntry* e = o && o->type == ObjectType::Mesh ? scene_.mesh(o->mesh) : nullptr;
    if (!e) return failure("no mesh object with that id");
    const EditMesh& m = *e->edit;
    constexpr size_t kMaxListed = 1500;  // a bigger mesh costs too many tokens to list
    Json s = Json::object();
    s.set("ok", true);
    s.set("id", Json(id));
    s.set("name", o->name);
    s.set("vertCount", Json(static_cast<int64_t>(m.verts().size())));
    s.set("faceCount", Json(static_cast<int64_t>(m.faces().size())));
    if (m.verts().size() <= kMaxListed && m.faces().size() <= kMaxListed) {
        Json verts = Json::array(), faces = Json::array();
        for (const auto& v : m.verts()) {
            Json p = Json::array();
            for (float c : {v.position.x, v.position.y, v.position.z}) p.push(rounded(c));
            verts.push(p);
        }
        for (int f = 0; f < static_cast<int>(m.faces().size()); ++f) faces.push(intArray(faceVerts(m, f)));
        s.set("verts", verts);
        s.set("faces", faces);
    } else {
        s.set("note", "too many elements to list; operations can still use indices (e.g. faces 0..faceCount-1)");
    }
    if (id == editObject()) {
        s.set("editSelection", selectionJson(elements()));
        s.set("selectMode", modeName(selectMode_));
    }
    return s;
}

Json Editor::editState() const {
    Json e = Json::object();
    const MeshEntry* entry = editEntry();
    e.set("object", Json(editObject()));
    e.set("mode", modeName(selectMode_));
    const MeshSelection& sel = elements();
    Json counts = Json::object(), selected = Json::object();
    counts.set("verts", Json(static_cast<int64_t>(entry->edit->verts().size())));
    counts.set("edges", Json(static_cast<int64_t>(entry->edit->edges().size())));
    counts.set("faces", Json(static_cast<int64_t>(entry->edit->faces().size())));
    selected.set("verts", Json(static_cast<int64_t>(sel.verts.size())));
    selected.set("edges", Json(static_cast<int64_t>(sel.edges.size())));
    selected.set("faces", Json(static_cast<int64_t>(sel.faces.size())));
    e.set("counts", counts);
    e.set("selected", selected);
    if (!lastOp_.operation.empty() && lastOp_.revision == stack_.revision()) {
        Json last = Json::object();
        last.set("operation", lastOp_.operation);
        for (const char* key : {"amount", "cuts"})
            if (const Json* v = lastOp_.params.find(key)) last.set(key, *v);
        e.set("lastOp", last);
    }
    return e;
}
