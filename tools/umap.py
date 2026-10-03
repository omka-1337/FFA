"""Actors of a UE2 level (.unr).

A map is the same package container as a script package, so the readers in
upkg.py open it unchanged. What differs is what the exports hold: instead of
classes and functions, a level is a set of object instances.

An instance is serialised as the same tagged property block that carries a
class's defaultproperties, with one addition. When an object carries the
RF_HasStack flag its record begins with the execution state frame:

    index  Node            the state's UStruct
    index  StateNode
    u64    ProbeMask
    u32    LatentAction
    index  Offset          only when Node is set

Skipping that, every actor class parses exactly to the end of its record. The
objects that do not are the ones with native C++ serialisation behind the
properties: Model and Polys (the BSP), StaticMeshInstance, TerrainSector. Those
are a separate decoding problem; this module reports them rather than guessing.
"""
import sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from udefaults import Tagged

RF_HAS_STACK = 0x02000000

# Objects whose records carry native binary payloads, not just properties.
NATIVE_PAYLOAD = {'Model', 'Polys', 'StaticMeshInstance', 'TerrainSector',
                  'Level', 'Texture', 'StaticMesh'}


class Map:
    def __init__(self, pkg):
        self.p = pkg
        self.tag = Tagged(pkg)

    def props_start(self, e):
        """Offset of the tagged block, past the state frame when present."""
        r = R(self.p.b, e['off'])
        if e['flags'] & RF_HAS_STACK:
            node = r.idx()
            r.idx()                  # StateNode
            r.p += 8 + 4             # ProbeMask, LatentAction
            if node:
                r.idx()              # Offset into the state's bytecode
        return r.p

    def properties(self, e, values=True):
        """Property dict of one object, or None when the record is not a plain
        tagged block."""
        if not e['size']:
            return None
        end = e['off'] + e['size']
        try:
            v = self.tag.parse(self.props_start(e), end, want_values=values)
        except Exception:
            return None
        if not v or v[1] != end:
            return None
        return {x['name']: x for x in v[0]}

    def actors(self, values=True):
        for e in self.p.exports:
            d = self.properties(e, values)
            if d is not None:
                yield e, d


def summarise(path):
    p = Package(path)
    m = Map(p)
    ok = collections.Counter()
    bad = collections.Counter()
    for e in p.exports:
        if not e['size']:
            continue
        cls = p.classof(e)
        if m.properties(e, values=False) is None:
            bad[cls] += 1
        else:
            ok[cls] += 1
    total = sum(ok.values()) + sum(bad.values())
    print('%-26s %5d objects, %5d parsed, %4d native' %
          (os.path.basename(path), total, sum(ok.values()), sum(bad.values())))
    return sum(ok.values()), sum(bad.values()), bad


def listing(path, cls_name):
    p = Package(path)
    m = Map(p)
    for e, d in m.actors():
        if p.classof(e) != cls_name:
            continue
        loc = d.get('Location', {}).get('value')
        rot = d.get('Rotation', {}).get('value')
        tag = d.get('Tag', {}).get('value')
        bits = ['%s' % e['name']]
        if loc:
            bits.append('at (%g, %g, %g)' % loc)
        if rot:
            bits.append('rot (%d, %d, %d)' % rot)
        if tag:
            bits.append("tag '%s'" % tag)
        bits.append('%d properties' % len(d))
        print('  ' + '  '.join(bits))


def main(argv):
    if len(argv) > 1 and not os.path.isdir(argv[0]):
        listing(argv[0], argv[1])
        return
    paths = argv
    if len(argv) == 1 and os.path.isdir(argv[0]):
        paths = [os.path.join(argv[0], f) for f in sorted(os.listdir(argv[0]))
                 if f.lower().endswith('.unr')]
    tot_ok = tot_bad = 0
    native = collections.Counter()
    for f in paths:
        a, b, kinds = summarise(f)
        tot_ok += a
        tot_bad += b
        native.update(kinds)
    if len(paths) > 1:
        print('\nTOTAL %d objects parsed, %d with native payloads' % (tot_ok, tot_bad))
        print('native payload classes:', dict(native.most_common(10)))


if __name__ == '__main__':
    main(sys.argv[1:])
