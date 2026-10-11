"""Ogg Vorbis, the game's music: 69 files of Music/, 44100 Hz stereo, made by
libVorbis I 20020717 (Sonic Foundry's encoder).

Written from the Vorbis I specification (Xiph.Org) and the Ogg framing it
names, not from a decoder: FFmpeg's decoding of the same files is only the
reference the output is checked against.

An Ogg stream is pages: "OggS", version, a header type, the granule position
(the sample count at the page's last packet's end), serial, page number, CRC,
then a segment count and that many lacing values; a packet is the lacing
values summed up to one under 255, and may run on into the next page.

Vorbis packets are read least significant bit first. Three headers come
first: identification (channels, rate, the two block sizes), comment, and
setup, which holds the codebooks, floors, residues, mappings and modes. Every
audio packet after names a mode, so a block size and a mapping; per channel a
floor curve is decoded, then the residue vectors, the channels' coupling is
undone, each vector is multiplied by its floor, transformed back by the
inverse MDCT, windowed, and overlapped with the block before. A packet gives
the samples from the middle of the block before to the middle of its own.

Usage: uogg.py <file.ogg> [seconds] [-o out.f32]
       writes interleaved 32 bit float samples, as ffmpeg -f f32le does
"""
import cmath, math, struct, sys


# --------------------------------------------------------------------- Ogg

def packets(data):
    """The stream's packets in order, each with the granule position of the
    page it ends on (or -1 when another packet ends after it there)."""
    pos, part = 0, b''
    while pos + 27 <= len(data):
        if data[pos:pos + 4] != b'OggS':
            raise ValueError('not an Ogg page at %d' % pos)
        granule, = struct.unpack_from('<q', data, pos + 6)
        nseg = data[pos + 26]
        lacing = data[pos + 27:pos + 27 + nseg]
        body = pos + 27 + nseg
        ends = [i for i, v in enumerate(lacing) if v < 255]
        at = body
        for i, v in enumerate(lacing):
            part += data[at:at + v]
            at += v
            if v < 255:
                yield part, granule if i == ends[-1] else -1
                part = b''
        pos = at


# ----------------------------------------------------------------- bits

class EndOfPacket(Exception):
    pass


class Bits:
    def __init__(self, data):
        self.d, self.pos, self.n = data, 0, len(data) * 8

    def read(self, k):
        if self.pos + k > self.n:
            self.pos = self.n
            raise EndOfPacket
        v = 0
        for i in range(k):
            p = self.pos + i
            v |= ((self.d[p >> 3] >> (p & 7)) & 1) << i
        self.pos += k
        return v

    def bit(self):
        return self.read(1)


def ilog(x):
    """The number of bits x takes: ilog(0) = 0, ilog(1) = 1, ilog(7) = 3."""
    return x.bit_length() if x > 0 else 0


def float32_unpack(x):
    mantissa = x & 0x1fffff
    exponent = (x & 0x7fe00000) >> 21
    if x & 0x80000000:
        mantissa = -mantissa
    return mantissa * 2.0 ** (exponent - 788)


def lookup1_values(entries, dims):
    """The greatest r with r ** dims <= entries."""
    r = int(entries ** (1.0 / dims))
    while (r + 1) ** dims <= entries:
        r += 1
    while r ** dims > entries:
        r -= 1
    return r


# ------------------------------------------------------------ codebooks

class Codebook:
    def __init__(self, b):
        if b.read(24) != 0x564342:
            raise ValueError('a codebook without its sync pattern')
        self.dims = b.read(16)
        self.entries = b.read(24)
        lengths = [0] * self.entries
        if not b.bit():
            sparse = b.bit()
            for i in range(self.entries):
                if not sparse or b.bit():
                    lengths[i] = b.read(5) + 1
        else:
            length = b.read(5) + 1
            i = 0
            while i < self.entries:
                n = b.read(ilog(self.entries - i))
                for k in range(i, i + n):
                    lengths[k] = length
                i += n
                length += 1
        self.lengths = lengths
        self.codes = assign_codewords(lengths)
        # decoding by (length, code)
        self.lookup = {}
        for e, (l, c) in enumerate(zip(lengths, self.codes)):
            if l:
                self.lookup[(l, c)] = e
        self.maxlen = max(lengths) if lengths else 0
        used = [l for l in lengths if l]
        self.single = len(used) == 1
        self.lookup_type = b.read(4)
        self.vectors = None
        if self.lookup_type in (1, 2):
            minimum = float32_unpack(b.read(32))
            delta = float32_unpack(b.read(32))
            value_bits = b.read(4) + 1
            sequence_p = b.bit()
            if self.lookup_type == 1:
                count = lookup1_values(self.entries, self.dims)
            else:
                count = self.entries * self.dims
            mult = [b.read(value_bits) for _ in range(count)]
            self.vectors = []
            for e in range(self.entries):
                v, last = [], 0.0
                if self.lookup_type == 1:
                    div = 1
                    for _ in range(self.dims):
                        off = (e // div) % count
                        x = mult[off] * delta + minimum + last
                        if sequence_p:
                            last = x
                        v.append(x)
                        div *= count
                else:
                    for i in range(self.dims):
                        x = mult[e * self.dims + i] * delta + minimum + last
                        if sequence_p:
                            last = x
                        v.append(x)
                self.vectors.append(v)
        elif self.lookup_type != 0:
            raise ValueError('codebook lookup type %d' % self.lookup_type)

    def scalar(self, b):
        """An entry, its codeword read a bit at a time, the codeword's top
        bit first."""
        if self.single:
            # the one entry, whose codeword is a single bit
            b.read(1)
            return next(e for e, l in enumerate(self.lengths) if l)
        code = 0
        for l in range(1, self.maxlen + 1):
            code = code << 1 | b.bit()
            e = self.lookup.get((l, code))
            if e is not None:
                return e
        raise ValueError('a codeword not in the codebook')

    def vector(self, b):
        return self.vectors[self.scalar(b)]


def assign_codewords(lengths):
    """Each entry in turn the lowest valued codeword of its length not taken
    by, nor the prefix of, nor prefixed by one already given."""
    # a trie: a node is None (free), 'leaf', or [zero, one]
    root = [None, None]
    codes = []

    def place(node, depth, want, code):
        # node is a list; place a leaf at depth `want` under it
        for b in (0, 1):
            child = node[b]
            c = code << 1 | b
            if depth + 1 == want:
                if child is None:
                    node[b] = 'leaf'
                    return c
                continue
            if child == 'leaf':
                continue
            if child is None:
                child = node[b] = [None, None]
            r = place(child, depth + 1, want, c)
            if r is not None:
                return r
        return None

    for l in lengths:
        if not l:
            codes.append(0)
            continue
        c = place(root, 0, l, 0)
        if c is None:
            raise ValueError('codeword lengths that overflow')
        codes.append(c)
    return codes


# ------------------------------------------------------------------ setup

class Floor1:
    def __init__(self, b):
        partitions = b.read(5)
        self.part_class = [b.read(4) for _ in range(partitions)]
        classes = max(self.part_class) + 1 if partitions else 0
        self.class_dims, self.class_sub, self.class_master, self.sub_books = [], [], [], []
        for _ in range(classes):
            self.class_dims.append(b.read(3) + 1)
            sub = b.read(2)
            self.class_sub.append(sub)
            self.class_master.append(b.read(8) if sub else None)
            self.sub_books.append([b.read(8) - 1 for _ in range(1 << sub)])
        self.multiplier = b.read(2) + 1
        rangebits = b.read(4)
        self.X = [0, 1 << rangebits]
        for c in self.part_class:
            for _ in range(self.class_dims[c]):
                self.X.append(b.read(rangebits))
        n = len(self.X)
        self.order = sorted(range(n), key=lambda i: self.X[i])
        # each point's neighbours among those before it in the list
        self.low, self.high = [0] * n, [0] * n
        for i in range(2, n):
            lo = hi = None
            for j in range(i):
                if self.X[j] < self.X[i] and (lo is None or self.X[j] > self.X[lo]):
                    lo = j
                if self.X[j] > self.X[i] and (hi is None or self.X[j] < self.X[hi]):
                    hi = j
            self.low[i], self.high[i] = lo, hi


class Residue:
    def __init__(self, b, kind):
        self.kind = kind
        self.begin = b.read(24)
        self.end = b.read(24)
        self.psize = b.read(24) + 1
        self.classes = b.read(6) + 1
        self.classbook = b.read(8)
        cascade = []
        for _ in range(self.classes):
            low = b.read(3)
            high = b.read(5) if b.bit() else 0
            cascade.append(high * 8 + low)
        self.books = [[b.read(8) if c >> j & 1 else None for j in range(8)] for c in cascade]


class Mapping:
    def __init__(self, b, channels):
        if b.read(16) != 0:
            raise ValueError('a mapping not of type 0')
        self.submaps = b.read(4) + 1 if b.bit() else 1
        self.coupling = []
        if b.bit():
            for _ in range(b.read(8) + 1):
                m = b.read(ilog(channels - 1))
                a = b.read(ilog(channels - 1))
                self.coupling.append((m, a))
        if b.read(2) != 0:
            raise ValueError('a mapping with reserved bits set')
        self.mux = [b.read(4) for _ in range(channels)] if self.submaps > 1 else [0] * channels
        self.floor, self.residue = [], []
        for _ in range(self.submaps):
            b.read(8)
            self.floor.append(b.read(8))
            self.residue.append(b.read(8))


# floor1's dB table: 256 steps over 140 dB, 10^(7 (i + 1) / 256 - 7)
INVERSE_DB = [10.0 ** (7.0 * (i + 1) / 256.0 - 7.0) for i in range(256)]


def render_point(x0, y0, x1, y1, x):
    dy = y1 - y0
    adx = x1 - x0
    off = abs(dy) * (x - x0) // adx
    return y0 - off if dy < 0 else y0 + off


def render_line(x0, y0, x1, y1, v, n):
    dy = y1 - y0
    adx = x1 - x0
    ady = abs(dy)
    base = int(dy / adx)             # toward zero
    sy = base - 1 if dy < 0 else base + 1
    ady -= abs(base) * adx
    y, err = y0, 0
    if x0 < n:
        v[x0] = y
    for x in range(x0 + 1, min(x1, n)):
        err += ady
        if err >= adx:
            err -= adx
            y += sy
        else:
            y += base
        v[x] = y


# ---------------------------------------------------------------- decoder

class Vorbis:
    def __init__(self, data):
        self.packets = packets(data)
        ident, _ = next(self.packets)
        b = Bits(ident)
        if b.read(8) != 1 or bytes(b.read(8) for _ in range(6)) != b'vorbis':
            raise ValueError('not a Vorbis identification header')
        if b.read(32) != 0:
            raise ValueError('a Vorbis version other than 0')
        self.channels = b.read(8)
        self.rate = b.read(32)
        b.read(32), b.read(32), b.read(32)
        self.bs = [1 << b.read(4), 1 << b.read(4)]
        next(self.packets)                      # comment
        setup, _ = next(self.packets)
        self.read_setup(Bits(setup))
        self.prev = None                        # the last block, windowed
        self.prev_n = 0
        self.windows = {}

    def read_setup(self, b):
        if b.read(8) != 5 or bytes(b.read(8) for _ in range(6)) != b'vorbis':
            raise ValueError('not a Vorbis setup header')
        self.books = [Codebook(b) for _ in range(b.read(8) + 1)]
        for _ in range(b.read(6) + 1):
            if b.read(16) != 0:
                raise ValueError('a time domain transform')
        self.floors = []
        for _ in range(b.read(6) + 1):
            kind = b.read(16)
            if kind != 1:
                raise ValueError('floor type %d (the game has only 1)' % kind)
            self.floors.append(Floor1(b))
        self.residues = []
        for _ in range(b.read(6) + 1):
            kind = b.read(16)
            if kind > 2:
                raise ValueError('residue type %d' % kind)
            self.residues.append(Residue(b, kind))
        self.mappings = [Mapping(b, self.channels) for _ in range(b.read(6) + 1)]
        self.modes = []
        for _ in range(b.read(6) + 1):
            blockflag = b.bit()
            b.read(16), b.read(16)
            self.modes.append((blockflag, b.read(8)))
        if not b.bit():
            raise ValueError('a setup header without its framing bit')

    # --- floor 1

    def floor_decode(self, f, b):
        """The floor's Y values, or None for a channel unused this packet."""
        if not b.bit():
            return None
        rng = [256, 128, 86, 64][f.multiplier - 1]
        Y = [b.read(ilog(rng - 1)), b.read(ilog(rng - 1))]
        for c in f.part_class:
            cdim, cbits = f.class_dims[c], f.class_sub[c]
            csub = (1 << cbits) - 1
            cval = self.books[f.class_master[c]].scalar(b) if cbits else 0
            for _ in range(cdim):
                book = f.sub_books[c][cval & csub]
                cval >>= cbits
                Y.append(self.books[book].scalar(b) if book >= 0 else 0)
        return Y

    def floor_curve(self, f, Y, n):
        """The floor's curve over the n coefficients, as amplitudes."""
        rng = [256, 128, 86, 64][f.multiplier - 1]
        count = len(f.X)
        final = list(Y)
        used = [True, True] + [False] * (count - 2)
        for i in range(2, count):
            lo, hi = f.low[i], f.high[i]
            pred = render_point(f.X[lo], final[lo], f.X[hi], final[hi], f.X[i])
            val = Y[i]
            highroom, lowroom = rng - pred, pred
            room = 2 * min(highroom, lowroom)
            if val:
                used[lo] = used[hi] = used[i] = True
                if val >= room:
                    final[i] = val - lowroom + pred if highroom > lowroom else pred - val + highroom - 1
                else:
                    final[i] = pred - (val + 1) // 2 if val & 1 else pred + val // 2
            else:
                final[i] = pred
        v = [0] * n
        lx, ly = 0, final[f.order[0]] * f.multiplier
        hx, hy = 0, ly
        for k in f.order[1:]:
            if used[k]:
                hy = final[k] * f.multiplier
                hx = f.X[k]
                render_line(lx, ly, hx, hy, v, n)
                lx, ly = hx, hy
        if hx < n:
            render_line(hx, hy, n, hy, v, n)
        return [INVERSE_DB[min(max(y, 0), 255)] for y in v]

    # --- residue

    def residue_decode(self, r, b, vectors, skip, n):
        """Add the residue into each channel's vector of n."""
        if r.kind == 2:
            if all(skip):
                return
            m = len(vectors)
            flat = [0.0] * (n * m)
            self.residue_partitions(r, b, [flat], [False], n * m, 1)
            for i in range(n * m):
                vectors[i % m][i // m] += flat[i]
            return
        self.residue_partitions(r, b, vectors, skip, n, r.kind)

    def residue_partitions(self, r, b, vectors, skip, size, fmt):
        begin, end = min(r.begin, size), min(r.end, size)
        psize = r.psize
        parts = (end - begin) // psize
        if parts <= 0:
            return
        classbook = self.books[r.classbook]
        per = classbook.dims
        classes = [[0] * (parts + per) for _ in vectors]
        try:
            for p in range(8):
                pc = 0
                while pc < parts:
                    if p == 0:
                        for j, v in enumerate(vectors):
                            if skip[j]:
                                continue
                            t = classbook.scalar(b)
                            for i in range(per - 1, -1, -1):
                                classes[j][pc + i] = t % r.classes
                                t //= r.classes
                    for _ in range(per):
                        if pc >= parts:
                            break
                        for j, v in enumerate(vectors):
                            if skip[j]:
                                continue
                            book = r.books[classes[j][pc]][p]
                            if book is None:
                                continue
                            cb = self.books[book]
                            off = begin + pc * psize
                            if fmt == 0:
                                step = psize // cb.dims
                                for i in range(step):
                                    vec = cb.vector(b)
                                    for k in range(cb.dims):
                                        v[off + i + k * step] += vec[k]
                            else:
                                i = 0
                                while i < psize:
                                    vec = cb.vector(b)
                                    for k in range(cb.dims):
                                        v[off + i] += vec[k]
                                        i += 1
                        pc += 1
        except EndOfPacket:
            pass

    # --- the packet

    def window(self, n, prev_long, next_long, blockflag):
        key = (n, prev_long, next_long, blockflag)
        if key in self.windows:
            return self.windows[key]
        s = self.bs[0]
        if blockflag and not prev_long:
            ls, le = n // 4 - s // 4, n // 4 + s // 4
        else:
            ls, le = 0, n // 2
        if blockflag and not next_long:
            rs, re = n * 3 // 4 - s // 4, n * 3 // 4 + s // 4
        else:
            rs, re = n // 2, n
        w = [0.0] * n
        for i in range(n):
            if ls <= i < le:
                x = (i - ls + 0.5) / (le - ls) * math.pi / 2
                w[i] = math.sin(math.pi / 2 * math.sin(x) ** 2)
            elif le <= i < rs:
                w[i] = 1.0
            elif rs <= i < re:
                x = (i - rs + 0.5) / (re - rs) * math.pi / 2 + math.pi / 2
                w[i] = math.sin(math.pi / 2 * math.sin(x) ** 2)
        self.windows[key] = w
        return w

    def decode(self, packet):
        """The packet's samples, a list a channel; none for the first."""
        b = Bits(packet)
        try:
            if b.bit():
                return None             # not an audio packet
            mode = self.modes[b.read(ilog(len(self.modes) - 1))]
            blockflag, mapping = mode
            n = self.bs[blockflag]
            prev_long = next_long = True
            if blockflag:
                prev_long, next_long = b.bit(), b.bit()
            m = self.mappings[mapping]
            half = n // 2
            floors, skip = [], []
            for ch in range(self.channels):
                f = self.floors[m.floor[m.mux[ch]]]
                Y = self.floor_decode(f, b)
                floors.append((f, Y))
                skip.append(Y is None)
            no_residue = list(skip)
            for mag, ang in m.coupling:
                if not no_residue[mag] or not no_residue[ang]:
                    no_residue[mag] = no_residue[ang] = False
            vectors = [[0.0] * half for _ in range(self.channels)]
            for s in range(m.submaps):
                chans = [c for c in range(self.channels) if m.mux[c] == s]
                self.residue_decode(self.residues[m.residue[s]], b, [vectors[c] for c in chans],
                                    [no_residue[c] for c in chans], half)
        except EndOfPacket:
            return None
        for mag, ang in reversed(m.coupling):
            M, A = vectors[mag], vectors[ang]
            for i in range(half):
                mv, av = M[i], A[i]
                if mv > 0:
                    if av > 0:
                        M[i], A[i] = mv, mv - av
                    else:
                        M[i], A[i] = mv + av, mv
                else:
                    if av > 0:
                        M[i], A[i] = mv, mv + av
                    else:
                        M[i], A[i] = mv - av, mv
        w = self.window(n, prev_long, next_long, blockflag)
        out = []
        blocks = []
        for ch in range(self.channels):
            f, Y = floors[ch]
            if Y is None:
                spec = [0.0] * half
            else:
                curve = self.floor_curve(f, Y, half)
                spec = [vectors[ch][i] * curve[i] for i in range(half)]
            y = imdct(spec)
            blocks.append([y[i] * w[i] for i in range(n)])
        # overlap: the right half of the block before over the left of this
        if self.prev is None:
            self.prev, self.prev_n = blocks, n
            return None
        pn = self.prev_n
        # the centres: the last's at pn / 2, this one's at n / 2, a quarter
        # of each apart
        start_prev = pn // 2                    # the last's centre
        shift = pn // 4 + n // 4                # from it to this one's centre
        for ch in range(self.channels):
            prev = self.prev[ch]
            cur = blocks[ch]
            # this block starts at (last's centre + shift) - n/2 in the last's frame
            off = start_prev + shift - n // 2
            o = [0.0] * shift
            for i in range(shift):
                t = start_prev + i
                v = prev[t] if t < pn else 0.0
                j = t - off
                if 0 <= j < n:
                    v += cur[j]
                o[i] = v
            out.append(o)
        self.prev, self.prev_n = blocks, n
        return out


# -------------------------------------------------------------- the MDCT

def fft(a):
    """In place, forward, e^(-2 pi i jk / n), n a power of two."""
    n = len(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        for s in range(0, n, size):
            wk = 1
            for k in range(size // 2):
                u, t = a[s + k], a[s + k + size // 2] * wk
                a[s + k], a[s + k + size // 2] = u + t, u - t
                wk *= w
        size <<= 1
    return a


def dct4(X):
    """Z[n] = sum X[k] cos(pi / M (n + 1/2)(k + 1/2)), by an FFT of M / 2."""
    M = len(X)
    h = M // 2
    v = [(X[2 * k] + 1j * X[M - 1 - 2 * k]) * cmath.exp(-1j * math.pi * (4 * k + 1) / (4 * M)) for k in range(h)]
    fft(v)
    Z = [0.0] * M
    for k in range(h):
        c = v[k] * cmath.exp(-1j * math.pi * k / M)
        Z[2 * k] = c.real
        Z[M - 1 - 2 * k] = -c.imag
    return Z


def imdct(X):
    """y[n] = sum X[k] cos(2 pi / N (n + N/4 + 1/2)(k + 1/2)), n < N, from the
    DCT-IV of the M = N / 2 coefficients, extended by its symmetries."""
    M = len(X)
    Z = dct4(X)
    N = 2 * M
    y = [0.0] * N
    for n in range(N):
        m = n + M // 2
        if m < M:
            y[n] = Z[m]
        elif m < 2 * M:
            y[n] = -Z[2 * M - 1 - m]
        else:
            y[n] = -Z[m - 2 * M]
    return y


def main(argv):
    out = None
    if '-o' in argv:
        i = argv.index('-o')
        out = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    v = Vorbis(open(argv[0], 'rb').read())
    seconds = float(argv[1]) if len(argv) > 1 else 1e9
    print('%s: %d channels, %d Hz, blocks %d and %d, %d codebooks, %d floors, %d residues (types %s), '
          '%d mappings, %d modes' % (argv[0], v.channels, v.rate, v.bs[0], v.bs[1], len(v.books), len(v.floors),
                                     len(v.residues), sorted(set(r.kind for r in v.residues)), len(v.mappings),
                                     len(v.modes)))
    f = open(out, 'wb') if out else None
    done = 0
    for pkt, granule in v.packets:
        s = v.decode(pkt)
        if s is None:
            continue
        frames = len(s[0])
        if f:
            f.write(struct.pack('<%df' % (frames * v.channels),
                                *[s[c][i] for i in range(frames) for c in range(v.channels)]))
        done += frames
        if done >= seconds * v.rate:
            break
    print('%d samples a channel' % done)


if __name__ == '__main__':
    main(sys.argv[1:])
