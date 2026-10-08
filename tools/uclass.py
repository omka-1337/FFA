"""Classes, properties and members of Unreal Engine 2 build 2226 packages.

Layouts established by brute force against every record in Shrek 2, taking exact
end of record alignment as the test:

  UField     SuperField, Next                                (compact indices)
  UStruct    UField + ScriptText, CppText, Children, FriendlyName, one unused
             index, Line u32, TextPos u32, ScriptSize u32, bytecode
  UProperty  UField + ArrayDim u16, ElementSize u16, PropertyFlags u32,
             Category index, RepOffset u16 when CPF_Net is set, then one
             reference for Object/Struct/Byte/Array/Delegate properties and two
             for ClassProperty
"""
import struct, sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R

CPF_EDIT = 0x00000001
CPF_CONST = 0x00000002
CPF_OPTIONAL = 0x00000010
CPF_NET = 0x00000020
CPF_PARM = 0x00000080
CPF_OUT = 0x00000100
CPF_RETURN = 0x00000400
CPF_COERCE = 0x00000800
CPF_NATIVE = 0x00001000
CPF_TRANSIENT = 0x00002000
CPF_CONFIG = 0x00004000
CPF_LOCALIZED = 0x00008000

PROP_REFS = {'ObjectProperty': 1, 'StructProperty': 1, 'ByteProperty': 1,
             'ArrayProperty': 1, 'DelegateProperty': 1, 'ClassProperty': 2}

SIMPLE = {'IntProperty': 'int', 'FloatProperty': 'float', 'BoolProperty': 'bool',
          'StrProperty': 'string', 'NameProperty': 'name',
          'StringProperty': 'string', 'PointerProperty': 'pointer'}


class Reader:
    def __init__(self, pkg):
        self.p = pkg
        self.cache = {}

    def field(self, idx):
        """Parse export `idx` (1 based, as object references count) far enough to
        get its links. Returns a dict, cached."""
        if idx in self.cache:
            return self.cache[idx]
        p = self.p
        e = p.exports[idx - 1]
        cls = p.classof(e)
        d = dict(name=e['name'], cls=cls, export=e, idx=idx,
                 super=0, next=0, children=0, flags=0, refs=[])
        r = R(p.b, e['off'])
        try:
            if cls == 'Class':
                # A class's own properties are its defaults, at the end of the
                # record, so it starts straight with its links: Super, Next.
                d['super'], d['next'] = r.idx(), r.idx()
            else:
                # Every other field begins with an empty property block, its
                # None, and then Super and Next. Read without it, a function's
                # Super passes for its Next and the member chain wanders off
                # into the parent class.
                r.idx()
                d['super'], d['next'] = r.idx(), r.idx()
            if cls.endswith('Property'):
                r.p += 4                              # ArrayDim, ElementSize
                d['flags'] = r.u32()
                r.idx()                               # Category
                if d['flags'] & CPF_NET:
                    r.u16()
                d['refs'] = [r.idx() for _ in range(PROP_REFS.get(cls, 0))]
            elif cls == 'Class':
                # Children follows ScriptText. Read one index later, it lands
                # on another class's member chain in nearly every class.
                r.idx()                               # ScriptText
                d['children'] = r.idx()
            elif cls in ('State', 'Function', 'Struct'):
                r.idx()                               # ScriptText
                d['children'] = r.idx()
        except Exception:
            pass
        self.cache[idx] = d
        return d

    def owned(self, idx):
        """Everything the export table says this object owns. Complete, but in
        serialisation order, which is not declaration order."""
        if not hasattr(self, '_owned'):
            self._owned = collections.defaultdict(list)
            for i, e in enumerate(self.p.exports):
                if e['pkg'] > 0:
                    self._owned[e['pkg']].append(i + 1)
        return [self.field(i) for i in self._owned.get(idx, [])]

    def members(self, idx):
        """Declaration order, by walking Children and then Next. Anything the
        export table owns but the chain does not reach is appended after."""
        d = self.field(idx)
        out, seen, cur = [], set(), d['children']
        while cur > 0 and cur not in seen:
            seen.add(cur)
            m = self.field(cur)
            out.append(m)
            cur = m['next']
        out.extend(m for m in self.owned(idx) if m['idx'] not in seen)
        return out

    def enum_values(self, idx):
        """Value names of an Enum record: three leading indices, a compact count,
        then that many name indices. Verified on every enum of two package
        versions."""
        e = self.p.exports[idx - 1]
        end = e['off'] + e['size']
        r = R(self.p.b, e['off'])
        try:
            r.idx(); r.idx(); r.idx()
            out = []
            for _ in range(r.idx()):
                if r.p > end:
                    return []
                i = r.idx()
                out.append(self.p.names[i] if 0 <= i < len(self.p.names) else '?')
            return out if r.p == end else []
        except Exception:
            return []

    def typename(self, d):
        cls = d['cls']
        if cls in SIMPLE:
            return SIMPLE[cls]
        ref = d['refs'][0] if d['refs'] else 0
        base = self.p.refname(ref) if ref else '?'
        if cls == 'ObjectProperty':
            return base
        if cls == 'StructProperty':
            return base
        if cls == 'ByteProperty':
            return 'byte' if not ref else f'byte/*{base}*/'
        if cls == 'ClassProperty':
            meta = self.p.refname(d['refs'][1]) if len(d['refs']) > 1 else '?'
            return f'class<{meta}>'
        if cls == 'ArrayProperty':
            inner = self.field(ref) if ref > 0 else None
            return f'array<{self.typename(inner) if inner else base}>'
        if cls == 'DelegateProperty':
            return f'delegate<{base}>'
        return cls

    def signature(self, d):
        """Render a function declaration from its parameter children."""
        parms = [m for m in self.members(d['idx'])
                 if m['cls'].endswith('Property') and (m['flags'] & CPF_PARM)]
        ret = next((m for m in parms if m['flags'] & CPF_RETURN), None)
        args = []
        for m in parms:
            if m is ret:
                continue
            mod = ''
            if m['flags'] & CPF_OUT:
                mod += 'out '
            if m['flags'] & CPF_OPTIONAL:
                mod += 'optional '
            if m['flags'] & CPF_COERCE:
                mod += 'coerce '
            args.append(f"{mod}{self.typename(m)} {m['name']}")
        rt = f"{self.typename(ret)} " if ret else ''
        return f"function {rt}{d['name']}({', '.join(args)})"


def dump_class(pkg, name, out=sys.stdout):
    rd = Reader(pkg)
    idx = next((i + 1 for i, e in enumerate(pkg.exports)
                if e['name'] == name and pkg.classof(e) == 'Class'), None)
    if idx is None:
        print(f'class {name} not found in {pkg.name}', file=out)
        return
    d = rd.field(idx)
    parent = pkg.refname(d['super']) if d['super'] else 'Object'
    print(f"class {name} extends {parent};   // {pkg.name}", file=out)
    members = rd.members(idx)
    kinds = collections.Counter(m['cls'] for m in members)
    print(f"// members: {len(members)}  {dict(kinds)}\n", file=out)
    for m in members:
        if m['cls'].endswith('Property'):
            mods = ''
            if m['flags'] & CPF_CONFIG:
                mods += 'config '
            if m['flags'] & CPF_TRANSIENT:
                mods += 'transient '
            if m['flags'] & CPF_LOCALIZED:
                mods += 'localized '
            if m['flags'] & CPF_NATIVE:
                mods += 'native '
            print(f"var {mods}{rd.typename(m)} {m['name']};", file=out)
    print(file=out)
    for m in members:
        if m['cls'] == 'Function':
            print(rd.signature(m) + ';', file=out)
    for m in members:
        if m['cls'] == 'State':
            print(f"state {m['name']};", file=out)


def dump_all(pkg, outdir):
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for i, e in enumerate(pkg.exports):
        if pkg.classof(e) != 'Class':
            continue
        with open(os.path.join(outdir, e['name'] + '.uc'), 'w') as f:
            dump_class(pkg, e['name'], out=f)
        n += 1
    return n


if __name__ == '__main__':
    pkg = Package(sys.argv[1])
    if len(sys.argv) > 3 and sys.argv[2] == '--all':
        print('classes dumped:', dump_all(pkg, sys.argv[3]))
    else:
        dump_class(pkg, sys.argv[2])
