"""Default property blocks of UE2 classes, and the tagged value format itself.

A class record ends with its defaultproperties: a tagged list of name and value
pairs terminated by the name `None`. The fields between the struct header and
that block (state masks, class flags, a GUID, variable length dependency and
import arrays) are not decoded yet, so the block is located rather than seeked
to: scan for an offset from which a tagged parse lands exactly on the end of the
record, then pick the candidate whose property names all resolve to real
properties of the class or its ancestors.

That semantic check is not optional. Several offsets per record usually parse
cleanly to the end, and the earliest one is the right answer only about a third
of the time, so without it the output would be quietly wrong.

Verified on every class of two package versions: 2002 of 2002 in Shrek 2 PC
(version 129) and 607 of 607 in the UnrealEngine2 Runtime (version 126), with
every property name valid.

The same tagged format carries actor properties inside .unr maps, so this reader
is also the way into level data.
"""
import sys, os, glob, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from uclass import Reader

# Property type codes as they appear in a tag's info byte.
T_BYTE, T_INT, T_BOOL, T_FLOAT, T_OBJECT, T_NAME, T_STRING = 1, 2, 3, 4, 5, 6, 7
T_CLASS, T_ARRAY, T_STRUCT, T_VECTOR, T_ROTATOR, T_STR = 8, 9, 10, 11, 12, 13
T_MAP, T_FIXEDARRAY = 14, 15

# Fixed widths encoded in the size bits; 5, 6 and 7 mean the size follows.
SIZE_BITS = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}


class Tagged:
    """Reader for one tagged property block."""

    def __init__(self, pkg):
        self.p = pkg

    def parse(self, start, end, want_values=True):
        """Returns (entries, position after the terminator) or None on garbage."""
        p = self.p
        r = R(p.b, start)
        out = []
        while True:
            if r.p >= end:
                return None
            nm = r.idx()
            if nm == p.none_idx:
                return out, r.p
            if nm < 0 or nm >= len(p.names):
                return None
            if r.p >= end:
                return None
            info = r.u8()
            t = info & 0x0F
            if t == 0 or t > T_FIXEDARRAY:
                return None
            struct_name = None
            if t == T_STRUCT:
                sn = r.idx()
                if sn < 0 or sn >= len(p.names):
                    return None
                struct_name = p.names[sn]
            bits = (info >> 4) & 0x07
            size = SIZE_BITS.get(bits)
            if size is None:
                size = {5: r.u8, 6: r.u16, 7: r.u32}[bits]()
            index = 0
            if (info & 0x80) and t != T_BOOL:
                b0 = r.u8()
                if b0 & 0x80:
                    if b0 & 0x40:
                        index = ((b0 & 0x3F) << 24) | (r.u8() << 16) | (r.u8() << 8) | r.u8()
                    else:
                        index = ((b0 & 0x7F) << 8) | r.u8()
                else:
                    index = b0
            value = None
            if t == T_BOOL:
                value = bool(info & 0x80)
            else:
                at = r.p
                if want_values:
                    value = self.value(t, at, size, struct_name)
                r.p = at + size
                if r.p > end:
                    return None
            out.append(dict(name=p.names[nm], type=t, index=index,
                            struct=struct_name, size=size, value=value))
            if len(out) > 4000:
                return None

    # Structs whose values are plain binary rather than a nested tagged list.
    ATOMIC = {'Vector': '<3f', 'Plane': '<4f', 'Rotator': '<3i',
              'Color': '4B', 'Range': '<2f', 'Scale': '<3f'}

    def value(self, t, at, size, struct_name):
        """Decode a scalar value. Composite values are left as raw bytes."""
        import struct as _s
        p = self.p
        b = p.b
        if t == T_STRUCT and struct_name in self.ATOMIC:
            fmt = self.ATOMIC[struct_name]
            need = _s.calcsize(fmt)
            if size >= need:
                vals = _s.unpack_from(fmt, b, at)
                return tuple(round(v, 4) if isinstance(v, float) else v for v in vals)
        if t == T_BYTE:
            return b[at]
        if t == T_INT:
            return _s.unpack_from('<i', b, at)[0]
        if t == T_FLOAT:
            return round(_s.unpack_from('<f', b, at)[0], 6)
        if t in (T_OBJECT, T_CLASS):
            return p.refname(R(b, at).idx())
        if t == T_NAME:
            i = R(b, at).idx()
            return p.names[i] if 0 <= i < len(p.names) else '?'
        if t == T_STR:
            r = R(b, at)
            n = r.idx()
            return b[r.p:r.p + max(0, n - 1)].decode('latin-1')
        if t == T_VECTOR and size >= 12:
            return tuple(round(v, 4) for v in _s.unpack_from('<3f', b, at))
        if t == T_ROTATOR and size >= 12:
            return tuple(_s.unpack_from('<3i', b, at))
        return b[at:at + size]

    def render(self, entry):
        t, v = entry['type'], entry['value']
        name = entry['name']
        if entry['index']:
            name = '%s(%d)' % (name, entry['index'])
        if entry.get('enum'):
            return '%s=%s' % (name, entry['enum'])
        if t == T_BOOL:
            return '%s=%s' % (name, 'True' if v else 'False')
        if t == T_STR:
            return '%s="%s"' % (name, v)
        if t == T_NAME:
            return "%s='%s'" % (name, v)
        if isinstance(v, tuple):
            labels = {'Vector': ('X', 'Y', 'Z'), 'Scale': ('X', 'Y', 'Z'),
                      'Plane': ('X', 'Y', 'Z', 'W'),
                      'Rotator': ('Pitch', 'Yaw', 'Roll'),
                      'Color': ('R', 'G', 'B', 'A'),
                      'Range': ('Min', 'Max')}.get(
                          entry['struct'],
                          ('X', 'Y', 'Z') if t == T_VECTOR else ('Pitch', 'Yaw', 'Roll'))
            body = ','.join('%s=%g' % (k, x) for k, x in zip(labels, v))
            return '%s=(%s)' % (name, body)
        if isinstance(v, (bytes, bytearray)):
            kind = entry['struct'] or {T_ARRAY: 'array', T_MAP: 'map'}.get(t, 'raw')
            return '%s=<%s, %d bytes>' % (name, kind, len(v))
        return '%s=%s' % (name, v)


class World:
    """Every package of one game, so super chains can cross package boundaries."""

    def __init__(self, paths=(), packages=None):
        self.pkgs, self.readers = {}, {}
        for f in sorted(paths):
            p = Package(f)
            self.pkgs[os.path.basename(f)] = p
        if packages:
            self.pkgs.update(packages)      # reuse already loaded packages
        for key, p in self.pkgs.items():
            self.readers[key] = Reader(p)
        self.classes, self.enums = {}, {}
        for key, p in self.pkgs.items():
            for i, e in enumerate(p.exports):
                cls = p.classof(e)
                if cls == 'Class':
                    self.classes.setdefault(e['name'], (key, i + 1))
                elif cls == 'Enum':
                    self.enums.setdefault(e['name'], (key, i + 1))

    def inherited_properties(self, key, idx, depth=0):
        """Property name -> (package key, field) for a class and its ancestors."""
        out = {}
        while idx and depth < 50:
            p, rd = self.pkgs[key], self.readers[key]
            for m in rd.members(idx):
                if m['cls'].endswith('Property'):
                    out.setdefault(m['name'], (key, m))
            sup = rd.field(idx)['super']
            if sup > 0:
                idx = sup
            elif sup < 0:
                nm = p.refname(sup)
                if nm not in self.classes:
                    break
                key, idx = self.classes[nm]
            else:
                break
            depth += 1
        return out

    def enum_for(self, key, field):
        """Value names of the enum a ByteProperty refers to, across packages."""
        if field['cls'] != 'ByteProperty' or not field['refs']:
            return None
        ref = field['refs'][0]
        if ref > 0:
            return self.readers[key].enum_values(ref) or None
        name = self.pkgs[key].refname(ref)
        if name in self.enums:
            ekey, eidx = self.enums[name]
            return self.readers[ekey].enum_values(eidx) or None
        return None

    def defaults(self, key, idx):
        """Locate and parse the defaults block of one class. Returns entries."""
        p = self.pkgs[key]
        e = p.exports[idx - 1]
        end = e['off'] + e['size']
        known = self.inherited_properties(key, idx)
        tag = Tagged(p)
        best = None
        for s in range(e['off'], end):
            # scan without decoding values: faster, and a wrong offset would
            # otherwise be asked to interpret garbage as references
            v = tag.parse(s, end, want_values=False)
            if not v or v[1] != end:
                continue
            entries = v[0]
            score = 1.0 if not entries else sum(
                x['name'] in known for x in entries) / len(entries)
            if best is None or (score, len(entries)) > (best[1], best[2]):
                best = (s, score, len(entries))
        if best is None:
            return None
        entries, _ = tag.parse(best[0], end)      # decode values for the winner
        for x in entries:                         # name enum values where we can
            if x['type'] == T_BYTE and x['name'] in known:
                pkey, field = known[x['name']]
                vals = self.enum_for(pkey, field)
                if vals and isinstance(x['value'], int) and x['value'] < len(vals):
                    x['enum'] = vals[x['value']]
        return entries, best[1]


def main(argv):
    system = argv[0]
    w = World(glob.glob(os.path.join(system, '*.u')))
    if len(argv) > 2:
        key, name = argv[1], argv[2]
        if name not in w.classes:
            print('class %s not found' % name)
            return
        pkey, idx = w.classes[name]
        best = w.defaults(pkey, idx)
        p = w.pkgs[pkey]
        tag = Tagged(p)
        print('// %s, from %s' % (name, pkey))
        print('defaultproperties\n{')
        for x in (best[0] if best else []):
            print('\t' + tag.render(x))
        print('}')
        return
    stats = collections.Counter()
    for key, p in w.pkgs.items():
        for i, e in enumerate(p.exports):
            if p.classof(e) != 'Class':
                continue
            best = w.defaults(key, i + 1)
            if best is None:
                stats['no block'] += 1
            elif best[1] == 1.0:
                stats['all names valid'] += 1
            else:
                stats['suspect'] += 1
    print('%s: %s' % (system, dict(stats)))


if __name__ == '__main__':
    main(sys.argv[1:])
