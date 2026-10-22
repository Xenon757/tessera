// raster_neon.cpp — the pixel loop with ARM NEON SIMD: 4 pixels per step.
//
// A float32x4_t holds 4 floats ("lanes") and one instruction works on all 4. Each
// lane is one pixel of a horizontal group of 4. Branches become masks: a compare
// gives all-ones or all-zeros per lane, and vbslq ("bit select") takes new values
// in the masked lanes and keeps the old ones elsewhere.
//
// Every operation mirrors the scalar loop in Renderer::rasterizeTriangle, in the
// same order. With floating-point contraction off (see CMakeLists.txt), both loops
// produce bit-identical images, which is how the SIMD path is tested.
#include "raster.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>

#include <algorithm>
#include <cstdint>

namespace {

// Edge values for the 4 pixel centers px at height py: the scalar
// sign * ((bx - ax) * (py - ay) - (by - ay) * (px - ax)), one lane per pixel.
inline float32x4_t edgeValues(const Edge& e, float32x4_t px, float py) {
    const float32x4_t rowTerm = vdupq_n_f32((e.bx - e.ax) * (py - e.ay));  // same for the whole row
    const float32x4_t colTerm = vmulq_f32(vdupq_n_f32(e.by - e.ay), vsubq_f32(px, vdupq_n_f32(e.ax)));
    return vmulq_f32(vdupq_n_f32(e.sign), vsubq_f32(rowTerm, colTerm));
}

// Edge::inside for 4 lanes: w > 0, or w == 0 on a top-left edge.
inline uint32x4_t insideMask(const Edge& e, float32x4_t w) {
    const uint32x4_t positive = vcgtq_f32(w, vdupq_n_f32(0.0f));
    if (!e.topLeft) return positive;
    return vorrq_u32(positive, vceqq_f32(w, vdupq_n_f32(0.0f)));
}

inline bool anyLane(uint32x4_t mask) { return vmaxvq_u32(mask) != 0; }

// Loads and stores of a 4-pixel group. A group only runs past the end of a row
// when the screen width isn't a multiple of 4; then just the first n pixels are
// touched, so we never read or write past the row (into another tile's pixels).
inline float32x4_t loadDepth(const float* p, int n) {
    if (n == 4) return vld1q_f32(p);
    float tmp[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    for (int i = 0; i < n; ++i) tmp[i] = p[i];
    return vld1q_f32(tmp);
}
inline void storeDepth(float* p, float32x4_t v, int n) {
    if (n == 4) return vst1q_f32(p, v);
    float tmp[4];
    vst1q_f32(tmp, v);
    for (int i = 0; i < n; ++i) p[i] = tmp[i];
}
inline uint32x4_t loadColor(const uint32_t* p, int n) {
    if (n == 4) return vld1q_u32(p);
    uint32_t tmp[4] = {};
    for (int i = 0; i < n; ++i) tmp[i] = p[i];
    return vld1q_u32(tmp);
}
inline void storeColor(uint32_t* p, uint32x4_t v, int n) {
    if (n == 4) return vst1q_u32(p, v);
    uint32_t tmp[4];
    vst1q_u32(tmp, v);
    for (int i = 0; i < n; ++i) p[i] = tmp[i];
}

}  // namespace

void Renderer::rasterizeNeon(Framebuffer& fb, const TriangleSetup& s) const {
    const ScreenVertex* v = s.tri->v;
    const int width = fb.width();
    uint32_t* colorBuf = fb.colorData();
    float* depthBuf = fb.depthData();
    uint16_t* overdrawBuf = fb.overdrawData();

    // Per-triangle constants, broadcast to all 4 lanes once.
    const Edge e0 = s.e[0], e1 = s.e[1], e2 = s.e[2];
    const float32x4_t invArea = vdupq_n_f32(s.invArea);
    const float32x4_t z0 = vdupq_n_f32(v[0].z), z1 = vdupq_n_f32(v[1].z), z2 = vdupq_n_f32(v[2].z);
    const int32x4_t laneIndex = {0, 1, 2, 3};
    const int32x4_t minX = vdupq_n_s32(s.minX), maxX = vdupq_n_s32(s.maxX);

    // Flat shading: one color for the whole triangle, computed exactly as the scalar loop does.
    const Lighting& lit = s.flatLight;
    const uint32x4_t flatColor = vdupq_n_u32(packColor(s.baseColor * lit.diffuse + Vec3{1, 1, 1} * lit.specular));

    for (int y = s.minY; y <= s.maxY; ++y) {
        const float py = y + 0.5f;  // sample at the pixel center
        // Groups start at a multiple of 4. Tiles are multiples of 4 wide, so a group
        // never straddles two tiles, and writing back a masked-off lane is safe.
        for (int x = s.minX & ~3; x <= s.maxX; x += 4) {
            const int32x4_t xs = vaddq_s32(vdupq_n_s32(x), laneIndex);  // x, x+1, x+2, x+3
            const float32x4_t px = vaddq_f32(vcvtq_f32_s32(xs), vdupq_n_f32(0.5f));

            const float32x4_t w0 = edgeValues(e0, px, py);
            const float32x4_t w1 = edgeValues(e1, px, py);
            const float32x4_t w2 = edgeValues(e2, px, py);
            // A lane is live if all three edges say inside AND it's within the
            // triangle's box clipped to this tile (the group can overhang the box).
            uint32x4_t live = vandq_u32(vandq_u32(insideMask(e0, w0), insideMask(e1, w1)), insideMask(e2, w2));
            live = vandq_u32(live, vandq_u32(vcgeq_s32(xs, minX), vcleq_s32(xs, maxX)));
            if (!anyLane(live)) continue;

            const float32x4_t b0 = vmulq_f32(w0, invArea);
            const float32x4_t b1 = vmulq_f32(w1, invArea);
            const float32x4_t b2 = vmulq_f32(w2, invArea);

            const size_t idx = static_cast<size_t>(y) * width + x;
            const int n = std::min(4, width - x);  // pixels of this group on screen
            if (s.depthTest) {
                // z = b0*z0 + b1*z1 + b2*z2, summed in the scalar loop's order.
                const float32x4_t z = vaddq_f32(vaddq_f32(vmulq_f32(b0, z0), vmulq_f32(b1, z1)), vmulq_f32(b2, z2));
                const float32x4_t oldZ = loadDepth(depthBuf + idx, n);
                live = vandq_u32(live, vcltq_f32(z, oldZ));  // smaller = closer
                if (!anyLane(live)) continue;
                storeDepth(depthBuf + idx, vbslq_f32(live, z, oldZ), n);
            }
            if (s.countOverdraw) {  // debug view only, so plain per-lane code is fine
                uint32_t liveLanes[4];
                vst1q_u32(liveLanes, live);
                for (int i = 0; i < n; ++i)
                    if (liveLanes[i] && overdrawBuf[idx + i] < UINT16_MAX) ++overdrawBuf[idx + i];
            }

            const uint32x4_t oldColor = loadColor(colorBuf + idx, n);
            storeColor(colorBuf + idx, vbslq_u32(live, flatColor, oldColor), n);
        }
    }
}

#endif  // __ARM_NEON
