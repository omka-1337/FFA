"""Polys, the editor's polygons of a brush, from UE2 packages.

A brush's Model is an empty BSP until it is built into the level; its shape is
in the Polys export its Model points at. The record is an empty property
block, then:

    i32       Num, the polygon count
    i32       Max, the same again
    each polygon:
      index     vertex count
      FVector   Base, Normal, TextureU, TextureV
      FVector   that many vertices, in the brush's own space
      u32       PolyFlags
      index     Actor, the brush
      index     Material
      index     ItemName, a name
      index     iLink
      index     iBrushPoly
      f32       LightMapScale

Read on every Polys export of the game's maps, the layout lands on the end of
every record; see `--check`.
"""
import sys, os, struct, glob

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from udefaults import Tagged


def read_polys(pkg, e):
    """The polygons of a Polys export, as dicts, or None when the record does
    not read to its exact end."""
    end = e['off'] + e['size']
    v = Tagged(pkg).parse(e['off'], end, want_values=False)
    if not v:
        return None
    b = pkg.b
    r = R(b, v[1])
    try:
        num, cap = r.i32(), r.i32()
        if num != cap or num < 0 or num * 50 > end - r.p:
            return None
        out = []
        for _ in range(num):
            nv = r.idx()
            if not 3 <= nv <= 64:
                return None
            base, normal, tu, tv = (struct.unpack_from('<3f', b, r.p + 12 * k) for k in range(4))
            r.p += 48
            verts = [struct.unpack_from('<3f', b, r.p + 12 * k) for k in range(nv)]
            r.p += 12 * nv
            flags = r.u32()
            actor, material, item, link, brush_poly = (r.idx() for _ in range(5))
            scale = struct.unpack_from('<f', b, r.p)[0]
            r.p += 4
            out.append(dict(base=base, normal=normal, texture_u=tu, texture_v=tv, vertices=verts,
                            flags=flags, actor=actor, material=material, item=item, link=link,
                            brush_poly=brush_poly, light_map_scale=scale))
    except (struct.error, IndexError):
        return None
    return out if r.p == end else None


def check(paths):
    total = ok = polys = unit = 0
    for f in paths:
        p = Package(f)
        for e in p.exports:
            if p.classof(e) != 'Polys' or not e['size']:
                continue
            total += 1
            got = read_polys(p, e)
            if got is None:
                continue
            ok += 1
            polys += len(got)
            unit += sum(abs(sum(x * x for x in q['normal']) - 1) < 1e-3 for q in got)
    print('%d of %d Polys read to the end, %d polygons, %d with a unit normal' % (ok, total, polys, unit))


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--check':
        check(args[1:])
    else:
        print(__doc__)
