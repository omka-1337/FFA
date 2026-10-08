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

The Level object says which of the actors are live. After an empty property
block its record holds:

    u32 x2     the actor count, twice (the array's count and its capacity)
    index      that many actors, LevelInfo first
    FURL       Protocol, Host, Map and Portal strings, an array of option
               strings, i32 Port, i32 Valid
    index      the level's Model
    f32        a time in seconds
    18 bytes   zero in every level of the game

A package keeps actors the level no longer lists: every one of them, 8537 in
Shrek 2, carries bDeleteMe, and none of the 22584 listed ones does. They are
deleted actors the editor saved anyway, and the game never sees them.
"""
import sys, os, struct, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from udefaults import Tagged

RF_HAS_STACK = 0x02000000

# Objects whose records carry native binary payloads, not just properties.
NATIVE_PAYLOAD = {'Model', 'Polys', 'StaticMeshInstance', 'TerrainSector',
                  'Level', 'Texture', 'StaticMesh'}


def fstring(r, b):
    """An FString: compact length including the terminator, then the bytes;
    a negative length means UTF-16."""
    n = r.idx()
    if n < 0:
        s = b[r.p:r.p - 2 * n - 2].decode('utf-16-le')
        r.p += -2 * n
        return s
    s = b[r.p:r.p + max(n - 1, 0)].decode('latin-1')
    r.p += max(n, 0)
    return s


class Level:
    """The Level record: its live actors, its URL and its Model."""

    def __init__(self, pkg, e):
        b, end = pkg.b, e['off'] + e['size']
        r = R(b, e['off'])
        if r.idx() != 0:
            raise ValueError('Level has properties')
        n, capacity = r.u32(), r.u32()
        if n != capacity or r.p + n > end:
            raise ValueError('actor list %d of %d' % (n, capacity))
        self.actors = [r.idx() for _ in range(n)]
        self.url = dict(protocol=fstring(r, b), host=fstring(r, b),
                        map=fstring(r, b), portal=fstring(r, b))
        self.url['options'] = [fstring(r, b) for _ in range(r.idx())]
        self.url['port'], self.url['valid'] = r.i32(), r.i32()
        self.model = r.idx()
        self.time = struct.unpack_from('<f', b, r.p)[0]
        r.p += 4
        self.rest = b[r.p:end]
        if len(self.rest) != 18 or any(self.rest):
            raise ValueError('Level tail is not the 18 zero bytes seen everywhere')


class Map:
    def __init__(self, pkg):
        self.p = pkg
        self.tag = Tagged(pkg)
        self._level = False

    def level(self):
        """The package's Level, or None for a package that is not a map."""
        if self._level is False:
            self._level = None
            for e in self.p.exports:
                if self.p.classof(e) == 'Level' and e['size']:
                    self._level = Level(self.p, e)
                    break
        return self._level

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

    def actors(self, values=True, live=False):
        """(export, properties) of every object that is a plain tagged block.
        With `live`, only the actors the Level lists, in a map that has one."""
        level = self.level() if live else None
        keep = set(level.actors) if level else None
        for i, e in enumerate(self.p.exports):
            if keep is not None and i + 1 not in keep:
                continue
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


def level_check(paths):
    """Every map's Level read to its end, and the actors it leaves out: all of
    them should carry bDeleteMe, and none of the listed ones should."""
    levels = listed = deleted = stray = flagged = 0
    for f in paths:
        p = Package(f)
        m = Map(p)
        try:
            level = m.level()
        except ValueError as ex:
            print('  %s: %s' % (os.path.basename(f), ex))
            continue
        if level is None:
            continue
        levels += 1
        live = set(level.actors)
        for i, e in enumerate(p.exports):
            d = m.properties(e)
            if d is None or 'Location' not in d and 'Level' not in d:
                continue                    # not an actor
            gone = bool(d.get('bDeleteMe', {}).get('value'))
            if i + 1 in live:
                listed += 1
                flagged += gone
            else:
                deleted += gone
                stray += not gone
    print('%d levels read to their end; %d actors listed, %d of them marked deleted; '
          '%d left out, %d marked deleted and %d not' % (levels, listed, flagged,
                                                          deleted + stray, deleted, stray))


def main(argv):
    if argv and argv[0] == '--levels':
        argv = argv[1:]
        paths = argv
        if len(argv) == 1 and os.path.isdir(argv[0]):
            paths = [os.path.join(argv[0], f) for f in sorted(os.listdir(argv[0]))
                     if f.lower().endswith('.unr')]
        return level_check(paths)
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
