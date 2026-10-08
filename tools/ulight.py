"""Baked lighting of placed static meshes: the StaticMeshInstance records.

Every StaticMeshActor names its own instance in its StaticMeshInstance
property. After an empty property block the record holds:

    index       colour count, one per vertex of the mesh
    per colour  u8 R, G, B, A; A is 255
    u32         revision of the colour stream
    index       light count, then each:
                  index  the light actor
                  index  mask length, then the mask: a bit per vertex, set
                         where the light reaches it; length ceil(verts / 8)
                  u32    applied, 0 or 1

The channel order is R G B A, unlike a texture's RGBA8 pixels, which are
B G R A. It was settled without looking: over the 402 meshes that a single
coloured light reaches, the hue of their colours read as R G B matches the
light's hue in 400.

Usage: ulight.py <map>...        survey every instance against its mesh
"""
import sys, os, struct, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from umap import Map
from udefaults import Tagged


class StaticMeshInstance:
    def __init__(self, pkg, e):
        self.p, self.e, self.name = pkg, e, e['name']
        end = e['off'] + e['size']
        v = Tagged(pkg).parse(Map(pkg).props_start(e), end, want_values=True)
        if not v:
            raise ValueError('%s: no property block' % self.name)
        b, r = pkg.b, R(pkg.b, v[1])
        n = r.idx()
        self.colors = [tuple(b[r.p + 4 * i:r.p + 4 * i + 4]) for i in range(n)]
        r.p += 4 * n
        self.revision = r.u32()
        self.lights = []            # (light actor, mask bytes, applied)
        for _ in range(r.idx()):
            actor = r.idx()
            m = r.idx()
            mask = b[r.p:r.p + m]
            r.p += m
            self.lights.append((actor, mask, r.u32()))
        if r.p != end:
            raise ValueError('%s: instance does not end on its record' % self.name)

    def reaches(self, light, vertex):
        """Whether light number `light` in this instance reaches a vertex."""
        mask = self.lights[light][1]
        return bool(mask[vertex >> 3] >> (vertex & 7) & 1)


def survey(paths):
    from ulevel import MeshLibrary
    c = collections.Counter()
    for f in paths:
        pkg = Package(f)
        mp = Map(pkg)
        root = os.path.dirname(os.path.dirname(os.path.abspath(f)))
        lib = MeshLibrary([os.path.join(root, d) for d in
                           ('StaticMeshes', 'Textures', 'Animations', 'System', 'Maps')])
        for e, d in mp.actors(values=True, live=True):
            smi, sm = d.get('StaticMeshInstance'), d.get('StaticMesh')
            if not smi or 'ref' not in smi or smi['ref'] <= 0:
                continue
            inst = StaticMeshInstance(pkg, pkg.exports[smi['ref'] - 1])
            c['instances'] += 1
            n = len(inst.colors)
            c['masks one bit a vertex'] += all(len(m) == (n + 7) // 8 for _, m, _ in inst.lights)
            c['lights are lights'] += all(a > 0 and 'light' in pkg.classof(pkg.exports[a - 1]).lower()
                                          for a, _, _ in inst.lights)
            if sm and 'ref' in sm:
                key, mesh = lib.resolve(pkg, f, sm['ref'])
                if mesh is not None:
                    c['colours one a mesh vertex' if n == len(mesh.verts)
                      else 'colours stale, mesh changed since'] += 1
    print('%d instances of live actors read to their end' % c['instances'])
    for k in ('masks one bit a vertex', 'lights are lights', 'colours one a mesh vertex',
              'colours stale, mesh changed since'):
        print('  %6d %s' % (c[k], k))


if __name__ == '__main__':
    paths = []
    for a in sys.argv[1:]:
        if os.path.isdir(a):
            paths += [os.path.join(a, f) for f in sorted(os.listdir(a)) if f.lower().endswith('.unr')]
        else:
            paths.append(a)
    survey(paths)
