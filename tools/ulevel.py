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
    """Yield (vertices, material name, surface flags) for every BSP polygon."""
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
        yield pts, (pkg.refname(s.material) if s else 'None'), flags


def main(argv):
    p = Package(argv[0])
    m = level_model(p)
    polys = list(polygons(m))
    mats = collections.Counter(mat for _, mat, _ in polys)
    print('%s: level model %s, %d polygons, %d triangles' %
          (os.path.basename(argv[0]), m.e['name'], len(polys),
           sum(len(v) - 2 for v, _, _ in polys)))
    print('materials:', dict(mats.most_common(8)))


if __name__ == '__main__':
    main(sys.argv[1:])
