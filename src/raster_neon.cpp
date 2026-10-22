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
#include <cmath>
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

// Four Vec3s in "structure of arrays" form: all 4 x's in one register, all y's in
// another, all z's in a third. One instruction then does the same component math
// for all 4 pixels.
struct Vec3x4 {
    float32x4_t x, y, z;
};

// a*p0 + b*p1 + c*p2 per component, summed in the same order as the scalar
// `a * p0 + b * p1 + c * p2` on Vec3s.
inline Vec3x4 interpolate(const Vec3& a, const Vec3& b, const Vec3& c, float32x4_t p0, float32x4_t p1,
                          float32x4_t p2) {
    auto one = [&](float av, float bv, float cv) {
        return vaddq_f32(vaddq_f32(vmulq_f32(vdupq_n_f32(av), p0), vmulq_f32(vdupq_n_f32(bv), p1)),
                         vmulq_f32(vdupq_n_f32(cv), p2));
    };
    return {one(a.x, b.x, c.x), one(a.y, b.y, c.y), one(a.z, b.z, c.z)};
}

inline float32x4_t dot(const Vec3x4& a, const Vec3x4& b) {
    return vaddq_f32(vaddq_f32(vmulq_f32(a.x, b.x), vmulq_f32(a.y, b.y)), vmulq_f32(a.z, b.z));
}

inline Vec3x4 broadcast(const Vec3& v) { return {vdupq_n_f32(v.x), vdupq_n_f32(v.y), vdupq_n_f32(v.z)}; }

// normalize() from vecmath.h: divide by the length, or return zero for a zero vector.
inline Vec3x4 normalize(const Vec3x4& v) {
    const float32x4_t len = vsqrtq_f32(dot(v, v));
    const uint32x4_t nonzero = vcgtq_f32(len, vdupq_n_f32(0.0f));
    const float32x4_t zero = vdupq_n_f32(0.0f);
    return {vbslq_f32(nonzero, vdivq_f32(v.x, len), zero), vbslq_f32(nonzero, vdivq_f32(v.y, len), zero),
            vbslq_f32(nonzero, vdivq_f32(v.z, len), zero)};
}

// packColor() for 4 pixels: clamp to [0, 1], scale to 0..255, round, pack ARGB.
inline uint32x4_t packColor(const Vec3x4& c) {
    auto to8 = [](float32x4_t v) {
        v = vminq_f32(vmaxq_f32(v, vdupq_n_f32(0.0f)), vdupq_n_f32(1.0f));
        // + 0.5 then truncate (what the float -> uint conversion does) = round to nearest.
        return vcvtq_u32_f32(vaddq_f32(vmulq_f32(v, vdupq_n_f32(255.0f)), vdupq_n_f32(0.5f)));
    };
    const uint32x4_t argb = vorrq_u32(vdupq_n_u32(0xFF000000u), vshlq_n_u32(to8(c.x), 16));
    return vorrq_u32(vorrq_u32(argb, vshlq_n_u32(to8(c.y), 8)), to8(c.z));
}

struct Lighting4 {
    float32x4_t diffuse, specular;
};

// Renderer::light() for 4 pixels; see it for the Blinn-Phong math.
inline Lighting4 light4(const Vec3x4& n, const Vec3x4& pos, const Vec3& lightDir, const Vec3& eye, float ambient,
                        float specular, float shininess) {
    const Vec3x4 l = broadcast(lightDir);
    const float32x4_t nDotL = dot(n, l);
    Lighting4 out;
    out.diffuse = vaddq_f32(vdupq_n_f32(ambient),
                            vmulq_f32(vdupq_n_f32(1.0f - ambient), vmaxq_f32(nDotL, vdupq_n_f32(0.0f))));

    const Vec3x4 e = broadcast(eye);
    const Vec3x4 toEye = normalize({vsubq_f32(e.x, pos.x), vsubq_f32(e.y, pos.y), vsubq_f32(e.z, pos.z)});
    const Vec3x4 halfVec = normalize({vaddq_f32(l.x, toEye.x), vaddq_f32(l.y, toEye.y), vaddq_f32(l.z, toEye.z)});
    const float32x4_t nDotH = vmaxq_f32(dot(n, halfVec), vdupq_n_f32(0.0f));
    // NEON has no pow instruction, so call the same std::pow as the scalar code,
    // once per lane, and only where the surface faces the light.
    float base[4], lit[4], spec[4];
    vst1q_f32(base, nDotH);
    vst1q_f32(lit, nDotL);
    for (int i = 0; i < 4; ++i) spec[i] = lit[i] > 0.0f ? specular * std::pow(base[i], shininess) : 0.0f;
    out.specular = vld1q_f32(spec);
    return out;
}

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

    const float32x4_t invW0 = vdupq_n_f32(v[0].invW), invW1 = vdupq_n_f32(v[1].invW), invW2 = vdupq_n_f32(v[2].invW);
    const Vec3x4 baseColor = broadcast(s.baseColor);

    // Flat shading: one color for the whole triangle, computed exactly as the scalar loop does.
    const Lighting& lit = s.flatLight;
    const uint32x4_t flatColor = vdupq_n_u32(::packColor(s.baseColor * lit.diffuse + Vec3{1, 1, 1} * lit.specular));

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

            // Perspective-correct weights: p_i = (b_i / w_i) / sum(b_j / w_j).
            float32x4_t p0 = vdupq_n_f32(0.0f), p1 = p0, p2 = p0;
            if (s.needsAttributes) {
                const float32x4_t q0 = vmulq_f32(b0, invW0), q1 = vmulq_f32(b1, invW1), q2 = vmulq_f32(b2, invW2);
                const float32x4_t invSum = vdivq_f32(vdupq_n_f32(1.0f), vaddq_f32(vaddq_f32(q0, q1), q2));
                p0 = vmulq_f32(q0, invSum);
                p1 = vmulq_f32(q1, invSum);
                p2 = vmulq_f32(q2, invSum);
            }

            Vec3x4 albedo = baseColor;
            if (s.texture) {
                // UVs in SIMD (screen-space weights for affine, perspective-correct
                // otherwise), then one texture fetch per live lane: the 4 texels sit
                // at unrelated addresses, so there's no single vector load for them.
                const float32x4_t w0 = s.affineUV ? b0 : p0, w1 = s.affineUV ? b1 : p1, w2 = s.affineUV ? b2 : p2;
                auto uvComponent = [&](float a, float b, float c) {
                    return vaddq_f32(vaddq_f32(vmulq_f32(vdupq_n_f32(a), w0), vmulq_f32(vdupq_n_f32(b), w1)),
                                     vmulq_f32(vdupq_n_f32(c), w2));
                };
                float u[4], vv[4], r[4] = {}, g[4] = {}, bl[4] = {};
                uint32_t liveLanes[4];
                vst1q_f32(u, uvComponent(v[0].uv.x, v[1].uv.x, v[2].uv.x));
                vst1q_f32(vv, uvComponent(v[0].uv.y, v[1].uv.y, v[2].uv.y));
                vst1q_u32(liveLanes, live);
                for (int i = 0; i < 4; ++i) {
                    if (!liveLanes[i]) continue;  // dead lanes can hold garbage UVs
                    const Vec3 t = s.bilinear ? s.texture->sampleBilinear({u[i], vv[i]})
                                              : s.texture->sampleNearest({u[i], vv[i]});
                    r[i] = t.x;
                    g[i] = t.y;
                    bl[i] = t.z;
                }
                albedo = {vmulq_f32(baseColor.x, vld1q_f32(r)), vmulq_f32(baseColor.y, vld1q_f32(g)),
                          vmulq_f32(baseColor.z, vld1q_f32(bl))};
            }

            uint32x4_t color = flatColor;  // untextured flat: one color for the whole triangle
            if (s.shading != Shading::Flat || s.texture) {
                Lighting4 l;
                if (s.shading == Shading::Flat) {
                    l.diffuse = vdupq_n_f32(s.flatLight.diffuse);
                    l.specular = vdupq_n_f32(s.flatLight.specular);
                } else if (s.shading == Shading::Gouraud) {
                    const Lighting* vl = s.vertexLight;
                    l.diffuse = vaddq_f32(vaddq_f32(vmulq_f32(p0, vdupq_n_f32(vl[0].diffuse)),
                                                    vmulq_f32(p1, vdupq_n_f32(vl[1].diffuse))),
                                          vmulq_f32(p2, vdupq_n_f32(vl[2].diffuse)));
                    l.specular = vaddq_f32(vaddq_f32(vmulq_f32(p0, vdupq_n_f32(vl[0].specular)),
                                                     vmulq_f32(p1, vdupq_n_f32(vl[1].specular))),
                                           vmulq_f32(p2, vdupq_n_f32(vl[2].specular)));
                } else {  // Phong
                    const Vec3x4 normal = normalize(interpolate(v[0].normal, v[1].normal, v[2].normal, p0, p1, p2));
                    const Vec3x4 pos = interpolate(v[0].worldPos, v[1].worldPos, v[2].worldPos, p0, p1, p2);
                    l = light4(normal, pos, lightDir, eye_, ambient, specular, shininess);
                }
                // albedo * diffuse + white * specular (1 * specular is exact, so just add it).
                color = packColor({vaddq_f32(vmulq_f32(albedo.x, l.diffuse), l.specular),
                                   vaddq_f32(vmulq_f32(albedo.y, l.diffuse), l.specular),
                                   vaddq_f32(vmulq_f32(albedo.z, l.diffuse), l.specular)});
            }

            const uint32x4_t oldColor = loadColor(colorBuf + idx, n);
            storeColor(colorBuf + idx, vbslq_u32(live, color, oldColor), n);
        }
    }
}

#endif  // __ARM_NEON
