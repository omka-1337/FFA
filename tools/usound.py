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
    2           i32 length in milliseconds, i32 30, i32 between -9 and -13,
                then curves of floats over time, not decoded yet. The length
                is 98.5 percent of the decoded duration on median, Bink
                rounding up to whole frames.

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
        """Version 2's i32s after the version: length in milliseconds, 30, a
        small negative number, and two more not understood yet."""
        if self.lipsync_version() != 2:
            return None
        return struct.unpack_from('<5i', self.lipsync, 4)

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
    total = size = exact = 0
    for path in paths:
        for s in sounds(path):
            total += 1
            size += len(s.data)
            types[s.file_type] += 1
            versions[s.lipsync_version()] += 1
            if s.lipsync_version() == 1:
                s.amplitudes()
                exact += 1
    print('%d sounds read to their lazy array end, %.1f MB of sound files' % (total, size / 1e6))
    print('file types: %s' % dict(types.most_common()))
    print('lip sync versions: %s; all %d of version 1 read to their end'
          % (dict(sorted(versions.items(), key=str)), exact))


if __name__ == '__main__':
    main(sys.argv[1:])
