"""BSP data of a UE2 level: the Model record.

A Model holds the level's brush geometry. Its record is a tagged property block,
then the UPrimitive prefix shared with static meshes, then the BSP arrays:

    FBox      6 floats and a u8 valid flag
    FSphere   4 floats
    index     vector count, then that many FVector  (plane normals)
    index     point count,  then that many FVector  (brush corners)
    index     node count,   then that many FBspNode
    ...       surfaces, verts and the rest, not decoded yet

A node is variable length, because seven of its fields are compact indices:

    FPlane    16 bytes, normal and distance
    u64       zone mask
    u8        node flags
    index x7  iVertPool, iSurf, iBack, iFront, iPlane, iCollisionBound,
              iRenderBound
    FSphere   16 bytes, the node's bounding sphere
    17 bytes  zero in every node seen, meaning unknown
    u8        vertex count of the node's polygon
    u8        zone
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


class Model:
    def __init__(self, pkg, export):
        self.p, self.e = pkg, export
        self.vectors = []
        self.points = []
        self.nodes = []
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
        n.num_vertices = r.u8()
        n.zone = r.u8()
        n.ints = struct.unpack_from('<5i', b, r.p)
        r.p += 20
        n.size = r.p - n.size
        return n

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
    total = good = nodes = 0
    for f in paths:
        for m in models(f):
            total += 1
            nodes += len(m.nodes)
            if m.sane():
                good += 1
            if len(paths) == 1:
                print('  %-16s %5d vectors %6d points %6d nodes  rest %d bytes'
                      % (m.e['name'], len(m.vectors), len(m.points),
                         len(m.nodes), m.rest))
    print('%d models, %d pass the reference checks, %d BSP nodes'
          % (total, good, nodes))


if __name__ == '__main__':
    main(sys.argv[1:])
