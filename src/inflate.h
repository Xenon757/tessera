// inflate.h — a from-scratch zlib decompressor (RFC 1950 wrapper around RFC 1951
// DEFLATE). Binary FBX files store their big arrays (vertices, indices) this way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Decompresses a zlib stream into `out`, which must come out exactly `expectedSize`
// bytes (FBX records the uncompressed size, which also caps runaway output from a
// corrupt file). Returns false and fills `error` for any malformed input.
bool zlibDecompress(const uint8_t* data, size_t size, size_t expectedSize, std::vector<uint8_t>& out,
                    std::string& error);
