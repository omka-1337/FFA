#include "world/Animation.h"

#include <cmath>
#include <cstring>
#include <strings.h>

namespace ffa {

namespace {

int32_t count(Reader& r, size_t each) {
    int32_t n = r.idx();
    if (n < 0 || size_t(n) * each > r.limit - r.p) throw FormatError("an array runs past the record");
    return n;
}

void skip(Reader& r, size_t n) {
    r.need(n);
    r.p += n;
}

void track(Reader& r) {
    r.u32();
    skip(r, size_t(count(r, 16)) * 16);
    skip(r, size_t(count(r, 12)) * 12);
    skip(r, size_t(count(r, 4)) * 4);
}

std::string nameAt(const Package& p, int32_t i) {
    if (!p.validName(i)) throw FormatError("a name index is out of range");
    return p.names[size_t(i)];
}

}  // namespace

MeshAnimation::MeshAnimation(const Package& p, int idx) : package(&p), index(idx) {
    const Export& e = p.exp(idx);
    size_t end = size_t(e.off) + size_t(e.size);
    Reader r(p.data, size_t(e.off), end);
    if (r.idx() != p.noneIndex) throw FormatError("the MeshAnimation has properties");
    uint32_t version = r.u32();
    for (int32_t i = 0, n = count(r, 9); i < n; ++i) {
        bones.push_back(nameAt(p, r.idx()));
        r.u32();
        r.i32();
    }
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        skip(r, 16);                        // RootSpeed3D, TrackTime
        r.i32();
        r.u32();                            // StartBone, Flags
        skip(r, size_t(count(r, 4)) * 4);   // bone indices
        for (int32_t k = 0, t = count(r, 1); k < t; ++k) track(r);
        track(r);                           // the root
        if (version >= 4) r.idx();
    }
    for (int32_t i = 0, n = count(r, 1); i < n; ++i) {
        AnimSequence s;
        r.f32();
        s.name = nameAt(p, r.idx());
        for (int32_t k = 0, g = count(r, 1); k < g; ++k) s.groups.push_back(nameAt(p, r.idx()));
        s.startFrame = r.i32();
        s.numFrames = r.i32();
        for (int32_t k = 0, m = count(r, 1); k < m; ++k) {
            AnimNotifyKey key;
            key.time = r.f32();
            key.name = nameAt(p, r.idx());
            key.object = r.idx();
            s.notifies.push_back(key);
        }
        s.rate = r.f32();
        sequences.push_back(std::move(s));
    }
    if (r.p != end) throw FormatError("the MeshAnimation does not end on its record");
}

const AnimSequence* MeshAnimation::find(const std::string& name) const {
    for (const AnimSequence& s : sequences)
        if (strcasecmp(s.name.c_str(), name.c_str()) == 0) return &s;
    return nullptr;
}

int32_t skeletalDefaultAnim(const Package& p, int idx) {
    // The reference skeleton has a signature: a compact count, then per bone
    // a name index, u32 flags, a unit quaternion, a position, a length, three
    // sizes, a child count and a parent index, every parent a bone. The
    // default animation reference follows the last bone.
    const Export& e = p.exp(idx);
    size_t end = size_t(e.off) + size_t(e.size);
    const uint8_t* b = p.data.data();
    for (size_t o = size_t(e.off) + 42; o + 60 < end; ++o) {
        try {
            Reader r(p.data, o, end);
            int32_t c = r.idx();
            if (c < 1 || c > 400) continue;
            size_t q = r.p;
            bool ok = true;
            std::vector<int32_t> parents;
            for (int32_t i = 0; i < c && ok; ++i) {
                Reader rr(p.data, q, end);
                int32_t ni = rr.idx();
                if (ni <= 0 || !p.validName(ni) || rr.p + 56 > end) {
                    ok = false;
                    break;
                }
                float quat[4];
                std::memcpy(quat, b + rr.p + 4, 16);
                float len = std::sqrt(quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3]);
                if (std::fabs(len - 1) > 1e-3f) {
                    ok = false;
                    break;
                }
                int32_t parent;
                std::memcpy(&parent, b + rr.p + 52, 4);
                parents.push_back(parent);
                q = rr.p + 56;
            }
            if (!ok) continue;
            bool parentsOk = true;
            for (int32_t pa : parents) parentsOk &= pa >= 0 && pa < c;
            if (!parentsOk) continue;
            Reader rr(p.data, q, end);
            return rr.idx();
        } catch (const FormatError&) {
            continue;
        }
    }
    return 0;
}

}  // namespace ffa
