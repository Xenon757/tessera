// fbx.h — a from-scratch reader for binary FBX files (Autodesk's interchange format).
//
// A binary FBX file is a tree of nodes. Each node has a name, a list of typed
// properties (numbers, strings, or arrays, which may be zlib-compressed), and child
// nodes. Meshes, models, and their connections are all just nodes; loadFBX (in
// mesh.cpp's loadModel) walks the tree for those.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mesh.h"

struct FbxProperty {
    // FBX type codes: Y int16, C bool, I int32, L int64, F float, D double,
    // S string, R raw bytes; lowercase i l f d b are arrays of those.
    char type = 0;
    double number = 0;     // any scalar number, converted
    int64_t integer = 0;   // integer scalars, exact (object IDs are 64-bit and don't fit a double)
    std::string string;    // S and R
    std::vector<uint8_t> array;  // array elements, decompressed, little-endian
    uint32_t count = 0;    // number of array elements

    bool isArray() const { return type == 'i' || type == 'l' || type == 'f' || type == 'd' || type == 'b'; }
    double element(uint32_t i) const;  // array element i, converted to double
};

struct FbxNode {
    std::string name;
    std::vector<FbxProperty> props;
    std::vector<FbxNode> children;

    // The first child with this name, or nullptr.
    const FbxNode* child(const char* childName) const;
};

// Parses a whole binary FBX file into its top-level nodes. Returns false and fills
// `error` for anything malformed, ASCII FBX, or big-endian files.
bool parseFBX(const std::vector<uint8_t>& file, std::vector<FbxNode>& roots, uint32_t& version,
              std::string& error);

// Loads every mesh in a binary FBX file into one triangle list.
bool loadFBX(const std::string& path, Mesh& out, std::string& error);
