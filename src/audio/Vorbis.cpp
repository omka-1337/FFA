#include "audio/Vorbis.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include "core/Package.h"

namespace ffa {

namespace {

// --- Ogg: the packets in order, and the granule of the page each ends on

struct Packet {
    size_t at, size;            // in `bytes`
    int64_t granule;            // -1 when another ends after it on its page
};

std::vector<Packet> oggPackets(const std::vector<uint8_t>& d, std::vector<uint8_t>& bytes) {
    std::vector<Packet> out;
    size_t pos = 0;
    size_t start = 0;
    bool open = false;
    while (pos + 27 <= d.size()) {
        if (std::memcmp(&d[pos], "OggS", 4) != 0) throw FormatError("not an Ogg page");
        int64_t granule = 0;
        std::memcpy(&granule, &d[pos + 6], 8);
        int nseg = d[pos + 26];
        if (pos + 27 + size_t(nseg) > d.size()) throw FormatError("an Ogg page past the file");
        const uint8_t* lacing = &d[pos + 27];
        size_t at = pos + 27 + size_t(nseg);
        int last = -1;
        for (int i = 0; i < nseg; ++i)
            if (lacing[i] < 255) last = i;
        for (int i = 0; i < nseg; ++i) {
            if (!open) {
                start = bytes.size();
                open = true;
            }
            size_t v = lacing[i];
            if (at + v > d.size()) throw FormatError("an Ogg segment past the file");
            bytes.insert(bytes.end(), d.begin() + long(at), d.begin() + long(at + v));
            at += v;
            if (v < 255) {
                out.push_back({start, bytes.size() - start, i == last ? granule : -1});
                open = false;
            }
        }
        pos = at;
    }
    return out;
}

// --- bits, least significant first

struct EndOfPacket {};

struct Bits {
    const uint8_t* d;
    size_t pos = 0, n;
    Bits(const uint8_t* data, size_t size) : d(data), n(size * 8) {}
    uint32_t read(int k) {
        if (pos + size_t(k) > n) {
            pos = n;
            throw EndOfPacket{};
        }
        uint32_t v = 0;
        for (int i = 0; i < k; ++i, ++pos) v |= uint32_t((d[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
    bool bit() { return read(1) != 0; }
};

int ilog(uint32_t x) {
    int b = 0;
    while (x) {
        ++b;
        x >>= 1;
    }
    return b;
}

float float32Unpack(uint32_t x) {
    double mantissa = double(x & 0x1fffff);
    int exponent = int((x & 0x7fe00000) >> 21);
    if (x & 0x80000000) mantissa = -mantissa;
    return float(std::ldexp(mantissa, exponent - 788));
}

uint32_t lookup1Values(uint32_t entries, uint32_t dims) {
    uint32_t r = uint32_t(std::pow(double(entries), 1.0 / double(dims)));
    auto pw = [&](uint64_t b) {
        uint64_t v = 1;
        for (uint32_t i = 0; i < dims && v <= entries; ++i) v *= b;
        return v;
    };
    while (pw(r + 1) <= entries) ++r;
    while (r > 0 && pw(r) > entries) --r;
    return r;
}

// --- codebooks

struct Codebook {
    int dims = 0;
    int entries = 0;
    // the codeword trie: a node's two children, an entry as -(entry + 1)
    std::vector<std::array<int32_t, 2>> trie;
    int single = -1;            // the one entry of a codebook of one
    std::vector<float> vectors; // entries x dims, with a lookup

    int scalar(Bits& b) const {
        if (single >= 0) {
            b.read(1);
            return single;
        }
        int32_t node = 0;
        for (;;) {
            int32_t next = trie[size_t(node)][b.bit() ? 1 : 0];
            if (next < 0) return -next - 1;
            if (next == 0) throw FormatError("a Vorbis codeword not in its codebook");
            node = next;
        }
    }
    const float* vector(Bits& b) const { return &vectors[size_t(scalar(b)) * size_t(dims)]; }
};

// Each entry in turn the lowest valued codeword of its length that is not
// taken, nor the prefix of one taken, nor has one taken as its prefix.
void assignCodewords(Codebook& cb, const std::vector<int>& lengths) {
    // a node: children 0 free, > 0 a node, < 0 an entry's leaf
    cb.trie.assign(1, {0, 0});
    std::vector<bool> full(1, false);
    // place a leaf `want` deep under node, the lowest code first
    std::function<bool(int32_t, int, int, int)> place = [&](int32_t node, int depth, int want, int entry) -> bool {
        for (int b = 0; b < 2; ++b) {
            int32_t child = cb.trie[size_t(node)][size_t(b)];
            if (depth + 1 == want) {
                if (child == 0) {
                    cb.trie[size_t(node)][size_t(b)] = -(entry + 1);
                    return true;
                }
                continue;
            }
            if (child < 0) continue;
            if (child == 0) {
                child = int32_t(cb.trie.size());
                cb.trie.push_back({0, 0});
                full.push_back(false);
                cb.trie[size_t(node)][size_t(b)] = child;
            }
            if (full[size_t(child)]) continue;
            if (place(child, depth + 1, want, entry)) {
                // a node is full when both its children are leaves or full
                auto done = [&](int32_t c) { return c < 0 || (c > 0 && full[size_t(c)]); };
                if (done(cb.trie[size_t(child)][0]) && done(cb.trie[size_t(child)][1])) full[size_t(child)] = true;
                return true;
            }
        }
        return false;
    };
    int used = 0, last = -1;
    for (int e = 0; e < int(lengths.size()); ++e)
        if (lengths[size_t(e)]) {
            ++used;
            last = e;
        }
    if (used == 1) {
        cb.single = last;
        return;
    }
    for (int e = 0; e < int(lengths.size()); ++e) {
        int l = lengths[size_t(e)];
        if (!l) continue;
        if (!place(0, 0, l, e)) throw FormatError("Vorbis codeword lengths that overflow");
    }
}

Codebook readCodebook(Bits& b) {
    Codebook cb;
    if (b.read(24) != 0x564342) throw FormatError("a Vorbis codebook without its sync pattern");
    cb.dims = int(b.read(16));
    cb.entries = int(b.read(24));
    std::vector<int> lengths(size_t(cb.entries), 0);
    if (!b.bit()) {
        bool sparse = b.bit();
        for (int i = 0; i < cb.entries; ++i)
            if (!sparse || b.bit()) lengths[size_t(i)] = int(b.read(5)) + 1;
    } else {
        int length = int(b.read(5)) + 1;
        for (int i = 0; i < cb.entries;) {
            int n = int(b.read(ilog(uint32_t(cb.entries - i))));
            for (int k = i; k < i + n && k < cb.entries; ++k) lengths[size_t(k)] = length;
            i += n;
            ++length;
        }
    }
    assignCodewords(cb, lengths);
    int type = int(b.read(4));
    if (type == 1 || type == 2) {
        float minimum = float32Unpack(b.read(32)), delta = float32Unpack(b.read(32));
        int valueBits = int(b.read(4)) + 1;
        bool sequence = b.bit();
        uint32_t count = type == 1 ? lookup1Values(uint32_t(cb.entries), uint32_t(cb.dims))
                                   : uint32_t(cb.entries) * uint32_t(cb.dims);
        std::vector<uint32_t> mult(count);
        for (uint32_t& m : mult) m = b.read(valueBits);
        cb.vectors.assign(size_t(cb.entries) * size_t(cb.dims), 0.0f);
        for (int e = 0; e < cb.entries; ++e) {
            float last = 0;
            uint64_t div = 1;
            for (int i = 0; i < cb.dims; ++i) {
                uint32_t off = type == 1 ? uint32_t((uint64_t(e) / div) % count) : uint32_t(e * cb.dims + i);
                float x = float(mult[off]) * delta + minimum + last;
                if (sequence) last = x;
                cb.vectors[size_t(e) * size_t(cb.dims) + size_t(i)] = x;
                div *= count;
            }
        }
    } else if (type != 0) {
        throw FormatError("a Vorbis codebook lookup of type " + std::to_string(type));
    }
    return cb;
}

// --- the setup

struct Floor1 {
    std::vector<int> partClass, classDims, classSub, classMaster;
    std::vector<std::vector<int>> subBooks;
    int multiplier = 1;
    std::vector<int> X, order, low, high;
};

struct Residue {
    int kind = 0;
    uint32_t begin = 0, end = 0, psize = 1;
    int classes = 1, classbook = 0;
    std::vector<std::array<int, 8>> books;   // -1 for none
};

struct Mapping {
    int submaps = 1;
    std::vector<std::pair<int, int>> coupling;
    std::vector<int> mux, floor, residue;
};

// floor 1's dB table: 256 steps over 140 dB
float inverseDb(int i) {
    static float table[256];
    static bool made = false;
    if (!made) {
        for (int k = 0; k < 256; ++k) table[k] = float(std::pow(10.0, 7.0 * (k + 1) / 256.0 - 7.0));
        made = true;
    }
    return table[std::clamp(i, 0, 255)];
}

int renderPoint(int x0, int y0, int x1, int y1, int x) {
    int dy = y1 - y0, adx = x1 - x0;
    int off = std::abs(dy) * (x - x0) / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

void renderLine(int x0, int y0, int x1, int y1, std::vector<int>& v, int n) {
    int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
    int base = dy / adx;             // toward zero, as C divides
    int sy = dy < 0 ? base - 1 : base + 1;
    ady -= std::abs(base) * adx;
    int y = y0, err = 0;
    if (x0 < n) v[size_t(x0)] = y;
    for (int x = x0 + 1; x < std::min(x1, n); ++x) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        v[size_t(x)] = y;
    }
}

// In place, forward, e^(-2 pi i jk / n), n a power of two.
void fft(std::vector<std::complex<float>>& a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t size = 2; size <= n; size <<= 1) {
        double ang = -2 * M_PI / double(size);
        std::complex<float> w(float(std::cos(ang)), float(std::sin(ang)));
        for (size_t s = 0; s < n; s += size) {
            std::complex<float> wk(1, 0);
            for (size_t k = 0; k < size / 2; ++k) {
                std::complex<float> u = a[s + k], t = a[s + k + size / 2] * wk;
                a[s + k] = u + t;
                a[s + k + size / 2] = u - t;
                wk *= w;
            }
        }
    }
}

// y[n] = sum X[k] cos(2 pi / N (n + N/4 + 1/2)(k + 1/2)), from the DCT-IV of
// the M = N / 2 coefficients by an FFT of M / 2, and its symmetries.
struct Imdct {
    int M = 0;
    std::vector<std::complex<float>> pre, post, buf;
    std::vector<float> z;
    explicit Imdct(int m) : M(m), pre(size_t(m / 2)), post(size_t(m / 2)), buf(size_t(m / 2)), z(size_t(m)) {
        for (int k = 0; k < m / 2; ++k) {
            pre[size_t(k)] = std::polar(1.0f, float(-M_PI * (4 * k + 1) / (4.0 * m)));
            post[size_t(k)] = std::polar(1.0f, float(-M_PI * k / double(m)));
        }
    }
    void run(const float* X, float* y) {
        int h = M / 2;
        for (int k = 0; k < h; ++k)
            buf[size_t(k)] = std::complex<float>(X[2 * k], X[M - 1 - 2 * k]) * pre[size_t(k)];
        fft(buf);
        for (int k = 0; k < h; ++k) {
            std::complex<float> c = buf[size_t(k)] * post[size_t(k)];
            z[size_t(2 * k)] = c.real();
            z[size_t(M - 1 - 2 * k)] = -c.imag();
        }
        for (int n = 0; n < 2 * M; ++n) {
            int m = n + M / 2;
            y[n] = m < M ? z[size_t(m)] : m < 2 * M ? -z[size_t(2 * M - 1 - m)] : -z[size_t(m - 2 * M)];
        }
    }
};

}  // namespace

struct VorbisDecoder::Impl {
    std::vector<uint8_t> file, bytes;
    std::vector<Packet> packets;
    size_t next = 3;                // the first audio packet
    int channels = 0, rate = 0, bs[2] = {0, 0};
    std::vector<Codebook> books;
    std::vector<Floor1> floors;
    std::vector<Residue> residues;
    std::vector<Mapping> mappings;
    std::vector<std::pair<bool, int>> modes;
    std::map<int, Imdct> imdcts;
    std::map<int, std::vector<float>> windows;
    // the last block, windowed, a channel each; samples ready to give
    std::vector<std::vector<float>> prev;
    int prevN = 0;
    std::vector<float> ready;       // interleaved
    size_t readyAt = 0;
    int64_t given = 0, total = -1;  // frames given, and the stream's length
    int64_t skip = 0, skipped = 0;  // frames at the start not part of it

    void setup(Bits& b);
    std::vector<int> floorDecode(const Floor1& f, Bits& b, bool& used);
    void floorCurve(const Floor1& f, const std::vector<int>& Y, int n, std::vector<float>& out);
    void residuePartitions(const Residue& r, Bits& b, std::vector<float*>& vecs, const std::vector<bool>& skip, int size,
                           int fmt);
    bool decodePacket(size_t i);
    const std::vector<float>& window(int n, bool prevLong, bool nextLong, bool blockflag);
};

void VorbisDecoder::Impl::setup(Bits& b) {
    if (b.read(8) != 5) throw FormatError("not a Vorbis setup header");
    for (int i = 0; i < 6; ++i) b.read(8);
    for (int i = 0, n = int(b.read(8)) + 1; i < n; ++i) books.push_back(readCodebook(b));
    for (int i = 0, n = int(b.read(6)) + 1; i < n; ++i)
        if (b.read(16) != 0) throw FormatError("a Vorbis time domain transform");
    for (int i = 0, n = int(b.read(6)) + 1; i < n; ++i) {
        if (b.read(16) != 1) throw FormatError("a Vorbis floor not of type 1");
        Floor1 f;
        int partitions = int(b.read(5));
        for (int k = 0; k < partitions; ++k) f.partClass.push_back(int(b.read(4)));
        int classes = partitions ? *std::max_element(f.partClass.begin(), f.partClass.end()) + 1 : 0;
        for (int c = 0; c < classes; ++c) {
            f.classDims.push_back(int(b.read(3)) + 1);
            int sub = int(b.read(2));
            f.classSub.push_back(sub);
            f.classMaster.push_back(sub ? int(b.read(8)) : -1);
            std::vector<int> sb;
            for (int k = 0; k < (1 << sub); ++k) sb.push_back(int(b.read(8)) - 1);
            f.subBooks.push_back(sb);
        }
        f.multiplier = int(b.read(2)) + 1;
        int rangebits = int(b.read(4));
        f.X = {0, 1 << rangebits};
        for (int c : f.partClass)
            for (int k = 0; k < f.classDims[size_t(c)]; ++k) f.X.push_back(int(b.read(rangebits)));
        int count = int(f.X.size());
        f.order.resize(size_t(count));
        for (int k = 0; k < count; ++k) f.order[size_t(k)] = k;
        std::stable_sort(f.order.begin(), f.order.end(), [&](int a, int c) { return f.X[size_t(a)] < f.X[size_t(c)]; });
        f.low.assign(size_t(count), 0);
        f.high.assign(size_t(count), 0);
        for (int k = 2; k < count; ++k) {
            int lo = -1, hi = -1;
            for (int j = 0; j < k; ++j) {
                if (f.X[size_t(j)] < f.X[size_t(k)] && (lo < 0 || f.X[size_t(j)] > f.X[size_t(lo)])) lo = j;
                if (f.X[size_t(j)] > f.X[size_t(k)] && (hi < 0 || f.X[size_t(j)] < f.X[size_t(hi)])) hi = j;
            }
            f.low[size_t(k)] = lo;
            f.high[size_t(k)] = hi;
        }
        floors.push_back(std::move(f));
    }
    for (int i = 0, n = int(b.read(6)) + 1; i < n; ++i) {
        Residue r;
        r.kind = int(b.read(16));
        if (r.kind > 2) throw FormatError("a Vorbis residue of type " + std::to_string(r.kind));
        r.begin = b.read(24);
        r.end = b.read(24);
        r.psize = b.read(24) + 1;
        r.classes = int(b.read(6)) + 1;
        r.classbook = int(b.read(8));
        std::vector<int> cascade;
        for (int c = 0; c < r.classes; ++c) {
            int low = int(b.read(3));
            int high = b.bit() ? int(b.read(5)) : 0;
            cascade.push_back(high * 8 + low);
        }
        for (int c : cascade) {
            std::array<int, 8> bk;
            for (int j = 0; j < 8; ++j) bk[size_t(j)] = (c >> j & 1) ? int(b.read(8)) : -1;
            r.books.push_back(bk);
        }
        residues.push_back(std::move(r));
    }
    for (int i = 0, n = int(b.read(6)) + 1; i < n; ++i) {
        if (b.read(16) != 0) throw FormatError("a Vorbis mapping not of type 0");
        Mapping m;
        m.submaps = b.bit() ? int(b.read(4)) + 1 : 1;
        if (b.bit())
            for (int k = 0, steps = int(b.read(8)) + 1; k < steps; ++k) {
                int mag = int(b.read(ilog(uint32_t(channels - 1))));
                int ang = int(b.read(ilog(uint32_t(channels - 1))));
                m.coupling.push_back({mag, ang});
            }
        if (b.read(2) != 0) throw FormatError("a Vorbis mapping with reserved bits set");
        m.mux.assign(size_t(channels), 0);
        if (m.submaps > 1)
            for (int& x : m.mux) x = int(b.read(4));
        for (int s = 0; s < m.submaps; ++s) {
            b.read(8);
            m.floor.push_back(int(b.read(8)));
            m.residue.push_back(int(b.read(8)));
        }
        mappings.push_back(std::move(m));
    }
    for (int i = 0, n = int(b.read(6)) + 1; i < n; ++i) {
        bool blockflag = b.bit();
        b.read(16);
        b.read(16);
        modes.push_back({blockflag, int(b.read(8))});
    }
    if (!b.bit()) throw FormatError("a Vorbis setup header without its framing bit");
    // what the packets will name must be there
    for (const Mapping& m : mappings)
        for (int s = 0; s < m.submaps; ++s)
            if (size_t(m.floor[size_t(s)]) >= floors.size() || size_t(m.residue[size_t(s)]) >= residues.size())
                throw FormatError("a Vorbis mapping naming what the setup has not");
    for (auto& [bf, m] : modes)
        if (size_t(m) >= mappings.size()) throw FormatError("a Vorbis mode naming no mapping");
    for (const Residue& r : residues) {
        if (size_t(r.classbook) >= books.size()) throw FormatError("a Vorbis residue naming no codebook");
        for (auto& bk : r.books)
            for (int x : bk)
                if (x >= int(books.size()) || (x >= 0 && books[size_t(x)].vectors.empty()))
                    throw FormatError("a Vorbis residue book without vectors");
    }
}

std::vector<int> VorbisDecoder::Impl::floorDecode(const Floor1& f, Bits& b, bool& used) {
    std::vector<int> Y;
    used = b.bit();
    if (!used) return Y;
    static const int ranges[4] = {256, 128, 86, 64};
    int rng = ranges[f.multiplier - 1];
    Y.push_back(int(b.read(ilog(uint32_t(rng - 1)))));
    Y.push_back(int(b.read(ilog(uint32_t(rng - 1)))));
    for (int c : f.partClass) {
        int cdim = f.classDims[size_t(c)], cbits = f.classSub[size_t(c)];
        int csub = (1 << cbits) - 1;
        int cval = cbits ? books.at(size_t(f.classMaster[size_t(c)])).scalar(b) : 0;
        for (int k = 0; k < cdim; ++k) {
            int book = f.subBooks[size_t(c)][size_t(cval & csub)];
            cval >>= cbits;
            Y.push_back(book >= 0 ? books.at(size_t(book)).scalar(b) : 0);
        }
    }
    return Y;
}

void VorbisDecoder::Impl::floorCurve(const Floor1& f, const std::vector<int>& Y, int n, std::vector<float>& out) {
    static const int ranges[4] = {256, 128, 86, 64};
    int rng = ranges[f.multiplier - 1];
    size_t count = f.X.size();
    std::vector<int> fin(Y);
    std::vector<bool> used(count, false);
    used[0] = used[1] = true;
    for (size_t i = 2; i < count; ++i) {
        int lo = f.low[i], hi = f.high[i];
        int pred = renderPoint(f.X[size_t(lo)], fin[size_t(lo)], f.X[size_t(hi)], fin[size_t(hi)], f.X[i]);
        int val = Y[i], highroom = rng - pred, lowroom = pred;
        int room = 2 * std::min(highroom, lowroom);
        if (val) {
            used[size_t(lo)] = used[size_t(hi)] = used[i] = true;
            if (val >= room)
                fin[i] = highroom > lowroom ? val - lowroom + pred : pred - val + highroom - 1;
            else
                fin[i] = (val & 1) ? pred - (val + 1) / 2 : pred + val / 2;
        } else {
            fin[i] = pred;
        }
    }
    std::vector<int> v(size_t(n), 0);
    int lx = 0, ly = fin[size_t(f.order[0])] * f.multiplier, hx = 0, hy = ly;
    for (size_t k = 1; k < count; ++k) {
        int idx = f.order[k];
        if (!used[size_t(idx)]) continue;
        hy = fin[size_t(idx)] * f.multiplier;
        hx = f.X[size_t(idx)];
        renderLine(lx, ly, hx, hy, v, n);
        lx = hx;
        ly = hy;
    }
    if (hx < n) renderLine(hx, hy, n, hy, v, n);
    out.resize(size_t(n));
    for (int i = 0; i < n; ++i) out[size_t(i)] = inverseDb(v[size_t(i)]);
}

void VorbisDecoder::Impl::residuePartitions(const Residue& r, Bits& b, std::vector<float*>& vecs,
                                            const std::vector<bool>& skip, int size, int fmt) {
    uint32_t begin = std::min(r.begin, uint32_t(size)), end = std::min(r.end, uint32_t(size));
    if (end <= begin) return;
    int parts = int((end - begin) / r.psize);
    if (parts <= 0) return;
    const Codebook& classbook = books[size_t(r.classbook)];
    int per = classbook.dims;
    std::vector<std::vector<int>> classes(vecs.size(), std::vector<int>(size_t(parts + per), 0));
    try {
        for (int pass = 0; pass < 8; ++pass) {
            for (int pc = 0; pc < parts;) {
                if (pass == 0)
                    for (size_t j = 0; j < vecs.size(); ++j) {
                        if (skip[j]) continue;
                        int t = classbook.scalar(b);
                        for (int i = per - 1; i >= 0; --i) {
                            classes[j][size_t(pc + i)] = t % r.classes;
                            t /= r.classes;
                        }
                    }
                for (int w = 0; w < per && pc < parts; ++w, ++pc)
                    for (size_t j = 0; j < vecs.size(); ++j) {
                        if (skip[j]) continue;
                        int book = r.books[size_t(classes[j][size_t(pc)])][size_t(pass)];
                        if (book < 0) continue;
                        const Codebook& cb = books[size_t(book)];
                        float* v = vecs[j] + begin + uint32_t(pc) * r.psize;
                        int dims = cb.dims;
                        if (fmt == 0) {
                            int step = int(r.psize) / dims;
                            for (int i = 0; i < step; ++i) {
                                const float* vec = cb.vector(b);
                                for (int k = 0; k < dims; ++k) v[i + k * step] += vec[k];
                            }
                        } else {
                            for (int i = 0; i < int(r.psize);) {
                                const float* vec = cb.vector(b);
                                for (int k = 0; k < dims && i < int(r.psize); ++k) v[i++] += vec[k];
                            }
                        }
                    }
            }
        }
    } catch (const EndOfPacket&) {
    }
}

const std::vector<float>& VorbisDecoder::Impl::window(int n, bool prevLong, bool nextLong, bool blockflag) {
    int key = n * 8 + (prevLong ? 4 : 0) + (nextLong ? 2 : 0) + (blockflag ? 1 : 0);
    auto it = windows.find(key);
    if (it != windows.end()) return it->second;
    int s = bs[0];
    int ls = 0, le = n / 2, rs = n / 2, re = n;
    if (blockflag && !prevLong) {
        ls = n / 4 - s / 4;
        le = n / 4 + s / 4;
    }
    if (blockflag && !nextLong) {
        rs = n * 3 / 4 - s / 4;
        re = n * 3 / 4 + s / 4;
    }
    std::vector<float> w(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) {
        if (i >= ls && i < le) {
            double x = (i - ls + 0.5) / (le - ls) * M_PI / 2;
            w[size_t(i)] = float(std::sin(M_PI / 2 * std::sin(x) * std::sin(x)));
        } else if (i >= le && i < rs) {
            w[size_t(i)] = 1;
        } else if (i >= rs && i < re) {
            double x = (i - rs + 0.5) / (re - rs) * M_PI / 2 + M_PI / 2;
            w[size_t(i)] = float(std::sin(M_PI / 2 * std::sin(x) * std::sin(x)));
        }
    }
    return windows.emplace(key, std::move(w)).first->second;
}

// One audio packet into `ready`: false when there are none left.
bool VorbisDecoder::Impl::decodePacket(size_t i) {
    const Packet& pk = packets[i];
    Bits b(&bytes[pk.at], pk.size);
    std::vector<std::vector<float>> vecs;
    std::vector<std::vector<int>> Ys(static_cast<size_t>(channels));
    std::vector<bool> used(static_cast<size_t>(channels), false);
    int n = 0;
    bool blockflag = false, prevLong = true, nextLong = true;
    const Mapping* m = nullptr;
    try {
        if (b.bit()) return true;   // not audio
        const auto& mode = modes.at(b.read(ilog(uint32_t(modes.size() - 1))));
        blockflag = mode.first;
        n = bs[blockflag ? 1 : 0];
        if (blockflag) {
            prevLong = b.bit();
            nextLong = b.bit();
        }
        m = &mappings[size_t(mode.second)];
        for (int ch = 0; ch < channels; ++ch) {
            bool u = false;
            Ys[size_t(ch)] = floorDecode(floors[size_t(m->floor[size_t(m->mux[size_t(ch)])])], b, u);
            used[size_t(ch)] = u;
        }
        std::vector<bool> noResidue(static_cast<size_t>(channels));
        for (int ch = 0; ch < channels; ++ch) noResidue[size_t(ch)] = !used[size_t(ch)];
        for (auto [mag, ang] : m->coupling)
            if (size_t(mag) < size_t(channels) && size_t(ang) < size_t(channels) &&
                (!noResidue[size_t(mag)] || !noResidue[size_t(ang)]))
                noResidue[size_t(mag)] = noResidue[size_t(ang)] = false;
        int half = n / 2;
        vecs.assign(size_t(channels), std::vector<float>(size_t(half), 0.0f));
        for (int s = 0; s < m->submaps; ++s) {
            std::vector<float*> vp;
            std::vector<bool> skip;
            for (int ch = 0; ch < channels; ++ch)
                if (m->mux[size_t(ch)] == s) {
                    vp.push_back(vecs[size_t(ch)].data());
                    skip.push_back(noResidue[size_t(ch)]);
                }
            const Residue& r = residues[size_t(m->residue[size_t(s)])];
            if (r.kind == 2) {
                bool all = std::all_of(skip.begin(), skip.end(), [](bool x) { return x; });
                if (all) continue;
                size_t mch = vp.size();
                std::vector<float> flat(size_t(half) * mch, 0.0f);
                std::vector<float*> one{flat.data()};
                residuePartitions(r, b, one, {false}, int(flat.size()), 1);
                for (size_t k = 0; k < flat.size(); ++k) vp[k % mch][k / mch] += flat[k];
            } else {
                residuePartitions(r, b, vp, skip, half, r.kind);
            }
        }
    } catch (const EndOfPacket&) {
        return true;
    }
    int half = n / 2;
    for (auto it = m->coupling.rbegin(); it != m->coupling.rend(); ++it) {
        if (size_t(it->first) >= size_t(channels) || size_t(it->second) >= size_t(channels)) continue;
        float* M = vecs[size_t(it->first)].data();
        float* A = vecs[size_t(it->second)].data();
        for (int k = 0; k < half; ++k) {
            float mv = M[k], av = A[k];
            if (mv > 0) {
                if (av > 0) {
                    A[k] = mv - av;
                } else {
                    A[k] = mv;
                    M[k] = mv + av;
                }
            } else {
                if (av > 0) {
                    A[k] = mv + av;
                } else {
                    A[k] = mv;
                    M[k] = mv - av;
                }
            }
        }
    }
    const std::vector<float>& w = window(n, prevLong, nextLong, blockflag);
    auto ii = imdcts.find(half);
    if (ii == imdcts.end()) ii = imdcts.emplace(half, Imdct(half)).first;
    std::vector<std::vector<float>> blocks(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(n)));
    std::vector<float> curve;
    for (int ch = 0; ch < channels; ++ch) {
        std::vector<float>& spec = vecs[size_t(ch)];
        if (!used[size_t(ch)]) {
            std::fill(spec.begin(), spec.end(), 0.0f);
        } else {
            floorCurve(floors[size_t(m->floor[size_t(m->mux[size_t(ch)])])], Ys[size_t(ch)], half, curve);
            for (int k = 0; k < half; ++k) spec[size_t(k)] *= curve[size_t(k)];
        }
        std::vector<float>& y = blocks[size_t(ch)];
        ii->second.run(spec.data(), y.data());
        for (int k = 0; k < n; ++k) y[size_t(k)] *= w[size_t(k)];
    }
    if (prev.empty()) {
        prev = std::move(blocks);
        prevN = n;
        return true;
    }
    // from the last block's centre to this one's: a quarter of each
    int pn = prevN, shift = pn / 4 + n / 4, startPrev = pn / 2, off = startPrev + shift - n / 2;
    size_t base = ready.size();
    ready.resize(base + size_t(shift) * size_t(channels));
    for (int ch = 0; ch < channels; ++ch) {
        const std::vector<float>& pv = prev[size_t(ch)];
        const std::vector<float>& cur = blocks[size_t(ch)];
        for (int k = 0; k < shift; ++k) {
            int t = startPrev + k;
            float v = t < pn ? pv[size_t(t)] : 0.0f;
            int j = t - off;
            if (j >= 0 && j < n) v += cur[size_t(j)];
            ready[base + size_t(k) * size_t(channels) + size_t(ch)] = v;
        }
    }
    prev = std::move(blocks);
    prevN = n;
    return true;
}

VorbisDecoder::VorbisDecoder(std::vector<uint8_t> file) : p_(std::make_unique<Impl>()) {
    Impl& p = *p_;
    p.file = std::move(file);
    p.packets = oggPackets(p.file, p.bytes);
    if (p.packets.size() < 3) throw FormatError("an Ogg stream without the Vorbis headers");
    try {
        Bits id(&p.bytes[p.packets[0].at], p.packets[0].size);
        if (id.read(8) != 1) throw FormatError("not a Vorbis identification header");
        char magic[6];
        for (char& c : magic) c = char(id.read(8));
        if (std::memcmp(magic, "vorbis", 6) != 0) throw FormatError("not Vorbis");
        if (id.read(32) != 0) throw FormatError("a Vorbis version other than 0");
        p.channels = int(id.read(8));
        p.rate = int(id.read(32));
        id.read(32);
        id.read(32);
        id.read(32);
        p.bs[0] = 1 << id.read(4);
        p.bs[1] = 1 << id.read(4);
        if (p.channels < 1 || p.rate <= 0 || p.bs[0] < 64 || p.bs[1] < p.bs[0] || p.bs[1] > 8192)
            throw FormatError("a Vorbis identification header out of range");
        Bits su(&p.bytes[p.packets[2].at], p.packets[2].size);
        p.setup(su);
    } catch (const EndOfPacket&) {
        throw FormatError("a Vorbis header cut short");
    }
    for (const Packet& pk : p.packets)
        if (pk.granule >= 0) p.total = pk.granule;
    // A stream whose first audio page ends at a granule short of what its
    // packets give starts that many samples in.
    int64_t made = 0;
    int lastN = 0;
    for (size_t i = 3; i < p.packets.size(); ++i) {
        const Packet& pk = p.packets[i];
        if (pk.size == 0 || (p.bytes[pk.at] & 1)) continue;
        Bits b(&p.bytes[pk.at], pk.size);
        int n = 0;
        try {
            b.read(1);
            n = p.bs[p.modes.at(b.read(ilog(uint32_t(p.modes.size() - 1)))).first ? 1 : 0];
        } catch (...) {
            break;
        }
        if (lastN) made += lastN / 4 + n / 4;
        lastN = n;
        if (pk.granule >= 0) {
            // not when that page is also the last, whose granule cuts the end
            bool later = false;
            for (size_t k = i + 1; k < p.packets.size() && !later; ++k) later = p.packets[k].granule >= 0;
            if (later) p.skip = std::max<int64_t>(0, made - pk.granule);
            break;
        }
    }
}

VorbisDecoder::~VorbisDecoder() = default;

int VorbisDecoder::channels() const { return p_->channels; }
int VorbisDecoder::rate() const { return p_->rate; }

size_t VorbisDecoder::read(float* out, size_t frames) {
    Impl& p = *p_;
    size_t done = 0;
    size_t ch = size_t(p.channels);
    while (done < frames) {
        size_t have = (p.ready.size() - p.readyAt) / ch;
        if (have == 0) {
            if (p.next >= p.packets.size()) break;
            p.ready.clear();
            p.readyAt = 0;
            try {
                p.decodePacket(p.next++);
            } catch (const FormatError&) {
                p.next = p.packets.size();
                break;
            }
            continue;
        }
        if (p.skipped < p.skip) {
            size_t drop = size_t(std::min<int64_t>(int64_t(have), p.skip - p.skipped));
            p.readyAt += drop * ch;
            p.skipped += int64_t(drop);
            continue;
        }
        size_t take = std::min(have, frames - done);
        // the last page's granule is the stream's length
        if (p.total >= 0) take = size_t(std::min<int64_t>(int64_t(take), std::max<int64_t>(0, p.total - p.given)));
        if (take == 0) break;
        std::memcpy(out + done * ch, &p.ready[p.readyAt], take * ch * sizeof(float));
        p.readyAt += take * ch;
        p.given += int64_t(take);
        done += take;
    }
    return done;
}

void VorbisDecoder::rewind() {
    Impl& p = *p_;
    p.next = 3;
    p.prev.clear();
    p.prevN = 0;
    p.ready.clear();
    p.readyAt = 0;
    p.given = 0;
    p.skipped = 0;
}

}  // namespace ffa
