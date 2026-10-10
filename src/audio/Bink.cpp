#include "audio/Bink.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <memory>

#include "core/Package.h"

namespace ffa {

namespace {

const int kCritical[25] = {100,  200,  300,  400,  510,  630,  770,  920,  1080,  1270,  1480,  1720, 2000,
                           2320, 2700, 3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500};
const int kRun[16] = {2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64};

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Least significant bit first.
struct Bits {
    const uint8_t* d;
    size_t pos = 0, n;
    Bits(const uint8_t* data, size_t size) : d(data), n(size * 8) {}
    uint32_t get(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; ++i, ++pos)
            if (pos < n) v |= uint32_t((d[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
    float real() {
        int power = int(get(5));
        float f = std::ldexp(float(get(23)), power - 23);
        return get(1) ? -f : f;
    }
    void align32() { pos = (pos + 31) & ~size_t(31); }
    bool left() const { return pos < n; }
};

// The inverse DCT, through a complex FFT of the block's length (Makhoul): with
// X the coefficients, X0 counted twice, V_k = e^(i pi k / 2n) (X_k - i X_n-k),
// v its inverse transform, and the samples v_0, v_n-1, v_1, v_n-2 and so on
// taken at the even and odd places. It gives (2 / n)(c_0 + sum c_k cos(pi k
// (2j + 1) / 2n)), what the direct sum gave, to float precision, in a
// hundredth of the time.
struct Idct {
    int n = 0;
    std::vector<std::complex<double>> twiddle, roots;
    std::vector<int> reversed;
    explicit Idct(int size) : n(size), twiddle(size_t(size)), roots(size_t(size / 2)), reversed(size_t(size)) {
        for (int k = 0; k < n; ++k) twiddle[size_t(k)] = std::polar(1.0, M_PI * k / (2.0 * n));
        for (int k = 0; k < n / 2; ++k) roots[size_t(k)] = std::polar(1.0, 2 * M_PI * k / n);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            reversed[size_t(i)] = r;
        }
    }
    void run(const std::vector<float>& c, std::vector<float>& out) const {
        std::vector<std::complex<double>> v(static_cast<size_t>(n));
        for (int k = 0; k < n; ++k) {
            double xk = k == 0 ? 2.0 * c[0] : c[size_t(k)];
            double xnk = k == 0 ? 0.0 : c[size_t(n - k)];
            v[size_t(reversed[size_t(k)])] = twiddle[size_t(k)] * std::complex<double>(xk, -xnk);
        }
        // the inverse transform, unscaled: e^(+2 pi i nk / n)
        for (int len = 2; len <= n; len <<= 1) {
            int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < len / 2; ++j) {
                    std::complex<double> w = roots[size_t(j * step)];
                    std::complex<double> a = v[size_t(i + j)], b = v[size_t(i + j + len / 2)] * w;
                    v[size_t(i + j)] = a + b;
                    v[size_t(i + j + len / 2)] = a - b;
                }
        }
        for (int m = 0; m < n / 2; ++m) {
            out[size_t(2 * m)] = float(v[size_t(m)].real() / n);
            out[size_t(2 * m + 1)] = float(v[size_t(n - 1 - m)].real() / n);
        }
    }
};

}  // namespace

DecodedSound decodeBink(const uint8_t* b, size_t size) {
    if (size < 64 || std::memcmp(b, "BIK", 3) != 0) throw FormatError("not a Bink file");
    uint32_t frames = le32(b + 8), tracks = le32(b + 40);
    if (tracks != 1) throw FormatError("a Bink file with other than one audio track");
    size_t o = 44 + 4;
    int rate = b[o] | b[o + 1] << 8, flags = b[o + 2] | b[o + 3] << 8;
    o += 4 + 4;
    if (!(flags & 0x1000) || (flags & 0x2000)) throw FormatError("a Bink sound not DCT and mono");
    if (o + 4 * (size_t(frames) + 1) > size) throw FormatError("a Bink frame table past the file");
    std::vector<uint32_t> offsets(frames + 1);
    for (uint32_t i = 0; i <= frames; ++i) offsets[i] = le32(b + o + 4 * i) & ~1u;

    const int bits = rate < 22050 ? 9 : rate < 44100 ? 10 : 11;
    const int n = 1 << bits, overlap = n / 16;
    const float root = float(n / (std::sqrt(double(n)) * 32768.0));
    float quant[96];
    for (int i = 0; i < 96; ++i) quant[i] = float(std::exp(i * 0.15289164787221953823)) * root;
    const int half = (rate + 1) / 2;
    std::vector<int> bands{2};
    for (int i = 1; i < 25 && kCritical[i - 1] < half; ++i) bands.push_back((kCritical[i - 1] * n / half) & ~1);
    bands.push_back(n);
    const Idct idct(n);

    DecodedSound out;
    out.rate = rate;
    std::vector<float> c(static_cast<size_t>(n)), block(static_cast<size_t>(n)), previous(static_cast<size_t>(overlap));
    std::vector<float> q(bands.size() - 1);
    bool first = true;
    for (uint32_t f = 0; f < frames; ++f) {
        size_t at = offsets[f];
        if (at + 4 > size) throw FormatError("a Bink frame past the file");
        uint32_t len = le32(b + at);
        if (!len) continue;
        if (at + 4 + len > size) throw FormatError("a Bink packet past the file");
        Bits g(b + at + 4, len);
        g.get(32);                              // the decoded size
        while (g.left()) {
            g.get(2);
            std::fill(c.begin(), c.end(), 0.0f);
            c[0] = g.real() * root;
            c[1] = g.real() * root;
            for (float& x : q) x = quant[std::min<uint32_t>(g.get(8), 95)];
            size_t k = 0;
            float qk = q[0];
            int i = 2;
            while (i < n) {
                int j = g.get(1) ? i + kRun[g.get(4)] * 8 : i + 8;
                j = std::min(j, n);
                int width = int(g.get(4));
                if (width == 0) {
                    i = j;
                    while (k < q.size() && bands[k] < i) qk = q[k++];
                } else {
                    for (; i < j; ++i) {
                        if (k < q.size() && bands[k] == i) qk = q[k++];
                        uint32_t v = g.get(width);
                        if (v) c[size_t(i)] = g.get(1) ? -qk * float(v) : qk * float(v);
                    }
                }
            }
            // type III, the first coefficient whole, scaled by 2 / n
            idct.run(c, block);
            if (!first)
                for (int t = 0; t < overlap; ++t)
                    block[size_t(t)] = (previous[size_t(t)] * float(overlap - t) + block[size_t(t)] * float(t)) / float(overlap);
            first = false;
            std::copy(block.end() - overlap, block.end(), previous.begin());
            out.samples.insert(out.samples.end(), block.begin(), block.end() - overlap);
            g.align32();
        }
    }
    return out;
}

DecodedSound decodeWav(const uint8_t* b, size_t size) {
    if (size < 12 || std::memcmp(b, "RIFF", 4) != 0 || std::memcmp(b + 8, "WAVE", 4) != 0)
        throw FormatError("not a RIFF WAV");
    DecodedSound out;
    int bitsPer = 0;
    for (size_t o = 12; o + 8 <= size;) {
        uint32_t len = le32(b + o + 4);
        const uint8_t* body = b + o + 8;
        if (o + 8 + len > size) len = uint32_t(size - o - 8);
        if (!std::memcmp(b + o, "fmt ", 4) && len >= 16) {
            if ((body[0] | body[1] << 8) != 1) throw FormatError("a WAV not PCM");
            out.channels = body[2] | body[3] << 8;
            out.rate = int(le32(body + 4));
            bitsPer = body[14] | body[15] << 8;
        } else if (!std::memcmp(b + o, "data", 4)) {
            if (bitsPer == 16)
                for (uint32_t i = 0; i + 1 < len; i += 2) out.samples.push_back(float(int16_t(body[i] | body[i + 1] << 8)) / 32768.0f);
            else if (bitsPer == 8)
                for (uint32_t i = 0; i < len; ++i) out.samples.push_back((float(body[i]) - 128.0f) / 128.0f);
            else
                throw FormatError("a WAV of other than 8 or 16 bits");
        }
        o += 8 + len + (len & 1);
    }
    if (!out.rate) throw FormatError("a WAV without its format");
    // mono, the channels' mean: a sound plays from where its actor is, and
    // the four of the game's that are stereo, the ground pound's shake and
    // the Fairy Godmother's office doors, played as mono ran at half speed
    // with their channels one after the other
    if (out.channels == 2) {
        for (size_t i = 0; i + 1 < out.samples.size(); i += 2) out.samples[i / 2] = 0.5f * (out.samples[i] + out.samples[i + 1]);
        out.samples.resize(out.samples.size() / 2);
        out.channels = 1;
    }
    return out;
}

}  // namespace ffa
