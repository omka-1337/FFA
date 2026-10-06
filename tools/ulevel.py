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
    """Finds and caches StaticMesh objects anywhere in a game's packages.

    A package is named by its file's stem, whatever the extension: Unreal does
    not tie object types to file types, and Shrek 2 keeps some static meshes in
    texture packages (the beanstalk bonus maps import
    Beanstalk.StaticEnviroment.fence_1 from Textures/Beanstalk.utx). So every
    package file in the given directories is a candidate, and each is opened
    only when an import actually names it.
    """
    EXTENSIONS = ('.u', '.usx', '.utx', '.ukx', '.uax', '.umx', '.unr')

    def __init__(self, dirs):
        self.files, self.pkgs, self.meshes, self.byname = {}, {}, {}, {}
        for d in dirs:
            if not os.path.isdir(d):
                continue
            for f in sorted(os.listdir(d)):
                stem, ext = os.path.splitext(f)
                if ext.lower() in self.EXTENSIONS:
                    self.files.setdefault(stem.lower(), os.path.join(d, f))

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

    def find(self, path, chain):
        """Export index of the StaticMesh named by an import chain, matching
        the group names too when the same object name occurs twice."""
        if path not in self.byname:
            p = self.package(path)
            names = collections.defaultdict(list)
            for i, e in enumerate(p.exports):
                if p.classof(e) == 'StaticMesh':
                    names[e['name'].lower()].append(i + 1)
            self.byname[path] = names
        hits = self.byname[path].get(chain[-1].lower(), [])
        if len(hits) > 1 and len(chain) > 2:
            p = self.package(path)
            for i in hits:
                if p.refname(p.exports[i - 1]['pkg']).lower() == chain[-2].lower():
                    return i
        return hits[0] if hits else None

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
            fpath = self.files.get(chain[0].lower())
            if fpath:
                idx = self.find(fpath, chain)
                if idx:
                    return (fpath, idx), self.mesh(fpath, idx)
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


class ClassDefaults:
    """Effective default properties of classes, merged along the inheritance
    chain. A class's defaults block stores only what differs from its parent,
    so a value is only known after walking from the root down to the class."""

    def __init__(self, system_dir):
        from udefaults import World
        self.dir = system_dir
        self.world = World(glob.glob(os.path.join(system_dir, '*.u')))
        self.cache = {}
        self.drawtypes = []
        if 'EDrawType' in self.world.enums:
            k, i = self.world.enums['EDrawType']
            self.drawtypes = self.world.readers[k].enum_values(i)

    def effective(self, cls):
        if cls in self.cache:
            return self.cache[cls]
        w, chain, name = self.world, [], cls
        while name in w.classes and len(chain) < 50:
            key, idx = w.classes[name]
            chain.append((key, idx))
            sup = w.readers[key].field(idx)['super']
            name = w.pkgs[key].refname(sup) if sup else None
        out = {}
        for key, idx in reversed(chain):            # root first, class last
            best = w.defaults(key, idx)
            for x in (best[0] if best else []):
                if x['index'] == 0:
                    out[x['name']] = dict(x, _pkg=key)
        self.cache[cls] = out
        return out

    def merged(self, cls, props):
        """Class defaults overlaid with an actor's own properties."""
        out = dict(self.effective(cls))
        out.update(props)
        return out

    def drawtype(self, props):
        e = props.get('DrawType')
        if not e:
            return 'DT_Sprite'                      # Actor's own default
        if e.get('enum'):
            return e['enum']
        v = e['value']
        return self.drawtypes[v] if isinstance(v, int) and v < len(self.drawtypes) else v

    def package(self, key):
        return self.world.pkgs[key], os.path.join(self.dir, key)


def static_mesh_instances(pkg, path, lib, defaults=None):
    """Yield (mesh key, Mesh, columns, location, actor name) for every actor
    drawn as a static mesh.

    With `defaults`, an actor's properties are merged over its class's
    effective defaults: that brings in actors which never set a mesh of their
    own (coins, crates, chains), honours inherited DrawType, bHidden and scale,
    and lets the class decide whether the actor is drawn as a static mesh at
    all. Without it, only actors carrying their own StaticMesh are placed."""
    mp = Map(pkg)
    for e, d in mp.actors(values=True):
        if defaults is not None:
            d = defaults.merged(pkg.classof(e), d)
            if defaults.drawtype(d) != 'DT_StaticMesh':
                continue
        sm = d.get('StaticMesh')
        if not sm or 'ref' not in sm:
            continue
        if d.get('bHidden', {}).get('value'):
            continue
        if '_pkg' in sm:                            # the reference is the class's
            hp, hpath = defaults.package(sm['_pkg'])
        else:
            hp, hpath = pkg, path
        key, mesh = lib.resolve(hp, hpath, sm['ref'])
        if mesh is None:
            continue
        cols, loc = actor_matrix(d)
        yield key, mesh, cols, loc, e['name']
