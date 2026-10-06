"""Textures of UE2 packages: the mip chain and the pixel formats Shrek 2 uses.

A Texture record is a tagged property block (Format, USize, VSize, Palette and
the rest) followed by the mip chain:

    index   mip count
    per mip:
      u32     skip offset, the absolute file offset just past the data
      index   data length
      bytes   pixel data
      u32     USize, u32 VSize, u8 UBits, u8 VBits

Proven on every texture in the game, 2121 of them across all packages: the chain
ends exactly at the end of each record, every skip offset points just past its
data, and every mip's data is exactly as long as its format requires at its size.

Formats seen, by ETextureFormat value:

    0  P8      one byte per pixel into a Palette, 189 textures
    3  DXT1    BC1, 5 textures
    5  RGBA8   four bytes per pixel, B G R A, 229 textures
    7  DXT3    BC2, 20 textures
    8  DXT5    BC3, 1656 textures, nearly everything
    10 G16     16 bit height, the terrain heightmaps

A Palette record is an empty property block, a compact count and that many
colours, four bytes each. All 177 palettes hold 256.

The two colour orders differ, and both were settled by eye on textures whose
colours are not in doubt. Direct RGBA8 pixels are B G R A: read that way a
redwood's bark is red brown and a level's title card is cream with red and dark
blue lettering; read the other way the bark turns blue. Palette entries are
R G B A: read that way a menu hourglass has a golden wooden frame on the classic
magenta mask colour, and the palette called Jred is red; read as B G R A the
frame turns blue and Jred turns blue too.

DXT decoding below follows the published S3TC formats; nothing about it is
specific to Unreal.
"""
import sys, os, struct, zlib, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from uterrain import properties_upto

P8, DXT1, RGBA8, DXT3, DXT5, L8, G16 = 0, 3, 5, 7, 8, 9, 10
FORMAT_NAMES = {0: 'P8', 1: 'RGBA7', 2: 'RGB16', 3: 'DXT1', 4: 'RGB8', 5: 'RGBA8',
                6: 'NODATA', 7: 'DXT3', 8: 'DXT5', 9: 'L8', 10: 'G16', 11: 'RRRGGGBBB'}


class Texture:
    def __init__(self, pkg, e):
        self.p, self.e, self.name = pkg, e, e['name']
        d, at = properties_upto(pkg, e)
        self.props = d or {}
        self.format = self.props.get('Format', {}).get('value', 0)
        r = R(pkg.b, at)
        self.mips = []
        for _ in range(r.idx()):
            skip = r.u32()
            n = r.idx()
            start = r.p
            r.p += n
            if skip != r.p:
                raise ValueError('mip skip offset does not match')
            w, h = r.u32(), r.u32()
            r.u8(); r.u8()
            self.mips.append((w, h, start, n))

    def palette(self, resolve=None):
        """256 RGBA tuples for a P8 texture. `resolve(pkg, ref)` finds a
        Palette in another package; a local one is read directly."""
        ref = self.props.get('Palette', {}).get('ref', 0)
        if ref > 0:
            pp, pe = self.p, self.p.exports[ref - 1]
        elif resolve:
            pp, pe = resolve(self.p, ref)
        else:
            return None
        _, at = properties_upto(pp, pe)
        r = R(pp.b, at)
        n = r.idx()
        out = []
        # Palette colours are R G B A, unlike direct RGBA8 pixels, which are
        # B G R A. Both were settled by eye on textures whose colours are not in
        # doubt; green, the colour Shrek 2 has most of, survives either order.
        for i in range(n):
            out.append(tuple(pp.b[r.p + 4 * i:r.p + 4 * i + 4]))
        return out

    def pick_mip(self, max_side=None):
        if max_side is None:
            return 0
        for i, (w, h, s, n) in enumerate(self.mips):
            if max(w, h) <= max_side:
                return i
        return len(self.mips) - 1

    def rgba(self, mip=0, resolve=None):
        """(width, height, bytes) of a mip decoded to RGBA."""
        w, h, s, n = self.mips[mip]
        data = self.p.b[s:s + n]
        f = self.format
        if f == RGBA8:
            out = bytearray(w * h * 4)
            out[0::4], out[1::4], out[2::4], out[3::4] = data[2::4], data[1::4], data[0::4], data[3::4]
            return w, h, bytes(out)
        if f == P8:
            pal = self.palette(resolve) or [(i, i, i, 255) for i in range(256)]
            return w, h, b''.join(bytes(pal[i]) for i in data)
        if f == G16:
            vals = struct.unpack('<%dH' % (w * h), data)
            return w, h, b''.join(bytes((v >> 8, v >> 8, v >> 8, 255)) for v in vals)
        if f == L8:
            return w, h, b''.join(bytes((v, v, v, 255)) for v in data)
        if f in (DXT1, DXT3, DXT5):
            return w, h, decode_dxt(data, w, h, f)
        raise ValueError('format %s not supported' % FORMAT_NAMES.get(f, f))


def _rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def decode_dxt(data, w, h, fmt):
    out = bytearray(w * h * 4)
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    size = 8 if fmt == DXT1 else 16
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * size
            alpha = None
            if fmt == DXT3:
                bits = int.from_bytes(data[o:o + 8], 'little')
                alpha = [((bits >> (4 * i)) & 15) * 17 for i in range(16)]
                o += 8
            elif fmt == DXT5:
                a0, a1 = data[o], data[o + 1]
                if a0 > a1:
                    lut = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    lut = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                bits = int.from_bytes(data[o + 2:o + 8], 'little')
                alpha = [lut[(bits >> (3 * i)) & 7] for i in range(16)]
                o += 8
            c0, c1 = struct.unpack_from('<HH', data, o)
            p0, p1 = _rgb565(c0), _rgb565(c1)
            if c0 > c1 or fmt != DXT1:
                pal = [p0 + (255,), p1 + (255,),
                       tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,),
                       tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            idx = struct.unpack_from('<I', data, o + 4)[0]
            for i in range(16):
                px, py = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if px >= w or py >= h:
                    continue
                c = pal[(idx >> (2 * i)) & 3]
                k = (py * w + px) * 4
                out[k:k + 3] = bytes(c[:3])
                out[k + 3] = alpha[i] if alpha is not None else c[3]
    return bytes(out)


def write_png(path, w, h, rgba):
    raw = b''.join(b'\x00' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n'
                           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main(argv):
    p = Package(argv[0])
    name = argv[1]
    e = next(x for x in p.exports if x['name'] == name and p.classof(x) == 'Texture')
    t = Texture(p, e)
    mip = t.pick_mip(int(argv[3]) if len(argv) > 3 else 512)
    w, h, px = t.rgba(mip)
    write_png(argv[2], w, h, px)
    print('%s: %s, %dx%d mip %d of %d -> %s' % (name, FORMAT_NAMES.get(t.format, t.format),
                                               w, h, mip, len(t.mips), argv[2]))


if __name__ == '__main__':
    main(sys.argv[1:])
