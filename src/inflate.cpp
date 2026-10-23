#include "inflate.h"

// DEFLATE in brief. The data is a series of blocks; each block is
//   stored         raw bytes, or
//   fixed Huffman  symbols coded with a code table the spec defines, or
//   dynamic        symbols coded with tables sent at the start of the block.
// A symbol is either a literal byte (0-255), end-of-block (256), or a length
// (257-285) that's followed by a distance: "copy `length` bytes from `distance`
// bytes back in the output" (LZ77). Huffman decoding follows Mark Adler's puff.c,
// the reference decoder: simple and exact, if not the fastest.

namespace {

constexpr int kMaxBits = 15;         // longest Huffman code DEFLATE allows
constexpr int kMaxLitLen = 288;      // literal/length alphabet size
constexpr int kMaxDist = 30;         // distance alphabet size

// Reads bits least-significant first, the order DEFLATE packs them.
struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;      // next byte to load
    uint32_t bitBuf = 0;  // bits loaded but not yet used, lowest first
    int bitCount = 0;
    bool overrun = false;

    uint32_t bits(int n) {
        while (bitCount < n) {
            if (pos >= size) {  // ran out of input: flag it and feed zeros
                overrun = true;
                return 0;
            }
            bitBuf |= static_cast<uint32_t>(data[pos++]) << bitCount;
            bitCount += 8;
        }
        const uint32_t v = bitBuf & ((1u << n) - 1);
        bitBuf >>= n;
        bitCount -= n;
        return v;
    }

    // Stored blocks start on a byte boundary: drop the rest of the current byte.
    void alignToByte() {
        bitBuf = 0;
        bitCount = 0;
    }
};

// A canonical Huffman code, described the way DEFLATE sends it: just a code
// length per symbol. Canonical means codes of the same length are consecutive
// numbers, assigned in symbol order, and shorter codes come first. So knowing how
// many codes have each length (`count`) and the symbols in code order (`symbol`)
// is enough to decode.
struct Huffman {
    uint16_t count[kMaxBits + 1] = {};
    uint16_t symbol[kMaxLitLen] = {};

    // Returns false if the lengths describe an impossible code (oversubscribed).
    // An incomplete code is allowed: DEFLATE permits it (e.g. one distance code).
    bool build(const uint8_t* lengths, int n) {
        for (int len = 0; len <= kMaxBits; ++len) count[len] = 0;
        for (int s = 0; s < n; ++s) ++count[lengths[s]];
        count[0] = 0;  // length 0 means "symbol unused"

        // Check the lengths can be real codes: at each length, the codes left
        // over from shorter lengths double, and we can't use more than that.
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left = left * 2 - count[len];
            if (left < 0) return false;
        }

        // offset[len] = where the first symbol of that length goes in `symbol`.
        uint16_t offset[kMaxBits + 1];
        offset[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) offset[len + 1] = offset[len] + count[len];
        for (int s = 0; s < n; ++s)
            if (lengths[s] != 0) symbol[offset[lengths[s]]++] = static_cast<uint16_t>(s);
        return true;
    }

    // Decodes one symbol, reading one bit at a time. `code` is the bits read so far;
    // `first` is the first code of the current length; codes in [first, first +
    // count) are valid at this length. Returns -1 for an invalid code.
    int decode(BitReader& in) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= static_cast<int>(in.bits(1));  // Huffman codes are packed MSB first
            const int n = count[len];
            if (code - first < n) return symbol[index + (code - first)];
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return -1;
    }
};

// Lengths 3..258 and distances 1..32768 are sent as a base code plus extra bits.
const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decodes one Huffman-coded block's symbols until end-of-block.
bool inflateCodes(BitReader& in, const Huffman& litLen, const Huffman& dist, std::vector<uint8_t>& out,
                  size_t limit, std::string& error) {
    for (;;) {
        const int sym = litLen.decode(in);
        if (sym < 0 || in.overrun) return error = "bad literal/length code", false;
        if (sym < 256) {  // a literal byte
            if (out.size() >= limit) return error = "more data than expected", false;
            out.push_back(static_cast<uint8_t>(sym));
            continue;
        }
        if (sym == 256) return true;  // end of block

        // A back-reference: length, then distance.
        const int li = sym - 257;
        if (li >= 29) return error = "bad length code", false;
        const size_t length = kLengthBase[li] + in.bits(kLengthExtra[li]);
        const int ds = dist.decode(in);
        if (ds < 0 || ds >= 30) return error = "bad distance code", false;
        const size_t distance = kDistBase[ds] + in.bits(kDistExtra[ds]);
        if (in.overrun) return error = "input ended early", false;
        if (distance > out.size()) return error = "distance points before the start", false;
        if (out.size() + length > limit) return error = "more data than expected", false;
        // Copy byte by byte: the source may overlap what we're writing (distance <
        // length repeats a pattern, e.g. distance 1 = run of the last byte).
        size_t from = out.size() - distance;
        for (size_t i = 0; i < length; ++i) out.push_back(out[from++]);
    }
}

// The fixed code table from the spec (block type 1).
void buildFixed(Huffman& litLen, Huffman& dist) {
    uint8_t lengths[kMaxLitLen];
    int s = 0;
    for (; s < 144; ++s) lengths[s] = 8;
    for (; s < 256; ++s) lengths[s] = 9;
    for (; s < 280; ++s) lengths[s] = 7;
    for (; s < 288; ++s) lengths[s] = 8;
    litLen.build(lengths, 288);
    for (s = 0; s < kMaxDist; ++s) lengths[s] = 5;
    dist.build(lengths, kMaxDist);
}

// Reads the code tables at the start of a dynamic block (block type 2). The
// code lengths themselves are Huffman coded, with a small "code length code".
bool readDynamicTables(BitReader& in, Huffman& litLen, Huffman& dist, std::string& error) {
    const int nLen = static_cast<int>(in.bits(5)) + 257;
    const int nDist = static_cast<int>(in.bits(5)) + 1;
    const int nCode = static_cast<int>(in.bits(4)) + 4;
    if (nLen > 286 || nDist > 30) return error = "bad code counts", false;

    // Code-length code lengths arrive in this order (most common first).
    static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[kMaxLitLen + kMaxDist] = {};
    for (int i = 0; i < nCode; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(in.bits(3));
    Huffman lenCode;
    if (!lenCode.build(lengths, 19)) return error = "bad code length code", false;

    // Now the literal/length and distance code lengths, as one run-length coded list:
    // 0-15 = a length, 16 = repeat the previous length 3-6 times,
    // 17 = 3-10 zeros, 18 = 11-138 zeros.
    int i = 0;
    while (i < nLen + nDist) {
        const int sym = lenCode.decode(in);
        if (sym < 0 || in.overrun) return error = "bad code lengths", false;
        if (sym < 16) {
            lengths[i++] = static_cast<uint8_t>(sym);
            continue;
        }
        uint8_t value = 0;
        int repeat;
        if (sym == 16) {
            if (i == 0) return error = "repeat with no previous length", false;
            value = lengths[i - 1];
            repeat = 3 + static_cast<int>(in.bits(2));
        } else if (sym == 17) {
            repeat = 3 + static_cast<int>(in.bits(3));
        } else {
            repeat = 11 + static_cast<int>(in.bits(7));
        }
        if (i + repeat > nLen + nDist) return error = "too many code lengths", false;
        while (repeat--) lengths[i++] = value;
    }
    if (lengths[256] == 0) return error = "no end-of-block code", false;
    if (!litLen.build(lengths, nLen)) return error = "bad literal/length code lengths", false;
    if (!dist.build(lengths + nLen, nDist)) return error = "bad distance code lengths", false;
    return true;
}

// Adler-32, zlib's checksum: two running sums mod 65521 (the largest prime below 2^16).
uint32_t adler32(const std::vector<uint8_t>& bytes) {
    uint32_t a = 1, b = 0;
    for (uint8_t byte : bytes) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

}  // namespace

bool zlibDecompress(const uint8_t* data, size_t size, size_t expectedSize, std::vector<uint8_t>& out,
                    std::string& error) {
    out.clear();
    out.reserve(expectedSize);
    // zlib header: CMF (method 8 = DEFLATE) and FLG, which together are a multiple
    // of 31, and no preset dictionary.
    if (size < 6) return error = "zlib stream too short", false;
    const uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
        return error = "not a zlib stream", false;

    BitReader in{data + 2, size - 2};
    for (;;) {
        const uint32_t last = in.bits(1);
        const uint32_t type = in.bits(2);
        if (in.overrun) return error = "input ended early", false;
        if (type == 0) {
            // Stored: LEN and its one's complement NLEN, then LEN raw bytes.
            in.alignToByte();
            if (in.pos + 4 > in.size) return error = "input ended early", false;
            const uint32_t len = in.data[in.pos] | (in.data[in.pos + 1] << 8);
            const uint32_t nlen = in.data[in.pos + 2] | (in.data[in.pos + 3] << 8);
            in.pos += 4;
            if (len != (~nlen & 0xFFFF)) return error = "bad stored block length", false;
            if (in.pos + len > in.size) return error = "input ended early", false;
            if (out.size() + len > expectedSize) return error = "more data than expected", false;
            out.insert(out.end(), in.data + in.pos, in.data + in.pos + len);
            in.pos += len;
        } else if (type == 1 || type == 2) {
            Huffman litLen, dist;
            if (type == 1)
                buildFixed(litLen, dist);
            else if (!readDynamicTables(in, litLen, dist, error))
                return false;
            if (!inflateCodes(in, litLen, dist, out, expectedSize, error)) return false;
        } else {
            return error = "bad block type", false;
        }
        if (last) break;
    }
    if (out.size() != expectedSize) return error = "decompressed size doesn't match", false;

    // The Adler-32 of the output follows, big-endian, on the next byte boundary.
    in.alignToByte();
    if (in.pos + 4 > in.size) return error = "missing checksum", false;
    const uint32_t stored = (static_cast<uint32_t>(in.data[in.pos]) << 24) | (in.data[in.pos + 1] << 16) |
                            (in.data[in.pos + 2] << 8) | in.data[in.pos + 3];
    if (stored != adler32(out)) return error = "checksum mismatch", false;
    return true;
}
