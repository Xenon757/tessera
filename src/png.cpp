#include "png.h"

#include <cstdlib>
#include <fstream>

#include "deflate.h"

namespace {

// CRC-32 (the zlib/PNG polynomial, reflected: 0xEDB88320), one table lookup per byte.
uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    static uint32_t table[256];
    static const bool built = [] {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return true;
    }();
    (void)built;
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {  // PNG numbers are big-endian
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, crc32(out.data() + start, out.size() - start));  // over the type and the data
}

// Paeth's predictor: whichever of left (a), up (b), up-left (c) is closest to a + b - c.
uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

}  // namespace

std::vector<uint8_t> encodePNG(const uint32_t* argb, int width, int height, int onlyFilter) {
    const size_t stride = static_cast<size_t>(width) * 3;
    std::vector<uint8_t> rgb(stride * height);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        rgb[i * 3 + 0] = static_cast<uint8_t>(argb[i] >> 16);
        rgb[i * 3 + 1] = static_cast<uint8_t>(argb[i] >> 8);
        rgb[i * 3 + 2] = static_cast<uint8_t>(argb[i]);
    }

    // Each row: a filter byte, then the row as differences from that filter's
    // prediction (mod 256). Try all five and keep the one whose bytes, read as
    // signed, add up smallest: the usual heuristic for "most compressible".
    std::vector<uint8_t> filtered;
    filtered.reserve((stride + 1) * height);
    std::vector<uint8_t> candidate(stride), best(stride);
    const std::vector<uint8_t> zeros(stride, 0);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = &rgb[y * stride];
        const uint8_t* up = y > 0 ? &rgb[(y - 1) * stride] : zeros.data();
        long bestScore = -1;
        int bestFilter = 0;
        for (int f = 0; f < 5; ++f) {
            if (onlyFilter >= 0 && f != onlyFilter) continue;
            long score = 0;
            for (size_t x = 0; x < stride; ++x) {
                const int a = x >= 3 ? row[x - 3] : 0, b = up[x], c = x >= 3 ? up[x - 3] : 0;
                const int predicted = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
                candidate[x] = static_cast<uint8_t>(row[x] - predicted);
                score += std::abs(static_cast<int8_t>(candidate[x]));
            }
            if (bestScore < 0 || score < bestScore) bestScore = score, bestFilter = f, best.swap(candidate);
        }
        filtered.push_back(static_cast<uint8_t>(bestFilter));
        filtered.insert(filtered.end(), best.begin(), best.end());
    }

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};  // the signature
    std::vector<uint8_t> header;
    put32(header, static_cast<uint32_t>(width));
    put32(header, static_cast<uint32_t>(height));
    header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bits per channel, RGB, deflate, filters, no interlace
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", zlibCompress(filtered.data(), filtered.size()));
    chunk(out, "IEND", {});
    return out;
}

bool savePNG(const std::string& path, const uint32_t* argb, int width, int height) {
    const std::vector<uint8_t> png = encodePNG(argb, width, height);
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(f);
}
