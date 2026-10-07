"""Writes small UE2 packages for the engine's tests.

The engine is meant to be proven on a game's corpus, and `ffa-script check`
does that. These packages are the other half: script whose result is known in
advance, so each language feature can be tested on its own. They follow the
layouts in docs/package-format.md, and every package is read back with the
readers of FFA-Tools before it is used. Each function must parse to the exact end
of its record with the memory sizes agreeing, each state's code must add up to
its ScriptSize, and udefaults must find each class's defaults block where it
was written. A fixture the readers cannot read is a bug here, not a test of
the engine.

Usage: fixtures.py <output dir> <FFA-Tools tools dir>
"""
import os
import struct
import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)
sys.path.insert(0, os.path.abspath(sys.argv[2]))

from upkg import Package as ReadPackage            # noqa: E402
from uscript import Script, struct_code             # noqa: E402
from udefaults import World                         # noqa: E402

FUNC_FINAL, FUNC_DEFINED, FUNC_ITERATOR, FUNC_LATENT = 0x1, 0x2, 0x4, 0x8
FUNC_PRE_OPERATOR, FUNC_NATIVE, FUNC_OPERATOR, FUNC_STATIC = 0x10, 0x400, 0x1000, 0x2000
FUNC_EVENT, FUNC_DELEGATE, FUNC_PUBLIC = 0x800, 0x100000, 0x20000
CPF_OPTIONAL, CPF_PARM, CPF_OUT, CPF_SKIP, CPF_RETURN = 0x10, 0x80, 0x100, 0x200, 0x400
STATE_AUTO = 0x2


def cidx(v):
    """Compact index: sign and continuation in the first byte with six value
    bits, then seven per further byte."""
    neg, v = v < 0, abs(v)
    out = bytearray([(v & 0x3F) | (0x80 if neg else 0) | (0x40 if v >> 6 else 0)])
    v >>= 6
    while v:
        out.append((v & 0x7F) | (0x80 if v >> 7 else 0))
        v >>= 7
    return bytes(out)


# ------------------------------------------------------------------ model
class Obj:
    kind = None

    def __init__(self, name, outer):
        self.name = name
        self.outer = outer
        self.pkg = outer.pkg if outer is not None else None
        self.export = 0

    def chain(self):
        """Members in declaration order, linked by Children and Next."""
        return []

    def owned(self):
        """Members owned but not on the chain."""
        return []


PROPERTY = {'int': 'IntProperty', 'float': 'FloatProperty', 'bool': 'BoolProperty',
            'byte': 'ByteProperty', 'name': 'NameProperty', 'string': 'StrProperty',
            'object': 'ObjectProperty', 'class': 'ClassProperty',
            'struct': 'StructProperty', 'array': 'ArrayProperty',
            'delegate': 'DelegateProperty'}


class Var(Obj):
    """A variable. Its type is a string for the simple kinds, or a tuple:
    ('object', cls), ('class', meta), ('struct', st), ('byte', enum),
    ('array', type), ('delegate', func)."""

    def __init__(self, name, outer, type, dim=1, flags=0):
        super().__init__(name, outer)
        self.type = type
        self.dim = dim
        self.flags = flags
        base = type if isinstance(type, str) else type[0]
        self.kind = PROPERTY[base]
        self.inner = None
        if base == 'array':
            self.inner = Var(name, self, type[1])

    def owned(self):
        return [self.inner] if self.inner else []


class Struct(Obj):
    kind = 'Struct'

    def __init__(self, name, outer, fields, sup=None):
        super().__init__(name, outer)
        self.sup = sup
        self.fields = [Var(n, self, t) for n, t in fields]

    def chain(self):
        return self.fields


class Enum(Obj):
    kind = 'Enum'

    def __init__(self, name, outer, values):
        super().__init__(name, outer)
        self.values = values


class Func(Obj):
    kind = 'Function'

    def __init__(self, name, outer, params=(), ret=None, locals=(), flags=0, native=0,
                 friendly=None):
        super().__init__(name, outer)
        self.params = []
        for p in params:
            n, t, f = (p + (0,))[:3]
            self.params.append(Var(n, self, t, flags=CPF_PARM | f))
        self.ret = Var('ReturnValue', self, ret, flags=CPF_PARM | CPF_OUT | CPF_RETURN) if ret else None
        self.locals = [Var(n, self, t, *rest) for n, t, *rest in locals]
        self.flags = flags | FUNC_PUBLIC | (0 if flags & FUNC_NATIVE else FUNC_DEFINED)
        self.native = native
        self.friendly = friendly or name
        self.code = []
        self.sup = None

    def var(self, name):
        for v in self.params + ([self.ret] if self.ret else []) + self.locals:
            if v.name.lower() == name.lower():
                return v
        raise KeyError('%s has no variable %s' % (self.name, name))

    def chain(self):
        return self.params + ([self.ret] if self.ret else []) + self.locals


class State(Obj):
    kind = 'State'

    def __init__(self, name, outer, sup=None, flags=0):
        super().__init__(name, outer)
        self.sup = sup
        self.flags = flags
        self.funcs = []
        self.code = []

    def func(self, name, **kw):
        f = Func(name, self, **kw)
        self.funcs.append(f)
        return f

    def chain(self):
        return self.funcs


class Class(Obj):
    kind = 'Class'

    def __init__(self, name, pkg, sup=None):
        super().__init__(name, None)
        self.pkg = pkg
        self.sup = sup
        self.vars, self.funcs, self.states, self.structs, self.enums = [], [], [], [], []
        self.defaults = []          # (name, value, kind, extra), see Writer.tagged

    def var(self, name, type, dim=1, flags=0):
        v = Var(name, self, type, dim, flags)
        self.vars.append(v)
        return v

    def func(self, name, **kw):
        f = Func(name, self, **kw)
        self.funcs.append(f)
        return f

    def state(self, name, **kw):
        s = State(name, self, **kw)
        self.states.append(s)
        return s

    def struct(self, name, fields, sup=None):
        s = Struct(name, self, fields, sup)
        self.structs.append(s)
        return s

    def enum(self, name, values):
        e = Enum(name, self, values)
        self.enums.append(e)
        return e

    def find(self, name):
        c = self
        while c is not None:
            for v in c.vars:
                if v.name.lower() == name.lower():
                    return v
            c = c.sup
        raise KeyError('%s has no variable %s' % (self.name, name))

    def chain(self):
        return self.structs + self.vars + self.funcs + self.states

    def owned(self):
        # Enums stay off the chain: which index of an enum record is its Next
        # is not established, and the readers find owned members regardless.
        return self.enums


class Pkg:
    def __init__(self, name):
        self.name = name
        self.classes = []

    def cls(self, name, sup=None):
        c = Class(name, self, sup)
        self.classes.append(c)
        return c


# ---------------------------------------------------------------- bytecode
# Statements and expressions are tuples: (op, operands...). ('label', 'x') is
# not a token but marks where the next statement starts.
NATIVE_OPS = {'local': 0x00, 'inst': 0x01, 'default': 0x02}


class Asm:
    def __init__(self, w, scope):
        self.w = w
        self.scope = scope          # the Func or State being assembled
        self.labels = {}

    def var(self, name):
        """A local or parameter of the function, else a class variable."""
        if isinstance(name, Var):
            return name
        if isinstance(self.scope, Func):
            try:
                return self.scope.var(name)
            except KeyError:
                pass
        owner = self.scope.outer
        while not isinstance(owner, Class):
            owner = owner.outer
        return owner.find(name)

    def program(self, stmts):
        first = self.emit_all(stmts)          # sizes do not depend on targets
        return self.emit_all(stmts) if first is not None else None

    def emit_all(self, stmts):
        out, mem = bytearray(), 0
        for s in stmts:
            if s[0] == 'label':
                self.labels[s[1]] = mem
                continue
            b, m = self.node(s, mem)
            out += b
            mem += m
        return bytes(out), mem

    def target(self, label):
        return self.labels.get(label, 0)

    def node(self, n, mem):
        op = n[0]
        w = self.w
        if op in NATIVE_OPS:
            return bytes([NATIVE_OPS[op]]) + cidx(w.ref(self.var(n[1]))), 5
        if op == 'return':
            b, m = self.node(n[1] if len(n) > 1 else ('nothing',), mem + 1)
            return b'\x04' + b, 1 + m
        if op == 'switch':
            b, m = self.node(n[2], mem + 2)
            return bytes([0x05, n[1]]) + b, 2 + m
        if op == 'jump':
            return b'\x06' + struct.pack('<H', self.target(n[1])), 3
        if op == 'jumpifnot':
            b, m = self.node(n[2], mem + 3)
            return b'\x07' + struct.pack('<H', self.target(n[1])) + b, 3 + m
        if op == 'stop':
            return b'\x08', 1
        if op == 'case':
            if n[1] is None:
                return b'\x0A\xFF\xFF', 3
            b, m = self.node(n[2], mem + 3)
            return b'\x0A' + struct.pack('<H', self.target(n[1])) + b, 3 + m
        if op == 'nothing':
            return b'\x0B', 1
        if op == 'labeltable':
            out = bytearray(b'\x0C')
            for name, label in n[1]:
                out += cidx(w.nm(name)) + struct.pack('<I', self.target(label))
            out += cidx(w.nm('None')) + struct.pack('<I', 0xFFFF)
            return bytes(out), 1 + 8 * (len(n[1]) + 1)
        if op == 'gotolabel':
            b, m = self.node(n[1], mem + 1)
            return b'\x0D' + b, 1 + m
        if op in ('let', 'letbool', 'dynel', 'arrel'):
            code = {'let': 0x0F, 'letbool': 0x14, 'dynel': 0x10, 'arrel': 0x1A}[op]
            a, ma = self.node(n[1], mem + 1)
            b, mb = self.node(n[2], mem + 1 + ma)
            return bytes([code]) + a + b, 1 + ma + mb
        if op == 'new':
            out, m = bytearray(b'\x11'), 1
            for k in n[1:5]:
                b, mk = self.node(k, mem + m)
                out += b
                m += mk
            return bytes(out), m
        if op in ('ctx', 'classctx'):
            a, ma = self.node(n[1], mem + 1)
            b, mb = self.node(n[2], mem + 1 + ma + 3)
            code = 0x19 if op == 'ctx' else 0x12
            return bytes([code]) + a + struct.pack('<HB', mb, 0) + b, 1 + ma + 3 + mb
        if op in ('metacast', 'dyncast'):
            b, m = self.node(n[2], mem + 5)
            return bytes([0x13 if op == 'metacast' else 0x2E]) + cidx(w.ref(n[1])) + b, 5 + m
        if op == 'self':
            return b'\x17', 1
        if op == 'skip':
            b, m = self.node(n[1], mem + 3)
            return b'\x18' + struct.pack('<H', m) + b, 3 + m
        if op in ('virtual', 'global'):
            out = bytearray([0x1B if op == 'virtual' else 0x38]) + cidx(w.nm(n[1]))
            b, m = self.args(n[2:], mem + 5)
            return bytes(out) + b, 5 + m
        if op == 'final':
            b, m = self.args(n[2:], mem + 5)
            return b'\x1C' + cidx(w.ref(n[1])) + b, 5 + m
        if op == 'int':
            return b'\x1D' + struct.pack('<i', n[1]), 5
        if op == 'float':
            return b'\x1E' + struct.pack('<f', n[1]), 5
        if op == 'str':
            s = n[1].encode('latin-1') + b'\0'
            return b'\x1F' + s, 1 + len(s)
        if op == 'obj':
            return b'\x20' + cidx(w.ref(n[1])), 5
        if op == 'name':
            return b'\x21' + cidx(w.nm(n[1])), 5
        if op == 'rot':
            return b'\x22' + struct.pack('<3i', *n[1:4]), 13
        if op == 'vect':
            return b'\x23' + struct.pack('<3f', *n[1:4]), 13
        if op == 'byte':
            return bytes([0x24, n[1]]), 2
        simple = {'int0': 0x25, 'int1': 0x26, 'true': 0x27, 'false': 0x28, 'none': 0x2A,
                  'nodelegate': 0x2B, 'iterpop': 0x30, 'iternext': 0x31}
        if op in simple:
            return bytes([simple[op]]), 1
        if op == 'boolvar':
            b, m = self.node(n[1], mem + 1)
            return b'\x2D' + b, 1 + m
        if op == 'iter':
            b, m = self.node(n[1], mem + 1)
            return b'\x2F' + b + struct.pack('<H', self.target(n[2])), 1 + m + 2
        if op in ('structeq', 'structne'):
            a, ma = self.node(n[2], mem + 5)
            b, mb = self.node(n[3], mem + 5 + ma)
            return bytes([0x32 if op == 'structeq' else 0x33]) + cidx(w.ref(n[1])) + a + b, 5 + ma + mb
        if op == 'delegate':
            return b'\x35' + cidx(w.nm(n[1])), 5
        if op == 'member':
            b, m = self.node(n[2], mem + 5)
            return b'\x36' + cidx(w.ref(n[1])) + b, 5 + m
        if op == 'dynlen':
            b, m = self.node(n[1], mem + 1)
            return b'\x37' + b, 1 + m
        if op == 'cast':
            b, m = self.node(n[2], mem + 1)
            return bytes([n[1]]) + b, 1 + m
        if op == 'native':
            index = n[1]
            head = bytes([index]) if index >= 0x70 and index < 0x100 else \
                bytes([0x60 + (index >> 8), index & 0xFF])
            b, m = self.args(n[2:], mem + len(head))
            return head + b, len(head) + m
        raise ValueError('no token %r' % (op,))

    def args(self, args, mem):
        out, m = bytearray(), 0
        for a in args:
            b, mb = self.node(a, mem + m)
            out += b
            m += mb
        return bytes(out) + b'\x16', m + 1


# ------------------------------------------------------------------ writer
T_BYTE, T_INT, T_BOOL, T_FLOAT, T_OBJECT, T_NAME, T_ARRAY, T_STRUCT, T_STR = 1, 2, 3, 4, 5, 6, 9, 10, 13


class Writer:
    def __init__(self, pkg):
        self.pkg = pkg
        self.names = ['None']
        self.imports = []
        self.import_refs = {}
        self.exports = []
        self.defaults_at = {}       # class name -> offset its defaults start at

    def nm(self, s):
        for i, n in enumerate(self.names):
            if n == s:
                return i
        self.names.append(s)
        return len(self.names) - 1

    def imp(self, cls_pkg, cls_name, outer, name):
        key = (cls_pkg, cls_name, outer, name)
        if key not in self.import_refs:
            self.imports.append(key)
            self.import_refs[key] = -len(self.imports)
        return self.import_refs[key]

    def kind_ref(self, kind):
        """The class of an export: an import of Core's class of that name."""
        return self.imp('Core', 'Class', self.imp('Core', 'Package', 0, 'Core'), kind)

    def ref(self, obj):
        if obj is None:
            return 0
        if obj.pkg is self.pkg:
            assert obj.export, obj.name
            return obj.export
        chain, o = [], obj
        while o is not None:
            chain.append(o)
            o = o.outer
        outer = self.imp('Core', 'Package', 0, obj.pkg.name)
        for o in reversed(chain):
            outer = self.imp('Core', o.kind, outer, o.name)
        return outer

    # ---------------------------------------------------------- layout
    def collect(self):
        order = []

        def add(o):
            order.append(o)
            o.export = len(order)
            for m in o.chain() + o.owned():
                add(m)
        for c in self.pkg.classes:
            add(c)
        return order

    def header(self, o, children, script=b'', size=0):
        nxt = self.next.get(id(o), 0)
        sup = self.ref(getattr(o, 'sup', None))
        friendly = self.nm(getattr(o, 'friendly', o.name))
        return (cidx(sup) + cidx(nxt) + cidx(0) + cidx(0) + cidx(children) +
                cidx(friendly) + cidx(0) + struct.pack('<III', 0, 0, size) + script)

    def body(self, o):
        if isinstance(o, Var):
            refs = b''
            t = o.type
            base = t if isinstance(t, str) else t[0]
            if base in ('object', 'struct', 'delegate'):
                refs = cidx(self.ref(t[1]))
            elif base == 'byte':
                refs = cidx(self.ref(t[1]) if not isinstance(t, str) else 0)
            elif base == 'array':
                refs = cidx(self.ref(o.inner))
            elif base == 'class':
                refs = cidx(0) + cidx(self.ref(t[1]))
            return (cidx(0) + cidx(0) + cidx(self.next.get(id(o), 0)) +
                    struct.pack('<HHI', o.dim, 4, o.flags) + cidx(0) + refs)
        if isinstance(o, Enum):
            return cidx(0) + cidx(0) + cidx(0) + cidx(len(o.values)) + \
                b''.join(cidx(self.nm(v)) for v in o.values)
        first = o.chain()[0].export if o.chain() else 0
        if isinstance(o, Struct):
            return self.header(o, first)
        if isinstance(o, Func):
            code, mem = Asm(self, o).program(o.code) if o.code else (b'', 0)
            return self.header(o, first, code, mem) + struct.pack('<HBI', o.native, 0, o.flags)
        if isinstance(o, State):
            asm = Asm(self, o)
            code, mem = asm.program(o.code) if o.code else (b'', 0)
            lto = 0xFFFF
            for s in o.code:
                if s[0] == 'label' and s[1] == '__labeltable':
                    lto = asm.labels[s[1]]
            return self.header(o, first, code, mem) + struct.pack('<QQHI', 0, 0, lto, o.flags)
        if isinstance(o, Class):
            head = self.header(o, first) + struct.pack('<QQHI', 0, 0, 0xFFFF, 0)
            # Class fields after the state tail are not decoded by the readers;
            # zeros stand in for them, and parse as nothing but None.
            filler = bytes(32)
            return head + filler, self.tagged(o)
        raise TypeError(o)

    def tagged(self, c):
        out = bytearray()
        for name, value in c.defaults:
            out += self.tag(c.find(name), value)
        out += cidx(self.nm('None'))
        return bytes(out)

    def tag(self, v, value, index=0):
        if isinstance(value, tuple) and value and value[0] == 'index':
            index, value = value[1], value[2]
        t = v.type
        base = t if isinstance(t, str) else t[0]
        struct_name = None
        if base == 'bool':
            info = T_BOOL | (0x80 if value else 0)
            return cidx(self.nm(v.name)) + bytes([info])
        if base == 'int':
            code, payload = T_INT, struct.pack('<i', value)
        elif base == 'float':
            code, payload = T_FLOAT, struct.pack('<f', value)
        elif base == 'byte':
            code, payload = T_BYTE, bytes([value])
        elif base == 'string':
            s = value.encode('latin-1') + b'\0'
            code, payload = T_STR, cidx(len(s)) + s
        elif base == 'name':
            code, payload = T_NAME, cidx(self.nm(value))
        elif base == 'object':
            code, payload = T_OBJECT, cidx(self.ref(value))
        elif base == 'struct':
            # Vector and its kind are raw memory, not a nested list
            code, payload, struct_name = T_STRUCT, struct.pack('<3f', *value), t[1].name
        elif base == 'array' and t[1] == 'int':
            code, payload = T_ARRAY, cidx(len(value)) + b''.join(struct.pack('<i', x) for x in value)
        else:
            raise ValueError('no default of type %r' % (t,))
        size = len(payload)
        bits = {1: 0, 2: 1, 4: 2, 12: 3, 16: 4}.get(size)
        extra = b''
        if bits is None:
            bits, extra = 5, bytes([size])
        info = code | (bits << 4) | (0x80 if index else 0)
        out = cidx(self.nm(v.name)) + bytes([info])
        if struct_name:
            out += cidx(self.nm(struct_name))
        out += extra
        if index:
            out += bytes([index])
        return out + payload

    def write(self, path):
        order = self.collect()
        self.next = {}
        for o in order:
            ch = o.chain()
            for a, b in zip(ch, ch[1:]):
                self.next[id(a)] = b.export
        # names and imports first, so the bodies' indices are all known
        bodies = []
        for o in order:
            b = self.body(o)
            bodies.append(b)
        for o in order:
            self.nm(o.name)
            self.kind_ref(o.kind) if o.kind != 'Class' else None
        for c in self.pkg.classes:
            if c.sup is not None:
                self.ref(c.sup)
        # names may have grown while the bodies were made; make them again so
        # every index is final
        bodies = [self.body(o) for o in order]
        for cls_pkg, cls_name, _, name in self.imports:
            self.nm(cls_pkg), self.nm(cls_name), self.nm(name)

        out = bytearray(64)
        name_off = len(out)
        for n in self.names:
            s = n.encode('latin-1') + b'\0'
            out += cidx(len(s)) + s + struct.pack('<I', 0x00070010)
        import_off = len(out)
        for cls_pkg, cls_name, outer, name in self.imports:
            out += cidx(self.nm(cls_pkg)) + cidx(self.nm(cls_name)) + struct.pack('<i', outer) + cidx(self.nm(name))
        placed = []
        for o, b in zip(order, bodies):
            off = len(out)
            if isinstance(b, tuple):
                head, tags = b
                out += head
                self.defaults_at[o.name] = len(out)
                out += tags
            else:
                out += b
            placed.append((off, len(out) - off))
        export_off = len(out)
        for o, (off, size) in zip(order, placed):
            cls = 0 if o.kind == 'Class' else self.kind_ref(o.kind)
            sup = self.ref(o.sup) if isinstance(o, Class) else 0
            outer = o.outer.export if o.outer is not None else 0
            out += cidx(cls) + cidx(sup) + struct.pack('<i', outer) + cidx(self.nm(o.name))
            out += struct.pack('<I', 0) + cidx(size) + (cidx(off) if size else b'')
        struct.pack_into('<IHHI', out, 0, 0x9E2A83C1, 129, 0, 0)
        struct.pack_into('<6I', out, 12, len(self.names), name_off, len(order), export_off,
                         len(self.imports), import_off)
        struct.pack_into('<16sIII', out, 36, bytes(16), 1, len(order), len(self.names))
        with open(path, 'wb') as f:
            f.write(out)
        return order


# ------------------------------------------------------------------ check
def verify(paths, writers):
    """Read the packages back with FFA-Tools and insist on exact agreement."""
    pkgs = {os.path.basename(p): ReadPackage(p) for p in paths}
    world = World(packages=pkgs)
    for key, p in pkgs.items():
        sc = Script(p)
        for i, e in enumerate(p.exports):
            cls = p.classof(e)
            if cls == 'Function':
                _, info = sc.function(e)
                assert info['aligned'] and info['sized'], (key, e['name'], info)
            elif cls == 'State':
                _, info = struct_code(sc, e)
                assert info['sized'], (key, e['name'], info)
                assert e['off'] + e['size'] - info['end'] == 22, (key, e['name'])
            elif cls == 'Class':
                # the block found must be the one written, name for name
                model = next(c for c in writers[key].pkg.classes if c.name == e['name'])
                got = world.defaults(key, i + 1)
                assert got is not None and got[1] == 1.0, (key, e['name'], got)
                found = [x['name'] for x in got[0]]
                assert found == [n for n, _ in model.defaults], (key, e['name'], found)


# ---------------------------------------------------------------- packages
def build_core():
    core = Pkg('Core')
    obj = core.cls('Object')
    obj.var('Name', 'name')
    obj.var('Outer', ('object', obj))
    vector = obj.struct('Vector', [('X', 'float'), ('Y', 'float'), ('Z', 'float')])
    obj.struct('Rotator', [('Pitch', 'int'), ('Yaw', 'int'), ('Roll', 'int')])
    obj.var('Class', ('class', obj))

    op = FUNC_FINAL | FUNC_NATIVE | FUNC_STATIC | FUNC_OPERATOR

    def native(name, index, params, ret=None, flags=FUNC_FINAL | FUNC_NATIVE, friendly=None):
        obj.func(name, params=params, ret=ret, flags=flags, native=index, friendly=friendly)

    native('Concat_StrStr', 112, [('A', 'string'), ('B', 'string')], 'string', op, '$')
    native('GotoState', 113, [('NewState', 'name', CPF_OPTIONAL), ('Label', 'name', CPF_OPTIONAL)])
    native('EqualEqual_ObjectObject', 114, [('A', ('object', obj)), ('B', ('object', obj))], 'bool', op, '==')
    native('EqualEqual_StrStr', 122, [('A', 'string'), ('B', 'string')], 'bool', op, '==')
    native('Len', 125, [('S', 'string')], 'int')
    native('InStr', 126, [('S', 'string'), ('T', 'string')], 'int')
    native('Mid', 127, [('S', 'string'), ('I', 'int'), ('J', 'int', CPF_OPTIONAL)], 'string')
    native('Left', 128, [('S', 'string'), ('I', 'int')], 'string')
    native('Not_PreBool', 129, [('A', 'bool')], 'bool', op | FUNC_PRE_OPERATOR, '!')
    native('AndAnd_BoolBool', 130, [('A', 'bool'), ('B', 'bool', CPF_SKIP)], 'bool', op, '&&')
    native('OrOr_BoolBool', 132, [('A', 'bool'), ('B', 'bool', CPF_SKIP)], 'bool', op, '||')
    native('Multiply_IntInt', 144, [('A', 'int'), ('B', 'int')], 'int', op, '*')
    native('Add_IntInt', 146, [('A', 'int'), ('B', 'int')], 'int', op, '+')
    native('Less_IntInt', 150, [('A', 'int'), ('B', 'int')], 'bool', op, '<')
    native('EqualEqual_IntInt', 154, [('A', 'int'), ('B', 'int')], 'bool', op, '==')
    native('AddEqual_IntInt', 161, [('A', 'int', CPF_OUT), ('B', 'int')], 'int', op, '+=')
    native('AddAdd_Int', 165, [('A', 'int', CPF_OUT)], 'int', op, '++')
    native('Add_VectorVector', 215, [('A', ('struct', vector)), ('B', ('struct', vector))],
           ('struct', vector), op, '+')
    native('VSize', 225, [('A', ('struct', vector))], 'float')
    native('Log', 231, [('S', 'string'), ('Tag', 'name', CPF_OPTIONAL)])
    native('EqualEqual_NameName', 254, [('A', 'name'), ('B', 'name')], 'bool', op, '==')
    native('IsInState', 281, [('TestState', 'name')], 'bool')
    native('GetStateName', 284, [], 'name')
    # Natives the test registers itself: an iterator and a latent function,
    # called by name rather than by index.
    obj.func('Counter', params=[('N', 'int'), ('Value', 'int', CPF_OUT)],
             flags=FUNC_FINAL | FUNC_NATIVE | FUNC_ITERATOR)
    obj.func('Wait', params=[('Seconds', 'float')], flags=FUNC_FINAL | FUNC_NATIVE | FUNC_LATENT)
    return core, obj, vector


def build_game(core_obj, vector):
    def fn_of(name):
        for f in core_obj.funcs:
            if f.name == name:
                return f
        raise KeyError(name)

    def nat(name, *args):
        f = fn_of(name)
        return ('native', f.native) + args if f.native else ('final', f) + args

    def add(a, b):
        return nat('Add_IntInt', a, b)

    game = Pkg('Game')
    base = game.cls('Base', core_obj)
    kind = base.enum('EKind', ['K_One', 'K_Two', 'K_Three'])
    pair = base.struct('Pair', [('A', 'int'), ('B', 'int')])
    base.var('Count', 'int')
    base.var('Ratio', 'float')
    base.var('Label', 'string')
    base.var('Items', ('array', 'int'))
    base.var('Pos', ('struct', vector))
    base.var('Slots', 'int', dim=4)
    base.var('Flag', 'bool')
    base.var('Other', ('object', base))
    base.var('Kind', ('byte', kind))
    ping = base.func('OnPing', ret='int', flags=FUNC_DELEGATE)
    base.var('__OnPing__Delegate', ('delegate', ping))
    ping.code = [('return', ('int', 1))]
    base.defaults = [('Count', 5), ('Ratio', 0.5), ('Label', 'hi'), ('Items', [1, 2, 3]),
                     ('Pos', (1.0, 2.0, 3.0)), ('Slots', ('index', 1, 7)), ('Flag', True),
                     ('Kind', 1)]

    child = game.cls('Child', base)
    child.defaults = [('Count', 6)]
    actor = game.cls('Actorish', base)

    f = base.func('Sum', params=[('N', 'int')], ret='int', locals=[('I', 'int'), ('S', 'int')],
                  flags=FUNC_STATIC)
    f.code = [
        ('let', ('local', 'S'), ('int0',)),
        ('let', ('local', 'I'), ('int0',)),
        ('label', 'top'),
        ('jumpifnot', 'end', nat('Less_IntInt', ('local', 'I'), ('local', 'N'))),
        nat('AddEqual_IntInt', ('local', 'S'), ('local', 'I')),
        nat('AddAdd_Int', ('local', 'I')),
        ('jump', 'top'),
        ('label', 'end'),
        ('return', ('local', 'S')),
        ('return', ('nothing',)),
    ]

    virt = base.func('Virt', ret='int')
    virt.code = [('return', ('int', 1))]
    cvirt = child.func('Virt', ret='int')
    cvirt.sup = virt
    cvirt.code = [('return', add(('final', virt), ('int', 10)))]

    f = base.func('Parse', params=[('Opts', 'string'), ('Key', 'string')], ret='string',
                  locals=[('I', 'int'), ('Rest', 'string')], flags=FUNC_STATIC)
    f.code = [
        ('let', ('local', 'I'), nat('InStr', ('local', 'Opts'),
                                    nat('Concat_StrStr', ('local', 'Key'), ('str', '=')))),
        ('jumpifnot', 'found', nat('Less_IntInt', ('local', 'I'), ('int0',))),
        ('return', ('str', '')),
        ('label', 'found'),
        ('let', ('local', 'Rest'), nat('Mid', ('local', 'Opts'),
                                       add(add(('local', 'I'), nat('Len', ('local', 'Key'))), ('int1',)))),
        ('let', ('local', 'I'), nat('InStr', ('local', 'Rest'), ('str', '?'))),
        ('jumpifnot', 'done', nat('Not_PreBool', nat('Less_IntInt', ('local', 'I'), ('int0',)))),
        ('let', ('local', 'Rest'), nat('Left', ('local', 'Rest'), ('local', 'I'))),
        ('label', 'done'),
        ('return', ('local', 'Rest')),
    ]

    # switch (V) { case 1: return 10; case 2: R = 20; case 3: R += 3; break; default: R = 99; }
    f = base.func('SwitchTest', params=[('V', 'int')], ret='int', locals=[('R', 'int')])
    f.code = [
        ('switch', 4, ('local', 'V')),
        ('case', 'c2', ('int1',)),
        ('return', ('int', 10)),
        ('label', 'c2'),
        ('case', 'c3', ('int', 2)),
        ('let', ('local', 'R'), ('int', 20)),
        ('label', 'c3'),
        ('case', 'cd', ('int', 3)),
        nat('AddEqual_IntInt', ('local', 'R'), ('int', 3)),
        ('jump', 'out'),
        ('label', 'cd'),
        ('case', None),
        ('let', ('local', 'R'), ('int', 99)),
        ('label', 'out'),
        ('return', ('local', 'R')),
    ]

    swap = base.func('Swap', params=[('A', 'int', CPF_OUT), ('B', 'int', CPF_OUT)], locals=[('T', 'int')])
    swap.code = [('let', ('local', 'T'), ('local', 'A')), ('let', ('local', 'A'), ('local', 'B')),
                 ('let', ('local', 'B'), ('local', 'T')), ('return', ('nothing',))]
    f = base.func('UseSwap', ret='int', locals=[('X', 'int'), ('Y', 'int')])
    f.code = [('let', ('local', 'X'), ('int1',)), ('let', ('local', 'Y'), ('int', 2)),
              ('final', swap, ('local', 'X'), ('local', 'Y')),
              ('return', add(nat('Multiply_IntInt', ('local', 'X'), ('int', 10)), ('local', 'Y')))]

    vx = vector.fields[0]
    f = base.func('StructCopy', ret='float', locals=[('A', ('struct', vector)), ('B', ('struct', vector))])
    f.code = [('let', ('member', vx, ('local', 'A')), ('float', 1.0)),
              ('let', ('local', 'B'), ('local', 'A')),
              ('let', ('member', vx, ('local', 'B')), ('float', 5.0)),
              ('return', ('member', vx, ('local', 'A')))]

    # Items[5] = 7 grows the array; reading Items[10] warns and gives 0;
    # Length = 2 shrinks it. Result: 6 * 100 + 7 * 10 + 0 + 2 (old length 3).
    f = base.func('ArrayTest', ret='int', locals=[('R', 'int'), ('L', 'int')])
    f.code = [('let', ('local', 'L'), ('dynlen', ('inst', 'Items'))),
              ('let', ('dynel', ('int', 5), ('inst', 'Items')), ('int', 7)),
              ('let', ('local', 'R'), nat('Multiply_IntInt', ('dynlen', ('inst', 'Items')), ('int', 100))),
              nat('AddEqual_IntInt', ('local', 'R'),
                  nat('Multiply_IntInt', ('dynel', ('int', 5), ('inst', 'Items')), ('int', 10))),
              nat('AddEqual_IntInt', ('local', 'R'), ('dynel', ('int', 10), ('inst', 'Items'))),
              ('let', ('dynlen', ('inst', 'Items')), ('int', 2)),
              ('return', add(add(('local', 'R'), ('local', 'L')), ('dynlen', ('inst', 'Items'))))]

    # Slots[2] = 5; result Slots[2] * 10 + Slots[1] (7 from the defaults);
    # Slots[9] is clamped to Slots[3] with a warning and reads 0.
    f = base.func('StaticArrayTest', ret='int')
    f.code = [('let', ('arrel', ('int', 2), ('inst', 'Slots')), ('int', 5)),
              ('return', add(add(nat('Multiply_IntInt', ('arrel', ('int', 2), ('inst', 'Slots')), ('int', 10)),
                                 ('arrel', ('int1',), ('inst', 'Slots'))),
                             ('arrel', ('int', 9), ('inst', 'Slots'))))]

    bump = base.func('Bump', ret='bool')
    bump.code = [nat('AddAdd_Int', ('inst', 'Count')), ('return', ('true',))]
    f = base.func('SkipTest', ret='int', locals=[('B', 'bool')])
    f.code = [('let', ('inst', 'Count'), ('int0',)),
              ('let', ('local', 'B'), nat('AndAnd_BoolBool', ('false',), ('skip', ('final', bump)))),
              ('let', ('local', 'B'), nat('OrOr_BoolBool', ('true',), ('skip', ('final', bump)))),
              ('let', ('local', 'B'), nat('AndAnd_BoolBool', ('true',), ('skip', ('final', bump)))),
              ('return', ('inst', 'Count'))]

    f = base.func('NoneTest', ret='int')
    f.code = [('let', ('inst', 'Other'), ('none',)),
              ('let', ('ctx', ('inst', 'Other'), ('inst', 'Count')), ('int', 4)),
              ('return', ('ctx', ('inst', 'Other'), ('inst', 'Count')))]

    # foreach Counter(10, V) { if (V == 5) break; if (V == 2) continue; S += V; }
    f = base.func('IterTest', ret='int', locals=[('V', 'int'), ('S', 'int')])
    f.code = [('iter', nat('Counter', ('int', 10), ('local', 'V')), 'pop'),
              ('jumpifnot', 'notfive', nat('EqualEqual_IntInt', ('local', 'V'), ('int', 5))),
              ('iterpop',),
              ('jump', 'after'),
              ('label', 'notfive'),
              ('jumpifnot', 'nottwo', nat('EqualEqual_IntInt', ('local', 'V'), ('int', 2))),
              ('jump', 'next'),
              ('label', 'nottwo'),
              nat('AddEqual_IntInt', ('local', 'S'), ('local', 'V')),
              ('label', 'next'),
              ('iternext',),
              ('label', 'pop'),
              ('iterpop',),
              ('label', 'after'),
              ('return', ('local', 'S'))]
    f = base.func('IterAll', ret='int', locals=[('V', 'int'), ('S', 'int')])
    f.code = [('iter', nat('Counter', ('int', 4), ('local', 'V')), 'pop'),
              nat('AddEqual_IntInt', ('local', 'S'), ('local', 'V')),
              ('iternext',),
              ('label', 'pop'),
              ('iterpop',),
              ('return', ('local', 'S'))]

    f = base.func('DefaultTest', ret='int')
    f.code = [('return', add(nat('Multiply_IntInt', ('default', 'Count'), ('int', 100)),
                             add(nat('Multiply_IntInt', ('classctx', ('obj', base), ('default', 'Count')),
                                     ('int', 10)),
                                 ('classctx', ('obj', base), ('virtual', 'Sum', ('int', 3))))))]

    pong = base.func('Pong', ret='int')
    pong.code = [('return', ('int', 7))]
    dlg = base.find('__OnPing__Delegate')
    f = base.func('DelegateTest', ret='int', locals=[('R', 'int')])
    f.code = [('let', ('local', 'R'), nat('Multiply_IntInt', ('virtual', 'OnPing'), ('int', 10))),
              ('let', ('inst', dlg), ('delegate', 'Pong')),
              ('return', add(('local', 'R'), ('virtual', 'OnPing')))]

    f = base.func('CastTest', ret='string')
    cat = lambda a, b: nat('Concat_StrStr', a, b)       # noqa: E731
    f.code = [('return', cat(cat(cat(cat(cat(cat(('cast', 0x53, ('int', 42)), ('str', ',')),
                                             ('cast', 0x55, ('float', 1.5))), ('str', ',')),
                                     ('cast', 0x54, ('true',))), ('str', ',')),
                             ('cast', 0x53, ('cast', 0x4A, ('str', '  -17x')))))]

    f = base.func('ObjCastTest', ret='int', locals=[('O', ('object', base))])
    f.code = [('let', ('local', 'O'), ('new', ('nothing',), ('nothing',), ('nothing',), ('obj', child))),
              ('return', add(nat('Multiply_IntInt',
                                 ('cast', 0x41, nat('Not_PreBool', nat('EqualEqual_ObjectObject',
                                                                       ('dyncast', child, ('local', 'O')),
                                                                       ('none',)))), ('int', 10)),
                             ('cast', 0x41, nat('EqualEqual_ObjectObject',
                                                ('dyncast', actor, ('local', 'O')), ('none',)))))]

    f = base.func('Loop')
    f.code = [('label', 'x'), ('jump', 'x')]
    f = base.func('Rec', params=[('N', 'int')], ret='int')
    f.code = [('return', ('virtual', 'Rec', add(('local', 'N'), ('int1',))))]

    f = base.func('NameTest', ret='bool')
    f.code = [('return', nat('EqualEqual_NameName', ('name', 'foo'), ('name', 'FOO')))]

    add_count = base.func('AddCount', params=[('X', 'int')], ret='int')
    add_count.code = [('return', add(('inst', 'Count'), ('local', 'X')))]
    f = base.func('CtxArgs', ret='int')
    f.code = [('let', ('inst', 'Other'), ('new', ('nothing',), ('nothing',), ('nothing',), ('obj', child))),
              ('let', ('ctx', ('inst', 'Other'), ('inst', 'Count')), ('int', 100)),
              ('let', ('inst', 'Count'), ('int', 3)),
              ('return', ('ctx', ('inst', 'Other'), ('virtual', 'AddCount', ('inst', 'Count'))))]

    pa = pair.fields[0]
    f = base.func('StructEqTest', ret='bool', locals=[('P', ('struct', pair)), ('Q', ('struct', pair))])
    f.code = [('let', ('member', pa, ('local', 'P')), ('int1',)),
              ('let', ('member', pa, ('local', 'Q')), ('int1',)),
              ('return', ('structeq', pair, ('local', 'P'), ('local', 'Q')))]

    f = base.func('VecTest', ret='float', locals=[('V', ('struct', vector))])
    f.code = [('let', ('local', 'V'), ('vect', 3.0, 4.0, 0.0)),
              ('return', nat('VSize', nat('Add_VectorVector', ('local', 'V'), ('vect', 0.0, 0.0, 0.0))))]

    # States. Idle is the auto state and overrides Virt; Walking runs code
    # that waits on a latent function between two changes to Count.
    idle = actor.state('Idle', flags=STATE_AUTO)
    sv = idle.func('Virt', ret='int')
    sv.code = [('return', ('int', 2))]
    bs = idle.func('BeginState')
    bs.code = [nat('Log', ('str', 'enter Idle')), ('return', ('nothing',))]
    es = idle.func('EndState')
    es.code = [nat('Log', ('str', 'leave Idle')), ('return', ('nothing',))]
    f = actor.func('GlobalTest', ret='int')
    f.code = [('return', add(nat('Multiply_IntInt', ('virtual', 'Virt'), ('int', 10)), ('global', 'Virt')))]
    walk = actor.state('Walking')
    walk.code = [('label', 'Begin'),
                 nat('AddEqual_IntInt', ('inst', 'Count'), ('int1',)),
                 nat('Wait', ('float', 1.0)),
                 nat('AddEqual_IntInt', ('inst', 'Count'), ('int', 10)),
                 ('gotolabel', ('name', 'Finish')),
                 nat('AddEqual_IntInt', ('inst', 'Count'), ('int', 1000)),
                 ('label', 'Finish'),
                 nat('AddEqual_IntInt', ('inst', 'Count'), ('int', 100)),
                 ('stop',),
                 ('label', '__labeltable'),
                 ('labeltable', [('Begin', 'Begin'), ('Finish', 'Finish')])]
    return game


def main(out):
    os.makedirs(out, exist_ok=True)
    core, core_obj, vector = build_core()
    game = build_game(core_obj, vector)
    writers, paths = {}, []
    for pkg in (core, game):
        w = Writer(pkg)
        path = os.path.join(out, pkg.name + '.u')
        w.write(path)
        writers[pkg.name + '.u'] = w
        paths.append(path)
    verify(paths, writers)
    print('fixtures written and verified:', ', '.join(os.path.basename(p) for p in paths))


if __name__ == '__main__':
    main(sys.argv[1])
