#include "fbx.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>

#include "inflate.h"

namespace {

// Bounds-checked little-endian reads from the file buffer. Any read past the end
// sets `failed`, so a truncated or corrupt file can't read out of bounds.
struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos = 0;
    bool failed = false;

    bool has(size_t n) {
        if (failed || n > data.size() - pos) failed = true;
        return !failed;
    }
    template <typename T>
    T read() {  // T is a plain number type; the host is little-endian like the file
        T v{};
        if (has(sizeof(T))) {
            std::memcpy(&v, &data[pos], sizeof(T));
            pos += sizeof(T);
        }
        return v;
    }
    void bytes(void* out, size_t n) {
        if (has(n)) {
            std::memcpy(out, &data[pos], n);
            pos += n;
        }
    }
};

size_t elementSize(char type) {
    switch (type) {
        case 'b': return 1;
        case 'i': case 'f': return 4;
        case 'l': case 'd': return 8;
    }
    return 0;
}

bool readProperty(Reader& in, FbxProperty& p, std::string& error) {
    p.type = in.read<char>();
    switch (p.type) {
        case 'Y': p.integer = in.read<int16_t>(); p.number = static_cast<double>(p.integer); break;
        case 'C': p.integer = in.read<uint8_t>(); p.number = static_cast<double>(p.integer); break;
        case 'I': p.integer = in.read<int32_t>(); p.number = static_cast<double>(p.integer); break;
        case 'L': p.integer = in.read<int64_t>(); p.number = static_cast<double>(p.integer); break;
        case 'F': p.number = in.read<float>(); break;
        case 'D': p.number = in.read<double>(); break;
        case 'S':
        case 'R': {
            const uint32_t len = in.read<uint32_t>();
            if (!in.has(len)) break;
            p.string.assign(reinterpret_cast<const char*>(&in.data[in.pos]), len);
            in.pos += len;
            break;
        }
        case 'i': case 'l': case 'f': case 'd': case 'b': {
            // Arrays: element count, encoding (0 = raw, 1 = zlib), stored byte size.
            p.count = in.read<uint32_t>();
            const uint32_t encoding = in.read<uint32_t>();
            const uint32_t stored = in.read<uint32_t>();
            if (!in.has(stored)) break;
            const size_t rawSize = static_cast<size_t>(p.count) * elementSize(p.type);
            if (encoding == 0) {
                if (stored != rawSize) return error = "array size mismatch", false;
                p.array.assign(in.data.begin() + in.pos, in.data.begin() + in.pos + stored);
            } else if (encoding == 1) {
                std::string zerr;
                if (!zlibDecompress(&in.data[in.pos], stored, rawSize, p.array, zerr))
                    return error = "compressed array: " + zerr, false;
            } else {
                return error = "unknown array encoding", false;
            }
            in.pos += stored;
            break;
        }
        default:
            return error = std::string("unknown property type '") + p.type + "'", false;
    }
    if (in.failed) return error = "file ends in the middle of a property", false;
    return true;
}

// Reads one node record at in.pos. Sets `isNull` for the all-zero record that ends
// a list of nodes. Node headers grew from 32-bit to 64-bit fields in FBX 7.5.
bool readNode(Reader& in, bool wide, FbxNode& node, bool& isNull, std::string& error, int depth) {
    if (depth > 64) return error = "nodes nested too deeply", false;
    const uint64_t endOffset = wide ? in.read<uint64_t>() : in.read<uint32_t>();
    const uint64_t numProps = wide ? in.read<uint64_t>() : in.read<uint32_t>();
    const uint64_t propsLen = wide ? in.read<uint64_t>() : in.read<uint32_t>();
    const uint8_t nameLen = in.read<uint8_t>();
    if (in.failed) return error = "file ends in the middle of a node", false;
    isNull = endOffset == 0;
    if (isNull) return true;
    if (endOffset > in.data.size() || endOffset <= in.pos) return error = "bad node end offset", false;

    node.name.resize(nameLen);
    in.bytes(node.name.data(), nameLen);
    const size_t propsStart = in.pos;
    if (numProps > propsLen) return error = "bad property count", false;  // each property is at least a byte
    node.props.resize(static_cast<size_t>(numProps));
    for (FbxProperty& p : node.props)
        if (!readProperty(in, p, error)) return false;
    if (in.pos != propsStart + propsLen) return error = "property list length mismatch in " + node.name, false;

    // Children fill the rest of the record, ending with a null record.
    while (in.pos < endOffset) {
        FbxNode child;
        bool childNull = false;
        if (!readNode(in, wide, child, childNull, error, depth + 1)) return false;
        if (childNull) break;
        node.children.push_back(std::move(child));
    }
    if (in.pos != endOffset) return error = "node " + node.name + " overruns its end", false;
    return true;
}

}  // namespace

double FbxProperty::element(uint32_t i) const {
    const uint8_t* p = array.data() + static_cast<size_t>(i) * elementSize(type);
    switch (type) {
        case 'b': return *p;
        case 'i': { int32_t v; std::memcpy(&v, p, 4); return v; }
        case 'l': { int64_t v; std::memcpy(&v, p, 8); return static_cast<double>(v); }
        case 'f': { float v; std::memcpy(&v, p, 4); return v; }
        case 'd': { double v; std::memcpy(&v, p, 8); return v; }
    }
    return 0;
}

const FbxNode* FbxNode::child(const char* childName) const {
    for (const FbxNode& c : children)
        if (c.name == childName) return &c;
    return nullptr;
}

bool parseFBX(const std::vector<uint8_t>& file, std::vector<FbxNode>& roots, uint32_t& version,
              std::string& error) {
    // Header: 21-byte magic, 0x1A, a byte-order flag (0 = little-endian), version.
    static const char kMagic[] = "Kaydara FBX Binary  ";  // 20 chars + its NUL = 21 bytes
    if (file.size() < 27 || std::memcmp(file.data(), kMagic, 21) != 0) {
        if (file.size() >= 5 && (file[0] == ';' || std::memcmp(file.data(), "FBXHe", 5) == 0))
            return error = "this is an ASCII FBX; re-export it as binary FBX", false;
        return error = "not a binary FBX file", false;
    }
    if (file[22] != 0) return error = "big-endian FBX isn't supported", false;
    Reader in{file};
    in.pos = 23;
    version = in.read<uint32_t>();
    const bool wide = version >= 7500;

    roots.clear();
    while (in.pos < file.size()) {
        FbxNode node;
        bool isNull = false;
        if (!readNode(in, wide, node, isNull, error, 0)) return false;
        if (isNull) break;  // what follows is a footer
        roots.push_back(std::move(node));
    }
    return true;
}

namespace {

// A node's numbers, however they're stored: FBX 7 uses one array property, while
// FBX 5 and 6 list every value as its own scalar property.
std::vector<double> numbers(const FbxNode* node) {
    std::vector<double> v;
    if (!node) return v;
    if (node->props.size() == 1 && node->props[0].isArray()) {
        const FbxProperty& p = node->props[0];
        v.resize(p.count);
        for (uint32_t i = 0; i < p.count; ++i) v[i] = p.element(i);
    } else {
        for (const FbxProperty& p : node->props)
            if (!p.isArray() && p.type != 'S' && p.type != 'R') v.push_back(p.number);
    }
    return v;
}

std::string firstString(const FbxNode* node) {
    return node && !node->props.empty() ? node->props[0].string : std::string();
}

// One per-corner attribute (normals or UVs). FBX stores these as a "layer
// element": a value array, how values map onto the mesh, and whether they're
// looked up directly or through an index array.
struct LayerElement {
    std::vector<double> values;
    std::vector<double> indices;  // only for IndexToDirect
    int width = 0;                // 3 for normals, 2 for UVs
    enum Mapping { ByPolygonVertex, ByControlPoint, ByPolygon, AllSame } mapping = ByPolygonVertex;
    bool indexed = false;

    bool empty() const { return values.empty(); }

    // Value for one polygon corner: corner = running polygon-vertex number,
    // point = its control point, polygon = the polygon's number.
    const double* at(size_t corner, size_t point, size_t polygon) const {
        size_t i = mapping == ByControlPoint ? point : mapping == ByPolygon ? polygon : mapping == AllSame ? 0 : corner;
        if (indexed) {
            if (i >= indices.size()) return nullptr;
            const double idx = indices[i];
            if (idx < 0) return nullptr;
            i = static_cast<size_t>(idx);
        }
        if ((i + 1) * width > values.size()) return nullptr;
        return &values[i * width];
    }
};

LayerElement readLayer(const FbxNode* layer, const char* valuesName, const char* indexName, int width) {
    LayerElement e;
    if (!layer) return e;
    e.width = width;
    e.values = numbers(layer->child(valuesName));
    const std::string mapping = firstString(layer->child("MappingInformationType"));
    if (mapping == "ByVertice" || mapping == "ByVertex" || mapping == "ByControlPoint") e.mapping = LayerElement::ByControlPoint;
    else if (mapping == "ByPolygon") e.mapping = LayerElement::ByPolygon;
    else if (mapping == "AllSame") e.mapping = LayerElement::AllSame;
    const std::string ref = firstString(layer->child("ReferenceInformationType"));
    e.indexed = ref == "IndexToDirect" || ref == "Index";
    if (e.indexed) e.indices = numbers(layer->child(indexName));
    return e;
}

// Appends one FBX mesh to `out` as triangles, with `transform` applied.
bool appendMesh(const FbxNode& meshNode, const Mat4& transform, Mesh& out, std::string& error) {
    const std::vector<double> positions = numbers(meshNode.child("Vertices"));
    const std::vector<double> polygonIndices = numbers(meshNode.child("PolygonVertexIndex"));
    const size_t pointCount = positions.size() / 3;

    // Normals: a LayerElementNormal (FBX 6, 7), or a plain per-point list (FBX 5).
    LayerElement normals = readLayer(meshNode.child("LayerElementNormal"), "Normals", "NormalsIndex", 3);
    if (normals.empty() && meshNode.child("Normals")) {
        normals.width = 3;
        normals.values = numbers(meshNode.child("Normals"));
        normals.mapping = LayerElement::ByControlPoint;
    }
    const LayerElement uvs = readLayer(meshNode.child("LayerElementUV"), "UV", "UVIndex", 2);

    // Smooth normals for meshes without any: area-weighted face normals summed per
    // control point (the same approach as the OBJ loader).
    std::vector<Vec3> smooth;
    if (normals.empty()) smooth.assign(pointCount, Vec3{});

    auto point = [&](size_t i) {
        return Vec3{static_cast<float>(positions[i * 3]), static_cast<float>(positions[i * 3 + 1]),
                    static_cast<float>(positions[i * 3 + 2])};
    };

    // Walk the polygons. A negative entry ends a polygon; its real index is -i - 1.
    const size_t firstVertex = out.vertices.size();
    std::vector<size_t> pointOf;  // per emitted vertex: its control point, for smooth normals
    std::vector<size_t> polyPoints, polyCorners;
    size_t polygon = 0;
    for (size_t corner = 0; corner < polygonIndices.size(); ++corner) {
        double raw = polygonIndices[corner];
        const bool last = raw < 0;
        if (last) raw = -raw - 1;
        if (raw >= static_cast<double>(pointCount)) return error = "polygon index out of range", false;
        polyPoints.push_back(static_cast<size_t>(raw));
        polyCorners.push_back(corner);
        if (!last) continue;

        // Fan-triangulate: (0, i, i + 1). Assumes convex polygons, like the OBJ loader.
        for (size_t i = 1; i + 1 < polyPoints.size(); ++i) {
            for (size_t k : {size_t{0}, i, i + 1}) {
                Vertex v;
                v.position = point(polyPoints[k]);
                if (const double* n = normals.empty() ? nullptr : normals.at(polyCorners[k], polyPoints[k], polygon))
                    v.normal = normalize(Vec3{static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2])});
                if (const double* t = uvs.empty() ? nullptr : uvs.at(polyCorners[k], polyPoints[k], polygon))
                    v.uv = {static_cast<float>(t[0]), static_cast<float>(t[1])};
                out.vertices.push_back(v);
                pointOf.push_back(polyPoints[k]);
            }
        }
        polyPoints.clear();
        polyCorners.clear();
        ++polygon;
    }
    if (!polyPoints.empty()) return error = "last polygon isn't terminated", false;

    if (normals.empty()) {
        for (size_t t = firstVertex; t < out.vertices.size(); t += 3) {
            const Vertex* v = &out.vertices[t];
            const Vec3 n = cross(v[1].position - v[0].position, v[2].position - v[0].position);
            for (size_t k = 0; k < 3; ++k) smooth[pointOf[t - firstVertex + k]] += n;
        }
        for (size_t i = firstVertex; i < out.vertices.size(); ++i)
            out.vertices[i].normal = normalize(smooth[pointOf[i - firstVertex]]);
    }
    if (!uvs.empty()) out.hasTexCoords = true;

    // Bake the transform in. Normals take the inverse-transpose of the 3x3 part,
    // which stays perpendicular to surfaces under non-uniform scale; it's the
    // cofactor matrix divided by the determinant. A negative determinant means the
    // transform mirrors, which turns the triangles inside out, so flip their winding.
    const float (*m)[4] = transform.m;
    const float c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2],
                c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const float c10 = m[0][2] * m[2][1] - m[0][1] * m[2][2], c11 = m[0][0] * m[2][2] - m[0][2] * m[2][0],
                c12 = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    const float c20 = m[0][1] * m[1][2] - m[0][2] * m[1][1], c21 = m[0][2] * m[1][0] - m[0][0] * m[1][2],
                c22 = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    const float det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (det == 0.0f) return error = "a mesh has a degenerate (zero-scale) transform", false;
    for (size_t i = firstVertex; i < out.vertices.size(); ++i) {
        Vertex& v = out.vertices[i];
        v.position = (transform * Vec4(v.position, 1.0f)).xyz();
        const Vec3 n = v.normal;
        v.normal = normalize(Vec3{c00 * n.x + c01 * n.y + c02 * n.z, c10 * n.x + c11 * n.y + c12 * n.z,
                                  c20 * n.x + c21 * n.y + c22 * n.z} / det);
    }
    if (det < 0.0f)
        for (size_t t = firstVertex; t < out.vertices.size(); t += 3) std::swap(out.vertices[t + 1], out.vertices[t + 2]);
    return true;
}

// ---- Transforms

// A model property as 3 numbers, wherever this FBX generation keeps it:
//   FBX 7:  Properties70 > P "name" type label flags x y z
//   FBX 6:  Properties60 > Property "name" type flags x y z
//   FBX 5:  Channel "Transform" > Channel "T"/"R"/"S" > Channel "X"/"Y"/"Z" > Default
Vec3 property3(const FbxNode* model, const char* name, Vec3 fallback) {
    if (!model) return fallback;
    auto fromProps = [&](const FbxNode& p, size_t first) {
        if (p.props.size() < first + 3) return fallback;
        return Vec3{static_cast<float>(p.props[first].number), static_cast<float>(p.props[first + 1].number),
                    static_cast<float>(p.props[first + 2].number)};
    };
    if (const FbxNode* p70 = model->child("Properties70"))
        for (const FbxNode& p : p70->children)
            if (!p.props.empty() && p.props[0].string == name) return fromProps(p, 4);
    if (const FbxNode* p60 = model->child("Properties60"))
        for (const FbxNode& p : p60->children)
            if (!p.props.empty() && p.props[0].string == name) return fromProps(p, 3);
    // FBX 5 channels only carry the three basic transforms.
    const char* channel = std::strcmp(name, "Lcl Translation") == 0 ? "T"
                          : std::strcmp(name, "Lcl Rotation") == 0  ? "R"
                          : std::strcmp(name, "Lcl Scaling") == 0   ? "S"
                                                                    : nullptr;
    for (const FbxNode& transform : model->children) {
        if (!channel || transform.name != "Channel" || firstString(&transform) != "Transform") continue;
        for (const FbxNode& c : transform.children) {
            if (c.name != "Channel" || firstString(&c) != channel) continue;
            Vec3 v = fallback;
            for (const FbxNode& axis : c.children) {
                const FbxNode* def = axis.child("Default");
                if (axis.name != "Channel" || !def || def->props.empty()) continue;
                const std::string a = firstString(&axis);
                const float value = static_cast<float>(def->props[0].number);
                (a == "X" ? v.x : a == "Y" ? v.y : v.z) = value;
            }
            return v;
        }
    }
    return fallback;
}

int propertyInt(const FbxNode* node, const char* name, int fallback) {
    if (!node) return fallback;
    for (const char* block : {"Properties70", "Properties60"}) {
        const FbxNode* props = node->child(block);
        if (!props) continue;
        const size_t valueIndex = block[10] == '7' ? 4 : 3;  // FBX 7 has one more label column
        for (const FbxNode& p : props->children)
            if (p.props.size() > valueIndex && p.props[0].string == name)
                return static_cast<int>(p.props[valueIndex].integer);
    }
    return fallback;
}

// Euler angles in degrees, applied in `order` (FBX's enum: 0 XYZ, 1 XZY, 2 YZX,
// 3 YXZ, 4 ZXY, 5 ZYX). "XYZ" means X is applied first, so with column vectors the
// matrix is Rz * Ry * Rx.
Mat4 eulerRotation(const Vec3& degrees, int order) {
    const Mat4 rx = Mat4::rotateX(radians(degrees.x)), ry = Mat4::rotateY(radians(degrees.y)),
               rz = Mat4::rotateZ(radians(degrees.z));
    switch (order) {
        case 1: return ry * rz * rx;  // XZY
        case 2: return rx * rz * ry;  // YZX
        case 3: return rz * rx * ry;  // YXZ
        case 4: return ry * rx * rz;  // ZXY
        case 5: return rx * ry * rz;  // ZYX
        default: return rz * ry * rx;  // XYZ (and spheric XYZ)
    }
}

// A rotation's inverse is its transpose.
Mat4 transposed(const Mat4& r) {
    Mat4 t;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) t.m[i][j] = r.m[j][i];
    return t;
}

// The FBX SDK's local transform for a model node:
//   T * Roff * Rp * Rpre * R * Rpost^-1 * Rp^-1 * Soff * Sp * S * Sp^-1
// T translation, R rotation (in the node's rotation order), S scaling; Rp/Sp are
// the rotation/scaling pivots, Roff/Soff offsets, Rpre/Rpost pre- and
// post-rotations (always XYZ). With zero pivots it's just T * Rpre * R * Rpost^-1 * S.
Mat4 localTransform(const FbxNode* model) {
    const Vec3 zero{0, 0, 0}, one{1, 1, 1};
    const Vec3 rp = property3(model, "RotationPivot", zero), sp = property3(model, "ScalingPivot", zero);
    return Mat4::translate(property3(model, "Lcl Translation", zero)) *
           Mat4::translate(property3(model, "RotationOffset", zero)) * Mat4::translate(rp) *
           eulerRotation(property3(model, "PreRotation", zero), 0) *
           eulerRotation(property3(model, "Lcl Rotation", zero), propertyInt(model, "RotationOrder", 0)) *
           transposed(eulerRotation(property3(model, "PostRotation", zero), 0)) * Mat4::translate(-rp) *
           Mat4::translate(property3(model, "ScalingOffset", zero)) * Mat4::translate(sp) *
           Mat4::scale(property3(model, "Lcl Scaling", one)) * Mat4::translate(-sp);
}

// 3ds Max's object offset: moves the mesh relative to its node, and (unlike the
// local transform) isn't inherited by child nodes.
Mat4 geometricTransform(const FbxNode* model) {
    const Vec3 zero{0, 0, 0};
    return Mat4::translate(property3(model, "GeometricTranslation", zero)) *
           eulerRotation(property3(model, "GeometricRotation", zero), 0) *
           Mat4::scale(property3(model, "GeometricScaling", Vec3{1, 1, 1}));
}

// Who's whose parent. Each FBX generation links nodes differently:
//   FBX 7:  Connections > C "OO" childId parentId (Geometry -> Model -> Model)
//   FBX 6:  Connections > Connect "OO" "Model::child" "Model::parent"
//   FBX 5:  a Model lists its children's names in a "Children" node
struct Hierarchy {
    std::unordered_map<const FbxNode*, const FbxNode*> modelOf;   // mesh node -> its model
    std::unordered_map<const FbxNode*, const FbxNode*> parentOf;  // model -> parent model

    explicit Hierarchy(const std::vector<FbxNode>& roots) {
        std::unordered_map<int64_t, const FbxNode*> byId;
        std::map<std::string, const FbxNode*> byName;
        auto index = [&](const FbxNode& n) {
            if (n.name != "Model" && n.name != "Geometry") return;
            if (!n.props.empty() && n.props[0].type == 'L') byId[n.props[0].integer] = &n;
            if (!n.props.empty() && n.props[0].type == 'S') byName[n.props[0].string] = &n;
        };
        for (const FbxNode& r : roots) {
            index(r);  // FBX 5 keeps models at the top level
            if (r.name == "Objects")
                for (const FbxNode& c : r.children) index(c);
        }
        for (const FbxNode& r : roots) {
            if (r.name == "Connections") {
                for (const FbxNode& c : r.children) {
                    if (c.props.size() < 3 || c.props[0].string != "OO") continue;
                    const FbxNode* child = nullptr;
                    const FbxNode* parent = nullptr;
                    if (c.props[1].type == 'L') {
                        auto ci = byId.find(c.props[1].integer), pi = byId.find(c.props[2].integer);
                        if (ci != byId.end() && pi != byId.end()) child = ci->second, parent = pi->second;
                    } else {
                        auto ci = byName.find(c.props[1].string), pi = byName.find(c.props[2].string);
                        if (ci != byName.end() && pi != byName.end()) child = ci->second, parent = pi->second;
                    }
                    if (!child || !parent || parent->name != "Model") continue;
                    if (child->name == "Geometry") modelOf[child] = parent;
                    else parentOf[child] = parent;
                }
            }
            if (r.name == "Model")
                if (const FbxNode* kids = r.child("Children"))
                    for (const FbxProperty& p : kids->props) {
                        auto ci = byName.find(p.string);
                        if (ci != byName.end()) parentOf[ci->second] = &r;
                    }
        }
    }

    // World transform of a model: its parents' transforms, then its own.
    Mat4 world(const FbxNode* model) const {
        Mat4 m = Mat4::identity();
        for (int depth = 0; model && depth < 256; ++depth) {  // depth cap guards against cycles
            m = localTransform(model) * m;
            auto p = parentOf.find(model);
            model = p == parentOf.end() ? nullptr : p->second;
        }
        return m;
    }
};

// GlobalSettings names the file's right ("Coord"), up, and front axes, each with
// a sign. Map them onto ours: +X right, +Y up, +Z front (toward the viewer).
// Each row of the result picks out one file axis.
Mat4 axisConversion(const std::vector<FbxNode>& roots) {
    const FbxNode* settings = nullptr;
    for (const FbxNode& r : roots)
        if (r.name == "GlobalSettings") settings = &r;
    if (!settings) return Mat4::identity();
    const int axes[3] = {propertyInt(settings, "CoordAxis", 0), propertyInt(settings, "UpAxis", 1),
                         propertyInt(settings, "FrontAxis", 2)};
    const int signs[3] = {propertyInt(settings, "CoordAxisSign", 1), propertyInt(settings, "UpAxisSign", 1),
                          propertyInt(settings, "FrontAxisSign", 1)};
    if (axes[0] == axes[1] || axes[1] == axes[2] || axes[0] == axes[2]) return Mat4::identity();
    Mat4 m = Mat4::identity();
    for (int row = 0; row < 3; ++row) {
        if (axes[row] < 0 || axes[row] > 2) return Mat4::identity();
        for (int col = 0; col < 3; ++col) m.m[row][col] = col == axes[row] ? (signs[row] < 0 ? -1.0f : 1.0f) : 0.0f;
    }
    return m;
}

// A mesh is any node with both control points and polygons. That one rule finds
// meshes in all three layouts: FBX 7 "Geometry" nodes, and the "Model" nodes that
// carry their geometry directly in FBX 5 and 6.
void findMeshes(const FbxNode& node, std::vector<const FbxNode*>& meshes) {
    if (node.child("Vertices") && node.child("PolygonVertexIndex")) meshes.push_back(&node);
    for (const FbxNode& c : node.children) findMeshes(c, meshes);
}

}  // namespace

bool loadFBX(const std::string& path, Mesh& out, std::string& error, bool convertAxes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return error = "could not open " + path, false;
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<FbxNode> roots;
    uint32_t version = 0;
    if (!parseFBX(file, roots, version, error)) return false;

    std::vector<const FbxNode*> meshes;
    for (const FbxNode& r : roots) findMeshes(r, meshes);
    const Hierarchy hierarchy(roots);
    const Mat4 axes = convertAxes ? axisConversion(roots) : Mat4::identity();
    out.vertices.clear();
    out.hasTexCoords = false;
    for (const FbxNode* mesh : meshes) {
        // FBX 7 meshes hang off a model; in FBX 5 and 6 the mesh node is the model.
        auto owner = hierarchy.modelOf.find(mesh);
        const FbxNode* model = owner != hierarchy.modelOf.end() ? owner->second : mesh->name == "Model" ? mesh : nullptr;
        const Mat4 transform = model ? axes * hierarchy.world(model) * geometricTransform(model) : axes;
        if (!appendMesh(*mesh, transform, out, error)) return false;
    }
    if (out.vertices.empty()) return error = "no meshes found in the file", false;
    return true;
}
