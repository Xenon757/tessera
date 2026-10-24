// deflate.h — a from-scratch zlib compressor: the other half of inflate.h. DEFLATE
// (RFC 1951) finds repeats with LZ77 (hash chains over a 32 KB window) and writes
// them with the fixed Huffman code; the zlib wrapper (RFC 1950) adds a header and an
// Adler-32 checksum. PNG export uses it for its image data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t size);
