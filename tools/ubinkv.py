"""Bink video, revision i, as the game's own System/binkw32.dll decodes it.

Worked out from that DLL's code, not from another decoder: its constant tables
(the sixteen code tables, the scan orders, the quantisers) are read from the
DLL itself at the addresses given here, so nothing of RAD's is copied into this
file. FFmpeg's decoding of the same files is the reference the output is
checked against. Addresses are the DLL's (image base 0x30000000).

A frame (0x3001f260) is the planes in turn: Y, then the two chroma planes at
half size, each size rounded up to a multiple of 8. Before Y a u32 counts the
bytes from it to the first chroma plane; the second chroma plane starts where
the reading of the first ended, rounded to its 32-bit word.

A plane (0x3001d2c0) is one bit stream, read 32-bit little endian words at a
time, least significant bit first. It starts with the code trees of the
bundles, then goes through the plane in rows of 8x8 blocks. At each row's start
each bundle that has run out reads its next part, in order: block types, sub
block types, colours, patterns, x and y motion, intra and inter DC, runs; a
bundle whose next part is empty is finished for the plane. Each block takes
its type from the block types and its data from the bundles, and some read the
plane's stream directly between them.

Usage: ubinkv.py <movie.bik> <binkw32.dll> [frames] [-o out.yuv]
       writes the frames as planar 4:2:0, as ffmpeg -pix_fmt yuv420p does
"""
import struct, sys, os


class Dll:
    """The DLL's memory by address."""
    def __init__(self, path):
        b = open(path, 'rb').read()
        pe = struct.unpack_from('<I', b, 0x3c)[0]
        n = struct.unpack_from('<H', b, pe + 6)[0]
        opt = struct.unpack_from('<H', b, pe + 20)[0]
        base = struct.unpack_from('<I', b, pe + 24 + 28)[0]
        self.b, self.sections = b, []
        for i in range(n):
            o = pe + 24 + opt + 40 * i
            vsize, va, rsize, raw = struct.unpack_from('<IIII', b, o + 8)
            self.sections.append((base + va, max(vsize, rsize), raw, rsize))

    def read(self, va, n):
        for start, size, raw, rsize in self.sections:
            if start <= va < start + size:
                off = va - start
                out = self.b[raw + off:raw + min(off + n, rsize)]
                return out + bytes(n - len(out))
        raise ValueError('address %#x is not in the DLL' % va)

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]


class Tables:
    TREES, MAXLEN = 0x30059000, 0x3004f838      # code table pointers, their lengths
    RUN_FILL = 0x3004f83c                       # block type symbols 12..15: repeats
    SCANS = 0x3004aea8                          # 16 orders of 64, run blocks
    QUANT_INTRA, QUANT_INTER = 0x3004b2c0, 0x3004d300  # 16 tables of 64 dwords

    def __init__(self, dll):
        self.maxlen = list(dll.read(self.MAXLEN, 16))
        # each table: 1 << maxlen bytes of (length << 4) | code
        self.trees = [list(dll.read(dll.u32(self.TREES + 4 * t), 1 << self.maxlen[t])) for t in range(16)]
        self.run_fill = list(dll.read(self.RUN_FILL, 16))
        self.scans = [list(dll.read(self.SCANS + 64 * s, 64)) for s in range(16)]
        self.quant_intra = [struct.unpack('<64i', dll.read(self.QUANT_INTRA + 256 * q, 256)) for q in range(16)]
        self.quant_inter = [struct.unpack('<64i', dll.read(self.QUANT_INTER + 256 * q, 256)) for q in range(16)]


# The order coefficients and residues are read in, to raster order: the value
# for raster position i is the one read at ORDER[i]. Taken from the order the
# DLL copies them out in (0x30021365) and adds the residue in (0x300219b2).
ORDER = [0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15, 24, 25, 44, 45, 16, 17, 20, 21, 26, 27, 46, 47,
         18, 19, 22, 23, 28, 29, 32, 33, 48, 49, 52, 53, 30, 31, 34, 35, 50, 51, 54, 55, 36, 37, 40, 41, 56, 57,
         60, 61, 38, 39, 42, 43, 58, 59, 62, 63]


class Bits:
    def __init__(self, data, at):
        self.d, self.pos = data, at * 8
        self.start = at

    def read(self, n):
        v = 0
        for i in range(n):
            p = self.pos + i
            byte = self.d[p >> 3] if (p >> 3) < len(self.d) else 0
            v |= ((byte >> (p & 7)) & 1) << i
        self.pos += n
        return v

    def bit(self):
        return self.read(1)

    def word_end(self):
        """Where reading ended, at the 32-bit word the DLL last loaded."""
        return self.start + ((self.pos - self.start * 8 + 31) // 32) * 4


class Tree:
    """A bundle's code: one of the sixteen tables, and the symbols its codes
    stand for (read_tree, 0x3001bc70)."""
    def __init__(self, bits, tables):
        t = bits.read(4)
        self.table, self.maxlen = tables.trees[t], tables.maxlen[t]
        if t == 0:
            self.syms = list(range(16))
            return
        if bits.bit():
            # the first n + 1 symbols listed, then the rest in order
            n = bits.read(3)
            listed = [bits.read(4) for _ in range(n + 1)]
            self.syms = listed + [s for s in range(16) if s not in listed]
            return
        depth = bits.read(2)
        s = []
        for i in range(0, 16, 2):
            s += [i + 1, i] if bits.bit() else [i, i + 1]
        size = 2
        for _ in range(depth):
            out = []
            for i in range(0, 16, 2 * size):
                out += merge(bits, s[i:i + size], s[i + size:i + 2 * size])
            s, size = out, size * 2
        self.syms = s

    def get(self, bits):
        peek = 0
        for i in range(self.maxlen):
            p = bits.pos + i
            byte = bits.d[p >> 3] if (p >> 3) < len(bits.d) else 0
            peek |= ((byte >> (p & 7)) & 1) << i
        e = self.table[peek]
        bits.pos += e >> 4
        return self.syms[e & 15]


def merge(bits, a, b):
    out = []
    while a and b:
        out.append(b.pop(0) if bits.bit() else a.pop(0))
    return out + a + b


def length_bits(n):
    return (n + 511).bit_length()


class Bundle:
    def __init__(self, lenbits):
        self.lenbits, self.data, self.at, self.done = lenbits, [], 0, False

    def empty(self):
        return not self.done and self.at >= len(self.data)

    def take(self):
        v = self.data[self.at]
        self.at += 1
        return v

    def start(self, bits):
        """The next part's length; 0 finishes the bundle for the plane."""
        n = bits.read(self.lenbits)
        if n == 0:
            self.done = True
        self.data, self.at = [], 0
        return n


def read_types(bundle, tree, bits, tables):
    if not bundle.empty():
        return
    n = bundle.start(bits)
    if not n:
        return
    if bits.bit():
        bundle.data = [bits.read(4)] * n
        return
    last = 0
    out = bundle.data
    while len(out) < n:
        v = tree.get(bits)
        if v < 12:
            out.append(v)
            last = v
        else:
            out += [last] * tables.run_fill[v]
    del out[n:]


def read_colours(bundle, low, high, state, bits):
    if not bundle.empty():
        return
    n = bundle.start(bits)
    if not n:
        return
    fill = bits.bit()
    count = 1 if fill else n
    out = []
    for _ in range(count):
        hi = high[state[0]].get(bits)
        lo = low.get(bits)
        state[0] = hi
        out.append(hi << 4 | lo)
    bundle.data = out * n if fill else out


def read_patterns(bundle, tree, bits):
    if not bundle.empty():
        return
    n = bundle.start(bits)
    bundle.data = [tree.get(bits) | tree.get(bits) << 4 for _ in range(n)]


def read_motion(bundle, tree, bits):
    if not bundle.empty():
        return
    n = bundle.start(bits)
    if not n:
        return
    if bits.bit():
        v = bits.read(4)
        if v and bits.bit():
            v = -v
        bundle.data = [v] * n
        return
    out = []
    for _ in range(n):
        v = tree.get(bits)
        if v and bits.bit():
            v = -v
        out.append(v)
    bundle.data = out


def read_dc(bundle, bits, signed):
    if not bundle.empty():
        return
    n = bundle.start(bits)
    if not n:
        return
    if signed:
        v = bits.read(10)
        if v and bits.bit():
            v = -v
    else:
        v = bits.read(11)
    out = [v]
    left = n - 1
    while left:
        group = min(8, left)
        b = bits.read(4)
        if b:
            for _ in range(group):
                d = bits.read(b)
                if d and bits.bit():
                    d = -d
                v += d
                out.append(v)
        else:
            out += [v] * group
        left -= group
    bundle.data = [((x + 0x8000) & 0xffff) - 0x8000 for x in out]


def read_runs(bundle, tree, bits):
    if not bundle.empty():
        return
    n = bundle.start(bits)
    if not n:
        return
    if bits.bit():
        bundle.data = [bits.read(4)] * n
    else:
        bundle.data = [tree.get(bits) for _ in range(n)]


def read_coefficients(bits, coef):
    """The DCT coefficients after the DC (0x30020b80), in read order."""
    n = bits.read(4)
    if not n:
        return
    lst = [4 << 2 | 0, 24 << 2 | 0, 44 << 2 | 0, 1 << 2 | 3, 2 << 2 | 3, 3 << 2 | 3]
    start = 0
    mask = 1 << (n - 1)

    def value(b):
        v = bits.read(b) | mask
        return -v if bits.bit() else v

    for b in range(n - 1, -1, -1):
        i = start
        while i < len(lst):
            e = lst[i]
            if not e or not bits.bit():
                i += 1
                continue
            idx, mode = e >> 2, e & 3
            if mode == 3:
                coef[idx] = value(b)
                lst[i] = 0
                i += 1
                continue
            if mode == 1:
                lst[i] = idx << 2 | 2
                lst += [(idx + 4) << 2 | 2, (idx + 8) << 2 | 2, (idx + 12) << 2 | 2]
                continue
            if mode == 0:
                lst[i] = (idx + 4) << 2 | 1
            else:
                lst[i] = 0
                i += 1
            for k in range(idx, idx + 4):
                if bits.bit():
                    lst.insert(start, k << 2 | 3)
                    i += 1
                else:
                    coef[k] = value(b)
        mask >>= 1


def read_residue(bits, res, limit):
    """A residue (0x300214a0): at most limit + 1 changes, in read order."""
    n = bits.read(3) + 1
    mask = 1 << (n - 1)
    lst = [4 << 2 | 0, 24 << 2 | 0, 44 << 2 | 0, 0 << 2 | 2]
    start = 0
    nz = []
    count = [0]

    class Stop(Exception):
        pass

    def changed():
        old = count[0]
        count[0] += 1
        if old == limit:
            raise Stop

    def place(k):
        nz.append(k)
        res[k] = -mask if bits.bit() else mask
        changed()

    try:
        for _ in range(n):
            for k in list(nz):
                if bits.bit():
                    res[k] += -mask if res[k] < 0 else mask
                    changed()
            i = start
            while i < len(lst):
                e = lst[i]
                if not e or not bits.bit():
                    i += 1
                    continue
                idx, mode = e >> 2, e & 3
                if mode == 3:
                    lst[i] = 0
                    place(idx)
                    i += 1
                    continue
                if mode == 1:
                    lst[i] = idx << 2 | 2
                    lst += [(idx + 4) << 2 | 2, (idx + 8) << 2 | 2, (idx + 12) << 2 | 2]
                    continue
                if mode == 0:
                    lst[i] = (idx + 4) << 2 | 1
                else:
                    lst[i] = 0
                    i += 1
                for k in range(idx, idx + 4):
                    if bits.bit():
                        lst.insert(start, k << 2 | 3)
                        i += 1
                    else:
                        place(k)
            mask >>= 1
    except Stop:
        pass


def butterfly(s):
    a0, a1, a2 = s[0] + s[4], s[0] - s[4], s[2] + s[6]
    a3 = ((s[2] - s[6]) * 2896 >> 11) - a2
    e0, e3, e1, e2 = a0 + a2, a0 - a2, a1 + a3, a1 - a3
    t, u, v, w = s[3] + s[5], s[5] - s[3], s[1] + s[7], s[1] - s[7]
    o, x = t + v, v - t
    bb = (w + u) * 3784 >> 11
    u2 = (u * -5352 >> 11) - o + bb
    x2 = (x * 2896 >> 11) - u2
    w2 = ((w * 2217 >> 11) - bb) + x2
    return [e0 + o, e1 + u2, e2 + x2, e3 - w2, e3 + w2, e2 - x2, e1 - u2, e0 - o]


def idct(coef, quant):
    """Raster coefficients, dequantised, to 64 values (0x3001f3e0): each out
    (x + 0x7f) >> 8, as the DLL stores them, a byte without clamping."""
    cols = [[0] * 8 for _ in range(8)]
    for c in range(8):
        s = [coef[c + 8 * r] * quant[c + 8 * r] >> 11 for r in range(8)]
        cols[c] = butterfly(s)
    out = []
    for r in range(8):
        out += [(v + 0x7f) >> 8 for v in butterfly([cols[c][r] for c in range(8)])]
    return out


class Plane:
    def __init__(self, w, h, data=None):
        self.w, self.h = w, h
        self.p = data if data is not None else bytearray(w * h)

    def block(self, x, y):
        return [self.p[(y + r) * self.w + x + c] for r in range(8) for c in range(8)]

    def at(self, x, y):
        x = min(max(x, 0), self.w - 1)
        y = min(max(y, 0), self.h - 1)
        return self.p[y * self.w + x]

    def ref(self, x, y):
        return [self.at(x + c, y + r) for r in range(8) for c in range(8)]

    def put(self, x, y, px, scale=1):
        n = 8 * scale
        for r in range(n):
            for c in range(n):
                self.p[(y + r) * self.w + x + c] = px[(r // scale) * 8 + c // scale] & 0xff


def decode_plane(data, at, w, h, cur, prev, t):
    bits = Bits(data, at)
    bw = w >> 3
    types = Tree(bits, t)
    subs = Tree(bits, t)
    high = [Tree(bits, t) for _ in range(16)]
    low = Tree(bits, t)
    pats = Tree(bits, t)
    xt = Tree(bits, t)
    yt = Tree(bits, t)
    runt = Tree(bits, t)
    b_types, b_subs = Bundle(length_bits(bw)), Bundle(length_bits(w >> 4))
    b_col, b_pat = Bundle(length_bits(bw * 64)), Bundle(length_bits(bw * 8))
    b_x, b_y = Bundle(length_bits(bw)), Bundle(length_bits(bw))
    b_intra, b_inter = Bundle(length_bits(bw)), Bundle(length_bits(bw))
    b_run = Bundle(length_bits(bw * 48))
    colour_state = [0]
    for y in range(0, h, 8):
        read_types(b_types, types, bits, t)
        read_types(b_subs, subs, bits, t)
        read_colours(b_col, low, high, colour_state, bits)
        read_patterns(b_pat, pats, bits)
        read_motion(b_x, xt, bits)
        read_motion(b_y, yt, bits)
        read_dc(b_intra, bits, False)
        read_dc(b_inter, bits, True)
        read_runs(b_run, runt, bits)
        x = 0
        while x < w:
            kind = b_types.take()
            if kind == 1:
                if y & 8 == 0:
                    scaled_block(b_subs.take(), x, y, cur, bits, t, b_col, b_pat, b_intra, b_run)
                x += 16
                continue
            block(kind, x, y, cur, prev, bits, t, b_col, b_pat, b_x, b_y, b_intra, b_inter, b_run)
            x += 8
    return bits.word_end()


def run_block(bits, t, b_col, b_run):
    scan = t.scans[bits.read(4)]
    px = [0] * 64
    i = 0
    while i < 63:
        if bits.bit():
            c = b_col.take()
            for _ in range(b_run.take() + 1):
                px[scan[i]] = c
                i += 1
        else:
            for _ in range(b_run.take() + 1):
                px[scan[i]] = b_col.take()
                i += 1
    if i == 63:
        px[scan[63]] = b_col.take()
    return px


def pattern_block(b_col, b_pat):
    c0, c1 = b_col.take(), b_col.take()
    px = []
    for _ in range(8):
        p = b_pat.take()
        px += [c1 if p >> k & 1 else c0 for k in range(8)]
    return px


def intra_block(bits, t, b_intra):
    coef = [0] * 64
    coef[0] = b_intra.take()
    read_coefficients(bits, coef)
    raster = [coef[ORDER[i]] for i in range(64)]
    return idct(raster, t.quant_intra[bits.read(4)])


def block(kind, x, y, cur, prev, bits, t, b_col, b_pat, b_x, b_y, b_intra, b_inter, b_run):
    if kind == 0:
        cur.put(x, y, prev.ref(x, y))
    elif kind == 2:
        cur.put(x, y, prev.ref(x + b_x.take(), y + b_y.take()))
    elif kind == 3:
        cur.put(x, y, run_block(bits, t, b_col, b_run))
    elif kind == 4:
        ref = prev.ref(x + b_x.take(), y + b_y.take())
        limit = bits.read(7)
        res = [0] * 64
        read_residue(bits, res, limit)
        cur.put(x, y, [ref[i] + res[ORDER[i]] for i in range(64)])
    elif kind == 5:
        cur.put(x, y, intra_block(bits, t, b_intra))
    elif kind == 6:
        cur.put(x, y, [b_col.take()] * 64)
    elif kind == 7:
        ref = prev.ref(x + b_x.take(), y + b_y.take())
        coef = [0] * 64
        coef[0] = b_inter.take()
        read_coefficients(bits, coef)
        raster = [coef[ORDER[i]] for i in range(64)]
        d = idct(raster, t.quant_inter[bits.read(4)])
        cur.put(x, y, [ref[i] + d[i] for i in range(64)])
    elif kind == 8:
        cur.put(x, y, pattern_block(b_col, b_pat))
    elif kind == 9:
        cur.put(x, y, [b_col.take() for _ in range(64)])
    else:
        raise ValueError('block type %d' % kind)


def scaled_block(sub, x, y, cur, bits, t, b_col, b_pat, b_intra, b_run):
    """A 16x16 block from an 8x8 one, each pixel doubled both ways."""
    if sub == 3:
        px = run_block(bits, t, b_col, b_run)
    elif sub == 5:
        px = intra_block(bits, t, b_intra)
    elif sub == 6:
        px = [b_col.take()] * 64
    elif sub == 8:
        px = pattern_block(b_col, b_pat)
    elif sub == 9:
        px = [b_col.take() for _ in range(64)]
    else:
        return
    cur.put(x, y, px, 2)


class Movie:
    def __init__(self, path, dll):
        self.d = open(path, 'rb').read()
        d = self.d
        if d[:3] != b'BIK':
            raise ValueError('not a Bink file')
        self.revision = chr(d[3])
        self.frames, = struct.unpack_from('<I', d, 8)
        self.width, self.height, self.fps_num, self.fps_den, self.flags, self.tracks = \
            struct.unpack_from('<6I', d, 20)
        table = 44 + 12 * self.tracks
        self.offsets = struct.unpack_from('<%dI' % (self.frames + 1), d, table)
        self.t = Tables(Dll(dll))
        w8, h8 = (self.width + 7) & ~7, (self.height + 7) & ~7
        cw, ch = ((self.width + 1) // 2 + 7) & ~7, ((self.height + 1) // 2 + 7) & ~7
        self.cur = [Plane(w8, h8), Plane(cw, ch), Plane(cw, ch)]
        self.prev = [Plane(w8, h8), Plane(cw, ch), Plane(cw, ch)]

    def frame(self, i):
        d = self.d
        at = self.offsets[i] & ~1
        for _ in range(self.tracks):          # the audio packets first
            n, = struct.unpack_from('<I', d, at)
            at += 4 + n
        self.cur, self.prev = self.prev, self.cur
        size, = struct.unpack_from('<I', d, at)
        decode_plane(d, at + 4, self.cur[0].w, self.cur[0].h, self.cur[0], self.prev[0], self.t)
        end = decode_plane(d, at + size, self.cur[1].w, self.cur[1].h, self.cur[1], self.prev[1], self.t)
        decode_plane(d, end, self.cur[2].w, self.cur[2].h, self.cur[2], self.prev[2], self.t)
        return self.cur

    def yuv(self, planes):
        """The frame cropped to the movie's size, Y then U then V."""
        out = bytearray()
        for k, p in enumerate(planes):
            w, h = (self.width, self.height) if k == 0 else ((self.width + 1) // 2, (self.height + 1) // 2)
            for r in range(h):
                out += p.p[r * p.w:r * p.w + w]
        return out


def main(argv):
    out = None
    if '-o' in argv:
        i = argv.index('-o')
        out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    m = Movie(argv[0], argv[1])
    count = int(argv[2]) if len(argv) > 2 else 1
    print('%s: revision %s, %dx%d, %d frames at %d/%d, flags %#x, %d audio tracks' % (
        argv[0], m.revision, m.width, m.height, m.frames, m.fps_num, m.fps_den, m.flags, m.tracks))
    f = open(out, 'wb') if out else None
    for i in range(min(count, m.frames)):
        planes = m.frame(i)
        if f:
            y, u, v = planes
            f.write(m.yuv([y, v, u]))
        print('frame %d decoded' % i)
    if f:
        f.close()


if __name__ == '__main__':
    main(sys.argv[1:])
