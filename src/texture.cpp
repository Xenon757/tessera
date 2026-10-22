#include "texture.h"

#include <cmath>

#include "framebuffer.h"

Texture Texture::checker(int size, int cells, const Vec3& a, const Vec3& b) {
    Texture t;
    t.width = t.height = size;
    t.texels.resize(static_cast<size_t>(size) * size);
    const uint32_t ca = packColor(a), cb = packColor(b);
    const int cell = size / cells;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            t.texels[static_cast<size_t>(y) * size + x] = ((x / cell + y / cell) % 2) ? cb : ca;
    return t;
}

Vec3 Texture::texel(int x, int y) const {
    // Positive modulo, so negative coordinates wrap too (C++ % keeps the sign).
    x = ((x % width) + width) % width;
    y = ((y % height) + height) % height;
    return unpackColor(texels[static_cast<size_t>(y) * width + x]);
}

Vec3 Texture::sampleNearest(const Vec2& uv) const {
    // Flip v: UV (0, 0) is the bottom-left, but texel row 0 is the top.
    const int x = static_cast<int>(std::floor(uv.x * width));
    const int y = static_cast<int>(std::floor((1.0f - uv.y) * height));
    return texel(x, y);
}

Vec3 Texture::sampleBilinear(const Vec2& uv) const {
    // Texel i covers [i, i+1) and its center is at i + 0.5. Subtracting 0.5 puts
    // us in "texel-center" coordinates: the integer part picks the 2x2 block of
    // centers around the sample, the fraction says how far across it we are.
    const float fx = uv.x * width - 0.5f;
    const float fy = (1.0f - uv.y) * height - 0.5f;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0, ty = fy - y0;

    // Blend horizontally along the top and bottom rows, then vertically.
    const Vec3 top = texel(x0, y0) * (1.0f - tx) + texel(x0 + 1, y0) * tx;
    const Vec3 bottom = texel(x0, y0 + 1) * (1.0f - tx) + texel(x0 + 1, y0 + 1) * tx;
    return top * (1.0f - ty) + bottom * ty;
}
