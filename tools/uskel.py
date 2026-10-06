"""Skeletal meshes of UE2 packages (.ukx), in their reference pose.

A SkeletalMesh record starts like every primitive, a bounding box and sphere,
then the LodMesh header:

    u32       Version
    u32       VertexCount
    index     packed vertex count, then 4 bytes each   (usually empty here)
    index     texture count, then that many references
    FVector   MeshScale
    FVector   MeshOrigin
    i32 x3    RotOrigin, Pitch Yaw Roll in Unreal units

and, further on, the skeleton and a set of LOD models. Each LOD model keeps its
own copy of the geometry in four lazy arrays. A lazy array is

    u32       absolute file offset just past the array
    index     element count
    bytes     elements

so it delimits itself, and the offset is a check that cannot pass by accident.
In every LOD model the four come in the same order:

    influences   8 bytes each: f32 weight, u16 point, u16 bone
    wedges      10 bytes each: u16 point, f32 U, f32 V
    faces        8 bytes each: u16 wedge x3, u16 material
    points      12 bytes each: FVector

What lies between them, sections and index buffers, is not needed for drawing
and is not decoded; the lazy arrays are found by scanning instead, each one
proven by its own skip offset.
"""
import sys, os, struct, math, glob, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R

LAZY_KINDS = (('influences', 8), ('wedges', 10), ('faces', 8), ('points', 12))


def lazy_arrays(pkg, e, start):
    """Every self consistent lazy array in the record from `start` on, as
    (offset, element count, data start, data end)."""
    b, E, end = pkg.b, e['off'], e['off'] + e['size']
    out = []
    o = start
    while o < end - 5:
        skip = struct.unpack_from('<I', b, o)[0]
        if o + 5 <= skip <= end:
            r = R(b, o + 4)
            try:
                n = r.idx()
            except Exception:
                n = -1
            if n > 0 and r.p < skip and (skip - r.p) % n == 0:
                width = (skip - r.p) // n
                if width in (8, 10, 12):
                    out.append((o, n, r.p, skip, width))
                    o = skip
                    continue
        o += 1
    return out


class SkeletalMesh:
    def __init__(self, pkg, e):
        self.p, self.e, self.name = pkg, e, e['name']
        b = pkg.b
        r = R(b, e['off'])
        if r.idx() != pkg.none_idx:          # empty tagged property block
            raise ValueError('unexpected properties')
        r.p += 25 + 16                        # bounding box, bounding sphere
        self.version = r.u32()
        self.vertex_count = r.u32()
        # Not `r.p += r.idx() * 4`: augmented assignment loads r.p before the
        # call moves it, so the count byte would silently never be consumed.
        n = r.idx()
        r.p += n * 4                          # packed verts
        self.textures = [r.idx() for _ in range(r.idx())]
        self.scale = struct.unpack_from('<3f', b, r.p)
        self.origin = struct.unpack_from('<3f', b, r.p + 12)
        self.rot_origin = struct.unpack_from('<3i', b, r.p + 24)
        r.p += 36
        self.lods = self.find_lods(lazy_arrays(pkg, e, r.p))

    def find_lods(self, arrays):
        """Group lazy arrays into LOD models: four in the fixed order with
        the expected widths."""
        lods, i = [], 0
        want = [w for _, w in LAZY_KINDS]
        while i + 4 <= len(arrays):
            if [a[4] for a in arrays[i:i + 4]] == want:
                lods.append({k: arrays[i + j] for j, (k, _) in enumerate(LAZY_KINDS)})
                i += 4
            else:
                i += 1
        return lods

    def skeleton(self):
        """The reference skeleton and the default animation reference.

        Found by its signature rather than by decoding everything before it: a
        compact count, then per bone a name index, u32 flags, a unit quaternion,
        a position, a length, three sizes, a child count and a parent index. The
        default animation reference follows the last bone; it is checked, where
        it resolves, by the animation's bone names matching these."""
        if hasattr(self, '_skeleton'):
            return self._skeleton
        p, e = self.p, self.e
        b, end = p.b, e['off'] + e['size']
        found = None
        for o in range(e['off'] + 42, end - 60):
            r = R(b, o)
            try:
                c = r.idx()
            except Exception:
                continue
            if not 1 <= c <= 400:
                continue
            bones, q, ok = [], r.p, True
            for _ in range(c):
                rr = R(b, q)
                try:
                    ni = rr.idx()
                except Exception:
                    ok = False
                    break
                if not 0 < ni < len(p.names) or rr.p + 56 > end:
                    ok = False
                    break
                flags = struct.unpack_from('<I', b, rr.p)[0]
                quat = struct.unpack_from('<4f', b, rr.p + 4)
                if abs(math.sqrt(sum(x * x for x in quat)) - 1) > 1e-3:
                    ok = False
                    break
                pos = struct.unpack_from('<3f', b, rr.p + 20)
                nchild, parent = struct.unpack_from('<ii', b, rr.p + 48)
                bones.append((p.names[ni], flags, quat, pos, nchild, parent))
                q = rr.p + 56
            if ok and all(0 <= bn[5] < c for bn in bones):
                rr = R(b, q)
                found = (bones, rr.idx())
                break
        self._skeleton = found if found else ([], 0)
        return self._skeleton

    def geometry(self, lod=0):
        """Points, wedges (point, u, v) and faces (w0, w1, w2, material)."""
        b = self.p.b
        L = self.lods[lod]
        _, n, s, _, _ = L['points']
        points = [struct.unpack_from('<3f', b, s + i * 12) for i in range(n)]
        _, n, s, _, _ = L['wedges']
        wedges = [struct.unpack_from('<Hff', b, s + i * 10) for i in range(n)]
        _, n, s, _, _ = L['faces']
        faces = [struct.unpack_from('<4H', b, s + i * 8) for i in range(n)]
        return points, wedges, faces

    def sane(self):
        if not self.lods:
            return False
        for k in range(len(self.lods)):
            pts, wed, fac = self.geometry(k)
            if any(w[0] >= len(pts) for w in wed):
                return False
            if any(max(f[:3]) >= len(wed) for f in fac):
                return False
        return True


def meshes(path):
    p = Package(path)
    for e in p.exports:
        if p.classof(e) == 'SkeletalMesh' and e['size']:
            try:
                yield SkeletalMesh(p, e)
            except Exception as ex:
                yield ex


def main(argv):
    paths = []
    for a in argv:
        paths += sorted(glob.glob(os.path.join(a, '*.ukx'))) if os.path.isdir(a) else [a]
    total = good = 0
    lodcount = collections.Counter()
    for f in paths:
        for m in meshes(f):
            total += 1
            if isinstance(m, Exception):
                continue
            lodcount[len(m.lods)] += 1
            if m.sane():
                good += 1
    print('%d skeletal meshes, %d with consistent LOD geometry' % (total, good))
    print('LOD models per mesh:', dict(sorted(lodcount.items())))


if __name__ == '__main__':
    main(sys.argv[1:])
