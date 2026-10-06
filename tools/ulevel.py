"""Assemble a level's geometry from the readers: BSP polygons for now.

A map holds one Model per brush plus one for the level itself. The brush models
are the shapes the editor's CSG pass started from, including subtractive volumes,
so drawing all of them overlays building blocks on the result. The level's own
Model is the one no actor points at through its Brush property; in every map
checked there is exactly one such Model.

A polygon is a node's run of verts: node.vert_pool to vert_pool + num_vertices,
each vert pointing into the model's Points. The surface behind the node gives
the material.
"""
import sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package
from umap import Map
from ubsp import Model

# Surface flags that mean "not drawn".
PF_INVISIBLE = 0x00000001
PF_PORTAL = 0x04000000


def level_model(pkg):
    """The Model that no Brush property refers to."""
    mp = Map(pkg)
    referenced = set()
    for e, d in mp.actors(values=True):
        if 'Brush' in d:
            referenced.add(d['Brush']['value'])
    candidates = [e for e in pkg.exports
                  if pkg.classof(e) == 'Model' and e['size']
                  and e['name'] not in referenced]
    if len(candidates) != 1:
        raise ValueError('expected one level model, found %d' % len(candidates))
    return Model(pkg, candidates[0])


def polygons(model, skip_hidden=True):
    """Yield (vertices, material name, surface flags, plane normal) for every
    BSP polygon. The node's plane normal points into playable space, which is
    the side the polygon is meant to be seen from."""
    pkg = model.p
    for n in model.nodes:
        if n.num_vertices < 3:
            continue
        s = model.surfs[n.surf] if 0 <= n.surf < len(model.surfs) else None
        flags = s.flags if s else 0
        if skip_hidden and flags & (PF_INVISIBLE | PF_PORTAL):
            continue
        pts = [model.points[pv] for pv, _ in
               model.verts[n.vert_pool:n.vert_pool + n.num_vertices]]
        yield pts, (pkg.refname(s.material) if s else 'None'), flags, n.plane[:3]


def main(argv):
    p = Package(argv[0])
    m = level_model(p)
    polys = list(polygons(m))
    mats = collections.Counter(p[1] for p in polys)
    print('%s: level model %s, %d polygons, %d triangles' %
          (os.path.basename(argv[0]), m.e['name'], len(polys),
           sum(len(p[0]) - 2 for p in polys)))
    print('materials:', dict(mats.most_common(8)))


if __name__ == '__main__':
    main(sys.argv[1:])


# --------------------------------------------------------------- static meshes
import math, glob
from umesh import Mesh

TWO_PI_OVER = 2 * math.pi / 65536.0      # Unreal rotation units: 65536 = 360 degrees


def import_path(pkg, ref):
    """Names along an import's outer chain, top level package first."""
    chain, o = [], ref
    while o < 0:
        imp = pkg.imports[-o - 1]
        chain.append(imp['name'])
        o = imp['pkg']
    return list(reversed(chain))


class MeshLibrary:
    """Finds and caches StaticMesh objects across the game's .usx packages."""

    def __init__(self, dirs):
        self.index, self.pkgs, self.meshes = {}, {}, {}
        for d in dirs:
            for f in glob.glob(os.path.join(d, '*.usx')):
                top = os.path.splitext(os.path.basename(f))[0].lower()
                p = self.package(f)
                for i, e in enumerate(p.exports):
                    if p.classof(e) == 'StaticMesh':
                        self.index.setdefault((top, e['name'].lower()), (f, i + 1))

    def package(self, path):
        if path not in self.pkgs:
            self.pkgs[path] = Package(path)
        return self.pkgs[path]

    def mesh(self, path, idx):
        key = (path, idx)
        if key not in self.meshes:
            p = self.package(path)
            try:
                self.meshes[key] = Mesh(p, p.exports[idx - 1])
            except Exception:
                self.meshes[key] = None
        return self.meshes[key]

    def resolve(self, pkg, path, ref):
        """(key, Mesh) for a reference held by `pkg` (loaded from `path`)."""
        if ref > 0:
            e = pkg.exports[ref - 1]
            if pkg.classof(e) != 'StaticMesh':
                return None, None
            self.pkgs.setdefault(path, pkg)
            return (path, ref), self.mesh(path, ref)
        if ref < 0:
            chain = import_path(pkg, ref)
            hit = self.index.get((chain[0].lower(), chain[-1].lower()))
            if hit:
                return hit, self.mesh(*hit)
        return None, None


def rotation_axes(pitch, yaw, roll):
    """Images of the local X, Y and Z axes, as Unreal's FRotationMatrix builds
    them: roll about X, then pitch about Y, then yaw about Z."""
    P, Y, Rl = pitch * TWO_PI_OVER, yaw * TWO_PI_OVER, roll * TWO_PI_OVER
    SP, CP, SY, CY, SR, CR = (math.sin(P), math.cos(P), math.sin(Y),
                              math.cos(Y), math.sin(Rl), math.cos(Rl))
    X = (CP * CY, CP * SY, SP)
    Yax = (SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP)
    Z = (-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP)
    return X, Yax, Z


def actor_matrix(d):
    """3x3 linear part (as three column vectors) and translation, in Unreal
    space, for an actor's property dict."""
    loc = d.get('Location', {}).get('value') or (0.0, 0.0, 0.0)
    rot = d.get('Rotation', {}).get('value') or (0, 0, 0)
    s = d.get('DrawScale', {}).get('value')
    s = 1.0 if s is None else s
    s3 = d.get('DrawScale3D', {}).get('value') or (1.0, 1.0, 1.0)
    X, Y, Z = rotation_axes(*rot)
    cols = (tuple(v * s * s3[0] for v in X), tuple(v * s * s3[1] for v in Y),
            tuple(v * s * s3[2] for v in Z))
    return cols, loc


def static_mesh_instances(pkg, path, lib):
    """Yield (mesh key, Mesh, columns, location, actor name) for every actor
    that carries its own StaticMesh reference."""
    mp = Map(pkg)
    for e, d in mp.actors(values=True):
        sm = d.get('StaticMesh')
        if not sm or 'ref' not in sm:
            continue
        if d.get('bHidden', {}).get('value'):
            continue
        key, mesh = lib.resolve(pkg, path, sm['ref'])
        if mesh is None:
            continue
        cols, loc = actor_matrix(d)
        yield key, mesh, cols, loc, e['name']
