// texture.h — CPU-side texture and sampling.
// Texels are packed 0xAARRGGBB, same as the framebuffer.
#pragma once

#include <cstdint>
#include <vector>

#include "vecmath.h"

struct Texture {
    int width = 0, height = 0;
    std::vector<uint32_t> texels;  // row 0 is the TOP of the image

    // `cells` x `cells` checkerboard alternating colors a and b. Its straight lines
    // make interpolation errors obvious: without perspective correction they bend.
    static Texture checker(int size, int cells, const Vec3& a, const Vec3& b);

    // UVs follow the OBJ convention: (0, 0) is the bottom-left of the image.
    // Outside [0, 1] the texture repeats.
    Vec3 sampleNearest(const Vec2& uv) const;
    Vec3 sampleBilinear(const Vec2& uv) const;

private:
    Vec3 texel(int x, int y) const;  // wraps x and y into range
};
