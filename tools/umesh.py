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
    ...         raw triangles and collision data, not decoded

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

    def sane(self):
        """Consistency checks that do not depend on the undecoded tail."""
        if not self.verts or not self.indices:
            return False
        if len(self.indices) % 3:
            return False
        return max(self.indices) < len(self.verts) and self.tail >= 0

    def __str__(self):
        return ('%-28s %5d verts  %5d tris  %d sections  %d UV sets  '
                '%5d colours  tail %d' %
                (self.name, len(self.verts), len(self.indices) // 3,
                 len(self.sections), len(self.uvs), len(self.colors), self.tail))


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
    verts = tris = 0
    for f in paths:
        shown = 0
        for m in meshes(f):
            total += 1
            if m.sane():
                good += 1
                verts += len(m.verts)
                tris += len(m.indices) // 3
            if len(paths) == 1 and shown < 20:
                print('  ' + str(m))
                shown += 1
    print('%d meshes, %d pass the consistency checks, %d vertices, %d triangles'
          % (total, good, verts, tris))


if __name__ == '__main__':
    main(sys.argv[1:])
