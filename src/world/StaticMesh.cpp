#include "world/StaticMesh.h"

#include <algorithm>
#include <cmath>

#include "script/Tagged.h"

namespace ffa {

namespace {

void skip(Reader& r, size_t n) {
    r.need(n);
    r.p += n;
}

int32_t count(Reader& r, size_t each) {
    int32_t n = r.idx();
    if (n < 0 || size_t(n) * each > r.limit - r.p) throw FormatError("an array runs past the record");
    return n;
}

// A stream: a count, that many elements, and a u32 revision.
void stream(Reader& r, size_t each) {
    skip(r, size_t(count(r, each)) * each);
    r.u32();
}

// Whether the segment a..b meets the box, by the slab test.
bool meetsBox(Vec3 a, Vec3 b, Vec3 lo, Vec3 hi) {
    float t0 = 0, t1 = 1;
    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z};
    const float mn[3] = {lo.x, lo.y, lo.z}, mx[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; ++k) {
        float d = pb[k] - pa[k];
        if (std::fabs(d) < 1e-12f) {
            if (pa[k] < mn[k] || pa[k] > mx[k]) return false;
            continue;
        }
        float u = (mn[k] - pa[k]) / d, v = (mx[k] - pa[k]) / d;
        if (u > v) std::swap(u, v);
        t0 = std::max(t0, u);
        t1 = std::min(t1, v);
        if (t0 > t1) return false;
    }
    return true;
}

}  // namespace

StaticMeshCollision::StaticMeshCollision(const Package& p, int idx) {
    const Export& e = p.exp(idx);
    size_t start = size_t(e.off), end = start + size_t(e.size);
    std::vector<TagEntry> props;
    size_t pos = 0;
    if (!parseTagged(p, start, end, props, pos)) throw FormatError("the StaticMesh has no property block");
    Reader r(p.data, pos, end);
    skip(r, 25 + 16);                       // FBox with its valid byte, FSphere
    skip(r, size_t(count(r, 14)) * 14);     // sections
    skip(r, 25);                            // render bounding box
    for (int32_t i = 0, n = count(r, 24); i < n; ++i) {
        Vec3 v;
        v.x = r.f32();
        v.y = r.f32();
        v.z = r.f32();
        skip(r, 12);                        // normal
        positions.push_back(v);
    }
    r.u32();
    stream(r, 4);                           // colours
    stream(r, 4);                           // alpha
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) stream(r, 8);   // UV streams
    r.u32();
    stream(r, 2);                           // index buffer
    stream(r, 2);                           // wireframe buffer
    collisionModel = r.idx();
    for (int32_t i = 0, n = count(r, 7); i < n; ++i) {
        MeshTriangle t;
        for (uint16_t& v : t.v) v = r.u16();
        t.section = r.idx();
        triangles.push_back(t);
    }
    int32_t nn = count(r, 20);
    std::vector<int16_t> q(size_t(nn) * 6);
    for (int32_t i = 0; i < nn; ++i) {
        MeshNode n;
        n.triangle = r.u16();
        n.coplanar = r.u16();
        n.front = r.u16();
        n.back = r.u16();
        for (int k = 0; k < 6; ++k) q[size_t(i) * 6 + size_t(k)] = int16_t(r.u16());
        nodes.push_back(n);
    }
    // The boxes are quantised to i16 by this per axis scale.
    Vec3 scale;
    scale.x = r.f32();
    scale.y = r.f32();
    scale.z = r.f32();
    for (size_t i = 0; i < nodes.size(); ++i) {
        const int16_t* b = &q[i * 6];
        nodes[i].min = {b[0] * scale.x, b[1] * scale.y, b[2] * scale.z};
        nodes[i].max = {b[3] * scale.x, b[4] * scale.y, b[5] * scale.z};
    }
    uint32_t lazyEnd = r.u32();             // the editor's raw triangles
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        skip(r, 36);
        uint32_t uvs = r.u32();
        if (uvs > 8) throw FormatError("a raw triangle has too many UV streams");
        skip(r, 24 * size_t(uvs) + 12 + 8);
    }
    if (r.p != lazyEnd) throw FormatError("the raw triangles do not end at their lazy array end");
    r.u32();                                // InternalVersion
    r.idx();                                // KarmaProps
    r.u32();
    if (r.p != end) throw FormatError("the StaticMesh does not end on its record");
    for (const MeshTriangle& t : triangles)
        for (uint16_t v : t.v)
            if (v >= positions.size()) throw FormatError("a collision triangle names a vertex out of range");
    for (const MeshNode& n : nodes) {
        if (n.triangle >= triangles.size()) throw FormatError("a collision node names a triangle out of range");
        for (uint16_t c : {n.coplanar, n.front, n.back})
            if (c != 0xFFFF && c >= nodes.size()) throw FormatError("a collision node names a node out of range");
    }
}

void StaticMeshCollision::node(size_t i, Vec3 a, Vec3 b, Hit& hit) const {
    // The trace so far ends at the nearest hit: nothing beyond it matters.
    for (size_t guard = 0; i != 0xFFFF && guard < nodes.size(); ++guard) {
        const MeshNode& n = nodes[i];
        Vec3 end = lerp(a, b, hit.time);
        // the box holds the node's whole subtree
        Vec3 pad{0.01f, 0.01f, 0.01f};
        if (!meetsBox(a, end, n.min - pad, n.max + pad)) return;
        const MeshTriangle& t = triangles[n.triangle];
        Vec3 v0 = positions[t.v[0]], v1 = positions[t.v[1]], v2 = positions[t.v[2]];
        Vec3 normal = cross(v1 - v0, v2 - v0);
        float da = dot(normal, a - v0), db = dot(normal, b - v0);
        if ((da > 0) != (db > 0) && da != db) {
            float f = da / (da - db);
            if (f >= 0 && f < hit.time) {
                Vec3 p = lerp(a, b, f);
                // inside all three edges, for either winding
                float c0 = dot(cross(v1 - v0, p - v0), normal);
                float c1 = dot(cross(v2 - v1, p - v1), normal);
                float c2 = dot(cross(v0 - v2, p - v2), normal);
                if ((c0 >= 0 && c1 >= 0 && c2 >= 0) || (c0 <= 0 && c1 <= 0 && c2 <= 0)) {
                    float len = length(normal);
                    hit.time = f;
                    hit.location = p;
                    hit.normal = (da > 0 ? normal : -normal) * (len > 0 ? 1 / len : 0);
                }
            }
        }
        // the coplanar chain and both sides; the loop takes the back side
        if (n.coplanar != 0xFFFF) node(n.coplanar, a, b, hit);
        if (n.front != 0xFFFF) node(n.front, a, b, hit);
        i = n.back;
    }
}

Hit StaticMeshCollision::lineCheck(Vec3 a, Vec3 b) const {
    Hit hit;
    hit.location = b;
    if (!nodes.empty()) node(0, a, b, hit);
    return hit;
}

}  // namespace ffa
