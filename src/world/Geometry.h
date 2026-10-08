// Small vector arithmetic for the level's geometry, in the engine's units.
#pragma once

#include <cmath>

namespace ffa {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

// A plane as the engine stores one: a unit normal and the distance W, so that
// a point P is in front when dot(N, P) >= W.
struct Plane {
    Vec3 n;
    float w = 0;
    float distance(Vec3 p) const { return dot(n, p) - w; }
};

// What a trace hit: how far along it, 0 to 1, where, and the surface normal
// facing back along the trace.
struct Hit {
    float time = 1;
    Vec3 location;
    Vec3 normal;
    int node = -1;              // the BSP node whose plane was hit
    bool startSolid = false;
    explicit operator bool() const { return time < 1; }
};

}  // namespace ffa
