#include "framebuffer.h"

#include <algorithm>
#include <cstdio>

void Framebuffer::resize(int width, int height) {
    width_ = width;
    height_ = height;
    size_t n = static_cast<size_t>(width) * height;
    color_.assign(n, 0xFF000000u);
    depth_.assign(n, 1.0f);
    overdraw_.assign(n, 0);
}

void Framebuffer::clear(uint32_t color, float depth) {
    std::fill(color_.begin(), color_.end(), color);
    std::fill(depth_.begin(), depth_.end(), depth);
    std::fill(overdraw_.begin(), overdraw_.end(), 0);
}

bool Framebuffer::savePPM(const char* path) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width_, height_);
    std::vector<unsigned char> row(static_cast<size_t>(width_) * 3);
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            uint32_t c = color_[static_cast<size_t>(y) * width_ + x];
            row[x * 3 + 0] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = c & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}
