#include "world/SkeletalMesh.h"

#include <cmath>
#include <cstring>
#include <strings.h>

namespace ffa {

void rotationAxes(int32_t pitch, int32_t yaw, int32_t roll, float out[3][3]);

Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

Quat qconj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

Vec3 qrot(Quat q, Vec3 v) {
    Quat r = qmul(qmul(q, Quat{v.x, v.y, v.z, 0}), qconj(q));
    return {r.x, r.y, r.z};
}

Quat qnlerp(Quat a, Quat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    Quat q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n == 0) n = 1;
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

namespace {

struct Lazy {
    size_t start = 0;
    int32_t count = 0;
    int width = 0;
};

// Every self consistent lazy array from `start` on: a u32 offset just past
// it, a compact count, and elements of 8, 10 or 12 bytes filling it exactly.
std::vector<Lazy> lazyArrays(const Package& p, size_t start, size_t end) {
    std::vector<Lazy> out;
    const uint8_t* b = p.data.data();
    size_t o = start;
    while (o + 5 < end) {
        uint32_t skip;
        std::memcpy(&skip, b + o, 4);
        if (o + 5 <= skip && skip <= end) {
            try {
                Reader r(p.data, o + 4, end);
                int32_t n = r.idx();
                if (n > 0 && r.p < skip && (skip - r.p) % size_t(n) == 0) {
                    size_t width = (skip - r.p) / size_t(n);
                    if (width == 8 || width == 10 || width == 12) {
                        out.push_back({r.p, n, int(width)});
                        o = skip;
                        continue;
                    }
                }
            } catch (const FormatError&) {
            }
        }
        ++o;
    }
    return out;
}

}  // namespace

SkeletalMesh::SkeletalMesh(const Package& p, int idx) : package(&p), index(idx) {
    const Export& e = p.exp(idx);
    size_t end = size_t(e.off) + size_t(e.size);
    const uint8_t* b = p.data.data();
    Reader r(p.data, size_t(e.off), end);
    if (r.idx() != p.noneIndex) throw FormatError("the SkeletalMesh has properties");
    r.need(41);
    r.p += 25 + 16;                         // bounding box, bounding sphere
    r.u32();
    r.u32();                                // Version, VertexCount
    int32_t packed = r.idx();
    r.need(size_t(packed) * 4);
    r.p += size_t(packed) * 4;
    for (int32_t i = 0, n = r.idx(); i < n && i < 256; ++i) materials.push_back(r.idx());
    scale = {r.f32(), r.f32(), r.f32()};
    origin = {r.f32(), r.f32(), r.f32()};
    for (int32_t& k : rotOrigin) k = r.i32();
    // the reference skeleton, by its signature
    for (size_t o = size_t(e.off) + 42; o + 60 < end && bones.empty(); ++o) {
        try {
            Reader s(p.data, o, end);
            int32_t c = s.idx();
            if (c < 1 || c > 400) continue;
            size_t q = s.p;
            std::vector<SkelBone> found;
            for (int32_t i = 0; i < c; ++i) {
                Reader rr(p.data, q, end);
                int32_t ni = rr.idx();
                if (ni <= 0 || !p.validName(ni) || rr.p + 56 > end) break;
                SkelBone bn;
                bn.name = p.names[size_t(ni)];
                float f[7];
                std::memcpy(f, b + rr.p + 4, 28);
                bn.rotation = {f[0], f[1], f[2], f[3]};
                float len = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2] + f[3] * f[3]);
                if (std::fabs(len - 1) > 1e-3f) break;
                bn.position = {f[4], f[5], f[6]};
                std::memcpy(&bn.parent, b + rr.p + 52, 4);
                found.push_back(bn);
                q = rr.p + 56;
            }
            if (found.size() != size_t(c)) continue;
            bool ok = true;
            for (const SkelBone& bn : found) ok &= bn.parent >= 0 && bn.parent < c;
            if (!ok) continue;
            bones = std::move(found);
            Reader rr(p.data, q, end);
            defaultAnim = rr.idx();
        } catch (const FormatError&) {
        }
    }
    // the first LOD model: four lazy arrays of the right widths in order
    std::vector<Lazy> arrays = lazyArrays(p, r.p, end);
    const int want[4] = {8, 10, 8, 12};
    for (size_t i = 0; i + 4 <= arrays.size(); ++i) {
        if (arrays[i].width != want[0] || arrays[i + 1].width != want[1] || arrays[i + 2].width != want[2] ||
            arrays[i + 3].width != want[3])
            continue;
        const Lazy& inf = arrays[i];
        const Lazy& wed = arrays[i + 1];
        const Lazy& fac = arrays[i + 2];
        const Lazy& pts = arrays[i + 3];
        for (int32_t k = 0; k < inf.count; ++k) {
            Influence x;
            std::memcpy(&x.weight, b + inf.start + size_t(k) * 8, 4);
            std::memcpy(&x.point, b + inf.start + size_t(k) * 8 + 4, 2);
            std::memcpy(&x.bone, b + inf.start + size_t(k) * 8 + 6, 2);
            influences.push_back(x);
        }
        for (int32_t k = 0; k < wed.count; ++k) {
            Wedge w;
            std::memcpy(&w.point, b + wed.start + size_t(k) * 10, 2);
            std::memcpy(&w.u, b + wed.start + size_t(k) * 10 + 2, 4);
            std::memcpy(&w.v, b + wed.start + size_t(k) * 10 + 6, 4);
            wedges.push_back(w);
        }
        for (int32_t k = 0; k < fac.count; ++k) {
            Face f;
            std::memcpy(&f, b + fac.start + size_t(k) * 8, 8);
            faces.push_back(f);
        }
        for (int32_t k = 0; k < pts.count; ++k) {
            Vec3 v;
            std::memcpy(&v, b + pts.start + size_t(k) * 12, 12);
            points.push_back(v);
        }
        break;
    }
    if (points.empty()) throw FormatError("the SkeletalMesh's geometry is not found");
    for (const Wedge& w : wedges)
        if (w.point >= points.size()) throw FormatError("a wedge names a point out of range");
    for (const Face& f : faces)
        for (uint16_t w : f.wedge)
            if (w >= wedges.size()) throw FormatError("a face names a wedge out of range");
    reference_ = compose(referenceLocals());
}

int SkeletalMesh::bone(const std::string& name) const {
    for (size_t i = 0; i < bones.size(); ++i)
        if (strcasecmp(bones[i].name.c_str(), name.c_str()) == 0) return int(i);
    return -1;
}

std::vector<BoneTransform> SkeletalMesh::referenceLocals() const {
    std::vector<BoneTransform> out;
    for (const SkelBone& b : bones) out.push_back({b.rotation, b.position});
    return out;
}

std::vector<BoneTransform> SkeletalMesh::compose(const std::vector<BoneTransform>& locals) const {
    std::vector<BoneTransform> g(locals.size());
    for (size_t i = 0; i < locals.size() && i < bones.size(); ++i) {
        Quat lq = i == 0 ? locals[i].q : qconj(locals[i].q);
        if (i == 0) {
            g[i] = {lq, locals[i].p};
        } else {
            const BoneTransform& pa = g[size_t(bones[i].parent)];
            g[i] = {qmul(pa.q, lq), pa.p + qrot(pa.q, locals[i].p)};
        }
    }
    return g;
}


const std::vector<int>& SkeletalMesh::trackMap(const MeshAnimation& anim, size_t chunk) const {
    std::vector<std::vector<int>>& per = tracks_[&anim];
    if (per.size() != anim.chunks.size()) per.assign(anim.chunks.size(), {});
    std::vector<int>& map = per[chunk];
    if (!map.empty() || bones.empty()) return map;
    map.assign(bones.size(), -1);
    const AnimChunk& c = anim.chunks[chunk];
    for (size_t i = 0; i < bones.size(); ++i) {
        int j = -1;
        for (size_t k = 0; k < anim.bones.size(); ++k)
            if (anim.bones[k] == bones[i].name) {
                j = int(k);
                break;
            }
        if (j < 0) continue;
        if (!c.boneIndices.empty()) {
            for (size_t k = 0; k < c.boneIndices.size(); ++k)
                if (c.boneIndices[k] == j && k < c.tracks.size()) map[i] = int(k);
        } else if (size_t(j) < c.tracks.size()) {
            map[i] = j;
        }
    }
    return map;
}

std::vector<BoneTransform> SkeletalMesh::locals(const MeshAnimation& anim, size_t sequence, float frame) const {
    std::vector<BoneTransform> out = referenceLocals();
    if (sequence >= anim.chunks.size()) return out;
    const AnimChunk& chunk = anim.chunks[sequence];
    const std::vector<int>& map = trackMap(anim, sequence);
    for (size_t i = 0; i < bones.size(); ++i) {
        if (map[i] < 0) continue;
        const AnimTrack* tr = &chunk.tracks[size_t(map[i])];
        if (!tr->rotations.empty()) {
            auto zero = [](const Quat& k) {
                return std::fabs(k.x) <= 1e-6f && std::fabs(k.y) <= 1e-6f && std::fabs(k.z) <= 1e-6f &&
                       std::fabs(k.w) <= 1e-6f;
            };
            const Quat ref = bones[i].rotation;
            auto q = [&](size_t k) { return zero(tr->rotations[k]) ? ref : tr->rotations[k]; };
            // as keyAt, with zero keys standing for the reference
            size_t n = tr->rotations.size();
            auto t = [&](size_t k) { return tr->times.size() == n ? tr->times[k] : float(k); };
            if (frame <= t(0) || n == 1) {
                out[i].q = q(0);
            } else {
                out[i].q = q(n - 1);
                for (size_t k = 1; k < n; ++k)
                    if (frame <= t(k)) {
                        float t0 = t(k - 1), t1 = t(k);
                        out[i].q = qnlerp(q(k - 1), q(k), t1 > t0 ? (frame - t0) / (t1 - t0) : 0.0f);
                        break;
                    }
            }
        }
        if (!tr->positions.empty()) {
            size_t n = tr->positions.size() / 3;
            auto pk = [&](size_t k) { return Vec3{tr->positions[3 * k], tr->positions[3 * k + 1], tr->positions[3 * k + 2]}; };
            auto t = [&](size_t k) { return tr->times.size() == n ? tr->times[k] : float(k); };
            if (frame <= t(0) || n == 1) {
                out[i].p = pk(0);
            } else {
                out[i].p = pk(n - 1);
                for (size_t k = 1; k < n; ++k)
                    if (frame <= t(k)) {
                        float t0 = t(k - 1), t1 = t(k);
                        out[i].p = lerp(pk(k - 1), pk(k), t1 > t0 ? (frame - t0) / (t1 - t0) : 0.0f);
                        break;
                    }
            }
        }
    }
    return out;
}

std::vector<Vec3> SkeletalMesh::skin(const std::vector<BoneTransform>& cur) const {
    std::vector<float> acc(points.size() * 4, 0.0f);
    for (const Influence& x : influences) {
        if (x.point >= points.size() || x.bone >= bones.size() || x.weight <= 0) continue;
        const BoneTransform& rf = reference_[x.bone];
        const BoneTransform& c = cur[x.bone];
        Vec3 local = qrot(qconj(rf.q), points[x.point] - rf.p);
        Vec3 w = qrot(c.q, local) + c.p;
        float* a = &acc[size_t(x.point) * 4];
        a[0] += x.weight * w.x;
        a[1] += x.weight * w.y;
        a[2] += x.weight * w.z;
        a[3] += x.weight;
    }
    std::vector<Vec3> out(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        const float* a = &acc[i * 4];
        out[i] = a[3] > 0 ? Vec3{a[0] / a[3], a[1] / a[3], a[2] / a[3]} : points[i];
    }
    return out;
}

Vec3 SkeletalMesh::toActor(Vec3 p) const {
    float ax[3][3];
    rotationAxes(rotOrigin[0], rotOrigin[1], rotOrigin[2], ax);
    Vec3 u{(p.x - origin.x) * scale.x, (p.y - origin.y) * scale.y, (p.z - origin.z) * scale.z};
    return {ax[0][0] * u.x + ax[1][0] * u.y + ax[2][0] * u.z, ax[0][1] * u.x + ax[1][1] * u.y + ax[2][1] * u.z,
            ax[0][2] * u.x + ax[1][2] * u.y + ax[2][2] * u.z};
}

}  // namespace ffa
