#include "world/Bsp.h"

#include <cmath>

#include "script/Tagged.h"

namespace ffa {

namespace {

Vec3 vec(Reader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

void skip(Reader& r, size_t n) {
    r.need(n);
    r.p += n;
}

int32_t count(Reader& r, size_t each) {
    int32_t n = r.idx();
    if (n < 0 || size_t(n) * each > r.limit - r.p) throw FormatError("an array runs past the record");
    return n;
}

}  // namespace

BspModel::BspModel(const Package& p, int idx) {
    const Export& e = p.exp(idx);
    size_t start = size_t(e.off), end = start + size_t(e.size);
    std::vector<TagEntry> props;
    size_t pos = 0;
    if (!parseTagged(p, start, end, props, pos)) throw FormatError("the Model has no property block");
    Reader r(p.data, pos, end);
    skip(r, 25 + 16);                       // FBox with its valid byte, FSphere
    for (int32_t i = 0, n = count(r, 12); i < n; ++i) vectors.push_back(vec(r));
    for (int32_t i = 0, n = count(r, 12); i < n; ++i) points.push_back(vec(r));
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        BspNode d;
        d.plane.n = vec(r);
        d.plane.w = r.f32();
        skip(r, 8);                         // zone mask
        d.flags = r.u8();
        d.vertPool = r.idx();
        d.surf = r.idx();
        d.back = r.idx();
        d.front = r.idx();
        d.coplanar = r.idx();
        d.collisionBound = r.idx();
        d.renderBound = r.idx();
        skip(r, 16 + 16);                   // bounding sphere, 16 bytes zero
        d.zoneBack = r.u8();
        d.zone = r.u8();
        d.numVerts = r.u8();
        d.leafBack = r.i32();
        d.leafFront = r.i32();
        skip(r, 12);                        // render section, first vertex, lightmap
        nodes.push_back(d);
    }
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        BspSurf s;
        s.material = r.idx();
        s.flags = r.u32();
        for (int k = 0; k < 6; ++k) r.idx();    // base, normal, U, V, lightmap, brush poly
        skip(r, 16 + 4);                    // plane, lightmap scale
        surfs.push_back(s);
    }
    for (int32_t i = 0, n = count(r, 2); i < n; ++i) {
        vertPoints.push_back(r.idx());
        r.idx();                            // iSide
    }
    r.i32();                                // NumSharedSides
    int32_t zones = r.i32();
    if (zones < 0 || zones > 64) throw FormatError("the Model's zone count is out of range");
    for (int32_t i = 0; i < zones; ++i) {
        zoneActors.push_back(r.idx());
        skip(r, 8 + 8 + 4);                 // Connectivity, Visibility, LastRenderTime
    }
    polys = r.idx();
    skip(r, size_t(count(r, 25)) * 25);     // node bounds
    for (int32_t i = 0, n = count(r, 4); i < n; ++i) leafHulls.push_back(r.i32());
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        BspLeaf l;
        l.zone = r.idx();
        l.permeating = r.idx();
        l.volumetric = r.idx();
        skip(r, 8);                         // VisibleZones
        leaves.push_back(l);
    }
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) r.idx();   // light lists
    r.u32();                                // RootOutside
    r.u32();                                // Linked
    // Drawing only from here: render sections, lightmaps, lightmap textures.
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        skip(r, size_t(count(r, 40)) * 40);
        r.u32();
        r.idx();
        skip(r, 12);
    }
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        for (int k = 0; k < 7; ++k) r.idx();
        skip(r, 64 + 36);                   // world to texel matrix, base and steps
        for (int32_t k = 0, m = count(r, 1); k < m; ++k) {
            r.idx();
            skip(r, size_t(count(r, 1)));   // shadow bitmap
            skip(r, 28);
        }
        r.idx();
        r.u32();
    }
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        r.idx();
        skip(r, size_t(count(r, 4)) * 4);
        skip(r, 12);
        for (int k = 0; k < 2; ++k) {
            uint32_t lazyEnd = r.u32();
            skip(r, size_t(count(r, 1)));
            if (r.p != lazyEnd) throw FormatError("a lightmap mip does not end at its lazy array end");
        }
        skip(r, 13);
    }
    if (r.p != end) throw FormatError("the Model does not end on its record");
    for (const BspNode& d : nodes) {
        if (d.leafBack >= int32_t(leaves.size()) || d.leafFront >= int32_t(leaves.size()))
            throw FormatError("a node names a leaf out of range");
        if (d.numVerts && (d.vertPool < 0 || size_t(d.vertPool) + d.numVerts > vertPoints.size()))
            throw FormatError("a node's polygon is out of range");
        // Only the vertices a node uses: the pool also keeps entries left from
        // editing that no node points at, with stale indices.
        for (int k = 0; k < d.numVerts; ++k) {
            int32_t v = vertPoints[size_t(d.vertPool + k)];
            if (v < 0 || size_t(v) >= points.size()) throw FormatError("a vertex names a point out of range");
        }
    }
}

std::vector<BrushPolygon> readPolys(const Package& p, int idx) {
    const Export& e = p.exp(idx);
    size_t start = size_t(e.off), end = start + size_t(e.size);
    std::vector<TagEntry> props;
    size_t pos = 0;
    if (!parseTagged(p, start, end, props, pos)) throw FormatError("the Polys has no property block");
    Reader r(p.data, pos, end);
    int32_t num = r.i32(), cap = r.i32();
    if (num != cap || num < 0 || size_t(num) * 50 > end - r.p) throw FormatError("the Polys count is out of range");
    std::vector<BrushPolygon> out;
    for (int32_t i = 0; i < num; ++i) {
        BrushPolygon q;
        int32_t nv = r.idx();
        if (nv < 3 || nv > 64) throw FormatError("a polygon's vertex count is out of range");
        vec(r);                             // Base
        q.normal = vec(r);
        vec(r);
        vec(r);                             // TextureU, TextureV
        for (int32_t k = 0; k < nv; ++k) q.vertices.push_back(vec(r));
        q.flags = r.u32();
        for (int k = 0; k < 5; ++k) r.idx();    // Actor, Material, ItemName, iLink, iBrushPoly
        r.f32();                            // LightMapScale
        out.push_back(std::move(q));
    }
    if (r.p != end) throw FormatError("the Polys does not end on its record");
    return out;
}

BspModel::Region BspModel::regionAt(Vec3 p) const {
    if (nodes.empty()) return {-1, 0};
    int i = 0;
    for (size_t guard = 0; guard <= nodes.size(); ++guard) {
        const BspNode& n = nodes[size_t(i)];
        bool front = n.plane.distance(p) >= 0;
        int32_t next = front ? n.front : n.back;
        if (!child(next)) return front ? Region{n.leafFront, n.zone} : Region{n.leafBack, n.zoneBack};
        i = next;
    }
    throw FormatError("the BSP walk did not end");
}

// The segment a..b, times ta..tb of the whole trace, within node i's space.
// `enter` is the normal of the last plane the trace crossed, facing the side
// it came from: the normal of the surface, should the far side be solid.
bool BspModel::segment(int i, Vec3 a, Vec3 b, float ta, float tb, Vec3 enter, int enterNode,
                       Hit& hit) const {
    const BspNode& n = nodes[size_t(i)];
    float da = n.plane.distance(a), db = n.plane.distance(b);
    if (da >= 0 && db >= 0) return side(n, true, a, b, ta, tb, enter, enterNode, hit);
    if (da < 0 && db < 0) return side(n, false, a, b, ta, tb, enter, enterNode, hit);
    // Across the plane: the near part first, then from the crossing on.
    float f = da / (da - db);
    Vec3 mid = lerp(a, b, f);
    float tm = ta + (tb - ta) * f;
    bool nearFront = da >= 0;
    if (side(n, nearFront, a, mid, ta, tm, enter, enterNode, hit)) return true;
    return side(n, !nearFront, mid, b, tm, tb, nearFront ? n.plane.n : -n.plane.n, i, hit);
}

bool BspModel::side(const BspNode& n, bool front, Vec3 a, Vec3 b, float ta, float tb, Vec3 enter,
                    int enterNode, Hit& hit) const {
    int32_t c = front ? n.front : n.back;
    if (child(c)) return segment(c, a, b, ta, tb, enter, enterNode, hit);
    if ((front ? n.leafFront : n.leafBack) >= 0) return false;
    hit.time = ta;
    hit.location = a;
    hit.normal = enter;
    hit.node = enterNode;
    hit.startSolid = enterNode < 0;
    return true;
}

Hit BspModel::lineCheck(Vec3 a, Vec3 b) const {
    Hit hit;
    hit.location = b;
    if (!nodes.empty()) segment(0, a, b, 0, 1, Vec3{}, -1, hit);
    return hit;
}

bool BspModel::onPolygon(int node, Vec3 p, float tolerance) const {
    // Through the chain of nodes coplanar with this one: the BSP splits a
    // surface into fragments, and the hit may be on any of them.
    for (size_t guard = 0, i = size_t(node); guard <= nodes.size(); ++guard) {
        const BspNode& n = nodes[i];
        if (n.numVerts >= 3 && std::fabs(n.plane.distance(p)) <= tolerance) {
            // A convex polygon holds the point when no two edges put it on
            // opposite sides, whichever way round the polygon is wound.
            bool pos = false, neg = false;
            for (int k = 0; k < n.numVerts; ++k) {
                Vec3 v0 = points[size_t(vertPoints[size_t(n.vertPool + k)])];
                Vec3 v1 = points[size_t(vertPoints[size_t(n.vertPool + (k + 1) % n.numVerts)])];
                Vec3 out = cross(v1 - v0, n.plane.n);
                float len = length(out);
                if (len == 0) continue;
                float d = dot(out, p - v0) / len;
                pos |= d > tolerance;
                neg |= d < -tolerance;
            }
            if (!(pos && neg)) return true;
        }
        if (!child(n.coplanar)) break;
        i = size_t(n.coplanar);
    }
    return false;
}

}  // namespace ffa
