"""Bink Audio, the codec of 3353 of Shrek 2's sounds (usound.py).

A sound's Bink file is RAD's container with one audio track and a 4 by 4
video track that carries nothing:

    char[4]     "BIK" and a version letter, "i" in all of the game's
    u32         file size less 8
    u32         frames
    u32         largest frame
    u32         frames again
    u32 x2      width and height, 4 by 4
    u32 x2      frame rate as a fraction, 15 / 1
    u32         video flags
    u32         audio tracks, 1
    per track   u32 the largest decoded packet; u16 sample rate and u16
                flags (0x1000 the DCT variant, 0x2000 stereo); u32 track id
    u32 x frames+1  frame offsets, the low bit a key frame's
    per frame   per audio track a u32 packet size and the packet; the video
                after it

An audio packet starts with a u32, the samples it decodes to as 16 bit bytes,
then blocks to its end, each of frame_len samples per channel of which the
last sixteenth overlaps the next block's first. The bits are read least
significant first. A block per channel, in the DCT variant:

    2 bits      skipped
    2 floats    the first two coefficients (5 bit exponent, 23 bit mantissa,
                sign)
    8 bits each the quantiser of every band, an index into exp(0.1529 i)
    runs        to frame_len: a flag, then 4 bits into a run length table
                times 8 or 8 without it; 4 bits of width; that many bits per
                coefficient with a sign bit after a non-zero one, each times
                its band's quantiser
    align       to 32 bits

The coefficients go through an inverse DCT (type III). The band edges are
the critical frequencies Bink shares with WMA, over half the sample rate.

How this was found: the container's layout and the block's fields follow the
codec as it is known publicly; every constant left open, the transform's scale
among them, was settled by measuring against FFmpeg's decoding of the game's
own files, which serves only as the reference output.

Usage: ubink.py <file.bik> [out.raw]   decode to 32 bit float, mono
"""
import sys, math, struct

CRITICAL = [100, 200, 300, 400, 510, 630, 770, 920, 1080, 1270, 1480, 1720, 2000, 2320, 2700,
            3150, 3700, 4400, 5300, 6400, 7700, 9500, 12000, 15500, 24500]
RLE = [2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32, 64]


class Bits:
    """Least significant bit first, from a bytes object."""

    def __init__(self, data):
        self.d, self.pos, self.n = data, 0, len(data) * 8

    def get(self, k):
        v = 0
        for i in range(k):
            p = self.pos + i
            v |= ((self.d[p >> 3] >> (p & 7)) & 1) << i
        self.pos += k
        return v

    def float(self):
        power = self.get(5)
        f = math.ldexp(self.get(23), power - 23)
        return -f if self.get(1) else f

    def align32(self):
        self.pos = (self.pos + 31) & ~31

    def left(self):
        return self.n - self.pos


class Header:
    def __init__(self, b):
        if b[:3] != b'BIK':
            raise ValueError('not a Bink file')
        self.version = chr(b[3])
        self.frames, = struct.unpack_from('<I', b, 8)
        tracks, = struct.unpack_from('<I', b, 40)
        if tracks != 1:
            raise ValueError('%d audio tracks' % tracks)
        w, h = struct.unpack_from('<II', b, 20)
        self.video = (w, h) != (4, 4)
        o = 44 + 4 * tracks
        self.rate, self.flags = struct.unpack_from('<HH', b, o)
        o += 4 * tracks + 4 * tracks
        self.offsets = [x & ~1 for x in struct.unpack_from('<%dI' % (self.frames + 1), b, o)]
        self.dct = bool(self.flags & 0x1000)
        self.channels = 2 if self.flags & 0x2000 else 1

    def packets(self, b):
        for i in range(self.frames):
            at = self.offsets[i]
            size, = struct.unpack_from('<I', b, at)
            if size:
                yield b[at + 4:at + 4 + size]


class Decoder:
    def __init__(self, rate, channels, dct):
        if not dct:
            raise ValueError('only the DCT variant is the game\'s')
        self.channels = channels
        bits = 9 if rate < 22050 else 10 if rate < 44100 else 11
        self.n = 1 << bits
        self.overlap = self.n // 16
        self.root = self.n / (math.sqrt(self.n) * 32768.0)
        self.quant = [math.exp(i * 0.15289164787221953823) * self.root for i in range(96)]
        half = (rate + 1) // 2
        nb = 1
        while nb < 25 and CRITICAL[nb - 1] < half:
            nb += 1
        self.bands = [2] + [(CRITICAL[i - 1] * self.n // half) & ~1 for i in range(1, nb)] + [self.n]
        self.previous = [None] * channels
        # the DCT-III matrix, once
        n = self.n
        self.cos = [[math.cos(math.pi * k * (2 * j + 1) / (2 * n)) for k in range(n)] for j in range(n)]

    def block(self, g):
        """A block: each channel's coefficients and samples in turn."""
        out = []
        g.get(2)
        for ch in range(self.channels):
            out.append(self.channel(g, ch))
        g.align32()
        return out

    def channel(self, g, ch):
        n = self.n
        c = [0.0] * n
        c[0] = g.float() * self.root
        c[1] = g.float() * self.root
        q = [self.quant[min(g.get(8), 95)] for _ in range(len(self.bands) - 1)]
        k, qk, i = 0, q[0], 2
        while i < n:
            if g.get(1):
                j = i + RLE[g.get(4)] * 8
            else:
                j = i + 8
            j = min(j, n)
            width = g.get(4)
            if width == 0:
                i = j
                while self.bands[k] < i:
                    qk = q[k]
                    k += 1
            else:
                while i < j:
                    if self.bands[k] == i:
                        qk = q[k]
                        k += 1
                    v = g.get(width)
                    if v:
                        c[i] = -qk * v if g.get(1) else qk * v
                    i += 1
        # inverse DCT, type III, the first coefficient whole
        # the first coefficient counted whole, and the sum scaled by 2 / n,
        # as FFmpeg's output measures
        out = [(c[0] + sum(c[k] * self.cos[j][k] for k in range(1, n))) * 2 / n for j in range(n)]
        if self.previous[ch] is not None:
            # the crossfade runs over the channels' samples interleaved, as
            # the DLL does it on its 16 bit output (0x3001b301): sample t of
            # channel ch is the (t * channels + ch)th of channels * overlap
            m = self.overlap * self.channels
            for t in range(self.overlap):
                i = t * self.channels + ch
                out[t] = (self.previous[ch][t] * (m - i) + out[t] * i) / m
        self.previous[ch] = out[n - self.overlap:]
        return out[:n - self.overlap]

    def packet(self, data):
        g = Bits(data)
        g.get(32)                   # the decoded size
        samples = []
        while g.left() > 0:
            chans = self.block(g)
            for i in range(len(chans[0])):
                samples += [c[i] for c in chans]
        return samples


def decode(b):
    h = Header(b)
    d = Decoder(h.rate, h.channels, h.dct)
    out = []
    for p in h.packets(b):
        out += d.packet(p)
    return h, out


def main(argv):
    b = open(argv[0], 'rb').read()
    h, out = decode(b)
    print('%s: version %s, %d Hz, %d channel(s), %s, %d samples' %
          (argv[0], h.version, h.rate, h.channels, 'DCT' if h.dct else 'RDFT', len(out)))
    if len(argv) > 1:
        open(argv[1], 'wb').write(struct.pack('<%df' % len(out), *out))


if __name__ == '__main__':
    main(sys.argv[1:])
