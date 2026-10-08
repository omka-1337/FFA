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
    i32       NumSharedSides
    i32       NumZones, then that many zones:
                index ZoneActor (its ZoneInfo, or 0), u64 Connectivity,
                u64 Visibility, f32 LastRenderTime
    index     Polys, the editor's polygon object
    index     bounds count, then FBox each (6 floats and a valid byte); a
              node's iRenderBound points here
    index     leaf hull count, then i32 each; a node's iCollisionBound points
              at a list of node indices ending in -1, then the hull's box as
              6 floats; bit 0x40000000 on an index means its plane is flipped
    index     leaf count, then each: index iZone, index iPermeating, index
              iVolumetric, u64 VisibleZones
    index     light count, then index each: lists of Light actors, each list
              ending in 0, that a leaf's iPermeating and iVolumetric start
    u32       RootOutside
    u32       Linked
    ...       lightmaps, not decoded yet; three empty arrays where there are
              none, as in a static mesh's collision model

A node is variable length, because seven of its fields are compact indices:

    FPlane    16 bytes, normal and distance
    u64       zone mask
    u8        node flags
    index x7  iVertPool, iSurf, iBack, iFront, iPlane, iCollisionBound,
              iRenderBound
    FSphere   16 bytes, the node's bounding sphere
    16 bytes  zero in every node seen, meaning unknown
    u8        zone behind the plane
    u8        zone in front of the plane
    u8        vertex count of the node's polygon
    i32 x2    iLeaf behind and in front of the plane, -1 for none
    i32 x3    meaning unknown, not leaves

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
HULL_FLIP = 0x40000000
NODE_INDICES = 7


class Node:
    __slots__ = ('plane', 'zone_mask', 'flags', 'vert_pool', 'surf', 'back',
                 'front', 'plane_index', 'collision_bound', 'render_bound',
                 'sphere', 'num_vertices', 'zone_back', 'zone', 'leaf_back',
                 'leaf_front', 'ints', 'size')


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
        self.zones = []
        self.node_bounds = []
        self.leaf_hulls = []
        self.leaves = []
        self.lights = []
        self.polys = 0
        self.root_outside = self.linked = None
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
        try:
            self.parse_rest(r, end)
        except (struct.error, IndexError, ValueError):
            pass

    def parse_rest(self, r, end):
        """Zones, bounds, leaf hulls, leaves and lights, up to the lightmaps.
        Leaves rest unchanged unless the walk stays inside the record."""
        b = self.p.b
        shared, nz = r.i32(), r.i32()
        if not 0 <= nz <= 64:
            raise ValueError('zone count %d' % nz)
        zones = []
        for _ in range(nz):
            actor = r.idx()
            con, vis, t = struct.unpack_from('<QQf', b, r.p)
            r.p += 20
            zones.append((actor, con, vis, t))
        polys = r.idx()

        def array(size, fmt):
            n = r.idx()
            if n < 0 or r.p + n * size > end:
                raise ValueError('array runs past the record')
            out = [struct.unpack_from(fmt, b, r.p + i * size) for i in range(n)]
            r.p += n * size
            return out
        bounds = array(25, '<6f')
        hulls = [x[0] for x in array(4, '<i')]
        leaves = []
        for _ in range(r.idx()):
            z, perm, vol = r.idx(), r.idx(), r.idx()
            leaves.append((z, perm, vol, struct.unpack_from('<Q', b, r.p)[0]))
            r.p += 8
            if r.p > end:
                raise ValueError('leaves run past the record')
        lights = [r.idx() for _ in range(r.idx())]
        root_outside, linked = r.u32(), r.u32()
        if r.p > end:
            raise ValueError('ran past the record')
        self.shared_sides, self.zones, self.polys = shared, zones, polys
        self.node_bounds, self.leaf_hulls, self.leaves, self.lights = bounds, hulls, leaves, lights
        self.root_outside, self.linked = root_outside, linked
        self.rest = end - r.p

    def hull(self, i):
        """The leaf hull a node's iCollisionBound starts: ((node, flipped)
        pairs, box). Bit 0x40000000 of an entry marks a plane used flipped."""
        h, nodes = self.leaf_hulls, []
        while h[i] != -1:
            nodes.append((h[i] & ~HULL_FLIP, bool(h[i] & HULL_FLIP)))
            i += 1
        box = struct.unpack('<6f', struct.pack('<6i', *h[i + 1:i + 7]))
        return nodes, box

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
        r.p += 16 + 16
        n.zone_back = r.u8()
        # These two are easy to swap, and swapping them still parses. The
        # vertex count is the second: with it no node in the game indexes
        # outside the point array, and the spacing between consecutive vertex
        # pools matches it rather than the first byte.
        n.zone = r.u8()
        n.num_vertices = r.u8()
        n.ints = struct.unpack_from('<5i', b, r.p)
        n.leaf_back, n.leaf_front = n.ints[0], n.ints[1]
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

    def leaf_at(self, p):
        """(leaf, zone) a point falls in: walk from the root as zone_at does
        and take the leaf and zone on the side the walk leaves by. Leaf -1 is
        solid. The side alone does not say so: behind a plane that bounds no
        solid, a sheet or a non solid brush, the walk leaves by the back into
        a leaf like any other."""
        i, nn = 0, len(self.nodes)
        for _ in range(nn + 1):
            n = self.nodes[i]
            front = (n.plane[0] * p[0] + n.plane[1] * p[1] + n.plane[2] * p[2]) >= n.plane[3]
            nxt = n.front if front else n.back
            if not 0 < nxt < nn:
                return (n.leaf_front, n.zone) if front else (n.leaf_back, n.zone_back)
            i = nxt
        raise ValueError('BSP walk did not end')

    def solid_at(self, p):
        return self.leaf_at(p)[0] < 0

    def zone_at(self, p):
        """Zone number of a point: walk from the root, front or back by the
        side of each plane the point is on, until there is no child on that
        side; the zone byte for that side is the answer. A child index of 0
        means none, as the root is nobody's child."""
        i, nn = 0, len(self.nodes)
        for _ in range(nn + 1):
            n = self.nodes[i]
            front = (n.plane[0] * p[0] + n.plane[1] * p[1] + n.plane[2] * p[2]) >= n.plane[3]
            nxt = n.front if front else n.back
            if not 0 < nxt < nn:
                return n.zone if front else n.zone_back
            i = nxt
        raise ValueError('BSP walk did not end')

    def sane(self):
        """Every node must reference the arrays it is supposed to reference,
        and the record must have been read up to its lightmaps."""
        nn = len(self.nodes)
        if self.root_outside is None:
            return False
        nl, nz = len(self.leaves), max(1, len(self.zones))
        if any(l[0] >= nz for l in self.leaves):
            return False
        for n in self.nodes:
            if not (-1 <= n.back < nn and -1 <= n.front < nn):
                return False
            if not (n.render_bound < len(self.node_bounds)
                    and -1 <= n.leaf_back < nl and -1 <= n.leaf_front < nl):
                return False
            if n.collision_bound >= 0:
                try:
                    hull, box = self.hull(n.collision_bound)
                except IndexError:
                    return False
                if not all(0 <= i < nn for i, _ in hull):
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


def zone_check(paths):
    """Compare the zone a BSP walk gives for each actor's location with the
    ZoneNumber the engine stored in the actor's Region when the level was
    saved. Only the level Model is walked, the one no Brush refers to."""
    from umap import Map
    from ulevel import level_model
    total = agree = 0
    misses = collections.Counter()
    for f in paths:
        pkg = Package(f)
        try:
            m = level_model(pkg)
        except ValueError:
            continue
        for e, d in Map(pkg).actors(values=True, live=True):
            reg, loc = d.get('Region'), d.get('Location', {}).get('value')
            if not reg or not loc:
                continue
            v = Tagged(pkg).parse(reg['at'], reg['at'] + reg['size'])
            zn = {x['name']: x for x in v[0]}.get('ZoneNumber') if v else None
            if zn is None:
                continue
            total += 1
            if m.zone_at(loc) == zn['value']:
                agree += 1
            else:
                misses[pkg.classof(e)] += 1
    print('%d of %d actors are in the zone their Region names' % (agree, total))
    for cls, n in misses.most_common(5):
        print('  %5d %s' % (n, cls))


def solid_check(paths):
    """Where each actor stands, solid or empty, by the BSP alone. Brushes and
    volumes are left out: they are not things standing in the level."""
    from umap import Map
    from ulevel import level_model
    total, solid = 0, collections.Counter()
    for f in paths:
        pkg = Package(f)
        try:
            m = level_model(pkg)
        except ValueError:
            continue
        for e, d in Map(pkg).actors(values=True, live=True):
            loc, cls = d.get('Location', {}).get('value'), pkg.classof(e)
            if not loc or 'Volume' in cls or cls in ('Brush', 'LevelInfo'):
                continue
            total += 1
            if m.solid_at(loc):
                solid[cls] += 1
    print('%d of %d actors stand in empty space' % (total - sum(solid.values()), total))
    for cls, n in solid.most_common(8):
        print('  %5d in solid: %s' % (n, cls))


def main(argv):
    zones = '--zones' in argv
    solid = '--solid' in argv
    argv = [a for a in argv if a not in ('--zones', '--solid')]
    paths = []
    for a in argv:
        if os.path.isdir(a):
            paths += [os.path.join(a, f) for f in sorted(os.listdir(a))
                      if f.lower().endswith(('.unr', '.usx'))]
        else:
            paths.append(a)
    if zones:
        return zone_check(paths)
    if solid:
        return solid_check(paths)
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
