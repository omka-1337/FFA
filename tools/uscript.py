"""UnrealScript bytecode reader for Unreal Engine 2 build 2226.

Two facts shape this module:

1. A UFunction's serialised ScriptSize is the size the bytecode occupies *in
   memory*, not on disk. Object references and names are compact indices on disk
   (1 to 5 bytes) but pointers in memory (4 bytes). So a script cannot be skipped
   by its byte count: it has to be walked token by token.

2. Jump targets are memory offsets into that in-memory array. To turn them into
   labels we track both offsets for every token. Summing the memory sizes and
   comparing the total against ScriptSize is a second, independent correctness
   check on top of the walk landing exactly on the record tail.

See docs/package-format.md for the record layouts this relies on.
"""
import struct, sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R

EX_EXTENDED_NATIVE = 0x60
EX_FIRST_NATIVE = 0x70
EX_END_FUNCTION_PARMS = 0x16

FUNC_NET = 0x00000040
FUNC_NATIVE = 0x00000400
# Every function carries exactly one access specifier, so these bits are what
# tells a real FunctionFlags word from a misaligned read.
FUNC_ACCESS = 0x000E0000      # Public | Private | Protected
FUNC_OPERATOR = 0x00001000
FUNC_PRE_OPERATOR = 0x00000010

# Memory footprint of each operand kind. Disk sizes are read separately; these
# are what the in-memory script array uses, which is what ScriptSize counts and
# what jump offsets index into.
MEM = {'obj': 4, 'name': 4}

OPS = {
    0x00: ('LocalVariable', ['obj']),
    0x01: ('InstanceVariable', ['obj']),
    0x02: ('DefaultVariable', ['obj']),
    0x03: ('StateVariable', ['obj']),
    0x04: ('Return', ['expr']),
    0x05: ('Switch', ['u8', 'expr']),
    0x06: ('Jump', ['u16']),
    0x07: ('JumpIfNot', ['u16', 'expr']),
    0x08: ('Stop', []),
    0x09: ('Assert', ['u16', 'expr']),
    0x0A: ('Case', ['case']),
    0x0B: ('Nothing', []),
    0x0C: ('LabelTable', ['labels']),
    0x0D: ('GotoLabel', ['expr']),
    0x0E: ('EatString', ['expr']),
    0x0F: ('Let', ['expr', 'expr']),
    0x10: ('DynArrayElement', ['expr', 'expr']),
    0x11: ('New', ['expr', 'expr', 'expr', 'expr']),
    0x12: ('ClassContext', ['expr', 'u16', 'u8', 'expr']),
    0x13: ('Metacast', ['obj', 'expr']),
    0x14: ('LetBool', ['expr', 'expr']),
    0x15: ('EndParmValue', []),
    0x16: ('EndFunctionParms', []),
    0x17: ('Self', []),
    0x18: ('Skip', ['u16', 'expr']),
    0x19: ('Context', ['expr', 'u16', 'u8', 'expr']),
    0x1A: ('ArrayElement', ['expr', 'expr']),
    0x1B: ('VirtualFunction', ['name', 'parms']),
    0x1C: ('FinalFunction', ['obj', 'parms']),
    0x1D: ('IntConst', ['u32']),
    0x1E: ('FloatConst', ['f32']),
    0x1F: ('StringConst', ['str']),
    0x20: ('ObjectConst', ['obj']),
    0x21: ('NameConst', ['name']),
    0x22: ('RotationConst', ['u32', 'u32', 'u32']),
    0x23: ('VectorConst', ['f32', 'f32', 'f32']),
    0x24: ('ByteConst', ['u8']),
    0x25: ('IntZero', []),
    0x26: ('IntOne', []),
    0x27: ('True', []),
    0x28: ('False', []),
    0x29: ('NativeParm', ['obj']),
    0x2A: ('NoObject', []),
    0x2B: ('NoDelegate', []),
    0x2C: ('IntConstByte', ['u8']),
    0x2D: ('BoolVariable', ['expr']),
    0x2E: ('DynamicCast', ['obj', 'expr']),
    0x2F: ('Iterator', ['expr', 'u16']),
    0x30: ('IteratorPop', []),
    0x31: ('IteratorNext', []),
    0x32: ('StructCmpEq', ['obj', 'expr', 'expr']),
    0x33: ('StructCmpNe', ['obj', 'expr', 'expr']),
    0x34: ('UnicodeStringConst', ['ustr']),
    0x35: ('InstanceDelegate', ['name']),
    0x36: ('StructMember', ['obj', 'expr']),
    0x37: ('DynArrayLength', ['expr']),
    0x38: ('GlobalFunction', ['name', 'parms']),
}

# 0x39 is EX_PrimitiveCast: a conversion code byte, then one expression. The
# codes are 0x39..0x5F, 0x39 itself being RotatorToVector; 7778 of the 8208
# casts in the game's script carry a code from 0x3A up and the other 215 carry
# 0x39. The codes also stand alone as tokens, 53 times, all at statement level.
EX_PRIMITIVE_CAST = 0x39
# 0x39..0x5F as conversions, one expression each. The target
# type of each is only needed for pretty printing, so unknown ones stay generic.
CASTS = {
    0x39: 'vector', 0x3A: 'int', 0x3B: 'bool', 0x3C: 'float', 0x3D: 'byte',
    0x3E: 'bool', 0x3F: 'float', 0x40: 'byte', 0x41: 'int', 0x42: 'float',
    0x43: 'byte', 0x44: 'int', 0x45: 'bool', 0x46: 'string', 0x47: 'bool',
    0x48: 'bool', 0x49: 'byte', 0x4A: 'int', 0x4B: 'bool', 0x4C: 'float',
    0x4D: 'vector', 0x4E: 'rotator', 0x4F: 'bool', 0x50: 'rotator',
    0x51: 'bool',
}
for _op in range(0x39, 0x60):
    OPS.setdefault(_op, (f'Cast_{CASTS.get(_op, "x%02x" % _op)}', ['expr']))

# Codes in the conversion range that stand alone are tokens of their own. Each
# layout was chosen by measurement, the one that makes the most functions both
# end on their record and agree with their declared size:
#   0x43 DelegateFunction: the delegate property, the name it is called by, and
#        the parameters; 8599 to 8623 aligned, 8547 to 8595 sized.
#   0x44 DelegateProperty: one name; with it all 8638 functions pass both.
#   0x40 DynArrayInsert and 0x41 DynArrayRemove: the array, an index and a
#        count; 0x45 LetDelegate: the delegate and its value. Lengths cannot
#        tell these apart, the extra operands parse as statements of their
#        own, so the count was chosen by what follows: read with one operand,
#        each leaves its others behind as bare expressions, `0;` and
#        `pris.Length;` after `pris.Remove`.
OPS[0x40] = ('DynArrayInsert', ['expr', 'expr', 'expr'])
OPS[0x41] = ('DynArrayRemove', ['expr', 'expr', 'expr'])
OPS[0x43] = ('DelegateFunction', ['obj', 'name', 'parms'])
OPS[0x44] = ('DelegateProperty', ['name'])
OPS[0x45] = ('LetDelegate', ['expr', 'expr'])


def read_tail(b, e):
    """FunctionFlags and tail length of a UFunction record.

    The tail is iNative u16, OperPrecedence u8, FunctionFlags u32, and, when the
    flags carry FUNC_Net, RepOffset u16. Read at the 9 byte position first: when
    that reading has FUNC_Net and exactly one access specifier, it is the tail.
    Testing only for an access bit at the 7 byte position is not enough, since
    there the u32 takes its top half from RepOffset, and 121 functions have a
    RepOffset whose bits fall on the access specifiers.
    """
    end = e['off'] + e['size']
    single = lambda f: bin(f & FUNC_ACCESS).count('1') == 1
    f6 = struct.unpack_from('<I', b, end - 6)[0]
    if f6 & FUNC_NET and single(f6):
        return f6, 9
    f4 = struct.unpack_from('<I', b, end - 4)[0]
    if single(f4):
        return f4, 7
    return f4, 7


class Node:
    """One bytecode token: operands, children, and where it sits in the engine's
    in-memory script array."""
    __slots__ = ('op', 'kids', 'vals', 'mem_off', 'mem_size', 'native')

    def __init__(self, op='?'):
        self.op = op
        self.kids = []
        self.vals = []
        self.mem_off = 0
        self.mem_size = 0
        self.native = 0


class Script:
    """Parses the bytecode of a package's functions into Node trees."""

    # A desynced parse must not be allowed to run off the end of the record and
    # allocate nodes across the rest of the file: that is unbounded memory.
    MAX_NODES = 20000

    def __init__(self, pkg):
        self.p = pkg
        self.b = pkg.b
        self.limit = 0          # disk offset the current record ends at
        self.budget = 0         # remaining node allowance for this record

    def token(self, r, mem):
        if r.p >= self.limit:
            raise ValueError('ran past the end of the record')
        self.budget -= 1
        if self.budget <= 0:
            raise ValueError('node budget exhausted')
        n = Node()
        n.mem_off = mem
        size = 1
        op = r.u8()
        if op >= EX_EXTENDED_NATIVE:
            if op >= EX_FIRST_NATIVE:
                n.native = op
            else:
                n.native = ((op - EX_EXTENDED_NATIVE) << 8) + r.u8()
                size += 1
            n.op = 'NativeCall'
            kids, ksize = self.parms(r, mem + size)
            n.kids = kids
            n.mem_size = size + ksize
            return n
        if op == EX_PRIMITIVE_CAST:
            # 0x39 is a prefix: the next byte is the conversion, then one
            # expression. Read as two tokens it gives the same length and the
            # wrong meaning, vector(float(1)) for float(1), and the code 0x39
            # itself, RotatorToVector, as a second prefix.
            code = r.u8()
            if not EX_PRIMITIVE_CAST <= code < 0x60:
                raise ValueError('primitive cast with conversion 0x%02x' % code)
            n.op = 'Cast_' + CASTS.get(code, 'x%02x' % code)
            kid = self.token(r, mem + 2)
            n.kids.append(kid)
            n.mem_size = 2 + kid.mem_size
            return n
        if op not in OPS:
            raise ValueError('unknown opcode 0x%02x' % op)
        n.op, kinds = OPS[op]
        for k in kinds:
            if k == 'expr':
                kid = self.token(r, mem + size)
                n.kids.append(kid)
                size += kid.mem_size
            elif k == 'parms':
                kids, ksize = self.parms(r, mem + size)
                n.kids.extend(kids)
                size += ksize
            elif k == 'obj':
                n.vals.append(('obj', r.idx()))
                size += MEM['obj']
            elif k == 'name':
                n.vals.append(('name', r.idx()))
                size += MEM['name']
            elif k == 'u8':
                n.vals.append(('int', r.u8()))
                size += 1
            elif k == 'u16':
                n.vals.append(('int', r.u16()))
                size += 2
            elif k == 'u32':
                n.vals.append(('int', r.u32()))
                size += 4
            elif k == 'f32':
                n.vals.append(('float', struct.unpack_from('<f', self.b, r.p)[0]))
                r.p += 4
                size += 4
            elif k == 'str':
                start = r.p
                while self.b[r.p]:
                    r.p += 1
                r.p += 1
                n.vals.append(('str', self.b[start:r.p - 1].decode('latin-1')))
                size += r.p - start
            elif k == 'ustr':
                start = r.p
                while struct.unpack_from('<H', self.b, r.p)[0]:
                    r.p += 2
                r.p += 2
                n.vals.append(('str', self.b[start:r.p - 2].decode('utf-16-le')))
                size += r.p - start
            elif k == 'case':
                off = r.u16()
                n.vals.append(('int', off))
                size += 2
                if off != 0xFFFF:
                    kid = self.token(r, mem + size)
                    n.kids.append(kid)
                    size += kid.mem_size
            elif k == 'labels':
                while True:
                    nm = r.idx()
                    r.u32()
                    size += MEM['name'] + 4
                    n.vals.append(('name', nm))
                    if nm == self.p.none_idx:
                        break
        n.mem_size = size
        return n

    def parms(self, r, mem):
        """Expressions up to and including the EndFunctionParms token."""
        kids, size = [], 0
        while True:
            if r.p >= self.limit:
                raise ValueError('ran past the end of the record')
            if self.b[r.p] == EX_END_FUNCTION_PARMS:
                r.p += 1
                return kids, size + 1
            kid = self.token(r, mem + size)
            kids.append(kid)
            size += kid.mem_size

    def function(self, e):
        """Parse one UFunction export.

        Returns (statements, info). info.aligned is whether the walk ended
        exactly on the record tail; info.sized is whether the memory sizes add
        up to the declared ScriptSize. Both must hold for the parse to be right.
        """
        end = e['off'] + e['size']
        flags, taillen = read_tail(self.b, e)
        target = end - taillen
        r = R(self.b, e['off'])
        for _ in range(7):      # None of the empty property block, Super, Next,
            r.idx()             # ScriptText, Children, FriendlyName, one unused
        r.u32(); r.u32()        # Line, TextPos
        declared = r.u32()      # ScriptSize, counted in memory bytes
        self.limit = target
        self.budget = self.MAX_NODES
        stmts, mem = [], 0
        while r.p < target:
            n = self.token(r, mem)
            stmts.append(n)
            mem += n.mem_size
        info = dict(flags=flags, declared=declared, mem=mem,
                    aligned=(r.p == target), sized=(mem == declared))
        return stmts, info


def struct_code(script, e):
    """Parse the bytecode of a State, whose tail length is not known in advance.

    A State shares the struct header of a Function, and its code runs until the
    tokens' memory sizes reach the declared ScriptSize. Returns (statements,
    info): info.end is the disk offset the code ends at, info.sized whether the
    walk met ScriptSize exactly. What follows the code is the state's tail.
    """
    b = script.b
    r = R(b, e['off'])
    for _ in range(7):          # as for a function
        r.idx()
    r.u32(); r.u32()            # Line, TextPos
    declared = r.u32()
    script.limit = e['off'] + e['size']
    script.budget = script.MAX_NODES
    stmts, mem = [], 0
    while mem < declared:
        n = script.token(r, mem)
        stmts.append(n)
        mem += n.mem_size
    return stmts, dict(declared=declared, mem=mem, end=r.p, sized=(mem == declared))


def build_native_table(packages):
    """native index -> declaration, gathered from every package.

    A native function's index is in the UFunction tail; its operator symbol, if
    it is an operator, is FriendlyName, field 6 of the record.
    """
    table = {}
    for p in packages:
        for e in p.exports:
            if p.classof(e) != 'Function':
                continue
            end = e['off'] + e['size']
            flags, back = read_tail(p.b, e)
            if not (flags & FUNC_NATIVE):
                continue
            idx = struct.unpack_from('<H', p.b, end - back)[0]
            if not idx or idx in table:
                continue
            r = R(p.b, e['off'])
            fields = [r.idx() for _ in range(7)]
            friendly = p.names[fields[5]] if fields[5] < len(p.names) else e['name']
            table[idx] = dict(name=e['name'], friendly=friendly, flags=flags,
                              operator=bool(flags & FUNC_OPERATOR),
                              preop=bool(flags & FUNC_PRE_OPERATOR))
    return table


def main(paths):
    pkgs = [Package(p) for p in sorted(paths)]
    table = build_native_table(pkgs)
    print('native declarations found: %d' % len(table))
    print('%-20s %7s %8s %8s %8s' % ('package', 'funcs', 'aligned', 'sized', 'both'))
    tot = collections.Counter()
    for p in pkgs:
        s = Script(p)
        funcs = [e for e in p.exports if p.classof(e) == 'Function']
        a = z = both = 0
        for e in funcs:
            try:
                _, info = s.function(e)
            except Exception:
                continue
            a += info['aligned']
            z += info['sized']
            both += info['aligned'] and info['sized']
        tot['f'] += len(funcs); tot['a'] += a; tot['z'] += z; tot['both'] += both
        print('%-20s %7d %8d %8d %8d' % (p.name, len(funcs), a, z, both))
    print('%-20s %7d %8d %8d %8d  (%.1f%% fully checked)' %
          ('TOTAL', tot['f'], tot['a'], tot['z'], tot['both'],
           100.0 * tot['both'] / max(tot['f'], 1)))


if __name__ == '__main__':
    main(sys.argv[1:])
