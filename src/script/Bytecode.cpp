#include "script/Bytecode.h"

#include <cstring>

namespace ffa {

namespace {

const uint32_t FUNC_ACCESS = 0x000E0000;   // Public | Private | Protected
const uint8_t EX_END_FUNCTION_PARMS = 0x16;

enum K : uint8_t { Expr, Parms, Obj, Nm, U8, U16, U32, F32, Str, UStr, Case, Labels };

struct OpInfo {
    const char* name;
    std::vector<K> operands;
};

const OpInfo& info(uint8_t op) {
    static const std::vector<OpInfo> table = [] {
        std::vector<OpInfo> t(0x60);
        auto set = [&](uint8_t op, const char* n, std::vector<K> k) { t[op] = {n, std::move(k)}; };
        set(0x00, "LocalVariable", {Obj});
        set(0x01, "InstanceVariable", {Obj});
        set(0x02, "DefaultVariable", {Obj});
        set(0x03, "StateVariable", {Obj});
        set(0x04, "Return", {Expr});
        set(0x05, "Switch", {U8, Expr});
        set(0x06, "Jump", {U16});
        set(0x07, "JumpIfNot", {U16, Expr});
        set(0x08, "Stop", {});
        set(0x09, "Assert", {U16, Expr});
        set(0x0A, "Case", {Case});
        set(0x0B, "Nothing", {});
        set(0x0C, "LabelTable", {Labels});
        set(0x0D, "GotoLabel", {Expr});
        set(0x0E, "EatString", {Expr});
        set(0x0F, "Let", {Expr, Expr});
        set(0x10, "DynArrayElement", {Expr, Expr});
        set(0x11, "New", {Expr, Expr, Expr, Expr});
        set(0x12, "ClassContext", {Expr, U16, U8, Expr});
        set(0x13, "Metacast", {Obj, Expr});
        set(0x14, "LetBool", {Expr, Expr});
        set(0x15, "EndParmValue", {});
        set(0x16, "EndFunctionParms", {});
        set(0x17, "Self", {});
        set(0x18, "Skip", {U16, Expr});
        set(0x19, "Context", {Expr, U16, U8, Expr});
        set(0x1A, "ArrayElement", {Expr, Expr});
        set(0x1B, "VirtualFunction", {Nm, Parms});
        set(0x1C, "FinalFunction", {Obj, Parms});
        set(0x1D, "IntConst", {U32});
        set(0x1E, "FloatConst", {F32});
        set(0x1F, "StringConst", {Str});
        set(0x20, "ObjectConst", {Obj});
        set(0x21, "NameConst", {Nm});
        set(0x22, "RotationConst", {U32, U32, U32});
        set(0x23, "VectorConst", {F32, F32, F32});
        set(0x24, "ByteConst", {U8});
        set(0x25, "IntZero", {});
        set(0x26, "IntOne", {});
        set(0x27, "True", {});
        set(0x28, "False", {});
        set(0x29, "NativeParm", {Obj});
        set(0x2A, "NoObject", {});
        set(0x2B, "NoDelegate", {});
        set(0x2C, "IntConstByte", {U8});
        set(0x2D, "BoolVariable", {Expr});
        set(0x2E, "DynamicCast", {Obj, Expr});
        set(0x2F, "Iterator", {Expr, U16});
        set(0x30, "IteratorPop", {});
        set(0x31, "IteratorNext", {});
        set(0x32, "StructCmpEq", {Obj, Expr, Expr});
        set(0x33, "StructCmpNe", {Obj, Expr, Expr});
        set(0x34, "UnicodeStringConst", {UStr});
        set(0x35, "InstanceDelegate", {Nm});
        set(0x36, "StructMember", {Obj, Expr});
        set(0x37, "DynArrayLength", {Expr});
        set(0x38, "GlobalFunction", {Nm, Parms});
        // 0x39 is the primitive cast prefix, read in token(); the codes after
        // it, 0x39 .. 0x5F, are conversions of one expression each, and most
        // also stand as tokens. Five do not: they are tokens of their own,
        // each layout the one that makes every function in Shrek 2 end on its
        // record and agree with its declared size (docs/package-format.md).
        for (int op = 0x39; op < 0x60; ++op) set(uint8_t(op), "Cast", {Expr});
        set(0x40, "DynArrayInsert", {Expr, Expr, Expr});
        set(0x41, "DynArrayRemove", {Expr, Expr, Expr});
        set(0x43, "DelegateFunction", {Obj, Nm, Parms});
        set(0x44, "DelegateProperty", {Nm});
        set(0x45, "LetDelegate", {Expr, Expr});
        return t;
    }();
    return table[op];
}

}  // namespace

const char* opName(Op op) {
    if (op == Op::NativeCall) return "NativeCall";
    return info(uint8_t(op)).name ? info(uint8_t(op)).name : "?";
}

FunctionTail readTail(const Package& p, const Export& e) {
    FunctionTail t;
    size_t end = size_t(e.off) + size_t(e.size);
    auto word = [&](size_t back) -> uint32_t {
        if (size_t(e.size) < back) return 0;
        uint32_t v;
        std::memcpy(&v, p.data.data() + end - back, 4);
        return v;
    };
    // RepOffset follows the flags exactly when they carry FUNC_Net, so try
    // the 9 byte reading first. Testing the 7 byte position for an access bit
    // alone misreads the 121 net functions whose RepOffset has bits there.
    auto single = [](uint32_t f) {
        uint32_t a = f & FUNC_ACCESS;
        return a && !(a & (a - 1));
    };
    uint32_t f4 = word(4), f6 = word(6);
    if ((f6 & 0x40) && single(f6)) {
        t.flags = f6;
        t.length = 9;
    } else {
        t.flags = f4;
        t.length = 7;
    }
    if (size_t(e.size) >= size_t(t.length)) {
        const uint8_t* q = p.data.data() + end - size_t(t.length);
        t.native = uint16_t(q[0] | q[1] << 8);
        t.precedence = q[2];
    }
    return t;
}

Ins BytecodeParser::token(Reader& r, uint32_t mem) {
    if (r.p >= r.limit) throw FormatError("ran past the end of the record");
    if (--budget_ <= 0) throw FormatError("node budget exhausted");
    Ins n;
    n.mem = mem;
    uint32_t size = 1;
    uint8_t op = r.u8();
    n.code = op;
    if (op >= 0x60) {
        n.op = Op::NativeCall;
        if (op >= 0x70) {
            n.native = op;
        } else {
            n.native = uint16_t(((op - 0x60) << 8) + r.u8());
            size += 1;
        }
        size += parms(r, mem + size, n.kids);
        n.msize = size;
        return n;
    }
    if (op == 0x39) {
        // EX_PrimitiveCast: a conversion code byte, then one expression. The
        // code may itself be 0x39, RotatorToVector.
        uint8_t conv = r.u8();
        if (conv < 0x39 || conv >= 0x60) throw FormatError("primitive cast with an unknown conversion");
        n.op = Op::Cast;
        n.code = conv;
        n.kids.push_back(token(r, mem + 2));
        n.msize = 2 + n.kids.back().msize;
        return n;
    }
    const OpInfo& oi = info(op);
    if (!oi.name) throw FormatError("unknown opcode");
    bool own = op == 0x40 || op == 0x41 || op == 0x43 || op == 0x44 || op == 0x45;
    n.op = op >= 0x39 && !own ? Op::Cast : Op(op);
    int floats = 0;
    for (K k : oi.operands) {
        switch (k) {
        case Expr: {
            n.kids.push_back(token(r, mem + size));
            size += n.kids.back().msize;
            break;
        }
        case Parms:
            size += parms(r, mem + size, n.kids);
            break;
        case Obj:
        case Nm:
            n.raw.push_back(r.idx());
            size += 4;
            break;
        case U8:
            n.raw.push_back(r.u8());
            size += 1;
            break;
        case U16:
            n.raw.push_back(r.u16());
            size += 2;
            break;
        case U32:
            n.raw.push_back(r.u32());
            size += 4;
            break;
        case F32:
            if (floats >= 3) throw FormatError("too many float operands");
            n.fv[floats++] = r.f32();
            size += 4;
            break;
        case Str: {
            // single byte characters, Latin-1, widened to the VM's UTF-16
            size_t start = r.p;
            for (uint8_t c; (c = r.u8()) != 0;) n.str += char16_t(c);
            size += uint32_t(r.p - start);
            break;
        }
        case UStr: {
            size_t start = r.p;
            for (uint16_t c; (c = r.u16()) != 0;) n.str += char16_t(c);
            size += uint32_t(r.p - start);
            break;
        }
        case Case: {
            uint16_t off = r.u16();
            n.raw.push_back(off);
            size += 2;
            if (off != 0xFFFF) {
                n.kids.push_back(token(r, mem + size));
                size += n.kids.back().msize;
            }
            break;
        }
        case Labels:
            // name, then the label's memory offset in the state's code
            while (true) {
                int32_t nm = r.idx();
                uint32_t off = r.u32();
                size += 8;
                n.raw.push_back(nm);
                n.raw.push_back(off);
                if (nm == p_.noneIndex) break;
            }
            break;
        }
    }
    n.msize = size;
    return n;
}

uint32_t BytecodeParser::parms(Reader& r, uint32_t mem, std::vector<Ins>& out) {
    uint32_t size = 0;
    while (true) {
        if (r.p >= r.limit) throw FormatError("ran past the end of the record");
        if (r.b[r.p] == EX_END_FUNCTION_PARMS) {
            r.p += 1;
            return size + 1;
        }
        out.push_back(token(r, mem + size));
        size += out.back().msize;
    }
}

Reader BytecodeParser::header(const Export& e, uint32_t& declared) {
    Reader r(p_.data, size_t(e.off), size_t(e.off) + size_t(e.size));
    for (int i = 0; i < 7; ++i) r.idx();   // None, Super, Next, ScriptText,
                                            // Children, FriendlyName, unused
    r.u32();
    r.u32();                                // Line, TextPos
    declared = r.u32();                     // ScriptSize, in memory bytes
    return r;
}

ParsedCode BytecodeParser::function(const Export& e) {
    ParsedCode pc;
    FunctionTail tail = readTail(p_, e);
    size_t target = size_t(e.off) + size_t(e.size) - size_t(tail.length);
    Reader r = header(e, pc.declared);
    r.limit = target;
    budget_ = kMaxNodes;
    uint32_t mem = 0;
    while (r.p < target) {
        pc.stmts.push_back(token(r, mem));
        mem += pc.stmts.back().msize;
    }
    pc.mem = mem;
    pc.end = r.p;
    pc.aligned = r.p == target;
    pc.sized = mem == pc.declared;
    return pc;
}

ParsedCode BytecodeParser::structCode(const Export& e) {
    ParsedCode pc;
    Reader r = header(e, pc.declared);
    budget_ = kMaxNodes;
    uint32_t mem = 0;
    while (mem < pc.declared) {
        pc.stmts.push_back(token(r, mem));
        mem += pc.stmts.back().msize;
    }
    pc.mem = mem;
    pc.end = r.p;
    pc.sized = mem == pc.declared;
    return pc;
}

}  // namespace ffa
