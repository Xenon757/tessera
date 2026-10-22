#include "mesh.h"

#include <cfloat>
#include <cstdlib>
#include <fstream>
#include <sstream>

void Mesh::bounds(Vec3& outMin, Vec3& outMax) const {
    outMin = {FLT_MAX, FLT_MAX, FLT_MAX};
    outMax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const Vertex& v : vertices) {
        outMin = {std::min(outMin.x, v.position.x), std::min(outMin.y, v.position.y),
                  std::min(outMin.z, v.position.z)};
        outMax = {std::max(outMax.x, v.position.x), std::max(outMax.y, v.position.y),
                  std::max(outMax.z, v.position.z)};
    }
}

namespace {

struct FaceIndex {
    int v = 0, vt = 0, vn = 0;  // 1-based after resolving; 0 = missing
};

// Resolves an OBJ index (1-based, or negative = relative to end) to 1-based.
int resolveIndex(int idx, size_t count) {
    if (idx < 0) return static_cast<int>(count) + idx + 1;
    return idx;
}

// Parses "v", "v/vt", "v//vn", or "v/vt/vn".
bool parseFaceToken(const std::string& tok, FaceIndex& out, size_t nv, size_t nvt, size_t nvn) {
    int parts[3] = {0, 0, 0};
    int part = 0;
    size_t start = 0;
    for (size_t i = 0; i <= tok.size() && part < 3; ++i) {
        if (i == tok.size() || tok[i] == '/') {
            if (i > start) parts[part] = std::atoi(tok.substr(start, i - start).c_str());
            ++part;
            start = i + 1;
        }
    }
    out.v = resolveIndex(parts[0], nv);
    out.vt = parts[1] ? resolveIndex(parts[1], nvt) : 0;
    out.vn = parts[2] ? resolveIndex(parts[2], nvn) : 0;
    return out.v >= 1 && static_cast<size_t>(out.v) <= nv &&
           (out.vt == 0 || (out.vt >= 1 && static_cast<size_t>(out.vt) <= nvt)) &&
           (out.vn == 0 || (out.vn >= 1 && static_cast<size_t>(out.vn) <= nvn));
}

}  // namespace

bool loadOBJ(const std::string& path, Mesh& out, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "could not open " + path;
        return false;
    }

    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    out.vertices.clear();

    std::string line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        std::istringstream ss(line);
        std::string type;
        ss >> type;

        if (type == "v") {
            Vec3 p;
            ss >> p.x >> p.y >> p.z;
            positions.push_back(p);
        } else if (type == "vt") {
            Vec2 t;
            ss >> t.x >> t.y;
            uvs.push_back(t);
        } else if (type == "vn") {
            Vec3 n;
            ss >> n.x >> n.y >> n.z;
            normals.push_back(normalize(n));
        } else if (type == "f") {
            std::vector<FaceIndex> face;
            std::string tok;
            while (ss >> tok) {
                FaceIndex fi;
                if (!parseFaceToken(tok, fi, positions.size(), uvs.size(), normals.size())) {
                    error = path + ":" + std::to_string(lineNo) + ": bad face index '" + tok + "'";
                    return false;
                }
                face.push_back(fi);
            }
            // Fan triangulation: (0, i, i+1).
            for (size_t i = 1; i + 1 < face.size(); ++i) {
                const FaceIndex* tri[3] = {&face[0], &face[i], &face[i + 1]};
                Vec3 p0 = positions[tri[0]->v - 1];
                Vec3 p1 = positions[tri[1]->v - 1];
                Vec3 p2 = positions[tri[2]->v - 1];
                Vec3 faceNormal = normalize(cross(p1 - p0, p2 - p0));
                for (const FaceIndex* fi : tri) {
                    Vertex vert;
                    vert.position = positions[fi->v - 1];
                    vert.uv = fi->vt ? uvs[fi->vt - 1] : Vec2{0, 0};
                    vert.normal = fi->vn ? normals[fi->vn - 1] : faceNormal;
                    out.vertices.push_back(vert);
                }
            }
        }
        // Other statements (o, g, s, usemtl, mtllib, ...) are ignored for now.
    }

    if (out.vertices.empty()) {
        error = path + ": no faces found";
        return false;
    }
    return true;
}

Mesh makeCube() {
    // Each face: 4 corners in counter-clockwise order seen from outside, + normal.
    struct Face {
        Vec3 c[4];
        Vec3 n;
    };
    const Face faces[6] = {
        {{{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}}, {0, 0, 1}},          // +Z
        {{{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}}, {0, 0, -1}},     // -Z
        {{{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}}, {1, 0, 0}},          // +X
        {{{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}}, {-1, 0, 0}},     // -X
        {{{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}}, {0, 1, 0}},          // +Y
        {{{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}, {0, -1, 0}},     // -Y
    };
    const Vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    const int quadToTris[6] = {0, 1, 2, 0, 2, 3};

    Mesh mesh;
    for (const Face& f : faces)
        for (int i : quadToTris) mesh.vertices.push_back({f.c[i], f.n, uv[i]});
    return mesh;
}
