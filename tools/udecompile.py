"""Render UnrealScript bytecode back into readable source.

Control flow is printed honestly as labels and gotos. The bytecode stores jumps
as memory offsets, so that is what the engine actually does; recovering if/else
and loops is a separate analysis pass and is not attempted here. Everything else
(expressions, operators, calls, casts, constants) is rendered as source.

See docs/package-format.md for the formats this builds on.
"""
import sys, os, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package
from uscript import Script, build_native_table, read_tail, FUNC_NATIVE
from uclass import Reader, CPF_PARM, CPF_RETURN, CPF_OUT, CPF_OPTIONAL, CPF_COERCE
from udefaults import World, Tagged

# Statements that end a basic block, used only to keep blank lines sensible.
TERMINATORS = {'Return', 'Jump', 'Stop', 'GotoLabel'}


class Printer:
    def __init__(self, pkg, natives):
        self.p = pkg
        self.nat = natives
        self.rd = Reader(pkg)
        self.owner = 0          # export index of the class being printed

    # ---------------------------------------------------------------- helpers
    def ref(self, node, i=0):
        """Name behind the i-th object or name operand of a node."""
        kind, val = node.vals[i]
        if kind == 'obj':
            return self.p.refname(val)
        if kind == 'name':
            return self.p.names[val] if val < len(self.p.names) else '?'
        return str(val)

    def ival(self, node, i):
        return node.vals[i][1]

    def args(self, kids):
        return ', '.join(self.expr(k) for k in kids)

    # ------------------------------------------------------------ expressions
    def expr(self, n):
        op = n.op
        if op == 'NativeCall':
            return self.native_call(n)
        if op in ('LocalVariable', 'InstanceVariable', 'DefaultVariable',
                  'StateVariable', 'NativeParm'):
            return self.ref(n)
        if op == 'Self':
            return 'self'
        if op == 'Nothing':
            return ''
        if op == 'NoObject':
            return 'none'
        if op == 'NoDelegate':
            return 'none'
        if op == 'True':
            return 'true'
        if op == 'False':
            return 'false'
        if op == 'IntZero':
            return '0'
        if op == 'IntOne':
            return '1'
        if op in ('IntConst', 'IntConstByte', 'ByteConst'):
            return str(self.ival(n, 0))
        if op == 'FloatConst':
            return repr(round(self.ival(n, 0), 6))
        if op in ('StringConst', 'UnicodeStringConst'):
            return '"%s"' % self.ival(n, 0).replace('"', '\\"')
        if op == 'NameConst':
            return "'%s'" % self.ref(n)
        if op == 'ObjectConst':
            return self.ref(n)
        if op == 'VectorConst':
            return 'vect(%g, %g, %g)' % tuple(v for _, v in n.vals[:3])
        if op == 'RotationConst':
            return 'rot(%d, %d, %d)' % tuple(v for _, v in n.vals[:3])
        if op == 'Context' and len(n.kids) == 2:
            return '%s.%s' % (self.expr(n.kids[0]), self.expr(n.kids[1]))
        if op == 'ClassContext' and len(n.kids) == 2:
            return '%s.static.%s' % (self.expr(n.kids[0]), self.expr(n.kids[1]))
        if op == 'StructMember':
            return '%s.%s' % (self.expr(n.kids[0]), self.ref(n))
        if op in ('ArrayElement', 'DynArrayElement') and len(n.kids) == 2:
            return '%s[%s]' % (self.expr(n.kids[1]), self.expr(n.kids[0]))
        if op == 'DynArrayLength':
            return '%s.Length' % self.expr(n.kids[0])
        if op == 'VirtualFunction':
            return '%s(%s)' % (self.ref(n), self.args(n.kids))
        if op == 'GlobalFunction':
            return 'global.%s(%s)' % (self.ref(n), self.args(n.kids))
        if op == 'FinalFunction':
            # A final call with no context runs on self, so a target owned by a
            # different class is the parent implementation: super.Name(...).
            ref = n.vals[0][1]
            prefix = ''
            if ref > 0 and self.p.exports[ref - 1]['pkg'] != self.owner:
                prefix = 'super.'
            elif ref < 0:
                prefix = 'super.'
            return '%s%s(%s)' % (prefix, self.ref(n), self.args(n.kids))
        if op == 'DynamicCast':
            return '%s(%s)' % (self.ref(n), self.expr(n.kids[0]))
        if op == 'Metacast':
            return 'class<%s>(%s)' % (self.ref(n), self.expr(n.kids[0]))
        if op.startswith('Cast_'):
            return '%s(%s)' % (op[5:], self.expr(n.kids[0]))
        if op in ('BoolVariable', 'EatString', 'Skip'):
            return self.expr(n.kids[-1]) if n.kids else ''
        if op == 'StructCmpEq':
            return '(%s == %s)' % (self.expr(n.kids[0]), self.expr(n.kids[1]))
        if op == 'StructCmpNe':
            return '(%s != %s)' % (self.expr(n.kids[0]), self.expr(n.kids[1]))
        if op == 'InstanceDelegate':
            return self.ref(n)
        if op == 'DelegateProperty':
            return self.ref(n)
        if op == 'DelegateFunction':
            # a call through the delegate property, by the delegate's name
            return '%s(%s)' % (self.p.names[n.vals[1][1]], self.args(n.kids))
        if op in ('DynArrayInsert', 'DynArrayRemove') and len(n.kids) == 3:
            return '%s.%s(%s, %s)' % (self.expr(n.kids[0]), op[8:],
                                      self.expr(n.kids[1]), self.expr(n.kids[2]))
        if op == 'New':
            return 'new(%s)' % self.args(n.kids)
        if n.kids:
            return '%s(%s)' % (op, self.args(n.kids))
        return op

    def native_call(self, n):
        d = self.nat.get(n.native)
        if not d:
            return 'native%d(%s)' % (n.native, self.args(n.kids))
        sym, name = d['friendly'], d['name']
        if d['operator'] and len(n.kids) == 2:
            return '(%s %s %s)' % (self.expr(n.kids[0]), sym, self.expr(n.kids[1]))
        if d['operator'] and len(n.kids) == 1:
            # a unary operator token is either pre or post; preop is flagged
            return ('%s%s' if d['preop'] else '%s%s') % (
                (sym, self.expr(n.kids[0])) if d['preop']
                else (self.expr(n.kids[0]), sym))
        return '%s(%s)' % (name, self.args(n.kids))

    # ------------------------------------------------------------- statements
    def targets(self, stmts):
        """Memory offsets that something jumps to."""
        out = set()

        def scan(n):
            if n.op in ('Jump', 'JumpIfNot', 'Iterator') and n.vals:
                out.add(self.ival(n, 0))
            if n.op == 'Case' and n.vals and self.ival(n, 0) != 0xFFFF:
                out.add(self.ival(n, 0))
            for k in n.kids:
                scan(k)
        for s in stmts:
            scan(s)
        return out

    def statement(self, n):
        op = n.op
        if op in ('Let', 'LetBool', 'LetDelegate') and len(n.kids) == 2:
            return '%s = %s;' % (self.expr(n.kids[0]), self.expr(n.kids[1]))
        if op == 'Return':
            inner = self.expr(n.kids[0]) if n.kids else ''
            return 'return %s;' % inner if inner else 'return;'
        if op == 'Jump':
            return 'goto L%04X;' % self.ival(n, 0)
        if op == 'JumpIfNot':
            return 'if (!(%s)) goto L%04X;' % (self.expr(n.kids[0]), self.ival(n, 0))
        if op == 'Switch':
            return 'switch (%s)' % self.expr(n.kids[-1])
        if op == 'Case':
            if self.ival(n, 0) == 0xFFFF:
                return 'default:'
            return 'case %s:   // next case at L%04X' % (
                self.expr(n.kids[0]) if n.kids else '?', self.ival(n, 0))
        if op == 'Stop':
            return 'stop;'
        if op == 'Nothing':
            return ''
        if op == 'GotoLabel':
            return 'goto %s;' % self.expr(n.kids[0])
        if op == 'Assert':
            return 'assert(%s);   // line %d' % (self.expr(n.kids[-1]), self.ival(n, 0))
        if op == 'Iterator':
            return 'foreach %s   // ends at L%04X' % (
                self.expr(n.kids[0]), self.ival(n, 0))
        if op == 'IteratorNext':
            return '// iterator next'
        if op == 'IteratorPop':
            return '// iterator done'
        if op == 'LabelTable':
            labels = [self.p.names[v] for k, v in n.vals if k == 'name']
            return '// label table: %s' % ', '.join(labels)
        return self.expr(n) + ';'

    def body(self, stmts, indent='\t'):
        lines, targets = [], self.targets(stmts)
        for n in stmts:
            if n.mem_off in targets:
                lines.append('L%04X:' % n.mem_off)
            text = self.statement(n)
            if text:
                lines.append(indent + text)
        return lines


def decompile_class(pkg, natives, name, out=sys.stdout, world=None):
    pr = Printer(pkg, natives)
    rd = pr.rd
    sc = Script(pkg)
    idx = next((i + 1 for i, e in enumerate(pkg.exports)
                if e['name'] == name and pkg.classof(e) == 'Class'), None)
    if idx is None:
        print('class %s not found in %s' % (name, pkg.name), file=out)
        return
    pr.owner = idx
    d = rd.field(idx)
    parent = pkg.refname(d['super']) if d['super'] else 'Object'
    print('class %s extends %s;   // %s' % (name, parent, pkg.name), file=out)
    print(file=out)
    members = rd.members(idx)
    for m in members:
        if m['cls'].endswith('Property'):
            print('var %s %s;' % (rd.typename(m), m['name']), file=out)
    for m in members:
        if m['cls'] != 'Function':
            continue
        flags, _ = read_tail(pkg.b, m['export'])
        sig = rd.signature(m)
        if flags & FUNC_NATIVE:
            print('\nnative %s;' % sig, file=out)
            continue
        locals_ = [x for x in rd.members(m['idx'])
                   if x['cls'].endswith('Property') and not (x['flags'] & CPF_PARM)]
        print('\n%s\n{' % sig, file=out)
        for l in locals_:
            print('\tlocal %s %s;' % (rd.typename(l), l['name']), file=out)
        if locals_:
            print(file=out)
        try:
            stmts, info = sc.function(m['export'])
            for line in pr.body(stmts):
                print(line, file=out)
            if not info['aligned'] or not info['sized']:
                print('\t// WARNING: incomplete parse of this function', file=out)
        except Exception as exc:
            print('\t// could not decompile: %s' % exc, file=out)
        print('}', file=out)
    if world is not None:
        key = next((k for k, v in world.pkgs.items() if v is pkg), None)
        best = world.defaults(key, idx) if key else None
        if best and best[0]:
            tag = Tagged(pkg)
            print('\ndefaultproperties\n{', file=out)
            for x in best[0]:
                print('\t' + tag.render(x), file=out)
            print('}', file=out)


def main(argv):
    system = argv[0]
    files = [os.path.join(system, f) for f in sorted(os.listdir(system))
             if f.endswith('.u')]
    pkgs = {os.path.basename(f): Package(f) for f in files}
    natives = build_native_table(pkgs.values())
    world = World(packages=pkgs)
    pkg = pkgs[argv[1]]
    if argv[2] == '--all':
        outdir = argv[3]
        os.makedirs(outdir, exist_ok=True)
        n = 0
        for e in pkg.exports:
            if pkg.classof(e) != 'Class':
                continue
            with open(os.path.join(outdir, e['name'] + '.uc'), 'w') as f:
                decompile_class(pkg, natives, e['name'], out=f, world=world)
            n += 1
        print('classes written: %d' % n)
    else:
        decompile_class(pkg, natives, argv[2], world=world)


if __name__ == '__main__':
    main(sys.argv[1:])
