#include "animation.h"

#include <algorithm>

float applyEasing(Easing easing, float u) {
    u = clampf(u, 0.0f, 1.0f);
    switch (easing) {
        case Easing::EaseIn: return u * u;
        case Easing::EaseOut: return 1.0f - (1.0f - u) * (1.0f - u);
        case Easing::EaseInOut: return u * u * (3.0f - 2.0f * u);
        case Easing::Step: return 0.0f;
        default: return u;
    }
}

void sampleTrack(const Track& track, float time, float out[4]) {
    const std::vector<Keyframe>& keys = track.keys;
    const int width = trackPropertyWidth(track.property);
    if (keys.empty()) return;
    const Keyframe* hold = time <= keys.front().time ? &keys.front() : time >= keys.back().time ? &keys.back() : nullptr;
    if (hold) {
        std::copy(hold->value, hold->value + width, out);
        return;
    }
    // The first key after `time`; the one before it starts the segment.
    const auto next = std::upper_bound(keys.begin(), keys.end(), time,
                                       [](float t, const Keyframe& k) { return t < k.time; });
    const Keyframe& a = *(next - 1);
    const Keyframe& b = *next;
    const float e = applyEasing(a.easing, (time - a.time) / (b.time - a.time));
    if (track.property == TrackProperty::Rotation) {
        const Quat q = slerp({a.value[0], a.value[1], a.value[2], a.value[3]},
                             {b.value[0], b.value[1], b.value[2], b.value[3]}, e);
        out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
        return;
    }
    for (int i = 0; i < width; ++i) out[i] = a.value[i] + (b.value[i] - a.value[i]) * e;
}

void readProperty(const Object& o, TrackProperty property, float out[4]) {
    auto put = [&](const Vec3& v) { out[0] = v.x, out[1] = v.y, out[2] = v.z; };
    switch (property) {
        case TrackProperty::Position: put(o.transform.position); break;
        case TrackProperty::Rotation: {
            const Quat& q = o.transform.rotation;
            out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
            break;
        }
        case TrackProperty::Scale: put(o.transform.scale); break;
        case TrackProperty::Color: put(o.type == ObjectType::Light ? o.light.color : o.material.color); break;
        case TrackProperty::LightIntensity: out[0] = o.light.intensity; break;
        case TrackProperty::CameraFov: out[0] = o.camera.fovY; break;
    }
}

Object animatedObject(const Object& o, float time) {
    Object r = o;
    for (const Track& track : o.tracks) {
        float v[4] = {0, 0, 0, 1};
        sampleTrack(track, time, v);
        const Vec3 v3{v[0], v[1], v[2]};
        switch (track.property) {
            case TrackProperty::Position: r.transform.position = v3; break;
            case TrackProperty::Rotation: r.transform.rotation = normalize(Quat{v[0], v[1], v[2], v[3]}); break;
            case TrackProperty::Scale: r.transform.scale = v3; break;
            case TrackProperty::Color:
                // Ease-in/out stays within the keys' range, so colors stay valid.
                (o.type == ObjectType::Light ? r.light.color : r.material.color) = v3;
                break;
            case TrackProperty::LightIntensity: r.light.intensity = std::max(0.0f, v[0]); break;
            case TrackProperty::CameraFov: r.camera.fovY = clampf(v[0], 1.0f, 170.0f); break;
        }
    }
    return r;
}
