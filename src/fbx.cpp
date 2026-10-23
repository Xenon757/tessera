#include "fbx.h"

#include <cstring>
#include <fstream>
#include <iterator>

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

// Appends one FBX mesh to `out` as triangles.
bool appendMesh(const FbxNode& meshNode, Mesh& out, std::string& error) {
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
    return true;
}

// A mesh is any node with both control points and polygons. That one rule finds
// meshes in all three layouts: FBX 7 "Geometry" nodes, and the "Model" nodes that
// carry their geometry directly in FBX 5 and 6.
void findMeshes(const FbxNode& node, std::vector<const FbxNode*>& meshes) {
    if (node.child("Vertices") && node.child("PolygonVertexIndex")) meshes.push_back(&node);
    for (const FbxNode& c : node.children) findMeshes(c, meshes);
}

}  // namespace

bool loadFBX(const std::string& path, Mesh& out, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return error = "could not open " + path, false;
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<FbxNode> roots;
    uint32_t version = 0;
    if (!parseFBX(file, roots, version, error)) return false;

    std::vector<const FbxNode*> meshes;
    for (const FbxNode& r : roots) findMeshes(r, meshes);
    out.vertices.clear();
    out.hasTexCoords = false;
    for (const FbxNode* m : meshes)
        if (!appendMesh(*m, out, error)) return false;
    if (out.vertices.empty()) return error = "no meshes found in the file", false;
    return true;
}
