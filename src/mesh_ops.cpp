#include "mesh_ops.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

namespace {

// ---------------------------------------------------------------- the polygon list

struct Poly {
    std::vector<int> verts;
    std::vector<Vec2> uvs;  // one per corner, or empty when the mesh has no UVs
    bool smooth = false;
};

struct PolyMesh {
    std::vector<Vec3> positions;
    std::vector<Poly> polys;
    bool hasUVs = false;

    int addVert(const Vec3& p) {
        positions.push_back(p);
        return static_cast<int>(positions.size()) - 1;
    }
};

PolyMesh toPolys(const EditMesh& m) {
    PolyMesh pm;
    pm.hasUVs = m.hasUVs;
    for (const auto& v : m.verts()) pm.positions.push_back(v.position);
    for (const auto& f : m.faces()) {  // poly i is face i
        Poly p;
        p.smooth = f.smooth;
        for (int l = f.loop, k = 0; k < f.count; ++k, l = m.loops()[l].next) {
            p.verts.push_back(m.loops()[l].vert);
            if (pm.hasUVs) p.uvs.push_back(m.loops()[l].uv);
        }
        pm.polys.push_back(std::move(p));
    }
    return pm;
}

using EdgeKey = std::pair<int, int>;
EdgeKey edgeKey(int a, int b) { return a < b ? EdgeKey{a, b} : EdgeKey{b, a}; }
Vec2 lerp2(const Vec2& a, const Vec2& b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }
Vec3 direction(const Vec3& v) {
    const float len = length(v);
    return len > 1e-12f ? v * (1.0f / len) : Vec3{0, 0, 0};
}
int wrap(int i, int n) { return (i % n + n) % n; }
Vec2 uvOf(const Poly& p, int corner) { return p.uvs.empty() ? Vec2{} : p.uvs[wrap(corner, static_cast<int>(p.verts.size()))]; }

// Appends a corner to a polygon being built (the UV only when the mesh has UVs).
void corner(const PolyMesh& pm, Poly& p, int vert, const Vec2& uv) {
    p.verts.push_back(vert);
    if (pm.hasUVs) p.uvs.push_back(uv);
}

// The polygons, back into an EditMesh: vertices no face uses are dropped and the rest
// renumbered, and the result selection (in polygon-list numbering) follows along.
bool build(const PolyMesh& pm, MeshSelection& sel, EditMesh& out, std::string& error) {
    std::vector<int> remap(pm.positions.size(), -1);
    EditMesh m;
    for (const Poly& p : pm.polys)
        for (int v : p.verts)
            if (remap[v] < 0) remap[v] = 0;  // used
    for (size_t i = 0; i < remap.size(); ++i)
        if (remap[i] == 0) remap[i] = m.addVert(pm.positions[i]);
    std::vector<int> faceRemap(pm.polys.size(), -1);
    for (size_t i = 0; i < pm.polys.size(); ++i) {
        std::vector<int> verts;
        for (int v : pm.polys[i].verts) verts.push_back(remap[v]);
        faceRemap[i] = m.addFace(verts, pm.polys[i].smooth, pm.hasUVs ? &pm.polys[i].uvs : nullptr);
    }
    m.hasUVs = pm.hasUVs;
    if (!m.validate(error)) return error = "the operation produced a broken mesh: " + error, false;

    MeshSelection mapped;
    for (int v : sel.verts)
        if (v >= 0 && v < static_cast<int>(remap.size()) && remap[v] >= 0) mapped.verts.push_back(remap[v]);
    for (const auto& [a, b] : sel.edges)
        if (remap[a] >= 0 && remap[b] >= 0 && m.findEdge(remap[a], remap[b]) != EditMesh::kNone)
            mapped.edges.push_back(edgeKey(remap[a], remap[b]));
    for (int f : sel.faces)
        if (f >= 0 && f < static_cast<int>(faceRemap.size()) && faceRemap[f] >= 0) mapped.faces.push_back(faceRemap[f]);
    for (auto* list : {&mapped.verts, &mapped.faces}) {  // sorted, no repeats
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    std::sort(mapped.edges.begin(), mapped.edges.end());
    mapped.edges.erase(std::unique(mapped.edges.begin(), mapped.edges.end()), mapped.edges.end());
    sel = mapped;
    out = std::move(m);
    return true;
}

// ---------------------------------------------------------------- extrude

bool extrude(PolyMesh& pm, const MeshSelection& sel, MeshSelection& result, std::string& error) {
    if (!sel.faces.empty()) {
        // A region of faces: its inside vertices move with it, its boundary edges
        // (used by one selected face only: the reverse direction isn't in the region)
        // grow side walls back to where the region was. Each vertex gets one copy per
        // "wedge": the selected faces around it that are linked through selected
        // edges at that vertex. Usually that's all of them, but where the region only
        // touches itself at a corner (two faces meeting at a point, or a boundary that
        // passes through the vertex twice), each wedge needs its own copy, or four
        // walls would share one edge.
        const std::vector<int> faces(sel.faces.begin(), sel.faces.end());
        std::map<std::pair<int, int>, int> directed;  // directed edge -> index into faces
        for (size_t k = 0; k < faces.size(); ++k) {
            const auto& v = pm.polys[faces[k]].verts;
            for (size_t i = 0; i < v.size(); ++i) directed[{v[i], v[(i + 1) % v.size()]}] = static_cast<int>(k);
        }
        // Union-find over (face, vertex) corners: two faces' corners at v are in the same
        // wedge when the faces share a selected edge that ends at v.
        std::map<std::pair<int, int>, std::pair<int, int>> parent;  // (face index, v) -> parent corner
        std::function<std::pair<int, int>(std::pair<int, int>)> root = [&](std::pair<int, int> c) {
            auto it = parent.find(c);
            if (it == parent.end() || it->second == c) return c;
            return it->second = root(it->second);
        };
        for (const auto& [edge, k] : directed) {
            const auto other = directed.find({edge.second, edge.first});
            if (other == directed.end()) continue;
            for (int v : {edge.first, edge.second}) {
                const auto a = root({k, v}), b = root({other->second, v});
                if (a != b) parent[a] = b;
            }
        }
        std::map<std::pair<int, int>, int> copy;  // wedge root -> the copy of its vertex
        std::vector<Poly> walls;
        for (size_t k = 0; k < faces.size(); ++k) {
            Poly& p = pm.polys[faces[k]];
            auto copyOf = [&](int v) {
                const auto key = root({static_cast<int>(k), v});
                if (!copy.count(key)) copy[key] = pm.addVert(pm.positions[v]);
                return copy[key];
            };
            const int n = static_cast<int>(p.verts.size());
            for (int i = 0; i < n; ++i) {
                const int a = p.verts[i], b = p.verts[(i + 1) % n];
                if (directed.count({b, a})) continue;  // an inside edge
                // The wall runs a -> b along the old boundary (matching the unselected
                // neighbor's b -> a), then up to the copies, which the moved face
                // runs a' -> b'.
                Poly wall;
                wall.smooth = p.smooth;
                corner(pm, wall, a, uvOf(p, i));
                corner(pm, wall, b, uvOf(p, i + 1));
                corner(pm, wall, copyOf(b), uvOf(p, i + 1));
                corner(pm, wall, copyOf(a), uvOf(p, i));
                walls.push_back(wall);
            }
            for (int& v : p.verts) v = copyOf(v);
        }
        pm.polys.insert(pm.polys.end(), walls.begin(), walls.end());
        result.faces = sel.faces;
        for (const auto& [from, to] : copy) result.verts.push_back(to);
        return true;
    }
    if (sel.edges.empty()) return error = "select faces (or open edges) to extrude", false;
    // Open edges (with one face): each grows a new face, sharing copies along a chain.
    std::map<EdgeKey, std::vector<std::pair<int, int>>> uses;  // edge -> (poly, corner where it starts)
    for (size_t f = 0; f < pm.polys.size(); ++f) {
        const auto& v = pm.polys[f].verts;
        for (size_t i = 0; i < v.size(); ++i) uses[edgeKey(v[i], v[(i + 1) % v.size()])].push_back({int(f), int(i)});
    }
    std::map<int, int> copy;
    for (const auto& [a, b] : sel.edges) {
        const auto& u = uses[edgeKey(a, b)];
        if (u.size() != 1) return error = "only open edges (with one face) can be extruded: select faces to extrude a region", false;
        for (int v : {a, b})
            if (!copy.count(v)) copy[v] = pm.addVert(pm.positions[v]);
    }
    for (const auto& [a, b] : sel.edges) {
        const auto [f, i] = uses[edgeKey(a, b)][0];
        const Poly& p = pm.polys[f];
        const int from = p.verts[i], to = p.verts[(i + 1) % p.verts.size()];
        Poly wall;  // runs to -> from along the edge (opposite to its face), then out to the copies
        wall.smooth = p.smooth;
        corner(pm, wall, to, uvOf(p, i + 1));
        corner(pm, wall, from, uvOf(p, i));
        corner(pm, wall, copy[from], uvOf(p, i));
        corner(pm, wall, copy[to], uvOf(p, i + 1));
        pm.polys.push_back(wall);
        result.edges.push_back(edgeKey(copy[a], copy[b]));
    }
    for (const auto& [from, to] : copy) result.verts.push_back(to);
    return true;
}

// ---------------------------------------------------------------- inset

bool inset(PolyMesh& pm, const MeshSelection& sel, float amount, MeshSelection& result, std::string& error) {
    if (sel.faces.empty()) return error = "select faces to inset", false;
    for (int f : sel.faces) {
        // Each corner moves `amount` toward the face's center (at most most of the way),
        // giving the inner face; quads ring the gap.
        const Poly p = pm.polys[f];
        const int n = static_cast<int>(p.verts.size());
        Vec3 center{0, 0, 0};
        Vec2 uvCenter{0, 0};
        for (int i = 0; i < n; ++i) {
            center = center + pm.positions[p.verts[i]] * (1.0f / n);
            const Vec2 uv = uvOf(p, i);
            uvCenter = {uvCenter.x + uv.x / n, uvCenter.y + uv.y / n};
        }
        Poly inner;
        inner.smooth = p.smooth;
        for (int i = 0; i < n; ++i) {
            const Vec3 at = pm.positions[p.verts[i]];
            const float toCenter = length(center - at);
            const float t = toCenter > 1e-12f ? std::min(amount, 0.95f * toCenter) / toCenter : 0.0f;
            corner(pm, inner, pm.addVert(at + (center - at) * t), lerp2(uvOf(p, i), uvCenter, t));
        }
        for (int i = 0; i < n; ++i) {
            Poly ring;  // the old edge i -> i+1, then in to the inner face's matching edge
            ring.smooth = p.smooth;
            corner(pm, ring, p.verts[i], uvOf(p, i));
            corner(pm, ring, p.verts[(i + 1) % n], uvOf(p, i + 1));
            corner(pm, ring, inner.verts[(i + 1) % n], uvOf(inner, i + 1));
            corner(pm, ring, inner.verts[i], uvOf(inner, i));
            pm.polys.push_back(ring);
        }
        pm.polys[f] = inner;
        result.verts.insert(result.verts.end(), inner.verts.begin(), inner.verts.end());
    }
    result.faces = sel.faces;
    return true;
}

// ---------------------------------------------------------------- subdivide

void subdivideOnce(PolyMesh& pm, std::vector<int>& faces) {
    const std::set<int> chosen(faces.begin(), faces.end());
    std::map<EdgeKey, int> middle;  // the new vertex halfway along each edge of a chosen face
    for (int f : chosen) {
        const auto& v = pm.polys[f].verts;
        for (size_t i = 0; i < v.size(); ++i) {
            const int a = v[i], b = v[(i + 1) % v.size()];
            if (!middle.count(edgeKey(a, b))) middle[edgeKey(a, b)] = pm.addVert((pm.positions[a] + pm.positions[b]) * 0.5f);
        }
    }
    std::vector<Poly> polys;
    std::vector<int> made;
    for (size_t f = 0; f < pm.polys.size(); ++f) {
        const Poly& p = pm.polys[f];
        const int n = static_cast<int>(p.verts.size());
        auto mid = [&](int i) { return middle[edgeKey(p.verts[wrap(i, n)], p.verts[wrap(i + 1, n)])]; };
        auto midUv = [&](int i) { return lerp2(uvOf(p, i), uvOf(p, i + 1), 0.5f); };
        if (!chosen.count(static_cast<int>(f))) {
            // A neighbor: just gains the midpoints on edges it shares, so no cracks open.
            Poly q;
            q.smooth = p.smooth;
            for (int i = 0; i < n; ++i) {
                corner(pm, q, p.verts[i], uvOf(p, i));
                if (middle.count(edgeKey(p.verts[i], p.verts[(i + 1) % n]))) corner(pm, q, mid(i), midUv(i));
            }
            polys.push_back(q);
            continue;
        }
        if (n == 3) {  // a triangle: three corner triangles and the middle one
            for (int i = 0; i < 3; ++i) {
                Poly q;
                q.smooth = p.smooth;
                corner(pm, q, p.verts[i], uvOf(p, i));
                corner(pm, q, mid(i), midUv(i));
                corner(pm, q, mid(i - 1), midUv(i - 1));
                made.push_back(static_cast<int>(polys.size()));
                polys.push_back(q);
            }
            Poly q;
            q.smooth = p.smooth;
            for (int i = 0; i < 3; ++i) corner(pm, q, mid(i), midUv(i));
            made.push_back(static_cast<int>(polys.size()));
            polys.push_back(q);
            continue;
        }
        // Anything else: one quad per corner, meeting at a new center vertex.
        Vec3 center{0, 0, 0};
        Vec2 uvCenter{0, 0};
        for (int i = 0; i < n; ++i) {
            center = center + pm.positions[p.verts[i]] * (1.0f / n);
            uvCenter = {uvCenter.x + uvOf(p, i).x / n, uvCenter.y + uvOf(p, i).y / n};
        }
        const int c = pm.addVert(center);
        for (int i = 0; i < n; ++i) {
            Poly q;
            q.smooth = p.smooth;
            corner(pm, q, p.verts[i], uvOf(p, i));
            corner(pm, q, mid(i), midUv(i));
            corner(pm, q, c, uvCenter);
            corner(pm, q, mid(i - 1), midUv(i - 1));
            made.push_back(static_cast<int>(polys.size()));
            polys.push_back(q);
        }
    }
    pm.polys = polys;
    faces = made;
}

// ---------------------------------------------------------------- merge, delete, move

bool merge(PolyMesh& pm, const MeshSelection& sel, MeshSelection& result, std::string& error) {
    if (sel.verts.size() < 2) return error = "select at least two vertices to merge", false;
    const int keep = sel.verts[0];
    Vec3 center{0, 0, 0};
    for (int v : sel.verts) center = center + pm.positions[v] * (1.0f / sel.verts.size());
    pm.positions[keep] = center;
    const std::set<int> gone(sel.verts.begin(), sel.verts.end());
    std::vector<Poly> polys;
    for (const Poly& p : pm.polys) {
        // Point merged corners at `keep`, then drop corners that now repeat their
        // neighbor; a face left with under 3 corners (or a corner twice) is gone.
        Poly q;
        q.smooth = p.smooth;
        for (size_t i = 0; i < p.verts.size(); ++i) {
            const int v = gone.count(p.verts[i]) ? keep : p.verts[i];
            if (!q.verts.empty() && q.verts.back() == v) continue;
            corner(pm, q, v, uvOf(p, static_cast<int>(i)));
        }
        while (q.verts.size() > 1 && q.verts.back() == q.verts.front()) {
            q.verts.pop_back();
            if (!q.uvs.empty()) q.uvs.pop_back();
        }
        std::set<int> distinct(q.verts.begin(), q.verts.end());
        if (q.verts.size() >= 3 && distinct.size() == q.verts.size()) polys.push_back(q);
    }
    pm.polys = polys;
    result.verts = {keep};
    return true;
}

bool remove(PolyMesh& pm, const MeshSelection& sel, const std::string& what, std::string& error) {
    std::vector<Poly> polys;
    const std::set<int> verts(sel.verts.begin(), sel.verts.end()), faces(sel.faces.begin(), sel.faces.end());
    const std::set<EdgeKey> edges(sel.edges.begin(), sel.edges.end());
    const bool useVerts = what == "verts", useEdges = what == "edges", useFaces = what == "faces";
    if (!useVerts && !useEdges && !useFaces) return error = "delete what: verts, edges, or faces", false;
    if ((useVerts && verts.empty()) || (useEdges && edges.empty()) || (useFaces && faces.empty()))
        return error = "nothing selected to delete", false;
    for (size_t f = 0; f < pm.polys.size(); ++f) {
        const auto& v = pm.polys[f].verts;
        bool doomed = useFaces && faces.count(static_cast<int>(f));
        for (size_t i = 0; i < v.size() && !doomed; ++i)
            doomed = (useVerts && verts.count(v[i])) || (useEdges && edges.count(edgeKey(v[i], v[(i + 1) % v.size()])));
        if (!doomed) polys.push_back(pm.polys[f]);
    }
    pm.polys = polys;  // vertices nothing uses anymore disappear when the mesh is rebuilt
    return true;
}

// ---------------------------------------------------------------- bevel

// The faces around vertex v in rotational order, walking corner to corner across
// shared edges: from v's corner in one face (whose edge leads v -> w), the other face
// on that edge runs w -> v, and its next corner is v again. False unless the faces
// close up around v with every edge shared by exactly two, oppositely wound faces.
bool fanAround(const EditMesh& m, int v, std::vector<int>& faces) {
    const auto& loops = m.loops();
    const int e = m.verts()[v].edge;
    if (e == EditMesh::kNone || m.edges()[e].loop == EditMesh::kNone) return false;
    int start = m.edges()[e].loop;
    if (loops[start].vert != v) start = loops[start].next;  // the loop on this edge that starts at v
    if (loops[start].vert != v) return false;
    int l = start;
    for (int guard = 0; guard < 1000; ++guard) {
        faces.push_back(loops[l].face);
        const int other = loops[l].radialNext;
        if (other == l || loops[other].radialNext != l || loops[other].vert == v) return false;
        l = loops[other].next;
        if (l == start) return true;
    }
    return false;
}

bool bevel(const EditMesh& m, PolyMesh& pm, const MeshSelection& sel, float amount, MeshSelection& result,
           std::string& error) {
    if (sel.edges.empty()) return error = "select edges to bevel", false;
    const std::set<EdgeKey> beveled(sel.edges.begin(), sel.edges.end());
    std::set<int> touched;
    for (const auto& [a, b] : beveled) touched.insert(a), touched.insert(b);
    std::map<int, std::vector<int>> fans;
    float shortest = 1e30f;
    for (int v : touched) {
        if (!fanAround(m, v, fans[v])) return error = "bevel needs the edges to be inside a closed surface", false;
        for (int f : fans[v]) {
            const auto& p = pm.polys[f].verts;
            for (size_t i = 0; i < p.size(); ++i)
                shortest = std::min(shortest, length(pm.positions[p[i]] - pm.positions[p[(i + 1) % p.size()]]));
        }
    }
    // Points on an edge from both of its ends must not cross: keep under half an edge.
    const float o = std::min(amount, 0.45f * shortest);
    if (!(o > 0)) return error = "the bevel amount must be positive", false;

    auto isBeveled = [&](int a, int b) { return beveled.count(edgeKey(a, b)) > 0; };
    // Pass 1: which edges get a point near v. A face with exactly one beveled edge at
    // v slides v along its other edge; that point is shared by both faces of the edge.
    std::set<std::pair<int, int>> slides;  // (v, w): a point on edge v-w, `o` from v
    for (const Poly& p : pm.polys) {
        const int n = static_cast<int>(p.verts.size());
        for (int i = 0; i < n; ++i) {
            const int v = p.verts[i], prev = p.verts[wrap(i - 1, n)], next = p.verts[wrap(i + 1, n)];
            if (!touched.count(v)) continue;
            const bool bp = isBeveled(prev, v), bn = isBeveled(v, next);
            if (bp && !bn) slides.insert({v, next});
            if (bn && !bp) slides.insert({v, prev});
        }
    }
    std::map<std::pair<int, int>, int> slidePoint;  // (v, w) -> new vertex
    for (const auto& [v, w] : slides)
        slidePoint[{v, w}] = pm.addVert(pm.positions[v] + direction(pm.positions[w] - pm.positions[v]) * o);

    // Pass 2: rewrite each face, replacing v's corner by its new points. Remember what
    // each corner became (and the UVs, which are per face) for the strips and corners.
    struct Emitted {
        std::vector<int> verts;
        std::vector<Vec2> uvs;
    };
    std::map<std::pair<int, int>, Emitted> emitted;  // (face, v) -> its corner's replacement
    std::vector<Poly> polys;
    for (size_t f = 0; f < pm.polys.size(); ++f) {
        const Poly& p = pm.polys[f];
        const int n = static_cast<int>(p.verts.size());
        Poly q;
        q.smooth = p.smooth;
        for (int i = 0; i < n; ++i) {
            const int v = p.verts[i], prev = p.verts[wrap(i - 1, n)], next = p.verts[wrap(i + 1, n)];
            if (!touched.count(v)) {
                corner(pm, q, v, uvOf(p, i));
                continue;
            }
            const Vec3 at = pm.positions[v];
            const float lp = length(pm.positions[prev] - at), ln = length(pm.positions[next] - at);
            const Vec2 uvP = lerp2(uvOf(p, i), uvOf(p, i - 1), lp > 0 ? o / lp : 0),
                       uvN = lerp2(uvOf(p, i), uvOf(p, i + 1), ln > 0 ? o / ln : 0);
            const bool bp = isBeveled(prev, v), bn = isBeveled(v, next);
            Emitted& e = emitted[{static_cast<int>(f), v}];
            auto put = [&](int vert, const Vec2& uv) { e.verts.push_back(vert), e.uvs.push_back(uv); };
            if (bp && bn) {  // between two beveled edges: a point inside the face, `o` in from both
                const Vec3 inside = at + direction(pm.positions[prev] - at) * o + direction(pm.positions[next] - at) * o;
                const Vec2 uv0 = uvOf(p, i);
                put(pm.addVert(inside), {uvP.x + uvN.x - uv0.x, uvP.y + uvN.y - uv0.y});
            } else if (bp) {
                put(slidePoint[{v, next}], uvN);
            } else if (bn) {
                put(slidePoint[{v, prev}], uvP);
            } else {  // neither: use the edges' points if the faces across made them, else keep v
                const bool hasP = slidePoint.count({v, prev}) > 0, hasN = slidePoint.count({v, next}) > 0;
                if (hasP) put(slidePoint[{v, prev}], uvP);
                if (!(hasP && hasN)) put(v, uvOf(p, i));
                if (hasN) put(slidePoint[{v, next}], uvN);
            }
            for (size_t k = 0; k < e.verts.size(); ++k) corner(pm, q, e.verts[k], e.uvs[k]);
        }
        polys.push_back(q);
    }

    std::vector<int> made;
    // One strip per beveled edge, between the points its two faces put next to it.
    // In the face running a -> b they're A1, B1; in the one running b -> a, A2, B2.
    for (const auto& [a, b] : beveled) {
        int f1 = -1, f2 = -1;
        for (int f : fans[a]) {
            const auto& v = pm.polys[f].verts;
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i] == a && v[(i + 1) % v.size()] == b) f1 = f;
                if (v[i] == b && v[(i + 1) % v.size()] == a) f2 = f;
            }
        }
        if (f1 < 0 || f2 < 0) return error = "bevel needs the edges to be inside a closed surface", false;
        const Emitted &a1 = emitted[{f1, a}], &b1 = emitted[{f1, b}], &a2 = emitted[{f2, a}], &b2 = emitted[{f2, b}];
        // B1 -> A1 (against f1's A1 -> B1), across to A2 -> B2 (against f2), and back.
        // Where both faces put the same point at an end (a vertex with only those two
        // faces), the quad is really a triangle.
        const int pts[4] = {b1.verts.front(), a1.verts.back(), a2.verts.front(), b2.verts.back()};
        const Vec2 uvs[4] = {b1.uvs.front(), a1.uvs.back(), a2.uvs.front(), b2.uvs.back()};
        Poly strip;
        strip.smooth = pm.polys[f1].smooth;
        for (int k = 0; k < 4; ++k)
            if (pts[k] != pts[(k + 3) % 4]) corner(pm, strip, pts[k], uvs[k]);  // skip a repeat of the previous corner
        if (strip.verts.size() < 3) continue;
        made.push_back(static_cast<int>(polys.size()));
        polys.push_back(strip);
    }
    // Where three or more distinct points ring a vertex, a corner face fills the gap.
    std::set<std::pair<int, int>> directed;  // every directed edge of the faces so far
    for (const Poly& p : polys)
        for (size_t i = 0; i < p.verts.size(); ++i) directed.insert({p.verts[i], p.verts[(i + 1) % p.verts.size()]});
    for (int v : touched) {
        std::vector<int> ring;
        std::vector<Vec2> uvs;
        for (int f : fans[v]) {
            const Emitted& e = emitted[{f, v}];
            for (size_t k = 0; k < e.verts.size(); ++k)
                if (ring.empty() || ring.back() != e.verts[k]) ring.push_back(e.verts[k]), uvs.push_back(e.uvs[k]);
        }
        while (ring.size() > 1 && ring.back() == ring.front()) ring.pop_back(), uvs.pop_back();
        // The original vertex can survive in more than one gap (two beveled edges that
        // aren't neighbors around it, like at the tip of a cone): then the ring passes
        // through it more than once, and each stretch from one pass to the next is its
        // own corner face.
        std::vector<std::vector<size_t>> pieces;
        const auto first = std::find(ring.begin(), ring.end(), v);
        if (first == ring.end()) {
            pieces.push_back({});
            for (size_t k = 0; k < ring.size(); ++k) pieces.back().push_back(k);
        } else {
            const size_t at = static_cast<size_t>(first - ring.begin());
            for (size_t k = 0; k < ring.size(); ++k) {
                const size_t idx = (at + k) % ring.size();
                if (ring[idx] == v) pieces.push_back({});
                pieces.back().push_back(idx);
            }
        }
        for (const auto& piece : pieces) {
            if (piece.size() < 3) continue;
            std::vector<int> cap;
            std::vector<Vec2> capUvs;
            for (size_t idx : piece) cap.push_back(ring[idx]), capUvs.push_back(uvs[idx]);
            // Wind it like its neighbors: each of its edges must run opposite to the
            // face or strip already using that edge. (Decided by topology, not by a
            // normal: a corner can be a sliver whose normal is too small to trust.)
            bool reverse = false;
            for (size_t k = 0; k < cap.size(); ++k) {
                const auto edge = std::make_pair(cap[k], cap[(k + 1) % cap.size()]);
                if (directed.count(edge)) {
                    reverse = true;
                    break;
                }
                if (directed.count({edge.second, edge.first})) break;
            }
            if (reverse) std::reverse(cap.begin(), cap.end()), std::reverse(capUvs.begin(), capUvs.end());
            Poly face;
            face.smooth = pm.polys[fans[v][0]].smooth;
            for (size_t k = 0; k < cap.size(); ++k) corner(pm, face, cap[k], capUvs[k]);
            made.push_back(static_cast<int>(polys.size()));
            polys.push_back(face);
        }
    }
    pm.polys = polys;
    result.faces = made;
    return true;
}

// ---------------------------------------------------------------- loop cut

bool loopCut(PolyMesh& pm, const MeshSelection& sel, int cuts, MeshSelection& result, std::string& error) {
    if (sel.edges.empty()) return error = "select an edge to cut across", false;
    cuts = std::max(1, std::min(cuts, 64));
    std::map<EdgeKey, std::vector<int>> facesOf;
    for (size_t f = 0; f < pm.polys.size(); ++f) {
        const auto& v = pm.polys[f].verts;
        for (size_t i = 0; i < v.size(); ++i) facesOf[edgeKey(v[i], v[(i + 1) % v.size()])].push_back(int(f));
    }
    // The ring: from the edge, across each quad to its opposite edge, and on into the
    // next quad, both ways, until it closes or meets a non-quad or an open edge. Ring
    // edges are kept "parallel": each x -> y has its x on the same side as the last.
    const EdgeKey first = edgeKey(sel.edges[0].first, sel.edges[0].second);
    if (!facesOf.count(first)) return error = "that edge isn't in the mesh", false;
    struct Step {
        std::pair<int, int> edge;  // x -> y
        int quad = -1;             // the quad between this edge and the next (-1: none)
    };
    std::set<int> visited;
    bool crossed = false;
    auto walk = [&](std::pair<int, int> edge, int face, std::vector<Step>& steps, bool& closed) {
        for (int guard = 0; guard < 100000; ++guard) {
            const auto& v = pm.polys[face].verts;
            if (v.size() != 4) return;  // the ring ends at a non-quad
            if (!visited.insert(face).second) {  // back through a quad by its other edges
                crossed = true;
                return;
            }
            int i = 0;
            while (!(edgeKey(v[i], v[(i + 1) % 4]) == edgeKey(edge.first, edge.second))) ++i;
            // The opposite edge is v[i+2] - v[i+3]; v[i+3] is next to v[i].
            const bool forward = v[i] == edge.first;
            const std::pair<int, int> opposite =
                forward ? std::make_pair(v[(i + 3) % 4], v[(i + 2) % 4]) : std::make_pair(v[(i + 2) % 4], v[(i + 3) % 4]);
            steps.back().quad = face;
            if (edgeKey(opposite.first, opposite.second) == first) {
                closed = true;
                return;
            }
            steps.push_back({opposite, -1});
            const auto& across = facesOf[edgeKey(opposite.first, opposite.second)];
            if (across.size() != 2) return;  // an open (or non-manifold) edge
            face = across[0] == face ? across[1] : across[0];
            edge = opposite;
        }
    };
    const auto& startFaces = facesOf[first];
    std::vector<Step> ahead = {{first, -1}};
    bool closed = false;
    walk(first, startFaces[0], ahead, closed);
    std::vector<Step> ring;
    if (!closed && startFaces.size() == 2) {
        std::vector<Step> behind = {{first, -1}};
        bool alsoClosed = false;
        walk(first, startFaces[1], behind, alsoClosed);
        // Walked the other way: reverse it so the quads sit between consecutive edges.
        for (size_t k = behind.size() - 1; k > 0; --k) ring.push_back({behind[k].edge, behind[k - 1].quad});
    }
    ring.insert(ring.end(), ahead.begin(), ahead.end());
    if (crossed) return error = "the ring of quads through that edge crosses itself: pick another edge", false;
    bool anyQuad = false;
    for (const Step& s : ring) anyQuad = anyQuad || s.quad >= 0;
    if (!anyQuad) return error = "there are no quads across that edge to cut", false;

    // Cut points along every ring edge: points[r][k] at k / (cuts + 1) from x to y.
    std::vector<std::vector<int>> points(ring.size());
    std::map<EdgeKey, int> ringIndex;
    for (size_t r = 0; r < ring.size(); ++r) {
        const auto [x, y] = ring[r].edge;
        ringIndex[edgeKey(x, y)] = static_cast<int>(r);
        points[r].push_back(x);
        for (int k = 1; k <= cuts; ++k) {
            const float t = static_cast<float>(k) / (cuts + 1);
            points[r].push_back(pm.addVert(pm.positions[x] + (pm.positions[y] - pm.positions[x]) * t));
        }
        points[r].push_back(y);
    }
    std::set<int> ringQuads;
    for (const Step& s : ring)
        if (s.quad >= 0) ringQuads.insert(s.quad);
    std::vector<Poly> polys;
    for (size_t f = 0; f < pm.polys.size(); ++f) {
        const Poly& p = pm.polys[f];
        const int n = static_cast<int>(p.verts.size());
        if (!ringQuads.count(static_cast<int>(f))) {
            // A face at either end of the ring: gains the cut points on its ring edge.
            Poly q;
            q.smooth = p.smooth;
            for (int i = 0; i < n; ++i) {
                corner(pm, q, p.verts[i], uvOf(p, i));
                const auto it = ringIndex.find(edgeKey(p.verts[i], p.verts[(i + 1) % n]));
                if (it == ringIndex.end()) continue;
                const auto& pts = points[it->second];
                const bool along = p.verts[i] == pts.front();
                for (int k = 1; k <= cuts; ++k) {
                    const int idx = along ? k : cuts + 1 - k;
                    corner(pm, q, pts[idx], lerp2(uvOf(p, i), uvOf(p, i + 1), static_cast<float>(k) / (cuts + 1)));
                }
            }
            polys.push_back(q);
            continue;
        }
        // A ring quad between ring edges r and r+1: cuts + 1 strips across it.
        size_t r = 0;
        while (ring[r].quad != static_cast<int>(f)) ++r;
        const size_t s = (r + 1) % ring.size();
        const auto& A = points[r];
        const auto& B = points[s];
        auto uvAt = [&](int vert) {
            for (int i = 0; i < n; ++i)
                if (p.verts[i] == vert) return uvOf(p, i);
            return Vec2{};
        };
        const Vec2 ax = uvAt(A.front()), ay = uvAt(A.back()), bx = uvAt(B.front()), by = uvAt(B.back());
        int i = 0;
        while (p.verts[i] != A.front()) ++i;
        const bool forward = p.verts[(i + 1) % 4] == A.back();  // the quad runs x -> y along edge r
        for (int k = 0; k <= cuts; ++k) {
            const float t0 = static_cast<float>(k) / (cuts + 1), t1 = static_cast<float>(k + 1) / (cuts + 1);
            Poly q;
            q.smooth = p.smooth;
            const int c[4] = {A[k], A[k + 1], B[k + 1], B[k]};
            const Vec2 u[4] = {lerp2(ax, ay, t0), lerp2(ax, ay, t1), lerp2(bx, by, t1), lerp2(bx, by, t0)};
            for (int j = 0; j < 4; ++j) {
                const int jj = forward ? j : 3 - j;  // same winding as the quad it replaces
                corner(pm, q, c[jj], u[jj]);
            }
            polys.push_back(q);
        }
    }
    pm.polys = polys;
    // The new loop(s): each cut's points joined across the ring.
    for (int k = 1; k <= cuts; ++k)
        for (size_t r = 0; r < ring.size(); ++r) {
            if (ring[r].quad < 0) continue;
            const size_t s = (r + 1) % ring.size();
            result.edges.push_back(edgeKey(points[r][k], points[s][k]));
            result.verts.push_back(points[r][k]);
            result.verts.push_back(points[s][k]);
        }
    return true;
}

}  // namespace

MeshSelection completeSelection(const EditMesh& mesh, const std::vector<int>& verts) {
    MeshSelection s;
    std::vector<bool> on(mesh.verts().size(), false);
    for (int v : verts)
        if (v >= 0 && v < static_cast<int>(on.size())) on[v] = true;
    for (size_t v = 0; v < on.size(); ++v)
        if (on[v]) s.verts.push_back(static_cast<int>(v));
    for (const auto& e : mesh.edges())
        if (on[e.v[0]] && on[e.v[1]]) s.edges.push_back(edgeKey(e.v[0], e.v[1]));
    for (size_t f = 0; f < mesh.faces().size(); ++f) {
        bool all = true;
        for (int l = mesh.faces()[f].loop, k = 0; k < mesh.faces()[f].count; ++k, l = mesh.loops()[l].next)
            all = all && on[mesh.loops()[l].vert];
        if (all) s.faces.push_back(static_cast<int>(f));
    }
    return s;
}

bool runMeshOp(const EditMesh& in, const std::string& operation, const MeshSelection& sel,
               const MeshOpParams& params, EditMesh& out, MeshSelection& selection, std::string& error) {
    // The selection must belong to this mesh.
    const int nv = static_cast<int>(in.verts().size()), nf = static_cast<int>(in.faces().size());
    for (int v : sel.verts)
        if (v < 0 || v >= nv) return error = "vertex " + std::to_string(v) + " isn't in the mesh", false;
    for (int f : sel.faces)
        if (f < 0 || f >= nf) return error = "face " + std::to_string(f) + " isn't in the mesh", false;
    for (const auto& [a, b] : sel.edges)
        if (a < 0 || b < 0 || a >= nv || b >= nv || in.findEdge(a, b) == EditMesh::kNone)
            return error = "there's no edge between vertices " + std::to_string(a) + " and " + std::to_string(b), false;

    PolyMesh pm = toPolys(in);
    MeshSelection result;
    bool ok = false;
    if (operation == "extrude") {
        ok = extrude(pm, sel, result, error);
    } else if (operation == "inset") {
        ok = inset(pm, sel, params.amount, result, error);
    } else if (operation == "bevel") {
        ok = bevel(in, pm, sel, params.amount, result, error);
    } else if (operation == "loopcut") {
        ok = loopCut(pm, sel, params.cuts, result, error);
    } else if (operation == "subdivide") {
        if (sel.faces.empty()) return error = "select faces to subdivide", false;
        std::vector<int> faces = sel.faces;
        for (int level = 0; level < std::max(1, std::min(params.cuts, 4)); ++level) subdivideOnce(pm, faces);
        result.faces = faces;
        ok = true;
    } else if (operation == "merge") {
        ok = merge(pm, sel, result, error);
    } else if (operation == "delete") {
        ok = remove(pm, sel, params.what, error);
    } else if (operation == "move") {
        if (sel.verts.size() != params.positions.size()) return error = "move needs one position per selected vertex", false;
        for (size_t i = 0; i < sel.verts.size(); ++i) pm.positions[sel.verts[i]] = params.positions[i];
        result.verts = sel.verts;
        ok = true;
    } else {
        return error = "unknown mesh operation '" + operation + "'", false;
    }
    if (!ok) return false;
    if (pm.polys.empty()) return error = "that would leave the mesh empty: delete the object instead", false;
    if (!build(pm, result, out, error)) return false;
    selection = result;
    return true;
}
