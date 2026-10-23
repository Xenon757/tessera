// vecmath.h — small vector/matrix library for the rasterizer.
// Conventions: right-handed, column vectors (v' = M * v), camera looks down -Z,
// clip space z in [-w, w] (OpenGL style), matrices stored row-major m[row][col].
#pragma once

#include <algorithm>
#include <cmath>

constexpr float kPi = 3.14159265358979323846f;

inline float radians(float deg) { return deg * (kPi / 180.0f); }
// std::max(lo, v) rather than std::max(v, lo): max(a, b) returns a unless a < b,
// and comparisons with NaN are false, so this order turns NaN into lo.
inline float clampf(float v, float lo, float hi) { return std::min(std::max(lo, v), hi); }

// float -> int without undefined behavior. Casting NaN, or a value outside int's
// range, is UB in C++ (ARM happens to return 0; other targets don't promise
// anything), so clamp in float first. NaN goes to lo. Exact for values in range.
inline int clampToInt(float v, int lo, int hi) {
    if (!(v >= static_cast<float>(lo))) return lo;  // also catches NaN
    if (v >= static_cast<float>(hi)) return hi;
    return static_cast<int>(v);
}

// ---------------------------------------------------------------- Vec2
struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
};

// ---------------------------------------------------------------- Vec3
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator*(const Vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }  // component-wise
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
};

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalize(const Vec3& v) {
    float len = length(v);
    return len > 0.0f ? v / len : Vec3{0, 0, 0};
}

// ---------------------------------------------------------------- Vec4
struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    Vec3 xyz() const { return {x, y, z}; }
    Vec4 operator+(const Vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
};

// ---------------------------------------------------------------- Mat4
struct Mat4 {
    float m[4][4] = {};

    static Mat4 identity() {
        Mat4 r;
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k) r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }

    Vec4 operator*(const Vec4& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
                m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
                m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w};
    }

    // Transforms a direction (w = 0): ignores translation.
    Vec3 transformDir(const Vec3& d) const { return ((*this) * Vec4(d, 0.0f)).xyz(); }

    static Mat4 translate(const Vec3& t) {
        Mat4 r = identity();
        r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;
        return r;
    }

    static Mat4 scale(const Vec3& s) {
        Mat4 r = identity();
        r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z;
        return r;
    }

    static Mat4 rotateX(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[1][1] = c; r.m[1][2] = -s;
        r.m[2][1] = s; r.m[2][2] = c;
        return r;
    }

    static Mat4 rotateY(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c;  r.m[0][2] = s;
        r.m[2][0] = -s; r.m[2][2] = c;
        return r;
    }

    static Mat4 rotateZ(float a) {
        Mat4 r = identity();
        float c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c; r.m[0][1] = -s;
        r.m[1][0] = s; r.m[1][1] = c;
        return r;
    }

    // OpenGL-style perspective projection. fovY in radians.
    static Mat4 perspective(float fovY, float aspect, float zNear, float zFar) {
        Mat4 r;
        float f = 1.0f / std::tan(fovY * 0.5f);
        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = (zFar + zNear) / (zNear - zFar);
        r.m[2][3] = (2.0f * zFar * zNear) / (zNear - zFar);
        r.m[3][2] = -1.0f;
        return r;
    }

    static Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 f = normalize(target - eye);
        Vec3 s = normalize(cross(f, up));
        Vec3 u = cross(s, f);
        Mat4 r = identity();
        r.m[0][0] = s.x;  r.m[0][1] = s.y;  r.m[0][2] = s.z;  r.m[0][3] = -dot(s, eye);
        r.m[1][0] = u.x;  r.m[1][1] = u.y;  r.m[1][2] = u.z;  r.m[1][3] = -dot(u, eye);
        r.m[2][0] = -f.x; r.m[2][1] = -f.y; r.m[2][2] = -f.z; r.m[2][3] = dot(f, eye);
        return r;
    }
};

// General 4x4 inverse by Gauss-Jordan elimination with partial pivoting: row-reduce
// [M | I] to [I | M^-1], each time swapping up the row with the largest pivot so
// small pivots don't amplify rounding. False (out untouched) for singular matrices.
inline bool invert(const Mat4& in, Mat4& out) {
    float a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) a[r][c] = in.m[r][c], a[r][c + 4] = r == c ? 1.0f : 0.0f;
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) pivot = r;
        if (std::fabs(a[pivot][col]) < 1e-12f) return false;
        if (pivot != col)
            for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[pivot][c]);
        const float inv = 1.0f / a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const float f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = a[r][c + 4];
    return true;
}

// Normals under a transform M must use the inverse-transpose of M's 3x3 part, not
// M itself: under non-uniform scale (stretching an object), M would tilt normals
// off their surfaces. The inverse-transpose is the cofactor matrix divided by the
// determinant. The result's translation part is zero; use it with transformDir.
// `det` (optional) receives the determinant: negative means M mirrors.
inline Mat4 normalMatrix(const Mat4& m, float* det = nullptr) {
    const float c00 = m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1], c01 = m.m[1][2] * m.m[2][0] - m.m[1][0] * m.m[2][2],
                c02 = m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0];
    const float c10 = m.m[0][2] * m.m[2][1] - m.m[0][1] * m.m[2][2], c11 = m.m[0][0] * m.m[2][2] - m.m[0][2] * m.m[2][0],
                c12 = m.m[0][1] * m.m[2][0] - m.m[0][0] * m.m[2][1];
    const float c20 = m.m[0][1] * m.m[1][2] - m.m[0][2] * m.m[1][1], c21 = m.m[0][2] * m.m[1][0] - m.m[0][0] * m.m[1][2],
                c22 = m.m[0][0] * m.m[1][1] - m.m[0][1] * m.m[1][0];
    const float d = m.m[0][0] * c00 + m.m[0][1] * c01 + m.m[0][2] * c02;
    if (det) *det = d;
    Mat4 n = Mat4::identity();
    if (d == 0.0f) return n;  // degenerate (zero scale): nothing sensible to return
    n.m[0][0] = c00 / d; n.m[0][1] = c01 / d; n.m[0][2] = c02 / d;
    n.m[1][0] = c10 / d; n.m[1][1] = c11 / d; n.m[1][2] = c12 / d;
    n.m[2][0] = c20 / d; n.m[2][1] = c21 / d; n.m[2][2] = c22 / d;
    return n;
}

// ---------------------------------------------------------------- Quat
// A rotation as a unit quaternion (x, y, z, w) = (axis * sin(angle/2), cos(angle/2)).
// The editor stores rotations this way: no gimbal lock, and slerp interpolates
// between two of them along the shortest arc at constant speed (keyframes).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;  // default: no rotation
    Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat axisAngle(const Vec3& axis, float angle) {
        const Vec3 a = normalize(axis) * std::sin(angle * 0.5f);
        return {a.x, a.y, a.z, std::cos(angle * 0.5f)};
    }

    // Hamilton product: (a * b) rotates by b first, then by a, like matrices.
    Quat operator*(const Quat& o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y, w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w, w * o.w - x * o.x - y * o.y - z * o.z};
    }

    Mat4 toMat4() const {
        Mat4 r = Mat4::identity();
        r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - z * w);     r.m[0][2] = 2 * (x * z + y * w);
        r.m[1][0] = 2 * (x * y + z * w);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - x * w);
        r.m[2][0] = 2 * (x * z - y * w);     r.m[2][1] = 2 * (y * z + x * w);     r.m[2][2] = 1 - 2 * (x * x + y * y);
        return r;
    }

    Vec3 rotate(const Vec3& v) const { return toMat4().transformDir(v); }

    // From a pure rotation matrix (Shepperd's method). Each of w, x, y, z can be
    // recovered from the diagonal, but dividing by a small one loses precision, so
    // compute the largest of the four first and get the rest from off-diagonal sums.
    static Quat fromMatrix(const Mat4& m) {
        const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
        Quat q;
        if (trace > 0.0f) {  // w is largest
            const float s = std::sqrt(trace + 1.0f) * 2.0f;  // s = 4w
            q = {(m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s, 0.25f * s};
        } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {  // x is largest
            const float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;  // s = 4x
            q = {0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s};
        } else if (m.m[1][1] > m.m[2][2]) {  // y is largest
            const float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;  // s = 4y
            q = {(m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s};
        } else {  // z is largest
            const float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;  // s = 4z
            q = {(m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s};
        }
        return q;
    }

    // The rotation that points an object's -Z axis along `forward`, keeping its +Y
    // as close to `up` as possible (cameras and lights look down their -Z).
    static Quat lookRotation(const Vec3& forward, const Vec3& up = {0, 1, 0}) {
        const Vec3 z = normalize(-forward);  // local +Z points backwards
        Vec3 x = cross(up, z);
        if (length(x) < 1e-6f) x = cross(Vec3{0, 0, 1}, z);  // looking straight along `up`
        x = normalize(x);
        const Vec3 y = cross(z, x);
        Mat4 m = Mat4::identity();
        m.m[0][0] = x.x; m.m[0][1] = y.x; m.m[0][2] = z.x;  // columns are the local axes
        m.m[1][0] = x.y; m.m[1][1] = y.y; m.m[1][2] = z.y;
        m.m[2][0] = x.z; m.m[2][1] = y.z; m.m[2][2] = z.z;
        return fromMatrix(m);
    }

    // Euler angles in degrees, XYZ order: X is applied first, then Y, then Z (the
    // same convention as Blender and the FBX loader), so q = qz * qy * qx.
    static Quat fromEulerXYZ(const Vec3& degrees) {
        return axisAngle({0, 0, 1}, radians(degrees.z)) * axisAngle({0, 1, 0}, radians(degrees.y)) *
               axisAngle({1, 0, 0}, radians(degrees.x));
    }

    // Back to Euler degrees. With M = Rz * Ry * Rx: M[2][0] = -sin(y), and
    // sqrt(M[0][0]^2 + M[1][0]^2) = |cos(y)|, so y = atan2(-M[2][0], cos y), which
    // stays accurate near +-90 degrees where asin doesn't. x and z come from atan2
    // of M[2][1], M[2][2] and M[1][0], M[0][0]. When cos(y) is ~0 (gimbal lock,
    // within ~0.006 degrees of +-90) only x - z or x + z is defined: set z = 0 and
    // read x from M[1][2] and M[1][1]. (Testing |sin y| against 1 instead fails:
    // float rounding leaves sin(90 degrees) at 0.99999976.)
    Vec3 toEulerXYZ() const {
        const Mat4 m = toMat4();
        const float cy = std::sqrt(m.m[0][0] * m.m[0][0] + m.m[1][0] * m.m[1][0]);
        const float ey = std::atan2(-m.m[2][0], cy);
        float ex, ez;
        if (cy > 1e-4f) {
            ex = std::atan2(m.m[2][1], m.m[2][2]);
            ez = std::atan2(m.m[1][0], m.m[0][0]);
        } else {
            ex = std::atan2(-m.m[1][2], m.m[1][1]);
            ez = 0.0f;
        }
        const float toDeg = 180.0f / kPi;
        return {ex * toDeg, ey * toDeg, ez * toDeg};
    }
};

inline float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

inline Quat normalize(const Quat& q) {
    const float len = std::sqrt(dot(q, q));
    return len > 0.0f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
}

// Spherical linear interpolation: constant angular speed along the shortest arc.
// q and -q are the same rotation, so if they point apart (dot < 0) flip one,
// or the interpolation goes the long way round.
inline Quat slerp(const Quat& a, Quat b, float t) {
    float d = dot(a, b);
    if (d < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f) {  // nearly equal: sin(theta) ~ 0, so lerp and renormalize
        return normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                              a.w + (b.w - a.w) * t});
    }
    const float theta = std::acos(d);
    const float s = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
