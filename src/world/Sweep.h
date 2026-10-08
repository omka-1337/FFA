// Moving a box through triangles: what physics needs, since the engine moves
// an actor as a box of its collision extent, Radius by Radius by Height.
//
// A box swept along a segment first touches a triangle where the segment
// enters the Minkowski sum of the two, a convex solid bounded on the 13 axes
// that can separate a box from a triangle: the triangle's normal, the box's
// three axes, and the nine crossings of a triangle edge with a box axis. On
// each axis the segment is inside over an interval; the hit is where the last
// interval opens, on that axis.
#pragma once

#include <cstdint>
#include <vector>

#include "world/Geometry.h"

namespace ffa {

struct Object;

struct Triangle {
    Vec3 v[3];
    Object* actor = nullptr;    // what it belongs to; none for the level's BSP
    bool oneSided = false;      // blocks only from the front, as its winding faces
};

// Where a box of half size `extent` moving from a by d first touches t. A box
// that starts overlapping it hits at time 0 when moving further in, and not at
// all when moving out. Returns false when it does not touch it before `best`.
bool sweepBox(const Triangle& t, Vec3 a, Vec3 d, Vec3 extent, float best, float& time, Vec3& normal);

// Whether a box of half size `extent` at c overlaps t.
bool overlapsBox(const Triangle& t, Vec3 c, Vec3 extent);

// A box tree over triangles, for finding those a swept box may touch.
class TriangleTree {
public:
    void build(std::vector<Triangle> tris);
    const std::vector<Triangle>& triangles() const { return tris_; }

    // Every triangle whose box meets the box lo..hi.
    template <class F>
    void query(Vec3 lo, Vec3 hi, F&& f) const {
        if (nodes_.empty()) return;
        int stack[128], n = 0;
        stack[n++] = 0;
        while (n) {
            const Node& nd = nodes_[size_t(stack[--n])];
            if (nd.hi.x < lo.x || nd.lo.x > hi.x || nd.hi.y < lo.y || nd.lo.y > hi.y || nd.hi.z < lo.z ||
                nd.lo.z > hi.z)
                continue;
            if (nd.count) {
                for (int i = 0; i < nd.count; ++i) f(tris_[size_t(nd.first + i)]);
            } else {
                stack[n++] = nd.left;
                stack[n++] = nd.right;
            }
        }
    }

private:
    struct Node {
        Vec3 lo, hi;
        int first = 0, count = 0;   // a leaf's triangles
        int left = 0, right = 0;    // an inner node's children
    };
    int build(int first, int count, int depth);
    std::vector<Triangle> tris_;
    std::vector<Node> nodes_;
};

}  // namespace ffa
