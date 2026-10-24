#include "deflate.h"

#include <algorithm>

namespace {

constexpr int kWindow = 32768;   // how far back a match may point (DEFLATE's limit)
constexpr int kMinMatch = 3, kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kMaxChain = 48;    // candidates tried per position: speed vs. ratio

// Bits go out least significant first, packed into bytes from their low bit.
// Huffman codes are defined most significant bit first, so they're reversed first.
struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t buffer = 0;
    int count = 0;
    void write(uint32_t value, int bits) {
        buffer |= value << count;
        count += bits;
        while (count >= 8) {
            out.push_back(static_cast<uint8_t>(buffer));
            buffer >>= 8;
            count -= 8;
        }
    }
    void writeCode(uint32_t code, int bits) {
        uint32_t reversed = 0;
        for (int i = 0; i < bits; ++i) reversed |= ((code >> i) & 1u) << (bits - 1 - i);
        write(reversed, bits);
    }
    void flush() {
        if (count > 0) out.push_back(static_cast<uint8_t>(buffer));
        buffer = 0;
        count = 0;
    }
};

// The fixed Huffman code for literal/length symbol s (RFC 1951 3.2.6):
//   0-143: 8 bits from 0x30, 144-255: 9 bits from 0x190, 256-279: 7 bits from 0,
//   280-287: 8 bits from 0xC0.
void writeLiteralLength(BitWriter& w, int s) {
    if (s < 144) w.writeCode(0x30 + s, 8);
    else if (s < 256) w.writeCode(0x190 + (s - 144), 9);
    else if (s < 280) w.writeCode(s - 256, 7);
    else w.writeCode(0xC0 + (s - 280), 8);
}

// Lengths 3-258 map to symbols 257-285, each a base plus some extra bits; so do
// distances 1-32768 with symbols 0-29 (5-bit fixed codes).
const int kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                           193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void writeMatch(BitWriter& w, int length, int distance) {
    int l = 28;
    while (kLengthBase[l] > length) --l;  // the largest base that fits
    writeLiteralLength(w, 257 + l);
    w.write(static_cast<uint32_t>(length - kLengthBase[l]), kLengthExtra[l]);
    int d = 29;
    while (kDistBase[d] > distance) --d;
    w.writeCode(static_cast<uint32_t>(d), 5);
    w.write(static_cast<uint32_t>(distance - kDistBase[d]), kDistExtra[d]);
}

uint32_t hash3(const uint8_t* p) {  // of the next 3 bytes: where a match could start
    return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & ((1u << kHashBits) - 1);
}

}  // namespace

std::vector<uint8_t> zlibCompress(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size / 2 + 64);
    // zlib header: deflate with a 32 KB window (0x78), and a check byte chosen so
    // the 16-bit header is a multiple of 31 (0x7801 = 30721 = 31 * 991).
    out.push_back(0x78);
    out.push_back(0x01);

    BitWriter w{out};
    w.write(1, 1);  // BFINAL: this is the last (and only) block
    w.write(1, 2);  // BTYPE 01: fixed Huffman codes

    // head[h]: the latest position whose next 3 bytes hash to h; prev[i % window]:
    // the position before it with the same hash. Walking that chain finds earlier
    // places the upcoming bytes may repeat.
    std::vector<int> head(1u << kHashBits, -1), prev(kWindow, -1);
    auto insert = [&](size_t i) {
        const uint32_t h = hash3(data + i);
        prev[i % kWindow] = head[h];
        head[h] = static_cast<int>(i);
    };
    size_t i = 0;
    while (i < size) {
        int bestLength = 0, bestDistance = 0;
        if (i + kMinMatch <= size) {
            const int limit = static_cast<int>(std::min<size_t>(kMaxMatch, size - i));
            int candidate = head[hash3(data + i)];
            for (int chain = 0; candidate >= 0 && chain < kMaxChain; ++chain) {
                const int distance = static_cast<int>(i) - candidate;
                if (distance > kWindow) break;  // older ones are out of reach too
                int length = 0;
                while (length < limit && data[candidate + length] == data[i + length]) ++length;
                if (length > bestLength) {
                    bestLength = length, bestDistance = distance;
                    if (length == limit) break;
                }
                candidate = prev[candidate % kWindow];
            }
        }
        if (bestLength >= kMinMatch) {
            writeMatch(w, bestLength, bestDistance);
            for (int k = 0; k < bestLength; ++k, ++i)  // index every position it covers
                if (i + kMinMatch <= size) insert(i);
        } else {
            writeLiteralLength(w, data[i]);
            if (i + kMinMatch <= size) insert(i);
            ++i;
        }
    }
    writeLiteralLength(w, 256);  // end of block
    w.flush();

    // Adler-32 of the uncompressed data, big-endian: two running sums mod 65521.
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < size; ++k) {
        a = (a + data[k]) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(adler >> shift));
    return out;
}
