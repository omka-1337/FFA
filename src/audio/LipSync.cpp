#include "audio/LipSync.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ffa {

const char* const kVisemeNames[V_Count] = {"AI", "E", "O", "U", "CDGKNRSthYZ", "L", "WQ", "MBP", "FV"};

namespace {

int32_t le32(const uint8_t* p) {
    return int32_t(uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
}

// A compact index, as packages write them; false past the end.
bool compact(const uint8_t* d, size_t size, size_t& p, int32_t& out) {
    if (p >= size) return false;
    uint8_t b0 = d[p++];
    int64_t v = b0 & 0x3F;
    if (b0 & 0x40)
        for (int shift = 6, n = 0; n < 4; ++n, shift += 7) {
            if (p >= size) return false;
            uint8_t c = d[p++];
            v |= int64_t(c & 0x7F) << shift;
            if (!(c & 0x80)) break;
        }
    out = int32_t(b0 & 0x80 ? -v : v);
    return true;
}

// Each phoneme channel's viseme. The letters a channel's peaks go with over
// the 1935 lines with subtitles: 0 e, ee; 1 er, r; 2 i, a; 3 the a of
// "Donkey"; 4 o, oo, ou; 5 w; 6 s; 7 sh, j, ch; 8 f, v; 9 th; 10 t, d, k;
// 11 m, b, p; 12 n, ng, g; 13 r. 14 never rises past 0.22.
const Viseme kPhoneme[15] = {V_E,   V_E,   V_AI, V_AI,  V_O,  V_WQ,  V_CDG, V_CDG,
                             V_FV,  V_CDG, V_CDG, V_MBP, V_CDG, V_CDG, V_L};

}  // namespace

bool parseLipSync(const uint8_t* d, size_t size, LipSync& out) {
    if (size < 4) return false;
    out = LipSync();
    out.version = le32(d);
    size_t p = 4;
    if (out.version == 1) {
        int32_t n;
        if (!compact(d, size, p, n) || n < 0 || p + size_t(n) + 4 != size) return false;
        out.rate = 50;
        out.first = 0;
        out.channels = 1;
        out.values.resize(size_t(n));
        for (int32_t i = 0; i < n; ++i) out.values[size_t(i)] = float(d[p + size_t(i)]) / 255.0f;
        return n > 0;
    }
    if (out.version != 2 || size < 24) return false;
    int32_t rate = le32(d + 8), first = le32(d + 12), last = le32(d + 16);
    p = 20;
    int32_t n;
    if (rate <= 0 || last < first || !compact(d, size, p, n)) return false;
    if (n != (last - first + 1) * out.channels || p + size_t(n) * 4 + 4 != size) return false;
    out.rate = float(rate);
    out.first = first;
    out.values.resize(size_t(n));
    std::memcpy(out.values.data(), d + p, size_t(n) * 4);
    return true;
}

FaceWeights faceAt(const LipSync& lips, float t) {
    FaceWeights w;
    int frames = lips.frames();
    if (frames == 0) return w;
    float f = t * lips.rate - float(lips.first);
    if (f < 0 || f > float(frames - 1)) return w;
    int k = std::min(int(f), frames - 1), k1 = std::min(k + 1, frames - 1);
    float u = f - float(k);
    auto at = [&](int c) {
        return lips.values[size_t(k * lips.channels + c)] * (1 - u) + lips.values[size_t(k1 * lips.channels + c)] * u;
    };
    if (lips.version == 1) {
        w.viseme[V_AI] = at(0);
        return w;
    }
    for (int c = 0; c < 15; ++c) w.viseme[kPhoneme[c]] += at(c);
    for (float& v : w.viseme) v = std::clamp(v, 0.0f, 1.0f);
    w.blink[0] = std::clamp(at(17), 0.0f, 1.0f);
    w.blink[1] = std::clamp(at(18), 0.0f, 1.0f);
    return w;
}

}  // namespace ffa
