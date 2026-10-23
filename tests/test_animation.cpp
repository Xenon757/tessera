// test_animation.cpp — keyframe evaluation: easing curves, sampling between and
// outside keys, rotation slerp, and which object fields each track drives.
#include <cmath>

#include "animation.h"
#include "check.h"

namespace {

bool near(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

Keyframe key(float time, float x, float y = 0, float z = 0, float w = 0, Easing easing = Easing::Linear) {
    Keyframe k;
    k.time = time;
    k.value[0] = x, k.value[1] = y, k.value[2] = z, k.value[3] = w;
    k.easing = easing;
    return k;
}

void testEasing() {
    for (Easing e : {Easing::Linear, Easing::EaseIn, Easing::EaseOut, Easing::EaseInOut}) {
        CHECK(applyEasing(e, 0) == 0 && applyEasing(e, 1) == 1);  // every curve meets the keys exactly
        float last = 0;
        for (int i = 1; i <= 100; ++i) {  // and never runs backwards
            const float v = applyEasing(e, i / 100.0f);
            CHECK(v >= last);
            last = v;
        }
    }
    CHECK(near(applyEasing(Easing::Linear, 0.25f), 0.25f));
    CHECK(applyEasing(Easing::EaseIn, 0.25f) < 0.25f);     // slow start
    CHECK(applyEasing(Easing::EaseOut, 0.25f) > 0.25f);    // fast start, slow end
    CHECK(near(applyEasing(Easing::EaseInOut, 0.5f), 0.5f) && applyEasing(Easing::EaseInOut, 0.1f) < 0.1f);
    CHECK(applyEasing(Easing::Step, 0.99f) == 0);
    CHECK(applyEasing(Easing::Linear, -1) == 0 && applyEasing(Easing::Linear, 2) == 1);  // clamped
}

void testSampling() {
    Track t;
    t.property = TrackProperty::Position;
    t.keys = {key(1, 0, 10, 0), key(3, 4, 10, -2, 0, Easing::Step), key(4, 8, 0, 0)};
    float v[4];
    sampleTrack(t, 0, v);  // before the first key: hold it
    CHECK(v[0] == 0 && v[1] == 10);
    sampleTrack(t, 2, v);  // halfway from key 1 to key 2, linear
    CHECK(near(v[0], 2) && near(v[1], 10) && near(v[2], -1));
    sampleTrack(t, 3, v);  // exactly on a key
    CHECK(v[0] == 4 && v[2] == -2);
    sampleTrack(t, 3.9f, v);  // key 2 is Step: holds until key 3
    CHECK(v[0] == 4);
    sampleTrack(t, 4, v);
    CHECK(v[0] == 8);
    sampleTrack(t, 100, v);  // after the last key: hold it
    CHECK(v[0] == 8 && v[1] == 0);

    Track one;
    one.property = TrackProperty::CameraFov;
    one.keys = {key(2, 35)};
    sampleTrack(one, 0, v);
    CHECK(v[0] == 35);
    sampleTrack(one, 9, v);
    CHECK(v[0] == 35);

    // Rotation: halfway between 0 and 90 degrees about Y is 45 degrees, by slerp.
    Track r;
    r.property = TrackProperty::Rotation;
    const Quat q0 = Quat::axisAngle({0, 1, 0}, 0), q1 = Quat::axisAngle({0, 1, 0}, radians(90));
    r.keys = {key(0, q0.x, q0.y, q0.z, q0.w), key(1, q1.x, q1.y, q1.z, q1.w)};
    sampleTrack(r, 0.5f, v);
    const Quat mid = Quat::axisAngle({0, 1, 0}, radians(45));
    CHECK(near(v[0], mid.x) && near(v[1], mid.y) && near(v[2], mid.z) && near(v[3], mid.w));
    // q and -q are the same rotation; slerp still takes the short way (no 360 spin).
    r.keys[1] = key(1, -q1.x, -q1.y, -q1.z, -q1.w);
    sampleTrack(r, 0.5f, v);
    const Quat got{v[0], v[1], v[2], v[3]};
    CHECK(near(std::fabs(dot(got, mid)), 1.0f));
}

void testAnimatedObject() {
    Object o;
    o.type = ObjectType::Mesh;
    o.transform.position = {5, 5, 5};
    o.material.color = {1, 1, 1};
    Track pos, color;
    pos.property = TrackProperty::Position;
    pos.keys = {key(0, 0, 0, 0), key(2, 2, 4, 6)};
    color.property = TrackProperty::Color;
    color.keys = {key(0, 1, 0, 0), key(1, 0, 0, 1)};
    o.tracks = {pos, color};
    const Object a = animatedObject(o, 1);
    CHECK(near(a.transform.position.x, 1) && near(a.transform.position.y, 2) && near(a.transform.position.z, 3));
    CHECK(near(a.material.color.x, 0) && near(a.material.color.z, 1));
    CHECK(a.transform.scale.x == 1);                 // not animated: untouched
    CHECK(o.transform.position.x == 5);              // the original is unchanged
    CHECK(animatedObject(o, 1).transform.position.x == a.transform.position.x);  // same t, same answer

    Object cam;
    cam.type = ObjectType::Camera;
    Track fov;
    fov.property = TrackProperty::CameraFov;
    fov.keys = {key(0, 20), key(1, 80)};
    cam.tracks = {fov};
    CHECK(near(animatedObject(cam, 0.5f).camera.fovY, 50));
}

}  // namespace

int main() {
    testEasing();
    testSampling();
    testAnimatedObject();
    return testResult("test_animation");
}
