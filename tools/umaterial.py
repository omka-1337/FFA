"""From a material reference to the texture that gives it its colour.

A surface or mesh rarely points at a Texture directly. Between them stand
shaders, blends and modifiers, and each class keeps the material it wraps under
its own property name. Surveyed over every package, these are the links that
lead towards the colour:

    Shader                          Diffuse, else FallbackMaterial
    FinalBlend, TexPanner, TexOscillator, TexRotator, TexScaler, TexEnvMap,
    ColorModifier, OpacityModifier, MaterialSwitch
                                    Material
    Combiner                        Material1
    MaterialSequence                the first Material in SequenceItems

Cubemaps are environment reflections, not a colour, and end the walk.

A static mesh names one material per section, in its Materials array of
{Material, EnableCollision} structs. An actor's Skins array, where present,
overrides them slot by slot.

Usage: umaterial.py <game dir>    survey every static mesh's materials
"""
import sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uterrain import properties_upto
from udefaults import struct_array, Tagged
from ulevel import import_path, MeshLibrary

FOLLOW = {
    'Shader': ('Diffuse', 'FallbackMaterial'),
    'Combiner': ('Material1', 'FallbackMaterial'),
}
for _c in ('FinalBlend', 'TexPanner', 'TexOscillator', 'TexRotator', 'TexScaler',
           'TexEnvMap', 'ColorModifier', 'OpacityModifier', 'MaterialSwitch'):
    FOLLOW[_c] = ('Material', 'FallbackMaterial')


class MaterialResolver:
    def __init__(self, lib):
        self.lib = lib            # a ulevel.MeshLibrary, for its package files
        self.cache = {}

    def locate(self, pkg, path, ref):
        """(package, path, export) for a reference, through imports."""
        if ref > 0:
            return pkg, path, pkg.exports[ref - 1]
        if ref < 0:
            chain = import_path(pkg, ref)
            fpath = self.lib.files.get(chain[0].lower())
            if not fpath:
                return None
            p = self.lib.package(fpath)
            cls = pkg.imports[-ref - 1]['cls']
            for e in p.exports:
                if e['name'].lower() == chain[-1].lower() and p.classof(e) == cls:
                    return p, fpath, e
        return None

    def texture(self, pkg, path, ref, depth=0):
        """(path, export index) of the base Texture, or None."""
        if not ref or depth > 12:
            return None
        hit = self.locate(pkg, path, ref)
        if not hit:
            return None
        p, fpath, e = hit
        key = (fpath, e['name'], p.classof(e))
        if key in self.cache:
            return self.cache[key]
        cls = p.classof(e)
        out = None
        if cls == 'Texture':
            out = (fpath, p.exports.index(e) + 1)
        else:
            props, _ = properties_upto(p, e)
            props = props or {}
            names = FOLLOW.get(cls, ())
            if cls == 'MaterialSequence':
                items = struct_array(p, props.get('SequenceItems')) or []
                refs = [it['Material']['ref'] for it in items if 'Material' in it]
            else:
                refs = [props[n]['ref'] for n in names if n in props and props[n].get('ref')]
            for r in refs:
                out = self.texture(p, fpath, r, depth + 1)
                if out:
                    break
        self.cache[key] = out
        return out


def mesh_materials(m):
    """Material references of a static mesh, one per section, from its
    Materials array of {Material, EnableCollision} structs."""
    v = Tagged(m.p).parse(m.e['off'], m.e['off'] + m.e['size'])
    props = {x['name']: x for x in v[0]} if v else {}
    items = struct_array(m.p, props.get('Materials'))
    if items is None:
        return None
    return [it.get('Material', {}).get('ref', 0) for it in items]


def survey(game):
    """For every static mesh in the game: does the Materials array parse, is
    there one material per section, do the non empty sections tile the index
    buffer exactly, and does each material lead to a texture."""
    from umesh import Mesh
    lib = MeshLibrary([os.path.join(game, d) for d in
                       ('StaticMeshes', 'Textures', 'Animations', 'System', 'Maps')])
    mr = MaterialResolver(lib)
    n = parsed = matched = tiled = 0
    counts, missing = collections.Counter(), []
    for path in sorted(set(lib.files.values())):
        p = lib.package(path)
        for i, e in enumerate(p.exports):
            if p.classof(e) != 'StaticMesh':
                continue
            try:
                m = Mesh(p, e)
            except Exception:
                continue
            n += 1
            mats = mesh_materials(m)
            if mats is None:
                continue
            parsed += 1
            ranges = m.section_ranges()
            matched += len(mats) == len(ranges)
            at = 0
            for first, faces in ranges:
                if faces:
                    at = at + faces * 3 if first == at else -1
            tiled += at == len(m.indices)
            for ref in mats:
                if not ref:
                    counts['empty slot'] += 1
                elif mr.texture(p, path, ref):
                    counts['texture'] += 1
                else:
                    counts['not located'] += 1
                    missing.append('%s.%s -> %s' % (os.path.basename(path), m.name,
                                                    '.'.join(import_path(p, ref))
                                                    if ref < 0 else p.exports[ref - 1]['name']))
    print('%d static meshes: Materials parsed %d, one per section %d, sections tile '
          'the index buffer %d' % (n, parsed, matched, tiled))
    print('material slots: ' + ', '.join('%s %d' % kv for kv in sorted(counts.items())))
    for x in missing:
        print('  not located:', x)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    survey(sys.argv[1])
