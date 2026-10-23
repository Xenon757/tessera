// test_zlib.cpp — the from-scratch zlib decoder against Python's zlib, plus
// corrupted streams that must be rejected (or decode exactly), never mis-decode.
//
// Usage: tessera_test_zlib <corpus.bin>   (written by tests/zlib_corpus.py)
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "check.h"
#include "inflate.h"

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: tessera_test_zlib <corpus.bin>\n"), 1;
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return std::fprintf(stderr, "can't open %s\n", argv[1]), 1;
    uint32_t count = 0;
    CHECK(std::fread(&count, 4, 1, f) == 1);
    std::vector<std::vector<uint8_t>> raws, comps;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nameLen = 0, rawLen = 0, compLen = 0;
        CHECK(std::fread(&nameLen, 4, 1, f) == 1 && std::fread(&rawLen, 4, 1, f) == 1 && std::fread(&compLen, 4, 1, f) == 1);
        std::string name(nameLen, '\0');
        std::vector<uint8_t> raw(rawLen), comp(compLen);
        CHECK(std::fread(name.data(), 1, nameLen, f) == nameLen);
        CHECK(std::fread(raw.data(), 1, rawLen, f) == rawLen);
        CHECK(std::fread(comp.data(), 1, compLen, f) == compLen);
        std::vector<uint8_t> out;
        std::string error;
        const bool ok = zlibDecompress(comp.data(), comp.size(), raw.size(), out, error);
        CHECK_MSG(ok && out == raw, "%s: %s", name.c_str(), ok ? "wrong output" : error.c_str());
        raws.push_back(std::move(raw));
        comps.push_back(std::move(comp));
    }
    std::fclose(f);
    CHECK(count > 100);

    // Corruption: flipped bits and truncations must never produce wrong output.
    std::mt19937 rng(7);
    int wrong = 0;
    for (int t = 0; t < 5000; ++t) {
        const size_t k = rng() % comps.size();
        std::vector<uint8_t> c = comps[k];
        if (c.empty()) continue;
        if (rng() % 3 == 0) c.resize(rng() % c.size());
        else
            for (int j = 0, flips = 1 + rng() % 4; j < flips; ++j) c[rng() % c.size()] ^= uint8_t(1u << (rng() % 8));
        std::vector<uint8_t> out;
        std::string error;
        if (zlibDecompress(c.data(), c.size(), raws[k].size(), out, error) && out != raws[k]) ++wrong;
    }
    CHECK_MSG(wrong == 0, "%d corrupted streams decoded to wrong data without an error", wrong);
    return testResult("test_zlib");
}
