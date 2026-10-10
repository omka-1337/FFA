#include "audio/BinkVideo.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "core/Package.h"

namespace ffa {

// The tables, at the DLL's addresses (image base 0x30000000); tools/ubinkv.py
// says what each is.
struct BinkTables {
    uint8_t maxlen[16] = {};
    std::vector<uint8_t> trees[16];     // 1 << maxlen entries of (length << 4) | code
    uint8_t runFill[16] = {};
    uint8_t scans[16][64] = {};
    int32_t quantIntra[16][64] = {}, quantInter[16][64] = {};
};

namespace {

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// The DLL's memory by address, what is past a section's raw data zero.
struct Dll {
    std::vector<uint8_t> b;
    struct Section {
        uint32_t start, size, raw, rawSize;
    };
    std::vector<Section> sections;

    explicit Dll(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw FormatError("no " + path);
        b.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') throw FormatError("not a DLL: " + path);
        uint32_t pe = le32(&b[0x3c]);
        if (size_t(pe) + 24 + 96 > b.size() || std::memcmp(&b[pe], "PE\0\0", 4) != 0) throw FormatError("not a DLL: " + path);
        int n = b[pe + 6] | b[pe + 7] << 8, opt = b[pe + 20] | b[pe + 21] << 8;
        uint32_t base = le32(&b[pe + 24 + 28]);
        for (int i = 0; i < n; ++i) {
            size_t o = pe + 24 + size_t(opt) + 40 * size_t(i);
            if (o + 24 > b.size()) throw FormatError("a DLL's sections past its end");
            uint32_t vsize = le32(&b[o + 8]), va = le32(&b[o + 12]), rsize = le32(&b[o + 16]), raw = le32(&b[o + 20]);
            if (size_t(raw) + rsize > b.size()) rsize = raw < b.size() ? uint32_t(b.size() - raw) : 0;
            sections.push_back({base + va, std::max(vsize, rsize), raw, rsize});
        }
    }
    void read(uint32_t va, void* to, size_t n) const {
        uint8_t* out = static_cast<uint8_t*>(to);
        std::memset(out, 0, n);
        for (const Section& s : sections)
            if (va >= s.start && va - s.start < s.size) {
                size_t off = va - s.start;
                if (off < s.rawSize) std::memcpy(out, &b[s.raw + off], std::min<size_t>(n, s.rawSize - off));
                return;
            }
        throw FormatError("not the game's binkw32.dll");
    }
    uint32_t u32(uint32_t va) const {
        uint8_t v[4];
        read(va, v, 4);
        return le32(v);
    }
};

// Least significant bit first, the movie's bytes as one stream; past its end
// zero.
struct Bits {
    const uint8_t* d;
    size_t size, pos, start;
    Bits(const std::vector<uint8_t>& data, size_t at) : d(data.data()), size(data.size()), pos(at * 8), start(at) {}
    uint32_t peek(int n) const {
        size_t byte = pos >> 3;
        uint64_t v = 0;
        for (size_t i = 0; i < 5; ++i)
            if (byte + i < size) v |= uint64_t(d[byte + i]) << (8 * i);
        return uint32_t(v >> (pos & 7)) & ((1u << n) - 1);
    }
    uint32_t read(int n) {
        if (!n) return 0;
        uint32_t v = peek(n);
        pos += size_t(n);
        return v;
    }
    bool bit() { return read(1) != 0; }
    // where the reading ended, at the 32-bit word the DLL last loaded
    size_t wordEnd() const { return start + (pos - start * 8 + 31) / 32 * 4; }
};

// The order coefficients and residues are read in, to raster order.
const uint8_t kOrder[64] = {0,  1,  4,  5,  8,  9,  12, 13, 2,  3,  6,  7,  10, 11, 14, 15, 24, 25, 44, 45, 16, 17,
                            20, 21, 26, 27, 46, 47, 18, 19, 22, 23, 28, 29, 32, 33, 48, 49, 52, 53, 30, 31, 34,
                            35, 50, 51, 54, 55, 36, 37, 40, 41, 56, 57, 60, 61, 38, 39, 42, 43, 58, 59, 62, 63};

// A bundle's code: one of the sixteen tables and the symbols its codes stand
// for (read_tree, 0x3001bc70).
struct Tree {
    const uint8_t* table = nullptr;
    int maxlen = 0;
    uint8_t syms[16];

    void read(Bits& bits, const BinkTables& t) {
        int k = int(bits.read(4));
        table = t.trees[k].data();
        maxlen = t.maxlen[k];
        for (int i = 0; i < 16; ++i) syms[i] = uint8_t(i);
        if (k == 0) return;
        if (bits.bit()) {
            // the first n + 1 symbols listed, then the rest in order
            int n = int(bits.read(3)) + 1;
            bool listed[16] = {};
            int at = 0;
            for (int i = 0; i < n; ++i) {
                int s = int(bits.read(4));
                listed[s] = true;
                syms[at++] = uint8_t(s);
            }
            for (int s = 0; s < 16 && at < 16; ++s)
                if (!listed[s]) syms[at++] = uint8_t(s);
            return;
        }
        int depth = int(bits.read(2));
        uint8_t s[16], out[16];
        for (int i = 0; i < 16; i += 2) {
            bool swap = bits.bit();
            s[i] = uint8_t(swap ? i + 1 : i);
            s[i + 1] = uint8_t(swap ? i : i + 1);
        }
        for (int size = 2; depth > 0; --depth, size *= 2) {
            int o = 0;
            for (int i = 0; i < 16; i += 2 * size) {
                int a = i, ae = i + size, b = i + size, be = i + 2 * size;
                while (a < ae && b < be) out[o++] = bits.bit() ? s[b++] : s[a++];
                while (a < ae) out[o++] = s[a++];
                while (b < be) out[o++] = s[b++];
            }
            std::memcpy(s, out, 16);
        }
        std::memcpy(syms, s, 16);
    }
    int get(Bits& bits) const {
        uint8_t e = table[bits.peek(maxlen)];
        bits.pos += e >> 4;
        return syms[e & 15];
    }
};

int lengthBits(int n) {
    int b = 0;
    for (unsigned v = unsigned(n + 511); v; v >>= 1) ++b;
    return b;
}

struct Bundle {
    int lenbits = 0;
    std::vector<int> data;
    size_t at = 0;
    bool done = false;

    explicit Bundle(int n) : lenbits(lengthBits(n)) {}
    bool empty() const { return !done && at >= data.size(); }
    int take() { return at < data.size() ? data[at++] : 0; }
    // the next part's length; 0 finishes the bundle for the plane
    int start(Bits& bits) {
        int n = int(bits.read(lenbits));
        if (!n) done = true;
        data.clear();
        at = 0;
        return n;
    }
};

void readTypes(Bundle& b, const Tree& tree, Bits& bits, const BinkTables& t) {
    if (!b.empty()) return;
    int n = b.start(bits);
    if (!n) return;
    if (bits.bit()) {
        b.data.assign(size_t(n), int(bits.read(4)));
        return;
    }
    int last = 0;
    while (int(b.data.size()) < n) {
        int v = tree.get(bits);
        if (v < 12) {
            b.data.push_back(v);
            last = v;
        } else {
            b.data.insert(b.data.end(), t.runFill[v], last);
        }
    }
    b.data.resize(size_t(n));
}

void readColours(Bundle& b, const Tree& low, const Tree* high, int& lastHigh, Bits& bits) {
    if (!b.empty()) return;
    int n = b.start(bits);
    if (!n) return;
    bool fill = bits.bit();
    for (int i = 0; i < (fill ? 1 : n); ++i) {
        int hi = high[lastHigh].get(bits);
        int lo = low.get(bits);
        lastHigh = hi;
        b.data.push_back(hi << 4 | lo);
    }
    if (fill) b.data.assign(size_t(n), b.data[0]);
}

void readPatterns(Bundle& b, const Tree& tree, Bits& bits) {
    if (!b.empty()) return;
    int n = b.start(bits);
    for (int i = 0; i < n; ++i) {
        int lo = tree.get(bits);
        b.data.push_back(lo | tree.get(bits) << 4);
    }
}

void readMotion(Bundle& b, const Tree& tree, Bits& bits) {
    if (!b.empty()) return;
    int n = b.start(bits);
    if (!n) return;
    if (bits.bit()) {
        int v = int(bits.read(4));
        if (v && bits.bit()) v = -v;
        b.data.assign(size_t(n), v);
        return;
    }
    for (int i = 0; i < n; ++i) {
        int v = tree.get(bits);
        if (v && bits.bit()) v = -v;
        b.data.push_back(v);
    }
}

void readDc(Bundle& b, Bits& bits, bool isSigned) {
    if (!b.empty()) return;
    int n = b.start(bits);
    if (!n) return;
    int v;
    if (isSigned) {
        v = int(bits.read(10));
        if (v && bits.bit()) v = -v;
    } else {
        v = int(bits.read(11));
    }
    b.data.push_back(v);
    for (int left = n - 1; left > 0;) {
        int group = std::min(8, left);
        int w = int(bits.read(4));
        for (int i = 0; i < group; ++i) {
            if (w) {
                int d = int(bits.read(w));
                if (d && bits.bit()) d = -d;
                v += d;
            }
            b.data.push_back(int(int16_t(uint16_t(v))));
        }
        left -= group;
    }
    b.data[0] = int(int16_t(uint16_t(b.data[0])));
}

void readRuns(Bundle& b, const Tree& tree, Bits& bits) {
    if (!b.empty()) return;
    int n = b.start(bits);
    if (!n) return;
    if (bits.bit()) {
        b.data.assign(size_t(n), int(bits.read(4)));
        return;
    }
    for (int i = 0; i < n; ++i) b.data.push_back(tree.get(bits));
}

// The DCT coefficients after the DC (0x30020b80), in read order: a list of
// entries idx << 2 | mode, walked once a pass from the top value bit down.
void readCoefficients(Bits& bits, int* coef) {
    int n = int(bits.read(4));
    if (!n) return;
    std::vector<int> list{4 << 2 | 0, 24 << 2 | 0, 44 << 2 | 0, 1 << 2 | 3, 2 << 2 | 3, 3 << 2 | 3};
    int mask = 1 << (n - 1);
    for (int b = n - 1; b >= 0; --b, mask >>= 1) {
        auto value = [&] {
            int v = int(bits.read(b)) | mask;
            return bits.bit() ? -v : v;
        };
        size_t i = 0;
        while (i < list.size()) {
            int e = list[i];
            if (!e || !bits.bit()) {
                ++i;
                continue;
            }
            int idx = e >> 2, mode = e & 3;
            if (mode == 3) {
                coef[idx & 63] = value();
                list[i++] = 0;
                continue;
            }
            if (mode == 1) {
                list[i] = idx << 2 | 2;
                list.insert(list.end(), {(idx + 4) << 2 | 2, (idx + 8) << 2 | 2, (idx + 12) << 2 | 2});
                continue;
            }
            if (mode == 0) {
                list[i] = (idx + 4) << 2 | 1;
            } else {
                list[i++] = 0;
            }
            for (int k = idx; k < idx + 4; ++k)
                if (bits.bit()) {
                    list.insert(list.begin(), k << 2 | 3);
                    ++i;
                } else {
                    coef[k & 63] = value();
                }
        }
    }
}

// A residue (0x300214a0): at most limit + 1 changes, in read order.
void readResidue(Bits& bits, int* res, int limit) {
    int n = int(bits.read(3)) + 1;
    int mask = 1 << (n - 1);
    std::vector<int> list{4 << 2 | 0, 24 << 2 | 0, 44 << 2 | 0, 0 << 2 | 2};
    std::vector<int> nz;
    int count = 0;
    auto changed = [&] { return count++ == limit; };
    auto place = [&](int k) {
        nz.push_back(k & 63);
        res[k & 63] = bits.bit() ? -mask : mask;
        return changed();
    };
    for (int pass = 0; pass < n; ++pass, mask >>= 1) {
        size_t refined = nz.size();
        for (size_t j = 0; j < refined; ++j)
            if (bits.bit()) {
                int& r = res[nz[j]];
                r += r < 0 ? -mask : mask;
                if (changed()) return;
            }
        size_t i = 0;
        while (i < list.size()) {
            int e = list[i];
            if (!e || !bits.bit()) {
                ++i;
                continue;
            }
            int idx = e >> 2, mode = e & 3;
            if (mode == 3) {
                list[i++] = 0;
                if (place(idx)) return;
                continue;
            }
            if (mode == 1) {
                list[i] = idx << 2 | 2;
                list.insert(list.end(), {(idx + 4) << 2 | 2, (idx + 8) << 2 | 2, (idx + 12) << 2 | 2});
                continue;
            }
            if (mode == 0) {
                list[i] = (idx + 4) << 2 | 1;
            } else {
                list[i++] = 0;
            }
            for (int k = idx; k < idx + 4; ++k)
                if (bits.bit()) {
                    list.insert(list.begin(), k << 2 | 3);
                    ++i;
                } else if (place(k)) {
                    return;
                }
        }
    }
}

// The butterfly of the IDCT (0x3001f3e0), each product shifted down 11.
void butterfly(const int* s, int* out) {
    int a0 = s[0] + s[4], a1 = s[0] - s[4], a2 = s[2] + s[6];
    int a3 = ((s[2] - s[6]) * 2896 >> 11) - a2;
    int e0 = a0 + a2, e3 = a0 - a2, e1 = a1 + a3, e2 = a1 - a3;
    int t = s[3] + s[5], u = s[5] - s[3], v = s[1] + s[7], w = s[1] - s[7];
    int o = t + v, x = v - t;
    int bb = (w + u) * 3784 >> 11;
    int u2 = (u * -5352 >> 11) - o + bb;
    int x2 = (x * 2896 >> 11) - u2;
    int w2 = ((w * 2217 >> 11) - bb) + x2;
    out[0] = e0 + o;
    out[1] = e1 + u2;
    out[2] = e2 + x2;
    out[3] = e3 - w2;
    out[4] = e3 + w2;
    out[5] = e2 - x2;
    out[6] = e1 - u2;
    out[7] = e0 - o;
}

// Coefficients in read order, dequantised and transformed: each out
// (x + 0x7f) >> 8, which the DLL stores as a byte without clamping.
void idct(const int* coef, const int32_t* quant, int* out) {
    int cols[8][8], s[8], row[8];
    for (int c = 0; c < 8; ++c) {
        for (int r = 0; r < 8; ++r) {
            int k = c + 8 * r;
            s[r] = int(int64_t(coef[kOrder[k]]) * quant[k] >> 11);
        }
        butterfly(s, cols[c]);
    }
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 8; ++c) s[c] = cols[c][r];
        butterfly(s, row);
        for (int c = 0; c < 8; ++c) out[r * 8 + c] = (row[c] + 0x7f) >> 8;
    }
}

using Plane = BinkVideo::Plane;

// 8x8 from a plane at x, y, what is past its edge the edge's.
void ref(const Plane& p, int x, int y, int* px) {
    if (x >= 0 && y >= 0 && x + 8 <= p.w && y + 8 <= p.h) {
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c) px[r * 8 + c] = p.p[size_t((y + r) * p.w + x + c)];
        return;
    }
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            int xx = std::clamp(x + c, 0, p.w - 1), yy = std::clamp(y + r, 0, p.h - 1);
            px[r * 8 + c] = p.p[size_t(yy * p.w + xx)];
        }
}

// 8x8 put at x, y, or each pixel doubled to 16x16; as bytes.
void put(Plane& p, int x, int y, const int* px, int scale = 1) {
    int n = 8 * scale;
    for (int r = 0; r < n && y + r < p.h; ++r)
        for (int c = 0; c < n && x + c < p.w; ++c) p.p[size_t((y + r) * p.w + x + c)] = uint8_t(px[(r / scale) * 8 + c / scale]);
}

struct Planer {
    Bits bits;
    const BinkTables& t;
    Tree types, subs, high[16], low, pats, xt, yt, runt;
    Bundle bTypes, bSubs, bCol, bPat, bX, bY, bIntra, bInter, bRun;
    int lastHigh = 0;

    Planer(const std::vector<uint8_t>& d, size_t at, int w, const BinkTables& tables)
        : bits(d, at), t(tables), bTypes(w >> 3), bSubs(w >> 4), bCol((w >> 3) * 64), bPat((w >> 3) * 8),
          bX(w >> 3), bY(w >> 3), bIntra(w >> 3), bInter(w >> 3), bRun((w >> 3) * 48) {
        types.read(bits, t);
        subs.read(bits, t);
        for (Tree& h : high) h.read(bits, t);
        low.read(bits, t);
        pats.read(bits, t);
        xt.read(bits, t);
        yt.read(bits, t);
        runt.read(bits, t);
    }

    void row() {
        readTypes(bTypes, types, bits, t);
        readTypes(bSubs, subs, bits, t);
        readColours(bCol, low, high, lastHigh, bits);
        readPatterns(bPat, pats, bits);
        readMotion(bX, xt, bits);
        readMotion(bY, yt, bits);
        readDc(bIntra, bits, false);
        readDc(bInter, bits, true);
        readRuns(bRun, runt, bits);
    }

    void run(int* px) {
        const uint8_t* scan = t.scans[bits.read(4)];
        std::fill(px, px + 64, 0);
        int i = 0;
        while (i < 63) {
            if (bits.bit()) {
                int c = bCol.take();
                for (int k = bRun.take() + 1; k > 0 && i < 64; --k) px[scan[i++]] = c;
            } else {
                for (int k = bRun.take() + 1; k > 0 && i < 64; --k) px[scan[i++]] = bCol.take();
            }
        }
        if (i == 63) px[scan[63]] = bCol.take();
    }
    void pattern(int* px) {
        int c0 = bCol.take(), c1 = bCol.take();
        for (int r = 0; r < 8; ++r) {
            int p = bPat.take();
            for (int k = 0; k < 8; ++k) px[r * 8 + k] = (p >> k & 1) ? c1 : c0;
        }
    }
    void intra(int* px) {
        int coef[64] = {};
        coef[0] = bIntra.take();
        readCoefficients(bits, coef);
        idct(coef, t.quantIntra[bits.read(4)], px);
    }
    void raw(int* px) {
        for (int i = 0; i < 64; ++i) px[i] = bCol.take();
    }

    void block(int kind, int x, int y, Plane& cur, const Plane& prev) {
        int px[64];
        switch (kind) {
        case 0: ref(prev, x, y, px); break;
        case 2: {
            int dx = bX.take();
            ref(prev, x + dx, y + bY.take(), px);
            break;
        }
        case 3: run(px); break;
        case 4: {
            int dx = bX.take();
            ref(prev, x + dx, y + bY.take(), px);
            int limit = int(bits.read(7));
            int res[64] = {};
            readResidue(bits, res, limit);
            for (int i = 0; i < 64; ++i) px[i] += res[kOrder[i]];
            break;
        }
        case 5: intra(px); break;
        case 6: std::fill(px, px + 64, bCol.take()); break;
        case 7: {
            int dx = bX.take();
            ref(prev, x + dx, y + bY.take(), px);
            int coef[64] = {}, d[64];
            coef[0] = bInter.take();
            readCoefficients(bits, coef);
            idct(coef, t.quantInter[bits.read(4)], d);
            for (int i = 0; i < 64; ++i) px[i] += d[i];
            break;
        }
        case 8: pattern(px); break;
        case 9: raw(px); break;
        default: throw FormatError("a Bink block of type " + std::to_string(kind));
        }
        put(cur, x, y, px);
    }

    // A 16x16 block from an 8x8 one, each pixel doubled both ways.
    void scaled(int sub, int x, int y, Plane& cur) {
        int px[64];
        switch (sub) {
        case 3: run(px); break;
        case 5: intra(px); break;
        case 6: std::fill(px, px + 64, bCol.take()); break;
        case 8: pattern(px); break;
        case 9: raw(px); break;
        default: return;
        }
        put(cur, x, y, px, 2);
    }
};

// A plane from its bit stream; where its reading ended.
size_t decodePlane(const std::vector<uint8_t>& d, size_t at, Plane& cur, const Plane& prev, const BinkTables& t) {
    Planer pl(d, at, cur.w, t);
    for (int y = 0; y < cur.h; y += 8) {
        pl.row();
        for (int x = 0; x < cur.w;) {
            int kind = pl.bTypes.take();
            if (kind == 1) {
                if (!(y & 8)) pl.scaled(pl.bSubs.take(), x, y, cur);
                x += 16;
                continue;
            }
            pl.block(kind, x, y, cur, prev);
            x += 8;
        }
    }
    return pl.bits.wordEnd();
}

}  // namespace

std::shared_ptr<const BinkTables> readBinkTables(const std::string& dllPath) {
    const Dll dll(dllPath);
    auto t = std::make_shared<BinkTables>();
    dll.read(0x3004f838, t->maxlen, 16);
    for (int k = 0; k < 16; ++k) {
        if (t->maxlen[k] < 1 || t->maxlen[k] > 16) throw FormatError("not the game's binkw32.dll");
        t->trees[k].resize(size_t(1) << t->maxlen[k]);
        dll.read(dll.u32(0x30059000 + 4 * uint32_t(k)), t->trees[k].data(), t->trees[k].size());
        for (uint8_t e : t->trees[k])
            if ((e >> 4) > t->maxlen[k]) throw FormatError("not the game's binkw32.dll");
    }
    dll.read(0x3004f83c, t->runFill, 16);
    dll.read(0x3004aea8, t->scans, sizeof t->scans);
    for (auto& s : t->scans)
        for (uint8_t i : s)
            if (i >= 64) throw FormatError("not the game's binkw32.dll");
    uint8_t q[16 * 64 * 4];
    dll.read(0x3004b2c0, q, sizeof q);
    for (int i = 0; i < 16 * 64; ++i) t->quantIntra[i / 64][i % 64] = int32_t(le32(q + 4 * i));
    dll.read(0x3004d300, q, sizeof q);
    for (int i = 0; i < 16 * 64; ++i) t->quantInter[i / 64][i % 64] = int32_t(le32(q + 4 * i));
    return t;
}

BinkVideo::BinkVideo(std::vector<uint8_t> file, std::shared_ptr<const BinkTables> tables)
    : d_(std::move(file)), t_(std::move(tables)) {
    const std::vector<uint8_t>& d = d_;
    if (d.size() < 44 || std::memcmp(d.data(), "BIK", 3) != 0) throw FormatError("not a Bink file");
    if (d[3] != 'i') throw FormatError(std::string("a Bink file of revision ") + char(d[3]));
    frames_ = int(le32(&d[8]));
    width_ = int(le32(&d[20]));
    height_ = int(le32(&d[24]));
    uint32_t num = le32(&d[28]), den = le32(&d[32]), flags = le32(&d[36]);
    tracks_ = int(le32(&d[40]));
    if (flags & 0x120000) throw FormatError("a Bink movie with alpha or without colour");
    if (width_ <= 0 || height_ <= 0 || width_ > 4096 || height_ > 4096 || frames_ <= 0 || tracks_ > 16)
        throw FormatError("a Bink header out of range");
    fps_ = den ? double(num) / double(den) : 0;
    size_t table = 44 + 12 * size_t(tracks_);
    if (table + 4 * (size_t(frames_) + 1) > d.size()) throw FormatError("a Bink frame table past the file");
    offsets_.resize(size_t(frames_) + 1);
    for (int i = 0; i <= frames_; ++i) offsets_[size_t(i)] = le32(&d[table + 4 * size_t(i)]) & ~1u;
    int w8 = (width_ + 7) & ~7, h8 = (height_ + 7) & ~7;
    int cw = ((width_ + 1) / 2 + 7) & ~7, ch = ((height_ + 1) / 2 + 7) & ~7;
    for (Plane* set : {cur_, prev_})
        for (int k = 0; k < 3; ++k) {
            set[k].w = k ? cw : w8;
            set[k].h = k ? ch : h8;
            set[k].p.assign(size_t(set[k].w) * size_t(set[k].h), 0);
        }
}

BinkVideo::~BinkVideo() = default;

void BinkVideo::rewind() {
    frame_ = -1;
    for (Plane* set : {cur_, prev_})
        for (int k = 0; k < 3; ++k) std::fill(set[k].p.begin(), set[k].p.end(), 0);
}

void BinkVideo::next() {
    if (frame_ + 1 >= frames_) return;
    ++frame_;
    size_t at = offsets_[size_t(frame_)];
    for (int k = 0; k < tracks_; ++k) {            // the audio packets first
        if (at + 4 > d_.size()) return;
        at += 4 + le32(&d_[at]);
    }
    if (at + 4 > d_.size()) return;
    size_t size = le32(&d_[at]);
    for (int k = 0; k < 3; ++k) std::swap(cur_[k], prev_[k]);
    decodePlane(d_, at + 4, cur_[0], prev_[0], *t_);
    size_t end = decodePlane(d_, at + size, cur_[1], prev_[1], *t_);
    decodePlane(d_, end, cur_[2], prev_[2], *t_);
}

void BinkVideo::rgba(uint8_t* out) const {
    const Plane &y = cur_[0], &cr = cur_[1], &cb = cur_[2];
    auto clamp8 = [](int v) { return uint8_t(std::clamp(v >> 16, 0, 255)); };
    for (int r = 0; r < height_; ++r) {
        const uint8_t* yp = &y.p[size_t(r * y.w)];
        const uint8_t* vp = &cr.p[size_t((r / 2) * cr.w)];
        const uint8_t* up = &cb.p[size_t((r / 2) * cb.w)];
        uint8_t* o = out + size_t(r) * size_t(width_) * 4;
        for (int c = 0; c < width_; ++c, o += 4) {
            int l = (yp[c] - 16) * 76309 + 32768, v = vp[c / 2] - 128, u = up[c / 2] - 128;
            o[0] = clamp8(l + 104597 * v);
            o[1] = clamp8(l - 53279 * v - 25675 * u);
            o[2] = clamp8(l + 132201 * u);
            o[3] = 255;
        }
    }
}

void BinkVideo::yuv(std::vector<uint8_t>& out) const {
    out.clear();
    const int cw = (width_ + 1) / 2, ch = (height_ + 1) / 2;
    for (int k : {0, 2, 1}) {
        const Plane& p = cur_[k];
        int w = k ? cw : width_, h = k ? ch : height_;
        for (int r = 0; r < h; ++r) out.insert(out.end(), p.p.begin() + r * p.w, p.p.begin() + r * p.w + w);
    }
}

}  // namespace ffa
