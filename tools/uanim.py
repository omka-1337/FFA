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


# ------------------------------------------------------------------- playback
#
# Convention, measured rather than assumed. Rotation keys and the reference
# skeleton store quaternions the same way: 39008 first keys of unanimated
# non-root bones equal the reference rotation exactly, and none equals its
# conjugate. Composing the hierarchy then needs the non-root rotations
# conjugated and the root's taken as is: only that reading puts the joints on the
# skin, a median of 1.8 units from the nearest skin point on Shrek, 1.3 on
# Donkey and 1.1 on Fiona, against 7.6 to 20 for the other three readings.

def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    return qmul(qmul(q, (v[0], v[1], v[2], 0.0)), qconj(q))[:3]


def qnlerp(a, b, t):
    if sum(x * y for x, y in zip(a, b)) < 0:
        b = tuple(-x for x in b)
    q = tuple(x + (y - x) * t for x, y in zip(a, b))
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return tuple(x / n for x in q)


def sample(track, frame, ref_q, ref_p):
    """Rotation and position of one track at a frame, by linear interpolation
    between keys. Zero quaternions mark keys with no rotation."""
    def at(keys, lerp, ref):
        if not keys:
            return ref
        times = track.times if len(track.times) == len(keys) else range(len(keys))
        if frame <= times[0] or len(keys) == 1:
            return keys[0]
        for i in range(1, len(keys)):
            if frame <= times[i]:
                t0, t1 = times[i - 1], times[i]
                f = (frame - t0) / (t1 - t0) if t1 > t0 else 0.0
                return lerp(keys[i - 1], keys[i], f)
        return keys[-1]
    quats = [q if any(abs(x) > 1e-6 for x in q) else ref_q for q in track.quats]
    q = at(quats, qnlerp, ref_q)
    p = at(track.positions, lambda a, b, f: tuple(x + (y - x) * f for x, y in zip(a, b)), ref_p)
    return q, p


def compose(bones, locals_):
    """Bone to mesh transforms (rotation, translation) from local ones, with the
    non-root conjugation described above."""
    G = []
    for i, ((name, flags, q, pos, nc, parent), (lq, lp)) in enumerate(zip(bones, locals_)):
        lq = lq if i == 0 else qconj(lq)
        if i == 0:
            G.append((lq, lp))
        else:
            pq, pp = G[parent]
            G.append((qmul(pq, lq), tuple(pp[k] + qrot(pq, lp)[k] for k in range(3))))
    return G


def pose_locals(bones, anim, seq_index, frame):
    """Local transform of every mesh bone at a frame of one sequence. Bones are
    matched to tracks by name; a bone the sequence does not animate keeps its
    reference transform."""
    names = [b[0] for b in anim.bones]
    chunk = anim.chunks[seq_index]
    out = []
    for name, flags, q, pos, nc, parent in bones:
        track = None
        if name in names:
            j = names.index(name)
            if chunk.bone_indices:
                if j in chunk.bone_indices:
                    track = chunk.tracks[chunk.bone_indices.index(j)]
            elif j < len(chunk.tracks):
                track = chunk.tracks[j]
        out.append(sample(track, frame, q, pos) if track else (q, pos))
    return out


def skin(mesh, anim, seq_index, frame, lod=0):
    """Mesh space positions of a skeletal mesh's points at a frame. Each point
    is moved by the weighted sum of its bones' change from the reference pose;
    weights are normalised per point."""
    bones, _ = mesh.skeleton()
    pts, _, _ = mesh.geometry(lod)
    ref = compose(bones, [(b[2], b[3]) for b in bones])
    cur = compose(bones, pose_locals(bones, anim, seq_index, frame))
    _, n, s, _, _ = mesh.lods[lod]['influences']
    b = mesh.p.b
    acc = [[0.0, 0.0, 0.0, 0.0] for _ in pts]
    for i in range(n):
        w, pi, bi = struct.unpack_from('<fHH', b, s + 8 * i)
        if pi >= len(pts) or bi >= len(bones) or w <= 0:
            continue
        rq, rp = ref[bi]
        cq, cp = cur[bi]
        local = qrot(qconj(rq), tuple(pts[pi][k] - rp[k] for k in range(3)))
        world = qrot(cq, local)
        a = acc[pi]
        for k in range(3):
            a[k] += w * (world[k] + cp[k])
        a[3] += w
    return [tuple(a[k] / a[3] for k in range(3)) if a[3] > 0 else pts[i]
            for i, a in enumerate(acc)]
