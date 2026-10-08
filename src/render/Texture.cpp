#include "render/Texture.h"

#include <algorithm>

#include "script/Tagged.h"

namespace ffa {

namespace {

enum { P8 = 0, DXT1 = 3, RGBA8 = 5, DXT3 = 7, DXT5 = 8, L8 = 9, G16 = 10 };

void rgb565(uint16_t c, uint8_t out[3]) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = uint8_t((r << 3) | (r >> 2));
    out[1] = uint8_t((g << 2) | (g >> 4));
    out[2] = uint8_t((b << 3) | (b >> 2));
}

size_t dataSize(int fmt, int w, int h) {
    size_t blocks = size_t(std::max(1, (w + 3) / 4)) * size_t(std::max(1, (h + 3) / 4));
    switch (fmt) {
    case P8: case L8: return size_t(w) * size_t(h);
    case RGBA8: return size_t(w) * size_t(h) * 4;
    case G16: return size_t(w) * size_t(h) * 2;
    case DXT1: return blocks * 8;
    case DXT3: case DXT5: return blocks * 16;
    }
    return 0;
}

}  // namespace

void decodeDxt(const uint8_t* data, size_t size, int w, int h, int fmt, uint8_t* out) {
    int bw = std::max(1, (w + 3) / 4), bh = std::max(1, (h + 3) / 4);
    size_t block = fmt == DXT1 ? 8 : 16;
    if (size < size_t(bw) * size_t(bh) * block) throw FormatError("DXT data is short");
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t* o = data + (size_t(by) * size_t(bw) + size_t(bx)) * block;
            uint8_t alpha[16];
            bool haveAlpha = false;
            if (fmt == DXT3) {
                for (int i = 0; i < 16; ++i) alpha[i] = uint8_t(((o[i / 2] >> (4 * (i & 1))) & 15) * 17);
                haveAlpha = true;
                o += 8;
            } else if (fmt == DXT5) {
                uint8_t a0 = o[0], a1 = o[1], lut[8] = {a0, a1};
                if (a0 > a1) {
                    for (int i = 1; i < 7; ++i) lut[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
                } else {
                    for (int i = 1; i < 5; ++i) lut[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
                    lut[6] = 0;
                    lut[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= uint64_t(o[2 + i]) << (8 * i);
                for (int i = 0; i < 16; ++i) alpha[i] = lut[(bits >> (3 * i)) & 7];
                haveAlpha = true;
                o += 8;
            }
            uint16_t c0 = uint16_t(o[0] | o[1] << 8), c1 = uint16_t(o[2] | o[3] << 8);
            uint8_t pal[4][4];
            rgb565(c0, pal[0]);
            rgb565(c1, pal[1]);
            pal[0][3] = pal[1][3] = 255;
            for (int k = 0; k < 3; ++k) {
                if (c0 > c1 || fmt != DXT1) {
                    pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
                    pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
                } else {
                    pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = 0;
                }
            }
            pal[2][3] = 255;
            pal[3][3] = (c0 > c1 || fmt != DXT1) ? 255 : 0;
            uint32_t idx = uint32_t(o[4]) | uint32_t(o[5]) << 8 | uint32_t(o[6]) << 16 | uint32_t(o[7]) << 24;
            for (int i = 0; i < 16; ++i) {
                int px = bx * 4 + (i & 3), py = by * 4 + (i >> 2);
                if (px >= w || py >= h) continue;
                const uint8_t* c = pal[(idx >> (2 * i)) & 3];
                uint8_t* d = out + (size_t(py) * size_t(w) + size_t(px)) * 4;
                d[0] = c[0];
                d[1] = c[1];
                d[2] = c[2];
                d[3] = haveAlpha ? alpha[i] : c[3];
            }
        }
}

TextureInfo readTexture(const ObjectRef& t) {
    TextureInfo info;
    std::vector<TagEntry> tags;
    size_t at = 0;
    if (!Library::properties(t, tags, at)) throw FormatError("the Texture has no property block");
    const Package& p = *t.pkg;
    info.format = tagInt(p, tags, "Format");
    info.palette = tagObject(p, tags, "Palette");
    info.masked = tagInt(p, tags, "bMasked");
    info.alphaTexture = tagInt(p, tags, "bAlphaTexture");
    info.twoSided = tagInt(p, tags, "bTwoSided");
    const Export& e = t.exp();
    size_t end = size_t(e.off + e.size);
    Reader r(p.data, at, end);
    int32_t n = r.idx();
    if (n < 0 || n > 16) throw FormatError("the Texture's mip count is out of range");
    for (int32_t i = 0; i < n; ++i) {
        uint32_t skip = r.u32();
        int32_t len = r.idx();
        if (len < 0) throw FormatError("a mip's length is negative");
        TextureInfo::Mip m;
        m.at = r.p;
        m.size = size_t(len);
        r.need(m.size);
        r.p += m.size;
        if (skip != r.p) throw FormatError("a mip's skip offset does not match its data");
        m.width = int(r.u32());
        m.height = int(r.u32());
        r.u8();
        r.u8();
        info.mips.push_back(m);
    }
    if (r.p != end) throw FormatError("the Texture does not end on its record");
    return info;
}

Image decodeTexture(Library& lib, const ObjectRef& t, int maxSide) {
    TextureInfo info = readTexture(t);
    if (info.mips.empty()) throw FormatError("the Texture has no mips");
    size_t pick = 0;
    if (maxSide > 0) {
        pick = info.mips.size() - 1;
        for (size_t i = 0; i < info.mips.size(); ++i)
            if (std::max(info.mips[i].width, info.mips[i].height) <= maxSide) {
                pick = i;
                break;
            }
    }
    const TextureInfo::Mip& m = info.mips[pick];
    if (m.width <= 0 || m.height <= 0 || m.width > 8192 || m.height > 8192)
        throw FormatError("a mip's size is out of range");
    if (m.size != dataSize(info.format, m.width, m.height)) throw FormatError("a mip's data is not its size");
    const uint8_t* d = t.pkg->data.data() + m.at;
    Image img;
    img.width = m.width;
    img.height = m.height;
    size_t px = size_t(m.width) * size_t(m.height);
    img.rgba.resize(px * 4);
    uint8_t* o = img.rgba.data();
    switch (info.format) {
    case RGBA8:
        for (size_t i = 0; i < px; ++i) {
            o[4 * i] = d[4 * i + 2];
            o[4 * i + 1] = d[4 * i + 1];
            o[4 * i + 2] = d[4 * i];
            o[4 * i + 3] = d[4 * i + 3];
        }
        break;
    case P8: {
        uint8_t pal[256][4];
        for (int i = 0; i < 256; ++i) pal[i][0] = pal[i][1] = pal[i][2] = uint8_t(i), pal[i][3] = 255;
        ObjectRef pr = lib.resolve(*t.pkg, info.palette);
        if (pr) {
            std::vector<TagEntry> tags;
            size_t at = 0;
            if (Library::properties(pr, tags, at)) {
                Reader r(pr.pkg->data, at, size_t(pr.exp().off + pr.exp().size));
                int32_t n = std::min(r.idx(), 256);
                for (int32_t i = 0; i < n; ++i)
                    for (int k = 0; k < 4; ++k) pal[i][k] = r.u8();
            }
        }
        for (size_t i = 0; i < px; ++i) std::copy(pal[d[i]], pal[d[i]] + 4, o + 4 * i);
        break;
    }
    case L8:
        for (size_t i = 0; i < px; ++i) o[4 * i] = o[4 * i + 1] = o[4 * i + 2] = d[i], o[4 * i + 3] = 255;
        break;
    case G16:
        for (size_t i = 0; i < px; ++i) o[4 * i] = o[4 * i + 1] = o[4 * i + 2] = d[2 * i + 1], o[4 * i + 3] = 255;
        break;
    case DXT1: case DXT3: case DXT5:
        decodeDxt(d, m.size, m.width, m.height, info.format, o);
        break;
    default:
        throw FormatError("a texture format not decoded");
    }
    return img;
}

}  // namespace ffa
