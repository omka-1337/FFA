"""BSP data of a UE2 level: the Model record.

A Model holds the level's brush geometry. Its record is a tagged property block,
then the UPrimitive prefix shared with static meshes, then the BSP arrays:

    FBox      6 floats and a u8 valid flag
    FSphere   4 floats
    index     vector count, then that many FVector  (plane normals)
    index     point count,  then that many FVector  (brush corners)
    index     node count,   then that many FBspNode
    index     surf count,   then that many FBspSurf
    index     vert count,   then that many FVert: two compact indices
    ...       zones, lightmaps, bounds and leaves, not decoded yet

A node is variable length, because seven of its fields are compact indices:

    FPlane    16 bytes, normal and distance
    u64       zone mask
    u8        node flags
    index x7  iVertPool, iSurf, iBack, iFront, iPlane, iCollisionBound,
              iRenderBound
    FSphere   16 bytes, the node's bounding sphere
    17 bytes  zero in every node seen, meaning unknown
    u8        zone
    u8        vertex count of the node's polygon
    i32 x5    typically -1, -1, 0, leaf, -1

The layout was not guessed. Node starts are findable independently, because a
node begins with a unit length plane normal followed by a small zone mask, and
scanning for that pattern finds exactly as many nodes as the array declares.
That gives every node's exact length, and a search over "how many compact
indices, how long a fixed tail" then has a single answer that fits all of them:
seven and fifty five.
"""
import sys, os, struct, math, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from udefaults import Tagged

NODE_TAIL = 55
NODE_INDICES = 7


class Node:
    __slots__ = ('plane', 'zone_mask', 'flags', 'vert_pool', 'surf', 'back',
                 'front', 'plane_index', 'collision_bound', 'render_bound',
                 'sphere', 'num_vertices', 'zone', 'ints', 'size')


class Surf:
    __slots__ = ('material', 'flags', 'base', 'normal', 'texture_u', 'texture_v',
                 'light_map', 'brush_poly', 'plane', 'light_map_scale')


class Model:
    def __init__(self, pkg, export):
        self.p, self.e = pkg, export
        self.vectors = []
        self.points = []
        self.nodes = []
        self.surfs = []
        self.verts = []
        self.rest = 0
        self.parse()

    def parse(self):
        p, e = self.p, self.e
        end = e['off'] + e['size']
        v = Tagged(p).parse(e['off'], end, want_values=False)
        if not v:
            raise ValueError('no property block')
        b = p.b
        r = R(b, v[1])
        self.bounds = struct.unpack_from('<6f', b, r.p)
        r.p += 25
        self.sphere = struct.unpack_from('<4f', b, r.p)
        r.p += 16
        n = r.idx()
        self.vectors = [struct.unpack_from('<3f', b, r.p + i * 12) for i in range(n)]
        r.p += n * 12
        n = r.idx()
        self.points = [struct.unpack_from('<3f', b, r.p + i * 12) for i in range(n)]
        r.p += n * 12
        for _ in range(r.idx()):
            self.nodes.append(self.node(r))
        for _ in range(r.idx()):
            self.surfs.append(self.surf(r))
        for _ in range(r.idx()):
            self.verts.append((r.idx(), r.idx()))
        self.rest = end - r.p

    def node(self, r):
        b = self.p.b
        n = Node()
        n.size = r.p
        n.plane = struct.unpack_from('<4f', b, r.p)
        r.p += 16
        n.zone_mask = struct.unpack_from('<Q', b, r.p)[0]
        r.p += 8
        n.flags = r.u8()
        (n.vert_pool, n.surf, n.back, n.front, n.plane_index,
         n.collision_bound, n.render_bound) = [r.idx() for _ in range(NODE_INDICES)]
        n.sphere = struct.unpack_from('<4f', b, r.p)
        r.p += 16 + 17
        # These two are easy to swap, and swapping them still parses. The
        # vertex count is the second: with it no node in the game indexes
        # outside the point array, and the spacing between consecutive vertex
        # pools matches it rather than the first byte.
        n.zone = r.u8()
        n.num_vertices = r.u8()
        n.ints = struct.unpack_from('<5i', b, r.p)
        r.p += 20
        n.size = r.p - n.size
        return n

    def surf(self, r):
        """Material reference, flags, six indices into the vector and point
        arrays, the surface plane, and the lightmap scale. That last float is
        32, the UE2 default, on 15355 of the game's 15361 surfaces and 16 on
        the rest. There is no texture panning field: panning is folded into
        the base point."""
        b = self.p.b
        s = Surf()
        s.material = r.idx()
        s.flags = r.u32()
        (s.base, s.normal, s.texture_u, s.texture_v,
         s.light_map, s.brush_poly) = [r.idx() for _ in range(6)]
        s.plane = struct.unpack_from('<4f', b, r.p)
        r.p += 16
        s.light_map_scale = struct.unpack_from('<f', b, r.p)[0]
        r.p += 4
        return s

    def sane(self):
        """Every node must reference the arrays it is supposed to reference."""
        nn = len(self.nodes)
        for n in self.nodes:
            if not (-1 <= n.back < nn and -1 <= n.front < nn):
                return False
            if n.plane_index < -1 or n.plane_index >= nn:
                return False
            L = math.sqrt(sum(x * x for x in n.plane[:3]))
            if abs(L - 1.0) > 1e-4:
                return False
        nv, np = len(self.vectors), len(self.points)
        for s in self.surfs:
            if not (0 <= s.base < np) and np:
                return False
            if nv and not all(0 <= i < nv for i in
                              (s.normal, s.texture_u, s.texture_v)):
                return False
            L = math.sqrt(sum(x * x for x in s.plane[:3]))
            if abs(L - 1.0) > 1e-4:
                return False
        # Only the vertices a node actually points at have to be valid. The
        # pool also holds entries no node references, left over from editing,
        # and those carry stale indices.
        for n in self.nodes:
            for pv, _ in self.verts[n.vert_pool:n.vert_pool + n.num_vertices]:
                if not 0 <= pv < np:
                    return False
        return True


def models(path):
    p = Package(path)
    for e in p.exports:
        if p.classof(e) == 'Model' and e['size']:
            try:
                yield Model(p, e)
            except Exception:
                continue


def main(argv):
    paths = []
    for a in argv:
        if os.path.isdir(a):
            paths += [os.path.join(a, f) for f in sorted(os.listdir(a))
                      if f.lower().endswith('.unr')]
        else:
            paths.append(a)
    total = good = nodes = surfs = verts = 0
    for f in paths:
        for m in models(f):
            total += 1
            nodes += len(m.nodes)
            surfs += len(m.surfs)
            verts += len(m.verts)
            if m.sane():
                good += 1
            if len(paths) == 1:
                print('  %-16s %5d vectors %6d points %6d nodes %5d surfs '
                      '%6d verts  rest %d bytes'
                      % (m.e['name'], len(m.vectors), len(m.points),
                         len(m.nodes), len(m.surfs), len(m.verts), m.rest))
    print('%d models, %d pass the reference checks, %d nodes, %d surfaces, '
          '%d verts' % (total, good, nodes, surfs, verts))


if __name__ == '__main__':
    main(sys.argv[1:])
