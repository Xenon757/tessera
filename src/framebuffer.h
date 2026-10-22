// framebuffer.h — CPU-side color + depth buffers.
// Color is packed 0xAARRGGBB, which matches SDL_PIXELFORMAT_ARGB8888, so the
// buffer can be uploaded to a texture with no conversion.
#pragma once

#include <cstdint>
#include <vector>

#include "vecmath.h"

inline uint32_t packColor(const Vec3& rgb) {
    auto to8 = [](float c) { return static_cast<uint32_t>(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (to8(rgb.x) << 16) | (to8(rgb.y) << 8) | to8(rgb.z);
}

inline Vec3 unpackColor(uint32_t c) {
    return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f};
}

class Framebuffer {
public:
    Framebuffer(int width, int height) { resize(width, height); }

    void resize(int width, int height);

    // Depth is stored in [0, 1] (0 = near plane, 1 = far plane), so clear to 1.
    void clear(uint32_t color, float depth = 1.0f);

    int width() const { return width_; }
    int height() const { return height_; }

    uint32_t* colorData() { return color_.data(); }
    const uint32_t* colorData() const { return color_.data(); }
    float* depthData() { return depth_.data(); }
    uint16_t* overdrawData() { return overdraw_.data(); }  // shaded-pixel counts (overdraw view only)
    const uint16_t* overdrawData() const { return overdraw_.data(); }

    void setPixel(int x, int y, uint32_t c) {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return;
        color_[static_cast<size_t>(y) * width_ + x] = c;
    }

    uint32_t getPixel(int x, int y) const { return color_[static_cast<size_t>(y) * width_ + x]; }

    // Writes a binary PPM (P6). Handy for headless runs and regression images.
    bool savePPM(const char* path) const;

private:
    int width_ = 0, height_ = 0;
    std::vector<uint32_t> color_;
    std::vector<float> depth_;
    std::vector<uint16_t> overdraw_;
};
