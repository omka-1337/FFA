"""Static mesh geometry from UE2 packages (.usx and embedded in maps).

A StaticMesh record is a tagged property block (Materials and so on) followed by
a native payload. The payload was recovered by measurement, not from any
existing implementation:

    FBox        6 floats, then a u8 valid flag        bounding box
    FSphere     4 floats, centre and radius           no valid flag
    index       section count
    sections    14 bytes each: i32, then five u16
    FBox        25 bytes, the render bounding box
    stream      vertices:  24 bytes each, position and normal, then u32 revision
    stream      colours:    4 bytes each, then u32 revision
    stream      alpha:      4 bytes each, then u32 revision
    index       number of UV streams
    stream      each UV:    8 bytes each, two floats, then u32 revision
    u32         one more word after the UV streams
    stream      index buffer:      u16 each, then u32 revision
    stream      wireframe buffer:  u16 each, then u32 revision
    index       CollisionModel, a Model export, or 0
    index       collision triangle count, then each as u16 x3 vertex indices
                and an index, the section its material comes from
    index       collision node count, then 20 bytes each:
                  u16 x4  triangle, coplanar node, front node, back node,
                          65535 for none
                  i16 x6  bounding box, min xyz then max xyz, quantised
    FVector     the quantisation scale: per axis, the largest |coordinate|
                of the collision triangles / 32767
    lazy array  raw triangles: u32 offset of the array's end, a count, then
                each as FVector x3 corners, u32 NumUVs, NumUVs x 3 FVector2D,
                FColor x3, i32 section, u32 smoothing mask
    u32         InternalVersion, 9 in every mesh of the game
    index       KarmaProps, a KMeshProps export, or 0
    u32         0 in every mesh

The collision tree is a BSP over the collision triangles. Each node holds one
triangle; the coplanar node continues a chain of triangles in the same plane,
and every triangle under the front node reaches the front of that plane, every
one under the back node its back, so a triangle crossing the plane is listed on
both sides. A node's box bounds its whole subtree.

The section count was the one real trap: the byte after the bounding sphere
looks like the sphere's valid flag, by analogy with the box, but it is the
section count. Treating it as a flag puts every later field one byte out.

What is decoded is everything a renderer needs: positions, normals, UVs, vertex
colours, the triangle index buffer, and the per section ranges into it. The tail
holds the editor's raw triangles and the collision tree.
"""
import sys, os, struct, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from udefaults import Tagged


class Mesh:
    def __init__(self, pkg, export):
        self.p, self.e = pkg, export
        self.name = export['name']
        self.sections = []
        self.verts = []
        self.uvs = []
        self.colors = []
        self.indices = []
        self.wireframe = 0
        self.tail = 0
        self.collision_model = 0
        self.collision_triangles = []
        self.collision_nodes = []
        self.collision_scale = (0.0, 0.0, 0.0)
        self.raw_triangles = []
        self.internal_version = None
        self.karma_props = 0
        self.parse()

    def parse(self):
        p, e = self.p, self.e
        end = e['off'] + e['size']
        v = Tagged(p).parse(e['off'], end, want_values=False)
        if not v:
            raise ValueError('no property block')
        r = R(p.b, v[1])
        self.bounds = struct.unpack_from('<6f', p.b, r.p)
        r.p += 25
        self.sphere = struct.unpack_from('<4f', p.b, r.p)
        r.p += 16
        for _ in range(r.idx()):
            f = struct.unpack_from('<i5H', p.b, r.p)
            r.p += 14
            self.sections.append(f)
        r.p += 25                                   # render bounding box
        n = r.idx()
        for i in range(n):
            self.verts.append(struct.unpack_from('<6f', p.b, r.p + i * 24))
        r.p += n * 24 + 4
        n = r.idx()
        self.colors = [struct.unpack_from('<I', p.b, r.p + i * 4)[0] for i in range(n)]
        r.p += n * 4 + 4
        n = r.idx()                                 # alpha stream
        r.p += n * 4 + 4
        for s in range(r.idx()):
            n = r.idx()
            uv = [struct.unpack_from('<2f', p.b, r.p + i * 8) for i in range(n)]
            r.p += n * 8 + 4
            self.uvs.append(uv)
        r.p += 4
        n = r.idx()
        self.indices = list(struct.unpack_from('<%dH' % n, p.b, r.p)) if n else []
        r.p += n * 2 + 4
        n = r.idx()
        self.wireframe = n
        r.p += n * 2 + 4
        self.tail = end - r.p
        try:
            self.parse_tail(r, end)
        except (struct.error, IndexError, ValueError):
            pass

    def parse_tail(self, r, end):
        """Collision and the editor's raw triangles. Sets tail to 0 only when
        the walk lands exactly on the end of the record."""
        b = self.p.b
        model = r.idx()
        tris = []
        for _ in range(r.idx()):
            v = struct.unpack_from('<3H', b, r.p)
            r.p += 6
            tris.append(v + (r.idx(),))
        n = r.idx()
        if r.p + 20 * n + 16 > end:
            raise ValueError('collision nodes run past the record')
        nodes = [struct.unpack_from('<4H6h', b, r.p + 20 * i) for i in range(n)]
        r.p += 20 * n
        scale = struct.unpack_from('<3f', b, r.p)
        r.p += 12
        lazy_end = r.u32()
        raw = []
        for _ in range(r.idx()):
            corners = [struct.unpack_from('<3f', b, r.p + 12 * k) for k in range(3)]
            r.p += 36
            nuv = r.u32()
            uv = struct.unpack_from('<%df' % (6 * nuv), b, r.p)
            r.p += 24 * nuv + 12
            section, smoothing = struct.unpack_from('<iI', b, r.p)
            r.p += 8
            raw.append((corners, uv, section, smoothing))
            if r.p > lazy_end:
                raise ValueError('raw triangles run past their lazy array')
        if r.p != lazy_end:
            raise ValueError('raw triangles do not end at their lazy array end')
        version = r.u32()
        karma = r.idx()
        r.u32()
        if r.p != end:
            raise ValueError('tail does not end on the record')
        self.collision_model, self.collision_triangles = model, tris
        self.collision_nodes, self.collision_scale = nodes, scale
        self.raw_triangles, self.internal_version, self.karma_props = raw, version, karma
        self.tail = 0

    def node_box(self, i):
        """A collision node's bounding box in mesh space, (min, max)."""
        n, s = self.collision_nodes[i], self.collision_scale
        return (tuple(n[4 + k] * s[k] for k in range(3)),
                tuple(n[7 + k] * s[k] for k in range(3)))

    def section_ranges(self):
        """(first index, triangle count) per section, in material order. A
        section is i32, then u16 FirstIndex, FirstVertex, LastVertex, a copy of
        the face count, and NumFaces. Empty sections, a material slot with no
        triangles, carry 65535 for both vertex bounds and no faces."""
        return [(s[1], s[5]) for s in self.sections]

    def sane(self):
        """The record read to its exact end, and every index in range."""
        if not self.verts or not self.indices or len(self.indices) % 3:
            return False
        nv, nn = len(self.verts), len(self.collision_nodes)
        return (self.tail == 0 and max(self.indices) < nv
                and all(max(t[:3]) < nv for t in self.collision_triangles)
                and all(n[0] < len(self.collision_triangles)
                        and all(c == 65535 or c < nn for c in n[1:4])
                        for n in self.collision_nodes))

    def __str__(self):
        return ('%-28s %5d verts  %5d tris  %d sections  %d UV sets  '
                '%5d colours  %5d collision tris  %5d nodes  tail %d' %
                (self.name, len(self.verts), len(self.indices) // 3,
                 len(self.sections), len(self.uvs), len(self.colors),
                 len(self.collision_triangles), len(self.collision_nodes), self.tail))


def meshes(path):
    p = Package(path)
    for e in p.exports:
        if p.classof(e) == 'StaticMesh' and e['size']:
            try:
                yield Mesh(p, e)
            except Exception:
                continue


def main(argv):
    paths = []
    for a in argv:
        if os.path.isdir(a):
            paths += [os.path.join(a, f) for f in sorted(os.listdir(a))
                      if f.lower().endswith(('.usx', '.unr'))]
        else:
            paths.append(a)
    total = good = 0
    verts = tris = ctris = nodes = models = 0
    for f in paths:
        shown = 0
        for m in meshes(f):
            total += 1
            if m.sane():
                good += 1
                verts += len(m.verts)
                tris += len(m.indices) // 3
                ctris += len(m.collision_triangles)
                nodes += len(m.collision_nodes)
                models += m.collision_model != 0
            if len(paths) == 1 and shown < 20:
                print('  ' + str(m))
                shown += 1
    print('%d meshes, %d read to their exact end with every index in range, '
          '%d vertices, %d triangles' % (total, good, verts, tris))
    print('collision: %d triangles, %d tree nodes, %d meshes with a collision model'
          % (ctris, nodes, models))


if __name__ == '__main__':
    main(sys.argv[1:])
