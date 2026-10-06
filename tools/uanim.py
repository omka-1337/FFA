"""Skeletal animation: MeshAnimation records in .ukx packages.

Layout, proven against all 134 MeshAnimation records of Shrek 2 by landing on the
exact end of every one:

    u32       Version, 0 or 4
    index     bone count, then per bone: name index, u32 flags, i32 parent
    index     motion chunk count, then per chunk:
      FVector   RootSpeed3D
      f32       TrackTime
      i32       StartBone
      u32       Flags
      index     bone index count, then that many i32
      index     track count, then that many tracks
      track     the root track
      index     one more field, version 4 only, zero in all 1543 chunks
    index     sequence count, then per sequence:
      f32       unknown, 0 to 1
      index     name
      index     group count, then that many name indices
      i32       StartFrame
      i32       NumFrames
      index     notify count, then per notify: f32 time, name index, object index
      f32       Rate, frames per second

A track is a u32 of flags and three arrays: rotation keys (unit quaternions, 16
bytes), position keys (FVector), and key times (f32). One chunk per sequence;
a sequence's chunk holds one track per animated bone.

The per chunk field is the one that took finding. In a record with a single
chunk it looks like one stray byte after the moves; only a record with two
chunks shows that it belongs to each chunk, because the second chunk starts one
byte later than the first one ends.
"""
import sys, os, struct, glob, collections, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R


class Track:
    __slots__ = ('flags', 'quats', 'positions', 'times')


class Chunk:
    __slots__ = ('root_speed', 'track_time', 'start_bone', 'flags', 'bone_indices',
                 'tracks', 'root')


class Sequence:
    __slots__ = ('unknown', 'name', 'groups', 'start_frame', 'num_frames',
                 'notifies', 'rate')


class MeshAnimation:
    def __init__(self, pkg, e):
        self.p, self.e, self.name = pkg, e, e['name']
        b = pkg.b
        end = e['off'] + e['size']
        r = R(b, e['off'])
        if r.idx() != pkg.none_idx:
            raise ValueError('unexpected properties')
        self.version = r.u32()
        self.bones = []
        for _ in range(r.idx()):
            nm = r.idx()
            self.bones.append((pkg.names[nm], r.u32(), r.i32()))
        self.chunks = []
        for _ in range(r.idx()):
            c = Chunk()
            c.root_speed = struct.unpack_from('<3f', b, r.p)
            c.track_time = struct.unpack_from('<f', b, r.p + 12)[0]
            r.p += 16
            c.start_bone = r.i32()
            c.flags = r.u32()
            n = r.idx()
            c.bone_indices = list(struct.unpack_from('<%di' % n, b, r.p)) if n else []
            r.p += 4 * n
            c.tracks = [self.track(r) for _ in range(r.idx())]
            c.root = self.track(r)
            if self.version >= 4:
                r.idx()
            self.chunks.append(c)
            if r.p > end:
                raise ValueError('ran past the record')
        self.sequences = []
        for _ in range(r.idx()):
            s = Sequence()
            s.unknown = struct.unpack_from('<f', b, r.p)[0]
            r.p += 4
            s.name = pkg.names[r.idx()]
            s.groups = [pkg.names[r.idx()] for _ in range(r.idx())]
            s.start_frame = r.i32()
            s.num_frames = r.i32()
            s.notifies = []
            for _ in range(r.idx()):
                t = struct.unpack_from('<f', b, r.p)[0]
                r.p += 4
                s.notifies.append((t, pkg.names[r.idx()], r.idx()))
            s.rate = struct.unpack_from('<f', b, r.p)[0]
            r.p += 4
            self.sequences.append(s)
        self.exact = (r.p == end)

    def track(self, r):
        b = self.p.b
        t = Track()
        t.flags = r.u32()
        n = r.idx()
        t.quats = [struct.unpack_from('<4f', b, r.p + 16 * i) for i in range(n)]
        r.p += 16 * n
        n = r.idx()
        t.positions = [struct.unpack_from('<3f', b, r.p + 12 * i) for i in range(n)]
        r.p += 12 * n
        n = r.idx()
        t.times = list(struct.unpack_from('<%df' % n, b, r.p)) if n else []
        r.p += 4 * n
        return t

    def unit_quaternions(self):
        bad = 0
        for c in self.chunks:
            for t in c.tracks:
                for q in t.quats:
                    if abs(math.sqrt(sum(x * x for x in q)) - 1) > 1e-3:
                        bad += 1
        return bad


def animations(path):
    p = Package(path)
    for e in p.exports:
        if p.classof(e) == 'MeshAnimation' and e['size']:
            yield MeshAnimation(p, e)


def main(argv):
    paths = []
    for a in argv:
        paths += sorted(glob.glob(os.path.join(a, '*.ukx'))) if os.path.isdir(a) else [a]
    n = exact = seqs = keys = badq = 0
    for f in paths:
        for a in animations(f):
            n += 1
            exact += a.exact
            seqs += len(a.sequences)
            keys += sum(len(t.quats) for c in a.chunks for t in c.tracks)
            badq += a.unit_quaternions()
    print('%d MeshAnimation records, %d parsed to the exact end, %d sequences, '
          '%d rotation keys, %d not unit length' % (n, exact, seqs, keys, badq))


if __name__ == '__main__':
    main(sys.argv[1:])
