// test_math.cpp — quaternions and the normal matrix.
#include <cmath>
#include <random>

#include "check.h"
#include "vecmath.h"

namespace {

bool near(const Vec3& a, const Vec3& b, float eps = 1e-5f) { return length(a - b) <= eps; }

// Same rotation? q and -q are, so compare the rotations, not the numbers.
bool sameRotation(const Quat& a, const Quat& b, float eps = 1e-5f) { return std::fabs(std::fabs(dot(a, b)) - 1.0f) <= eps; }

void testAgainstMatrices() {
    // Quaternions agree with the matrix rotations the renderer already uses.
    const Vec3 v{0.3f, -1.2f, 2.5f};
    for (float angle : {0.0f, 0.5f, 1.7f, -2.9f}) {
        CHECK(near(Quat::axisAngle({1, 0, 0}, angle).rotate(v), Mat4::rotateX(angle).transformDir(v)));
        CHECK(near(Quat::axisAngle({0, 1, 0}, angle).rotate(v), Mat4::rotateY(angle).transformDir(v)));
        CHECK(near(Quat::axisAngle({0, 0, 1}, angle).rotate(v), Mat4::rotateZ(angle).transformDir(v)));
    }
    // (a * b) rotates by b first: the same order as matrix products.
    const Quat a = Quat::axisAngle({1, 2, 3}, 0.7f), b = Quat::axisAngle({-1, 0, 2}, 1.3f);
    CHECK(near((a * b).rotate(v), (a.toMat4() * b.toMat4()).transformDir(v)));
}

void testEuler() {
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> deg(-179.0f, 179.0f), pitch(-89.0f, 89.0f);
    for (int i = 0; i < 2000; ++i) {
        const Vec3 e{deg(rng), pitch(rng), deg(rng)};
        const Quat q = Quat::fromEulerXYZ(e);
        // XYZ order: X applied first, so the matrix is Rz * Ry * Rx.
        const Mat4 m = Mat4::rotateZ(radians(e.z)) * Mat4::rotateY(radians(e.y)) * Mat4::rotateX(radians(e.x));
        CHECK(near(q.rotate({1, 2, 3}), m.transformDir({1, 2, 3}), 1e-4f));
        // Round trip, compared as rotations (Euler angles aren't unique).
        CHECK(sameRotation(Quat::fromEulerXYZ(q.toEulerXYZ()), q, 1e-5f));
    }
    // Gimbal lock (y = +-90): still the same rotation after a round trip.
    for (float y : {90.0f, -90.0f}) {
        const Quat q = Quat::fromEulerXYZ({30, y, 50});
        CHECK(sameRotation(Quat::fromEulerXYZ(q.toEulerXYZ()), q, 1e-4f));
    }
}

void testSlerp() {
    const Quat a = Quat::axisAngle({0, 1, 0}, 0.2f), b = Quat::axisAngle({0, 1, 0}, 1.4f);
    CHECK(sameRotation(slerp(a, b, 0.0f), a));
    CHECK(sameRotation(slerp(a, b, 1.0f), b));
    CHECK(sameRotation(slerp(a, b, 0.25f), Quat::axisAngle({0, 1, 0}, 0.5f)));  // constant angular speed
    // Shortest arc: -b is the same rotation as b, so the halfway point must not change.
    const Quat nb{-b.x, -b.y, -b.z, -b.w};
    CHECK(sameRotation(slerp(a, nb, 0.5f), Quat::axisAngle({0, 1, 0}, 0.8f)));
    // Nearly equal rotations take the lerp branch and stay unit length.
    const Quat c = slerp(a, Quat::axisAngle({0, 1, 0}, 0.2001f), 0.5f);
    CHECK(std::fabs(dot(c, c) - 1.0f) < 1e-5f);
}

void testNormalMatrix() {
    // Stretch a slanted surface unevenly: the plain matrix tilts its normal off the
    // surface, the normal matrix keeps it perpendicular.
    const Mat4 m = Mat4::scale({3, 0.5f, 1}) * Mat4::rotateZ(0.4f);
    const Vec3 tangent = normalize(Vec3{1, 1, 0}), normal = normalize(Vec3{1, -1, 0});
    const Vec3 t = m.transformDir(tangent);
    CHECK(std::fabs(dot(t, normalize(normalMatrix(m).transformDir(normal)))) < 1e-5f);
    CHECK(std::fabs(dot(t, normalize(m.transformDir(normal)))) > 0.1f);  // the old way really was wrong
    float det = 0;
    normalMatrix(Mat4::scale({-1, 1, 1}), &det);
    CHECK(det < 0);  // mirroring shows up as a negative determinant
}

void testFromMatrix() {
    std::mt19937 rng(8);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f), angle(-3.1f, 3.1f);
    for (int i = 0; i < 2000; ++i) {  // random rotations hit all four branches
        const Quat q = Quat::axisAngle({unit(rng), unit(rng), unit(rng) + 0.01f}, angle(rng));
        CHECK(sameRotation(Quat::fromMatrix(q.toMat4()), q, 1e-5f));
    }
    // lookRotation: -Z ends up along `forward`, +Y stays as upright as possible.
    const Vec3 forward = normalize(Vec3{-7, -5, -7});
    const Quat look = Quat::lookRotation(forward);
    CHECK(near(look.rotate({0, 0, -1}), forward));
    CHECK(std::fabs(look.rotate({1, 0, 0}).y) < 1e-5f);  // no roll: the right axis stays horizontal
    CHECK(look.rotate({0, 1, 0}).y > 0);
    CHECK(near(Quat::lookRotation({0, -1, 0}).rotate({0, 0, -1}), {0, -1, 0}));  // straight down still works
}

}  // namespace

int main() {
    testFromMatrix();
    testAgainstMatrices();
    testEuler();
    testSlerp();
    testNormalMatrix();
    return testResult("test_math");
}
