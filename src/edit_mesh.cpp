#include "edit_mesh.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

int EditMesh::addVert(const Vec3& position) {
    verts_.push_back({position, kNone});
    return static_cast<int>(verts_.size()) - 1;
}

EditMesh::DiskLink& EditMesh::diskOf(int edge, int vert) {
    Edge& e = edges_[edge];
    return e.disk[e.v[0] == vert ? 0 : 1];
}

const EditMesh::DiskLink& EditMesh::diskOf(int edge, int vert) const {
    const Edge& e = edges_[edge];
    return e.disk[e.v[0] == vert ? 0 : 1];
}

int EditMesh::findEdge(int a, int b) const {
    const int first = verts_[a].edge;
    if (first == kNone) return kNone;
    // Walk a's disk cycle: every edge touching a, once each.
    int e = first;
    do {
        const Edge& edge = edges_[e];
        if ((edge.v[0] == a && edge.v[1] == b) || (edge.v[0] == b && edge.v[1] == a)) return e;
        e = diskOf(e, a).next;
    } while (e != first);
    return kNone;
}

int EditMesh::addEdge(int a, int b) {
    const int existing = findEdge(a, b);
    if (existing != kNone) return existing;
    const int e = static_cast<int>(edges_.size());
    edges_.push_back({});
    edges_[e].v[0] = a;
    edges_[e].v[1] = b;
    // Splice the new edge into each endpoint's circular disk list.
    for (int vert : {a, b}) {
        DiskLink& link = diskOf(e, vert);
        const int first = verts_[vert].edge;
        if (first == kNone) {  // the vertex's first edge: a cycle of one
            link.next = link.prev = e;
            verts_[vert].edge = e;
        } else {  // insert after `first`
            DiskLink& firstLink = diskOf(first, vert);
            const int after = firstLink.next;
            link.prev = first;
            link.next = after;
            firstLink.next = e;
            diskOf(after, vert).prev = e;
        }
    }
    return e;
}

int EditMesh::addFace(const std::vector<int>& verts, bool smooth, const std::vector<Vec2>* uvs,
                      const std::vector<Vec3>* normals) {
    const int n = static_cast<int>(verts.size());
    if (n < 3) return kNone;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (verts[i] == verts[j]) return kNone;  // a vertex twice: no valid polygon

    const int face = static_cast<int>(faces_.size());
    const int firstLoop = static_cast<int>(loops_.size());
    faces_.push_back({firstLoop, n, smooth});
    for (int i = 0; i < n; ++i) {
        Loop loop;
        loop.vert = verts[i];
        loop.edge = addEdge(verts[i], verts[(i + 1) % n]);
        loop.face = face;
        loop.next = firstLoop + (i + 1) % n;
        loop.prev = firstLoop + (i + n - 1) % n;
        if (uvs) loop.uv = (*uvs)[i];
        if (normals) loop.normal = (*normals)[i];
        loops_.push_back(loop);
    }
    // Splice each loop into its edge's circular radial list.
    for (int l = firstLoop; l < firstLoop + n; ++l) {
        Edge& edge = edges_[loops_[l].edge];
        if (edge.loop == kNone) {
            loops_[l].radialNext = loops_[l].radialPrev = l;
            edge.loop = l;
        } else {
            const int first = edge.loop, after = loops_[first].radialNext;
            loops_[l].radialPrev = first;
            loops_[l].radialNext = after;
            loops_[first].radialNext = l;
            loops_[after].radialPrev = l;
        }
    }
    return face;
}

bool EditMesh::validate(std::string& error) const {
    const int nv = static_cast<int>(verts_.size()), ne = static_cast<int>(edges_.size()),
              nl = static_cast<int>(loops_.size()), nf = static_cast<int>(faces_.size());
    auto bad = [&](const std::string& what) { return error = what, false; };
    auto inRange = [](int i, int n) { return i >= 0 && i < n; };

    for (int e = 0; e < ne; ++e) {
        const Edge& edge = edges_[e];
        if (!inRange(edge.v[0], nv) || !inRange(edge.v[1], nv) || edge.v[0] == edge.v[1])
            return bad("edge " + std::to_string(e) + " has bad vertices");
        for (int i = 0; i < 2; ++i) {
            const DiskLink& d = edge.disk[i];
            if (!inRange(d.next, ne) || !inRange(d.prev, ne)) return bad("edge " + std::to_string(e) + " disk link out of range");
            if (diskOf(d.next, edge.v[i]).prev != e || diskOf(d.prev, edge.v[i]).next != e)
                return bad("edge " + std::to_string(e) + " disk cycle broken");
        }
        if (edge.loop != kNone) {
            if (!inRange(edge.loop, nl)) return bad("edge " + std::to_string(e) + " loop out of range");
            // Walk the radial cycle: every loop on it must use this edge.
            int l = edge.loop, steps = 0;
            do {
                if (loops_[l].edge != e) return bad("radial cycle of edge " + std::to_string(e) + " has a foreign loop");
                if (loops_[loops_[l].radialNext].radialPrev != l) return bad("radial links broken");
                l = loops_[l].radialNext;
                if (++steps > nl) return bad("radial cycle doesn't close");
            } while (l != edge.loop);
        }
    }
    for (int v = 0; v < nv; ++v) {
        const int first = verts_[v].edge;
        if (first == kNone) continue;
        if (!inRange(first, ne)) return bad("vertex " + std::to_string(v) + " edge out of range");
        int e = first, steps = 0;
        do {
            if (edges_[e].v[0] != v && edges_[e].v[1] != v) return bad("disk cycle of vertex " + std::to_string(v) + " has a foreign edge");
            e = diskOf(e, v).next;
            if (++steps > ne) return bad("disk cycle doesn't close");
        } while (e != first);
    }
    for (int f = 0; f < nf; ++f) {
        const Face& face = faces_[f];
        if (face.count < 3 || !inRange(face.loop, nl)) return bad("face " + std::to_string(f) + " is malformed");
        int l = face.loop;
        for (int i = 0; i < face.count; ++i) {
            const Loop& loop = loops_[l];
            if (loop.face != f) return bad("face " + std::to_string(f) + " has a foreign loop");
            if (loops_[loop.next].prev != l) return bad("face loop links broken");
            const Edge& edge = edges_[loop.edge];
            const int nextVert = loops_[loop.next].vert;
            // The loop's edge must join its vertex to the next corner's vertex.
            if (!((edge.v[0] == loop.vert && edge.v[1] == nextVert) || (edge.v[1] == loop.vert && edge.v[0] == nextVert)))
                return bad("loop edge doesn't connect its corners");
            l = loop.next;
        }
        if (l != face.loop) return bad("face " + std::to_string(f) + " corner cycle doesn't close");
    }
    return true;
}

Vec3 EditMesh::faceNormalUnnormalized(int face) const {
    // Newell's method: sum over the polygon's edges of the cross-product terms. It
    // equals the cross product for triangles and averages sensibly for non-planar n-gons.
    Vec3 n{0, 0, 0};
    int l = faces_[face].loop;
    for (int i = 0; i < faces_[face].count; ++i) {
        const Vec3& a = verts_[loops_[l].vert].position;
        const Vec3& b = verts_[loops_[loops_[l].next].vert].position;
        n += Vec3{(a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y)};
        l = loops_[l].next;
    }
    return n;
}

Mesh EditMesh::triangulate() const {
    // Smooth vertex normals: sum the normals of the smooth faces around each
    // vertex, each weighted by the angle its corner makes at that vertex (as
    // Blender does). Weighting by area instead leans toward bigger faces: on a UV
    // sphere every normal would tilt toward the equator. Flat faces don't
    // contribute (and use their own normal), so smooth meets flat with a crisp edge.
    std::vector<Vec3> smoothNormal;
    if (!hasCustomNormals) {
        smoothNormal.assign(verts_.size(), Vec3{0, 0, 0});
        for (int f = 0; f < static_cast<int>(faces_.size()); ++f) {
            if (!faces_[f].smooth) continue;
            const Vec3 n = normalize(faceNormalUnnormalized(f));
            int l = faces_[f].loop;
            for (int i = 0; i < faces_[f].count; ++i, l = loops_[l].next) {
                const Vec3& p = verts_[loops_[l].vert].position;
                const Vec3 toNext = normalize(verts_[loops_[loops_[l].next].vert].position - p);
                const Vec3 toPrev = normalize(verts_[loops_[loops_[l].prev].vert].position - p);
                smoothNormal[loops_[l].vert] += n * std::acos(clampf(dot(toNext, toPrev), -1.0f, 1.0f));
            }
        }
    }

    Mesh out;
    out.hasTexCoords = hasUVs;
    for (int f = 0; f < static_cast<int>(faces_.size()); ++f) {
        const Face& face = faces_[f];
        const Vec3 flat = face.smooth || hasCustomNormals ? Vec3{} : normalize(faceNormalUnnormalized(f));
        auto vertex = [&](int l) {
            const Loop& loop = loops_[l];
            Vertex v;
            v.position = verts_[loop.vert].position;
            v.uv = loop.uv;
            v.normal = hasCustomNormals ? loop.normal : face.smooth ? normalize(smoothNormal[loop.vert]) : flat;
            return v;
        };
        // Fan from the first corner: (0, i, i + 1). Fine for convex polygons, which
        // is what primitives and imports produce; concave n-gons need ear clipping
        // once edit mode can make them.
        const int first = face.loop;
        int l = loops_[first].next;
        for (int i = 1; i + 1 < face.count; ++i, l = loops_[l].next) {
            out.vertices.push_back(vertex(first));
            out.vertices.push_back(vertex(l));
            out.vertices.push_back(vertex(loops_[l].next));
        }
    }
    return out;
}

EditMesh EditMesh::fromTriangles(const Mesh& mesh) {
    EditMesh em;
    em.hasUVs = mesh.hasTexCoords;
    em.hasCustomNormals = true;  // keep the loader's normals exactly
    // Merge vertices by exact position: key on the float bit patterns, so it's
    // deterministic and never merges points that differ at all.
    struct Key {
        uint32_t x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return (k.x * 73856093u) ^ (k.y * 19349663u) ^ (k.z * 83492791u); }
    };
    std::unordered_map<Key, int, KeyHash> index;
    auto vertOf = [&](const Vec3& p) {
        Key k;
        std::memcpy(&k.x, &p.x, 4);
        std::memcpy(&k.y, &p.y, 4);
        std::memcpy(&k.z, &p.z, 4);
        auto it = index.find(k);
        if (it != index.end()) return it->second;
        const int v = em.addVert(p);
        index.emplace(k, v);
        return v;
    };
    for (size_t t = 0; t + 2 < mesh.vertices.size(); t += 3) {
        const Vertex* v = &mesh.vertices[t];
        const std::vector<int> corners = {vertOf(v[0].position), vertOf(v[1].position), vertOf(v[2].position)};
        const std::vector<Vec2> uvs = {v[0].uv, v[1].uv, v[2].uv};
        const std::vector<Vec3> normals = {v[0].normal, v[1].normal, v[2].normal};
        em.addFace(corners, true, &uvs, &normals);  // returns kNone (skipped) if two corners merged
    }
    return em;
}
