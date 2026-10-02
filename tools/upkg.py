"""Reader for Unreal Engine 2 packages, build 2226 / file version 129 (Shrek 2 PC).

Record layout confirmed against Core.u by hand:
  UObject  tagged property list, terminated by the name 'None'
  UField   SuperField, Next                      (object refs, compact index)
  UStruct  ScriptText, Children, FriendlyName, Line u32, TextPos u32,
           ScriptSize u32, script bytes
  UFunction iNative u16, OperPrecedence u8, FunctionFlags u32,
           RepOffset u16 when FUNC_Net is set
"""
import struct, sys, os, collections

FUNC_FINAL = 0x00000001
FUNC_NET = 0x00000040
FUNC_SIMULATED = 0x00000100
FUNC_EXEC = 0x00000200
FUNC_NATIVE = 0x00000400
FUNC_EVENT = 0x00000800
FUNC_STATIC = 0x00002000
RF_HAS_STACK = 0x02000000


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.b, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.b, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v

    def idx(self):
        b0 = self.u8()
        neg, val = b0 & 0x80, b0 & 0x3F
        if b0 & 0x40:
            shift = 6
            while True:
                b = self.u8()
                val |= (b & 0x7F) << shift
                shift += 7
                if not (b & 0x80):
                    break
        return -val if neg else val


class Package:
    def __init__(self, path):
        self.path = path
        self.name = os.path.basename(path)
        self.b = b = open(path, 'rb').read()
        r = R(b)
        self.tag = r.u32()
        if self.tag != 0x9E2A83C1:
            raise ValueError(f'{self.name}: not an Unreal package')
        self.ver, self.lic = r.u16(), r.u16()
        self.flags = r.u32()
        nc, no = r.u32(), r.u32()
        ec, eo = r.u32(), r.u32()
        ic, io = r.u32(), r.u32()
        if self.ver < 68:
            r.u32(); r.u32()
        else:
            r.p += 16
            for _ in range(r.u32()):
                r.u32(); r.u32()

        self.names = []
        r = R(b, no)
        for _ in range(nc):
            ln = r.idx()
            self.names.append(b[r.p:r.p + ln].split(b'\0')[0].decode('latin-1'))
            r.p += ln
            r.u32()
        self.none_idx = self.names.index('None') if 'None' in self.names else 0

        self.imports = []
        r = R(b, io)
        for _ in range(ic):
            cp, cn = r.idx(), r.idx()
            pkg = r.i32()
            on = r.idx()
            self.imports.append(dict(cls=self.names[cn], pkg=pkg, name=self.names[on]))

        self.exports = []
        r = R(b, eo)
        for _ in range(ec):
            ci, si = r.idx(), r.idx()
            pkg = r.i32()
            on = r.idx()
            fl = r.u32()
            ssz = r.idx()
            soff = r.idx() if ssz > 0 else 0
            self.exports.append(dict(cls_ref=ci, super_ref=si, pkg=pkg,
                                     name=self.names[on], flags=fl,
                                     size=ssz, off=soff))

    def refname(self, ref):
        if ref > 0:
            return self.exports[ref - 1]['name']
        if ref < 0:
            return self.imports[-ref - 1]['name']
        return 'None'

    def classof(self, e):
        return self.refname(e['cls_ref']) if e['cls_ref'] else 'Class'

    def skip_properties(self, r):
        while True:
            n = r.idx()
            if n == self.none_idx:
                return
            info = r.u8()
            t = info & 0x0F
            if t == 10:
                r.idx()
            sizebits = (info >> 4) & 0x07
            size = {0: 1, 1: 2, 2: 4, 3: 12, 4: 16}.get(sizebits)
            if size is None:
                size = {5: r.u8, 6: r.u16, 7: r.u32}[sizebits]()
            if info & 0x80:
                r.u8()
            if t != 3:
                r.p += size

    def parse_function(self, e):
        """FunctionFlags are the final field of the record, so read them from the
        end. The middle of the UStruct record varies in this build and is not
        needed for the native/script question."""
        end = e['off'] + e['size']
        VALID = 0x01FFFFFF
        ff = struct.unpack_from('<I', self.b, end - 4)[0]
        if ff & FUNC_NET or ff & ~VALID:
            ff2 = struct.unpack_from('<I', self.b, end - 6)[0]
            if not (ff2 & ~VALID) and (ff2 & FUNC_NET):
                ff = ff2
        if ff & ~VALID or ff == 0:
            return dict(ok=False, native=False, flags=0, size=e['size'])
        return dict(ok=True, native=bool(ff & FUNC_NATIVE), flags=ff, size=e['size'])

def main(paths):
    tot = collections.Counter()
    print(f"{'package':20} {'ver':>7} {'exports':>8} {'classes':>8} {'funcs':>7} "
          f"{'parsed':>7} {'native':>7} {'nat%':>6} {'bytecode':>10}")
    for path in sorted(paths):
        p = Package(path)
        hist = collections.Counter(p.classof(e) for e in p.exports)
        funcs = [e for e in p.exports if p.classof(e) == 'Function']
        res = [p.parse_function(e) for e in funcs]
        good = [x for x in res if x['ok']]
        nat = [x for x in good if x['native']]
        sb = sum(x['size'] for x in good)
        tot['funcs'] += len(funcs); tot['good'] += len(good)
        tot['nat'] += len(nat); tot['bytes'] += sb
        tot['classes'] += hist.get('Class', 0)
        print(f"{p.name:20} {p.ver:>5}/{p.lic} {len(p.exports):8} {hist.get('Class',0):8} "
              f"{len(funcs):7} {len(good):7} {len(nat):7} "
              f"{100*len(nat)/max(len(good),1):5.1f}% {sb/1024:9.1f}KB")
    print(f"{'ВСЬОГО':20} {'':7} {'':8} {tot['classes']:8} {tot['funcs']:7} "
          f"{tot['good']:7} {tot['nat']:7} "
          f"{100*tot['nat']/max(tot['good'],1):5.1f}% {tot['bytes']/1024:9.1f}KB")


if __name__ == '__main__':
    main(sys.argv[1:])
