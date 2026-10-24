// png.h — writes PNG images without a library. A PNG is a signature and a list of
// chunks (length, type, data, CRC-32): IHDR (size and pixel format), IDAT (the
// pixels, zlib-compressed by deflate.h), IEND. Before compressing, each row is
// "filtered": stored as its difference from a prediction (the pixel to the left,
// above, their average, or the Paeth guess), which turns smooth images into runs
// of small numbers that compress far better.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// An 8-bit RGB PNG of `width` x `height` pixels in the framebuffer's 0xAARRGGBB
// format, rows top to bottom (alpha is dropped: renders are opaque).
// `onlyFilter` (0-4) uses that row filter everywhere instead of choosing per row:
// for tests, so every filter's encoding gets checked.
std::vector<uint8_t> encodePNG(const uint32_t* argb, int width, int height, int onlyFilter = -1);

bool savePNG(const std::string& path, const uint32_t* argb, int width, int height);
