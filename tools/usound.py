"""Sounds of a UE2 package (.uax, and some in .u and .ukx).

A Sound record has a property block, empty in every sound of Shrek 2, then:

    index       FileType, a name: "bik" or "WAV"
    lazy array  the sound file itself: u32 offset of the array's end, a compact
                count, then that many bytes
    ...         KnowWonder's lip sync data, in one of three versions

The file is stored whole, so it plays as it is: a RIFF WAV, or Bink Audio,
which FFmpeg decodes. In Shrek 2 3353 sounds are Bink and 416 WAV.

The lip sync block after it starts with an i32 version, and each version was
checked against the sound's own length as FFmpeg decodes it:

    0           nothing more (25), or one i32: -1 (442) or the length of the
                sound as 16 bit PCM in bytes
    1           a compact count, that many amplitude bytes, one every 20 ms,
                and the length of the sound as 16 bit PCM in bytes. Read to
                the exact end of all 270.
    2           i32 length in milliseconds, i32 frames a second (30), i32
                the first frame's number and i32 the last's, the sound
                starting at frame 0 (the first is -9 to -14), a compact count
                of floats, that many, 26 a frame, then the length of the
                sound as 16 bit PCM in bytes. Read to the exact end of all
                2681. The length is 98.5 percent of the decoded duration on
                median, Bink rounding up to whole frames.

Version 2's 26 channels, told apart by what they do over the dialogue:

    0-14        phoneme weights, 0 to 1, by the letters of the subtitled lines
                their peaks go with (1935 lines): 0 e, ee; 1 er, r; 2 i, a;
                3 the a of "Donkey"; 4 o, oo, ou; 5 w; 6 s; 7 sh, j, ch; 8 f,
                v; 9 th; 10 t, d, k; 11 m, b, p; 12 n, ng, g; 13 r; 14 never
                past 0.22
    15, 16      nearly equal, to 1.15, a fifth of the time; not placed yet
    17, 18      equal, short rises to 1: the left and right blinks
    19-25       signed, to 9: 22 and 23 are -21, 24 and 25 -19 frame by
                frame in most lines, the eyes turning against the head

Usage: usound.py <package or directory>...           survey
       usound.py <package> -o <dir>                  write every sound as a file
"""
import sys, os, struct, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from umap import Map
from udefaults import Tagged

EXTENSIONS = {'bik': '.bik', 'wav': '.wav'}


class Sound:
    def __init__(self, pkg, e):
        self.p, self.e, self.name = pkg, e, e['name']
        end = e['off'] + e['size']
        v = Tagged(pkg).parse(Map(pkg).props_start(e), end, want_values=True)
        if not v:
            raise ValueError('%s: no property block' % self.name)
        self.properties = {x['name']: x for x in v[0]}
        r = R(pkg.b, v[1])
        self.file_type = pkg.names[r.idx()]
        lazy_end = r.u32()
        n = r.idx()
        self.data = pkg.b[r.p:r.p + n]
        r.p += n
        if r.p != lazy_end or r.p > end:
            raise ValueError('%s: sound data does not end at its lazy array end' % self.name)
        self.lipsync = pkg.b[r.p:end]

    @property
    def extension(self):
        return EXTENSIONS.get(self.file_type.lower(), '.' + self.file_type.lower())

    def lipsync_version(self):
        return struct.unpack_from('<i', self.lipsync)[0] if len(self.lipsync) >= 4 else None

    def lipsync_header(self):
        """Version 2's i32s after the version: length in milliseconds, frames
        a second, the first frame's number and the last's."""
        if self.lipsync_version() != 2:
            return None
        return struct.unpack_from('<4i', self.lipsync, 4)

    def curves(self):
        """Version 2: (first frame, frames a second, frames of 26 floats; length
        as 16 bit PCM in bytes), or None."""
        if self.lipsync_version() != 2:
            return None
        _, fps, first, last = self.lipsync_header()
        r = R(self.lipsync, 20)
        n = r.idx()
        if n != (last - first + 1) * 26 or r.p + 4 * n + 4 != len(self.lipsync):
            raise ValueError('%s: version 2 lip sync does not end on its record' % self.name)
        values = struct.unpack_from('<%df' % n, self.lipsync, r.p)
        frames = [values[k:k + 26] for k in range(0, n, 26)]
        return first, fps, frames, struct.unpack_from('<i', self.lipsync, r.p + 4 * n)[0]

    def amplitudes(self):
        """Version 1: (amplitude bytes, one per 20 ms; length as 16 bit PCM in
        bytes), or None."""
        if self.lipsync_version() != 1:
            return None
        r = R(self.lipsync, 4)
        n = r.idx()
        amps = self.lipsync[r.p:r.p + n]
        r.p += n
        if r.p + 4 != len(self.lipsync):
            raise ValueError('%s: version 1 lip sync does not end on its record' % self.name)
        return amps, struct.unpack_from('<i', self.lipsync, r.p)[0]


def sounds(path):
    p = Package(path)
    for e in p.exports:
        if p.classof(e) == 'Sound' and e['size']:
            yield Sound(p, e)


def main(argv):
    if '-o' in argv:
        i = argv.index('-o')
        out, paths = argv[i + 1], argv[:i] + argv[i + 2:]
        os.makedirs(out, exist_ok=True)
        n = 0
        for path in paths:
            for s in sounds(path):
                with open(os.path.join(out, s.name + s.extension), 'wb') as f:
                    f.write(s.data)
                n += 1
        print('%d sounds written to %s' % (n, out))
        return
    paths = []
    for a in argv:
        if os.path.isdir(a):
            paths += [os.path.join(a, f) for f in sorted(os.listdir(a))
                      if f.lower().endswith(('.uax', '.u', '.ukx'))]
        else:
            paths.append(a)
    types, versions = collections.Counter(), collections.Counter()
    total = size = exact = exact2 = 0
    for path in paths:
        for s in sounds(path):
            total += 1
            size += len(s.data)
            types[s.file_type] += 1
            versions[s.lipsync_version()] += 1
            if s.lipsync_version() == 1:
                s.amplitudes()
                exact += 1
            if s.lipsync_version() == 2:
                s.curves()
                exact2 += 1
    print('%d sounds read to their lazy array end, %.1f MB of sound files' % (total, size / 1e6))
    print('file types: %s' % dict(types.most_common()))
    print('lip sync versions: %s; all %d of version 1 and %d of version 2 read to their end'
          % (dict(sorted(versions.items(), key=str)), exact, exact2))


if __name__ == '__main__':
    main(sys.argv[1:])
