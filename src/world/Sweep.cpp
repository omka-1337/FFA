#include "world/Sweep.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ffa {

bool sweepBox(const Triangle& t, Vec3 a, Vec3 d, Vec3 extent, float best, float& time, Vec3& normal) {
    const Vec3 e[3] = {t.v[1] - t.v[0], t.v[2] - t.v[1], t.v[0] - t.v[2]};
    Vec3 n = cross(e[0], t.v[2] - t.v[0]);
    if (dot(n, n) < 1e-12f) return false;
    // which side of the plane the box starts on
    float side = dot(n, a - t.v[0]);
    if (t.oneSided && side < 0) return false;
    Vec3 axes[13];
    int count = 0;
    axes[count++] = n;
    axes[count++] = {1, 0, 0};
    axes[count++] = {0, 1, 0};
    axes[count++] = {0, 0, 1};
    for (const Vec3& edge : e)
        for (Vec3 b : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
            Vec3 c = cross(edge, b);
            if (dot(c, c) > 1e-10f * dot(edge, edge)) axes[count++] = c;
        }
    const float inf = std::numeric_limits<float>::infinity();
    float enter = -inf, exit = inf;
    Vec3 enterAxis = n;
    for (int k = 0; k < count; ++k) {
        Vec3 L = axes[k];
        float len = length(L);
        L = L * (1 / len);
        float p0 = dot(L, t.v[0]), p1 = dot(L, t.v[1]), p2 = dot(L, t.v[2]);
        float r = extent.x * std::fabs(L.x) + extent.y * std::fabs(L.y) + extent.z * std::fabs(L.z);
        float lo = std::min({p0, p1, p2}) - r, hi = std::max({p0, p1, p2}) + r;
        float s = dot(L, a), v = dot(L, d);
        if (std::fabs(v) < 1e-9f) {
            if (s < lo || s > hi) return false;
            continue;
        }
        float t0 = (lo - s) / v, t1 = (hi - s) / v;
        // the segment enters through the side facing it
        Vec3 face = v > 0 ? -L : L;
        if (t0 > t1) std::swap(t0, t1);
        if (t0 > enter) {
            enter = t0;
            enterAxis = face;
        }
        exit = std::min(exit, t1);
        if (enter > exit || enter >= best || exit < 0) return false;
    }
    if (enter < 0) {
        // Already touching: a hit only for a box going further into the
        // triangle's plane, so that one resting on the ground can leave it.
        Vec3 out = side >= 0 ? n : -n;
        if (dot(d, out) >= 0) return false;
        time = 0;
        normal = out * (1 / length(out));
        return true;
    }
    if (enter > 1) return false;
    time = enter;
    normal = enterAxis;
    return true;
}

bool overlapsBox(const Triangle& t, Vec3 c, Vec3 extent) {
    const Vec3 e[3] = {t.v[1] - t.v[0], t.v[2] - t.v[1], t.v[0] - t.v[2]};
    Vec3 n = cross(e[0], t.v[2] - t.v[0]);
    if (dot(n, n) < 1e-12f) return false;
    auto separates = [&](Vec3 L) {
        float p0 = dot(L, t.v[0] - c), p1 = dot(L, t.v[1] - c), p2 = dot(L, t.v[2] - c);
        float r = extent.x * std::fabs(L.x) + extent.y * std::fabs(L.y) + extent.z * std::fabs(L.z);
        return std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r;
    };
    if (separates(n)) return false;
    for (Vec3 b : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
        if (separates(b)) return false;
        for (const Vec3& edge : e) {
            Vec3 x = cross(edge, b);
            if (dot(x, x) > 1e-10f * dot(edge, edge) && separates(x)) return false;
        }
    }
    return true;
}

void TriangleTree::build(std::vector<Triangle> tris) {
    tris_ = std::move(tris);
    nodes_.clear();
    if (tris_.empty()) return;
    nodes_.reserve(tris_.size() / 2 + 1);
    build(0, int(tris_.size()), 0);
}

int TriangleTree::build(int first, int count, int depth) {
    int self = int(nodes_.size());
    nodes_.push_back({});
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (int i = first; i < first + count; ++i)
        for (const Vec3& v : tris_[size_t(i)].v) {
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
        }
    nodes_[size_t(self)].lo = lo;
    nodes_[size_t(self)].hi = hi;
    // depth is bounded by the query's stack: 64 levels hold any split
    if (count <= 4 || depth >= 60) {
        nodes_[size_t(self)].first = first;
        nodes_[size_t(self)].count = count;
        return self;
    }
    // Halve along the longest axis of the box, by triangle centres.
    Vec3 size = hi - lo;
    int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
    auto centre = [axis](const Triangle& t) {
        const float* c0 = &t.v[0].x;
        const float* c1 = &t.v[1].x;
        const float* c2 = &t.v[2].x;
        return c0[axis] + c1[axis] + c2[axis];
    };
    int mid = first + count / 2;
    std::nth_element(tris_.begin() + first, tris_.begin() + mid, tris_.begin() + first + count,
                     [&](const Triangle& x, const Triangle& y) { return centre(x) < centre(y); });
    int left = build(first, mid - first, depth + 1);
    int right = build(mid, first + count - mid, depth + 1);
    nodes_[size_t(self)].left = left;
    nodes_[size_t(self)].right = right;
    return self;
}

}  // namespace ffa
