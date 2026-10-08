"""Terrain of a UE2 level: TerrainInfo actors and their heightmaps.

A TerrainInfo's tagged properties give its heightmap texture (TerrainMap), the
grid spacing and height scale (TerrainScale), its Location, and two bitmaps:
which quads are visible (holes) and which way each quad is split into triangles.
Its record also carries a native payload after the properties, so it is read up
to the property terminator rather than to the end of the record.

The heightmap is a G16 texture, one unsigned 16 bit height per pixel. Only its
first mip is read here, and only for that format: textures as such are a
separate piece of work, and this minimal reader should give way to it.

The texture's native payload, verified to the byte on the heightmaps seen:

    index   mip count
    per mip:
      u32     skip offset, the absolute file offset just past the data
      index   data length in bytes
      bytes   pixel data
      u32     USize, u32 VSize, u8 UBits, u8 VBits

Heightmap to world, for grid point (x, y) with height h:

    X = Location.X + (x - USize / 2) * TerrainScale.X
    Y = Location.Y + (y - VSize / 2) * TerrainScale.Y
    Z = Location.Z + (h - 32767) * TerrainScale.Z / 256

and the actor's Rotation is ignored.

This was first fitted, by counting how many ground placed static meshes come to
rest on the surface under each candidate. That settled the orientation (no
transpose, no flip), the Z divisor (256 doubles the count against 128) and the
rotation (applying a 15 degree yaw cut the count from 25 percent to 8). It could
not settle a half cell offset; the anchors are too noisy.

It was then proven exactly against the engine's own data. Each TerrainSector
records its quad range and a bounding box the editor computed. Over all 1360
sectors of all 22 terrains, the formula reproduces those boxes to 0.0005 units
horizontally, which fixes the centring at USize / 2, and to 0.0007 vertically.
The vertical match needs a zero of 32767: with 32768 every sector came out lower
by exactly one height step, which is what an off by one looks like and not what
noise looks like.

The native payload after the properties holds the engine's own copy of the
grid, read to the end of all 22 terrains:

    index       sector count, then TerrainSector references
    index       vertex count, then FVector each: the grid in world space
    i32 x2      SectorsX, SectorsY
    index       count (one per vertex), then two FVector normals each, the
                two triangles of the quad whose corner the vertex is; the last
                row and column are unused
    FCoords x2  ToWorld and ToHeightmap: origin, then three axes
    i32 x2      HeightmapX, HeightmapY
    index       vertex count, then the baked light: u8 R, G, B, A per vertex

The stored vertices agree with the formula above at every vertex, a second
proof independent of the sector boxes. The stored normals settle the split of
each quad, which the bitmap alone could not: without the edge turn bit a quad
is cut from (x, y) to (x+1, y+1), with it along the other diagonal. The light's
channel order is R G B, decided like a static mesh's by the sun: on all five
levels with a coloured sun, the light's hue as R G B is the sun's.
"""
import sys, os, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from umap import Map
from udefaults import Tagged

TEXF_G16 = 10
HEIGHT_ZERO = 32767        # proven against every stored sector box, see above


def properties_upto(pkg, e):
    """Tagged properties of an object whose record continues with native data."""
    mp = Map(pkg)
    v = Tagged(pkg).parse(mp.props_start(e), e['off'] + e['size'], want_values=True)
    if not v:
        return None, None
    return {x['name']: x for x in v[0]}, v[1]


def read_heightmap(pkg, e):
    """(USize, VSize, heights) of a G16 texture's first mip."""
    d, at = properties_upto(pkg, e)
    if d is None or d.get('Format', {}).get('value') != TEXF_G16:
        raise ValueError('%s is not a G16 texture' % e['name'])
    r = R(pkg.b, at)
    if r.idx() < 1:
        raise ValueError('no mips')
    skip = r.u32()
    n = r.idx()
    data = pkg.b[r.p:r.p + n]
    if skip != r.p + n:
        raise ValueError('mip skip offset does not match the data length')
    X, Y = d['USize']['value'], d['VSize']['value']
    if n != X * Y * 2:
        raise ValueError('mip size %d does not match %dx%d G16' % (n, X, Y))
    return X, Y, struct.unpack('<%dH' % (X * Y), data)


def bits(entry):
    """A bitmap property: compact count of u32 words, each word 32 quads."""
    raw = entry['value'] if entry else None
    if not isinstance(raw, (bytes, bytearray)):
        return None
    r = R(raw, 0)
    n = r.idx()
    words = struct.unpack_from('<%dI' % n, raw, r.p)
    return words


class Terrain:
    def __init__(self, pkg, e, find_texture):
        d, _ = properties_upto(pkg, e)
        self.name = e['name']
        self.location = d.get('Location', {}).get('value') or (0.0, 0.0, 0.0)
        self.scale = d.get('TerrainScale', {}).get('value') or (64.0, 64.0, 64.0)
        tex_pkg, tex = find_texture(pkg, d['TerrainMap'])
        self.X, self.Y, self.heights = read_heightmap(tex_pkg, tex)
        self.visible = bits(d.get('QuadVisibilityBitmap'))
        self.edge_turn = bits(d.get('EdgeTurnBitmap'))
        self.layers = read_layers(pkg, e)
        self.native = read_native(pkg, e)

    def _bit(self, words, x, y, default):
        if not words:
            return default
        i = x + y * self.X
        return bool(words[i >> 5] >> (i & 31) & 1) if (i >> 5) < len(words) else default

    def vertex(self, x, y):
        sx, sy, sz = self.scale
        L = self.location
        h = self.heights[y * self.X + x]
        return (L[0] + (x - self.X / 2) * sx,
                L[1] + (y - self.Y / 2) * sy,
                L[2] + (h - HEIGHT_ZERO) * sz / 256.0)

    def height_at(self, wx, wy):
        gx = (wx - self.location[0]) / self.scale[0] + self.X / 2
        gy = (wy - self.location[1]) / self.scale[1] + self.Y / 2
        if not (0 <= gx < self.X - 1 and 0 <= gy < self.Y - 1):
            return None
        i, j = int(gx), int(gy)
        fx, fy = gx - i, gy - j
        H = self.heights
        h = (H[j * self.X + i] * (1 - fx) * (1 - fy) + H[j * self.X + i + 1] * fx * (1 - fy)
             + H[(j + 1) * self.X + i] * (1 - fx) * fy + H[(j + 1) * self.X + i + 1] * fx * fy)
        return self.location[2] + (h - HEIGHT_ZERO) * self.scale[2] / 256.0

    def triangles(self):
        """Index triples into the vertex grid, two per visible quad, split along
        the diagonal the edge turn bitmap selects: (x, y) to (x+1, y+1) without
        the bit, the other one with it. The engine's stored face normals are
        those of exactly these triangles."""
        out = []
        for y in range(self.Y - 1):
            for x in range(self.X - 1):
                if not self._bit(self.visible, x, y, True):
                    continue
                a, b = y * self.X + x, y * self.X + x + 1
                c, d = (y + 1) * self.X + x, (y + 1) * self.X + x + 1
                if self._bit(self.edge_turn, x, y, False):
                    out += [(a, b, c), (b, d, c)]
                else:
                    out += [(a, b, d), (a, d, c)]
        return out

    def vertices(self):
        return [self.vertex(x, y) for y in range(self.Y) for x in range(self.X)]

    def weights(self, alpha, aw, ah):
        """A layer's weight at every vertex, 0 to 255, from the alpha channel
        of its RGBA alpha map. A map as large as the heightmap has one texel per
        vertex; a smaller one is spread over the grid."""
        out = []
        for y in range(self.Y):
            ay = y * ah // self.Y
            for x in range(self.X):
                out.append(alpha[(ay * aw + x * aw // self.X) * 4 + 3])
        return out


def read_native(pkg, e):
    """The payload after a TerrainInfo's properties, as a dict, or None when it
    does not read to the end of the record."""
    _, at = properties_upto(pkg, e)
    b, end = pkg.b, e['off'] + e['size']
    r = R(b, at)
    try:
        sectors = [r.idx() for _ in range(r.idx())]
        n = r.idx()
        verts = [struct.unpack_from('<3f', b, r.p + 12 * i) for i in range(n)]
        r.p += 12 * n
        sx, sy = r.i32(), r.i32()
        n = r.idx()
        normals = [struct.unpack_from('<6f', b, r.p + 24 * i) for i in range(n)]
        r.p += 24 * n
        to_world = struct.unpack_from('<12f', b, r.p)
        to_heightmap = struct.unpack_from('<12f', b, r.p + 48)
        r.p += 96
        hx, hy = r.i32(), r.i32()
        n = r.idx()
        light = [tuple(b[r.p + 4 * i:r.p + 4 * i + 4]) for i in range(n)]
        r.p += 4 * n
    except (struct.error, IndexError):
        return None
    if r.p != end:
        return None
    return dict(sectors=sectors, vertices=verts, sectors_x=sx, sectors_y=sy,
                normals=normals, to_world=to_world, to_heightmap=to_heightmap,
                heightmap=(hx, hy), light=light)


def native_check(pkg, t):
    """(worst vertex difference from the formula, worst normal difference from
    the triangles) for one terrain with its native payload read."""
    nat = t.native
    V = t.vertices()
    dv = max(max(abs(p[i] - q[i]) for i in range(3)) for p, q in zip(V, nat['vertices']))

    def normal(a, b, c):
        u = [V[b][i] - V[a][i] for i in range(3)]
        v = [V[c][i] - V[a][i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        L = sum(x * x for x in n) ** 0.5 or 1.0
        return [x / L for x in n]
    dn = 0.0
    for y in range(t.Y - 1):
        for x in range(t.X - 1):
            a = y * t.X + x
            b, c, d = a + 1, a + t.X, a + t.X + 1
            if t._bit(t.edge_turn, x, y, False):
                pair = normal(a, b, c) + normal(b, d, c)
            else:
                pair = normal(a, b, d) + normal(a, d, c)
            dn = max(dn, max(abs(p - q) for p, q in zip(pair, nat['normals'][a])))
    return dv, dn


def read_layers(pkg, e):
    """The terrain's texture layers, in drawing order. Layers is a fixed array
    of TerrainLayer structs, each a tagged list of its own, and inside it
    TerrainMatrix is a Matrix of four tagged Planes. The matrix is the editor's
    own world to texture transform, used as a row vector: u = [x y z 1] times
    its first column, v times its second. On all 70 layers with a texture it is
    1 / (TerrainScale * UScale) on the axes TextureMapAxis picks, one repeat
    every UScale quads. Three layers have an all zero matrix and no texture to
    speak of; they are left out."""
    tag = Tagged(pkg)

    def fields(at, size):
        v = tag.parse(at, at + size, want_values=True)
        if not v or v[1] != at + size:
            raise ValueError('struct does not end on its size')
        return {x['name']: x for x in v[0]}

    v = tag.parse(Map(pkg).props_start(e), e['off'] + e['size'], want_values=True)
    out = []
    for entry in sorted((x for x in v[0] if x['name'] == 'Layers'), key=lambda x: x['index']):
        lay = fields(entry['at'], entry['size'])
        tm = lay.get('TerrainMatrix')
        if 'Texture' not in lay or not tm:
            continue
        rows = {k: fields(x['at'], x['size']) for k, x in fields(tm['at'], tm['size']).items()}
        get = lambda row, c: rows.get(row, {}).get(c, {}).get('value', 0.0)
        planes = ('XPlane', 'YPlane', 'ZPlane', 'WPlane')
        u = tuple(get(r, 'X') for r in planes)
        w = tuple(get(r, 'Y') for r in planes)
        if not any(u[:3]) and not any(w[:3]):
            continue
        out.append(dict(index=entry['index'], texture=lay['Texture'].get('ref', 0),
                        alpha=lay.get('AlphaMap', {}).get('ref', 0), u=u, v=w))
    return out


def find_texture_in(pkg, entry, files=None):
    """A heightmap is usually a local export of the map; otherwise it is an
    import, found by its top level package name among the game's files (a dict
    of lower case file stem to path, as MeshLibrary.files holds)."""
    ref = entry.get('ref', 0)
    if ref > 0:
        return pkg, pkg.exports[ref - 1]
    if ref < 0 and files:
        from ulevel import import_path
        chain = import_path(pkg, ref)
        path = files.get(chain[0].lower())
        if path:
            tp = Package(path)
            for e in tp.exports:
                if e['name'].lower() == chain[-1].lower() and tp.classof(e) == 'Texture':
                    return tp, e
    raise ValueError('heightmap %s not found' % entry.get('value'))


def terrains(pkg, files=None):
    find = lambda p, entry: find_texture_in(p, entry, files)
    for e in pkg.exports:
        if pkg.classof(e) == 'TerrainInfo' and e['size']:
            yield Terrain(pkg, e, find)


def sector_check(pkg, terrain_map):
    """Worst horizontal and vertical disagreement between the formula and the
    bounding boxes the engine stored in this map's TerrainSectors."""
    worst_xy = worst_z = 0.0
    n = 0
    for e in pkg.exports:
        if pkg.classof(e) != 'TerrainSector' or not e['size']:
            continue
        r = R(pkg.b, e['off'] + 1)            # empty property block: one None
        info = r.idx()
        qx, qy, ox, oy = (r.u32() for _ in range(4))
        box = struct.unpack_from('<6f', pkg.b, r.p)
        t = terrain_map.get(pkg.refname(info))
        if t is None:
            continue
        vs = [t.vertex(x, y) for y in range(oy, oy + qy + 1)
              for x in range(ox, ox + qx + 1)]
        lo = [min(v[i] for v in vs) for i in range(3)]
        hi = [max(v[i] for v in vs) for i in range(3)]
        worst_xy = max(worst_xy, *(abs(lo[i] - box[i]) for i in (0, 1)),
                       *(abs(hi[i] - box[3 + i]) for i in (0, 1)))
        worst_z = max(worst_z, abs(lo[2] - box[2]), abs(hi[2] - box[5]))
        n += 1
    return n, worst_xy, worst_z


def main(argv):
    p = Package(argv[0])
    files = None
    if len(argv) > 1:
        from ulevel import MeshLibrary
        files = MeshLibrary(argv[1:]).files
    T = {t.name: t for t in terrains(p, files)}
    for t in T.values():
        print('%s: %dx%d heightmap, scale %s, %d triangles'
              % (t.name, t.X, t.Y, t.scale, len(t.triangles())))
    n, xy, z = sector_check(p, T)
    print('%d sectors checked against their stored boxes: worst %.4f horizontally, '
          '%.4f vertically' % (n, xy, z))
    for t in T.values():
        if t.native is None:
            print('%s: native payload not read to its end' % t.name)
            continue
        dv, dn = native_check(p, t)
        print('%s: payload read to its end; stored vertices within %.4f of the formula, '
              'stored normals within %.4f of the triangles' % (t.name, dv, dn))


if __name__ == '__main__':
    main(sys.argv[1:])
