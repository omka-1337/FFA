#include "world/Terrain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ffa {

namespace {

int32_t count(Reader& r, size_t each) {
    int32_t n = r.idx();
    if (n < 0 || size_t(n) * each > r.limit - r.p) throw FormatError("an array runs past the record");
    return n;
}

Vec3 vec(Reader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

Vec3 unit(Vec3 v) {
    float l = length(v);
    return l > 0 ? v * (1 / l) : v;
}

}  // namespace

Terrain::Terrain(const Package& p, int idx, size_t propertiesEnd, std::vector<uint32_t> visible,
                 std::vector<uint32_t> edgeTurn)
    : visible_(std::move(visible)), edgeTurn_(std::move(edgeTurn)) {
    const Export& e = p.exp(idx);
    size_t end = size_t(e.off) + size_t(e.size);
    Reader r(p.data, propertiesEnd, end);
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) r.idx();   // sectors
    for (int32_t i = 0, n = count(r, 12); i < n; ++i) vertices.push_back(vec(r));
    r.i32();
    r.i32();                                // SectorsX, SectorsY
    for (int32_t i = 0, n = count(r, 24); i < n; ++i) {
        normals.push_back(vec(r));
        normals.push_back(vec(r));
    }
    r.need(96);
    r.p += 96;                              // two FCoords
    X = r.i32();
    Y = r.i32();
    int32_t lights = count(r, 4);
    r.need(size_t(lights) * 4);
    light.assign(p.data.begin() + long(r.p), p.data.begin() + long(r.p + size_t(lights) * 4));
    r.p += size_t(lights) * 4;
    if (r.p != end) throw FormatError("the TerrainInfo does not end on its record");
    if (X < 2 || Y < 2 || vertices.size() != size_t(X) * size_t(Y))
        throw FormatError("the terrain's vertices are not its heightmap grid");
    if (normals.size() != vertices.size() * 2) throw FormatError("the terrain's normals are not two per quad");
    // The grid is regular in X and Y: a vertex is origin + (x step, y step).
    origin_ = vertices[0];
    stepX_ = vertices[1].x - origin_.x;
    stepY_ = vertices[size_t(X)].y - origin_.y;
    if (stepX_ == 0 || stepY_ == 0) throw FormatError("the terrain's grid has no extent");
}

bool Terrain::bit(const std::vector<uint32_t>& w, int x, int y, bool def) const {
    size_t i = size_t(x) + size_t(y) * size_t(X);
    if (w.empty() || (i >> 5) >= w.size()) return def;
    return (w[i >> 5] >> (i & 31)) & 1;
}

void Terrain::quad(int x, int y, int out[2][3]) const {
    int a = y * X + x, b = a + 1, c = a + X, d = c + 1;
    if (turned(x, y)) {
        int t[2][3] = {{a, b, c}, {b, d, c}};
        std::copy(&t[0][0], &t[0][0] + 6, &out[0][0]);
    } else {
        int t[2][3] = {{a, b, d}, {a, d, c}};
        std::copy(&t[0][0], &t[0][0] + 6, &out[0][0]);
    }
}

float Terrain::normalCheck() const {
    float worst = 0;
    for (int y = 0; y + 1 < Y; ++y)
        for (int x = 0; x + 1 < X; ++x) {
            int q[2][3];
            quad(x, y, q);
            for (int k = 0; k < 2; ++k) {
                Vec3 v0 = vertices[size_t(q[k][0])], v1 = vertices[size_t(q[k][1])], v2 = vertices[size_t(q[k][2])];
                Vec3 n = unit(cross(v1 - v0, v2 - v0));
                Vec3 s = normals[size_t(y * X + x) * 2 + size_t(k)];
                worst = std::max({worst, std::fabs(n.x - s.x), std::fabs(n.y - s.y), std::fabs(n.z - s.z)});
            }
        }
    return worst;
}

bool Terrain::cell(int x, int y, Vec3 a, Vec3 b, Hit& hit) const {
    if (x < 0 || y < 0 || x + 1 >= X || y + 1 >= Y || !visible(x, y)) return false;
    int q[2][3];
    quad(x, y, q);
    bool any = false;
    Vec3 d = b - a;
    for (auto& t : q) {
        Vec3 v0 = vertices[size_t(t[0])], v1 = vertices[size_t(t[1])], v2 = vertices[size_t(t[2])];
        Vec3 e1 = v1 - v0, e2 = v2 - v0;
        Vec3 pv = cross(d, e2);
        float det = dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        float inv = 1 / det;
        Vec3 tv = a - v0;
        float u = dot(tv, pv) * inv;
        if (u < 0 || u > 1) continue;
        Vec3 qv = cross(tv, e1);
        float v = dot(d, qv) * inv;
        if (v < 0 || u + v > 1) continue;
        float f = dot(e2, qv) * inv;
        if (f < 0 || f >= hit.time) continue;
        Vec3 n = unit(cross(e1, e2));
        hit.time = f;
        hit.location = lerp(a, b, f);
        hit.normal = dot(n, d) > 0 ? -n : n;
        any = true;
    }
    return any;
}

Hit Terrain::lineCheck(Vec3 a, Vec3 b) const {
    Hit hit;
    hit.location = b;
    // Walk the quads the segment passes over, nearest first, in grid units.
    double u0 = (a.x - origin_.x) / stepX_, v0 = (a.y - origin_.y) / stepY_;
    double u1 = (b.x - origin_.x) / stepX_, v1 = (b.y - origin_.y) / stepY_;
    int x = int(std::floor(u0)), y = int(std::floor(v0));
    int xe = int(std::floor(u1)), ye = int(std::floor(v1));
    double du = u1 - u0, dv = v1 - v0;
    int sx = du > 0 ? 1 : -1, sy = dv > 0 ? 1 : -1;
    const double inf = std::numeric_limits<double>::infinity();
    double tdx = du != 0 ? std::fabs(1 / du) : inf, tdy = dv != 0 ? std::fabs(1 / dv) : inf;
    double tx = du != 0 ? ((sx > 0 ? std::floor(u0) + 1 - u0 : u0 - std::floor(u0)) * tdx) : inf;
    double ty = dv != 0 ? ((sy > 0 ? std::floor(v0) + 1 - v0 : v0 - std::floor(v0)) * tdy) : inf;
    for (int guard = 0; guard < 4 * (X + Y) + 4; ++guard) {
        if (cell(x, y, a, b, hit)) return hit;
        if (x == xe && y == ye) break;
        if (tx < ty) {
            if (tx > 1) break;
            tx += tdx;
            x += sx;
        } else {
            if (ty > 1) break;
            ty += tdy;
            y += sy;
        }
    }
    return hit;
}

}  // namespace ffa
