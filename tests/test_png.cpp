// test_png.cpp — the deflate compressor and PNG writer. Deflate output must
// decompress (with inflate.h) to exactly the input for every kind of data; the PNG
// and its source pixels are written to disk for png_check.py, which verifies them
// with Python's own zlib: an independent decoder.
#include <cstdio>
#include <fstream>
#include <random>
#include <string>

#include "check.h"
#include "deflate.h"
#include "inflate.h"
#include "png.h"

namespace {

bool roundTrips(const std::vector<uint8_t>& data, const char* what) {
    const std::vector<uint8_t> packed = zlibCompress(data.data(), data.size());
    std::vector<uint8_t> unpacked;
    std::string error;
    const bool ok = zlibDecompress(packed.data(), packed.size(), data.size(), unpacked, error) && unpacked == data;
    CHECK_MSG(ok, "%s: %s", what, error.c_str());
    return ok;
}

void testDeflate() {
    std::mt19937 rng(7);
    roundTrips({}, "empty");
    roundTrips({42}, "one byte");
    roundTrips(std::vector<uint8_t>(100000, 'a'), "a long run (matches overlap themselves)");
    std::vector<uint8_t> noise(70000);
    for (auto& b : noise) b = static_cast<uint8_t>(rng());
    roundTrips(noise, "random bytes (nothing to match)");
    std::vector<uint8_t> all(256 * 3);
    for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<uint8_t>(i);
    roundTrips(all, "every byte value");
    // A block repeated just inside the 32 KB window, then just outside it.
    std::vector<uint8_t> repeats(noise.begin(), noise.begin() + 1000);
    repeats.resize(32000 + 1000, 7);
    repeats.insert(repeats.end(), noise.begin(), noise.begin() + 1000);
    repeats.resize(repeats.size() + 33000, 9);
    repeats.insert(repeats.end(), noise.begin(), noise.begin() + 1000);
    roundTrips(repeats, "repeats at the window's edge");
    std::string text;
    for (int i = 0; i < 2000; ++i) text += "the quick brown fox " + std::to_string(rng() % 50) + " ";
    roundTrips(std::vector<uint8_t>(text.begin(), text.end()), "text");

    // It actually compresses: a megabyte of zeros shrinks to a few KB.
    const std::vector<uint8_t> zeros(1 << 20, 0);
    CHECK(zlibCompress(zeros.data(), zeros.size()).size() < 10000);
}

// An image that gives every row filter something to do: a smooth gradient (left,
// up, average, Paeth), noise (none), and hard-edged stripes. Odd sizes on purpose.
std::vector<uint32_t> testImage(int w, int h) {
    std::vector<uint32_t> img(static_cast<size_t>(w) * h);
    std::mt19937 rng(3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t r = x * 255 / w, g = y * 255 / h, b = (x + y) & 0xFF;
            if (y > h / 2 && x < w / 3) r = rng() & 0xFF, g = rng() & 0xFF, b = rng() & 0xFF;
            if (x > 2 * w / 3) r = g = b = (x / 4) % 2 ? 230 : 20;
            img[static_cast<size_t>(y) * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    testDeflate();
    // png_check.py reads these two files.
    const std::string dir = argc > 1 ? argv[1] : ".";
    const int w = 157, h = 93;
    const std::vector<uint32_t> img = testImage(w, h);
    CHECK(savePNG(dir + "/png_test.png", img.data(), w, h));
    std::ofstream raw(dir + "/png_test.rgb", std::ios::binary);
    for (uint32_t p : img) {
        const char rgb[3] = {static_cast<char>(p >> 16), static_cast<char>(p >> 8), static_cast<char>(p)};
        raw.write(rgb, 3);
    }
    const std::vector<uint8_t> png = encodePNG(img.data(), w, h);
    CHECK(png.size() < static_cast<size_t>(w) * h * 3);  // smaller than the raw pixels
    // The chooser may skip some filters on this image; force each in turn too.
    for (int f = 0; f < 5; ++f) {
        const std::vector<uint8_t> forced = encodePNG(img.data(), w, h, f);
        std::ofstream(dir + "/png_test_f" + std::to_string(f) + ".png", std::ios::binary)
            .write(reinterpret_cast<const char*>(forced.data()), static_cast<std::streamsize>(forced.size()));
    }
    return testResult("test_png");
}
