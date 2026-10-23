// test_loaders.cpp — OBJ and FBX loading.
//
//   fbx scene      FBX files written by tests/make_fbx.py (hierarchy, pre-rotation,
//                  rotation orders, geometric offset, non-uniform scale, Z-up, raw and
//                  zlib arrays, 32- and 64-bit headers) load to the positions Python
//                  computed independently
//   obj            n-gons, negative indices, v/vt/vn combinations
//   fuzz           corrupted OBJ and FBX files never crash or read out of bounds
//                  (most useful in the sanitizer build: -DTESSERA_SANITIZE=ON)
//   ufbx           optional: set TESSERA_UFBX_DATA to a folder with files from
//                  github.com/ufbx/ufbx/tree/main/data to compare against reference
//                  OBJ exports of real Blender/Maya/3ds Max scenes
//
// Usage: tessera_test_loaders <dir from make_fbx.py>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "check.h"
#include "fbx.h"

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void writeFile(const std::string& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

std::vector<Vec3> readPoints(const std::string& path, const char* prefix) {
    std::vector<Vec3> points;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        if (prefix) {
            std::string tag;
            ss >> tag;
            if (tag != prefix) continue;
        }
        Vec3 p;
        if (ss >> p.x >> p.y >> p.z) points.push_back(p);
    }
    return points;
}

void testFbxScene(const std::string& dir) {
    const std::vector<Vec3> expected = readPoints(dir + "/expected.txt", nullptr);
    CHECK(expected.size() == 6 * 6 + 4 * 3);  // 6 quads -> 12 triangles, 1 hexagon -> 4 triangles
    for (const char* name : {"scene_7400.fbx", "scene_7500z.fbx"}) {
        Mesh mesh;
        std::string error;
        const bool ok = loadModel(dir + "/" + name, mesh, error);
        CHECK_MSG(ok, "%s: %s", name, error.c_str());
        if (!ok) continue;
        CHECK_MSG(mesh.vertices.size() == expected.size(), "%s: %zu vertices, expected %zu", name,
                  mesh.vertices.size(), expected.size());
        float worst = 0;
        for (size_t i = 0; i < std::min(mesh.vertices.size(), expected.size()); ++i)
            worst = std::max(worst, length(mesh.vertices[i].position - expected[i]));
        CHECK_MSG(worst < 1e-4f, "%s: positions off by up to %g", name, worst);
        CHECK(mesh.hasTexCoords);
        // Normals must face the same way as each triangle's winding.
        int against = 0;
        for (size_t t = 0; t + 2 < mesh.vertices.size(); t += 3) {
            const Vertex* v = &mesh.vertices[t];
            const Vec3 face = cross(v[1].position - v[0].position, v[2].position - v[0].position);
            if (dot(face, v[0].normal + v[1].normal + v[2].normal) <= 0) ++against;
        }
        CHECK_MSG(against == 0, "%s: %d triangles' normals face against their winding", name, against);
    }
}

void testObj(const std::string& dir) {
    const std::string path = dir + "/basic.obj";
    const char* obj =
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0.5 2 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1 4/1/1\n"  // quad with v/vt/vn
        "f -2 -3 -1\n"                 // negative (relative) indices
        "f 1//1 2//1 5//1\n";          // v//vn
    writeFile(path, obj, std::strlen(obj));
    Mesh mesh;
    std::string error;
    CHECK_MSG(loadModel(path, mesh, error), "%s", error.c_str());
    CHECK(mesh.triangleCount() == 4);
    CHECK(mesh.hasTexCoords);
    if (mesh.triangleCount() == 4) {
        CHECK(mesh.vertices[3].position.x == 0 && mesh.vertices[5].position.y == 1);  // fan (0, 2, 3)
        CHECK(mesh.vertices[6].position.x == 0 && mesh.vertices[6].position.y == 1);  // -2 of 5 -> vertex 4 (0,1,0)
    }
    const char* bad = "v 0 0 0\nf 1 2 3\n";
    writeFile(path, bad, std::strlen(bad));
    CHECK(!loadModel(path, mesh, error));  // out-of-range index is an error, not a crash
}

void testFuzz(const std::string& dir) {
    std::vector<std::vector<uint8_t>> seeds = {readFile(dir + "/scene_7400.fbx"), readFile(dir + "/scene_7500z.fbx")};
    std::mt19937 rng(42);
    const std::string fbxPath = dir + "/fuzz.fbx", objPath = dir + "/fuzz.obj";
    for (int t = 0; t < 3000; ++t) {
        std::vector<uint8_t> d = seeds[rng() % seeds.size()];
        switch (rng() % 4) {
            case 0: d.resize(rng() % d.size()); break;
            case 1: for (int j = 0, n = 1 + rng() % 8; j < n; ++j) d[rng() % d.size()] ^= uint8_t(1u << (rng() % 8)); break;
            case 2: for (int j = 0, n = 1 + rng() % 4; j < n; ++j) d[rng() % d.size()] = uint8_t(rng()); break;
            default: {  // overwrite something that may be a size or offset
                const size_t p = 27 + rng() % (d.size() - 31);
                const uint32_t v = (rng() % 2) ? 0xFFFFFFFFu : uint32_t(rng() % 100000);
                for (int k = 0; k < 4; ++k) d[p + k] = uint8_t(v >> (8 * k));
            }
        }
        writeFile(fbxPath, d.data(), d.size());
        Mesh m;
        std::string error;
        loadModel(fbxPath, m, error);  // must return (true or false) without crashing
    }
    const std::string objSeed = "v 1 0 0\nv 0 1 0\nv 0 0 1\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf -1 -2 -3\n";
    const char* junk[] = {"f 99999999999999999999 1 2\n", "f -99999999999 1 2\n", "f 1/2/3/4/5 1 1\n",
                          "v nan inf -inf\n", "f //1 2// 3\n", "f\n", "f 0 0 0\n"};
    for (int t = 0; t < 3000; ++t) {
        std::string d = objSeed;
        switch (rng() % 3) {
            case 0: d.resize(rng() % (d.size() + 1)); break;
            case 1: for (int j = 0, n = 1 + rng() % 8; j < n && !d.empty(); ++j) d[rng() % d.size()] = "0123456789-/ \nfvtn.e"[rng() % 20]; break;
            default: d.insert(rng() % (d.size() + 1), junk[rng() % 7]);
        }
        writeFile(objPath, d.data(), d.size());
        Mesh m;
        std::string error;
        loadModel(objPath, m, error);
    }
}

// Optional: real exporter files from the ufbx test corpus, compared (in the file's
// own axes) against reference OBJ exports of the same scenes.
void testUfbxCorpus() {
    const char* dirEnv = std::getenv("TESSERA_UFBX_DATA");
    if (!dirEnv) {
        std::printf("test_loaders: TESSERA_UFBX_DATA not set, skipping the ufbx corpus\n");
        return;
    }
    const std::string dir = dirEnv;
    const char* pairs[][2] = {
        {"max_geometry_transform_7700_binary.fbx", "max_geometry_transform.obj"},
        {"maya_cube_7500_binary.fbx", "maya_cube.obj"},
        {"maya_cube_6100_binary.fbx", "maya_cube.obj"},
        {"blender_279_default_7400_binary.fbx", "blender_279_default.obj"},
        {"blender_340_mirrored_normals_7400_binary.fbx", "blender_340_mirrored_normals.obj"},
        {"max7_cube_normals_5000_binary.fbx", "max7_cube_normals.obj"},
    };
    for (auto& pair : pairs) {
        Mesh mesh;
        std::string error;
        if (!loadFBX(dir + "/" + pair[0], mesh, error, false)) {
            CHECK_MSG(false, "%s: %s", pair[0], error.c_str());
            continue;
        }
        const std::vector<Vec3> ref = readPoints(dir + "/" + pair[1], "v");
        auto nearest = [](const Vec3& p, const std::vector<Vec3>& set) {
            float best = FLT_MAX;
            for (const Vec3& q : set) best = std::min(best, length(p - q));
            return best;
        };
        std::vector<Vec3> ours;
        for (const Vertex& v : mesh.vertices) ours.push_back(v.position);
        float worst = 0, size = 0;
        for (const Vec3& p : ref) worst = std::max(worst, nearest(p, ours)), size = std::max(size, length(p));
        for (const Vec3& p : ours) worst = std::max(worst, nearest(p, ref));
        CHECK_MSG(!ref.empty() && worst <= 1e-4f * std::max(1.0f, size), "%s: off by %g", pair[0], worst);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: tessera_test_loaders <dir from make_fbx.py>\n"), 1;
    const std::string dir = argv[1];
    testFbxScene(dir);
    testObj(dir);
    testFuzz(dir);
    testUfbxCorpus();
    return testResult("test_loaders");
}
