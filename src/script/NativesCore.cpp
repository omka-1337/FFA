// The natives Object declares in Core: operators, maths, strings, names,
// states. Keys are "class.function" as the function is named in the package,
// where an operator's name spells out its symbol and operand types:
// Add_IntInt is int + int, AddAdd_PreInt is ++int, AddAdd_Int is int++.
//
// Arithmetic follows the C the engine is written in: int wraps at 32 bits,
// float is single precision, and integer division by zero warns and gives 0.
#include <algorithm>
#include <cmath>

#include "script/VM.h"

namespace ffa {

namespace {

int32_t wrap(int64_t v) { return int32_t(uint32_t(uint64_t(v))); }

Value I(int32_t v) { return Value::Int(v); }
Value F(float v) { return Value::Float(v); }
Value B(bool v) { return Value::Bool(v); }
Value S(String v) { return Value::Str(std::move(v)); }

// Text for one value of a property, read and written as the engine's
// ImportText and ExportText have it for SetPropertyText and GetPropertyText:
// numbers, True and False, names and strings as they are, an enum by its
// value's name or number, an object by its path or, in the caller's package,
// its name, and a struct as (Field=value,...). A name, an enum and an object
// are one token.
String trim(const String& t) {
    size_t a = t.find_first_not_of(u" \t"), b = t.find_last_not_of(u" \t");
    return a == String::npos ? String() : t.substr(a, b - a + 1);
}

// Top level parts of "a,b,(c,d),\"e,f\"", quotes and brackets kept whole.
std::vector<String> topParts(const String& t) {
    std::vector<String> out;
    String cur;
    int depth = 0;
    bool quoted = false;
    for (char16_t ch : t) {
        if (ch == u'"') quoted = !quoted;
        if (!quoted && (ch == u'(' || ch == u'[')) ++depth;
        if (!quoted && (ch == u')' || ch == u']')) --depth;
        if (!quoted && depth == 0 && ch == u',') {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur += ch;
    }
    if (!trim(cur).empty() || !out.empty()) out.push_back(trim(cur));
    return out;
}

// The first token of a text, as the engine reads a name or an object: up to
// a space, a comma or a closing bracket. KnowWonder's cutscene actions rely on
// it, setting PlayAnim's BaseAnim from "IDLESTART LOOP", its options and all,
// and the game plays IdleStart.
String token(const String& t) {
    size_t e = t.find_first_of(u" \t,)");
    return e == String::npos ? t : t.substr(0, e);
}

bool importText(VM& vm, Object* self, const Prop* p, String text, Value& out, int depth = 0) {
    text = trim(text);
    if (depth > 8) return false;
    if (p->kind == Kind::Name || p->kind == Kind::Byte || p->kind == Kind::Object || p->kind == Kind::Class)
        text = token(text);
    std::string t = utf8(text);
    switch (p->kind) {
    case Kind::Int: out = Value::Int(parseInt(text)); return true;
    case Kind::Float: out = Value::Float(parseFloat(text)); return true;
    case Kind::Bool: out = Value::Bool(iequals(t, "true") || iequals(t, "yes") || parseInt(text) != 0); return true;
    case Kind::Name: out = Value::Nm(Name(t)); return true;
    case Kind::Str:
        if (text.size() >= 2 && text.front() == u'"' && text.back() == u'"') text = text.substr(1, text.size() - 2);
        out = Value::Str(text);
        return true;
    case Kind::Byte: {
        if (EnumType* e = p->enumType())
            for (size_t i = 0; i < e->values.size(); ++i)
                if (iequals(e->values[i].str(), t)) {
                    out = Value::Int(int32_t(i));
                    return true;
                }
        out = Value::Int(parseInt(text) & 0xFF);
        return true;
    }
    case Kind::Object:
    case Kind::Class: {
        if (iequals(t, "none") || t.empty()) {
            out = Value::Obj(nullptr);
            return true;
        }
        // Class'Package.Name', or the path alone
        size_t q = t.find('\'');
        if (q != std::string::npos && t.back() == '\'') t = t.substr(q + 1, t.size() - q - 2);
        Object* o = vm.linker.findObject(t);
        if (!o && self) {
            Object* top = self;
            while (top->outer) top = top->outer;
            if (top != self) o = vm.linker.findObject(top->name.str() + "." + t);
        }
        if (!o && p->kind == Kind::Class) o = vm.linker.findClass(t.substr(t.rfind('.') + 1));
        if (!o) return false;
        out = Value::Obj(o);
        return true;
    }
    case Kind::Struct: {
        StructType* st = p->structType();
        if (!st || text.size() < 2 || text.front() != u'(' || text.back() != u')') return false;
        Value v = st->make();
        for (const String& part : topParts(text.substr(1, text.size() - 2))) {
            size_t eq = part.find(u'=');
            if (eq == String::npos) continue;
            Prop* f = st->field(Name(utf8(trim(part.substr(0, eq)))));
            if (!f) continue;
            Value fv;
            if (importText(vm, self, f, part.substr(eq + 1), fv, depth + 1)) v.st().f[size_t(f->slot)] = f->coerce(fv);
        }
        out = v;
        return true;
    }
    default:
        return false;
    }
}

String exportText(const Prop* p, const Value& v, int depth = 0) {
    switch (p->kind) {
    case Kind::Int: return formatInt(v.i());
    case Kind::Float: return formatFloat(v.f());
    case Kind::Bool: return widen(v.b() ? "True" : "False");
    case Kind::Name: return widen(v.n().str());
    case Kind::Str: return v.s();
    case Kind::Byte:
        if (EnumType* e = p->enumType())
            if (v.i() >= 0 && size_t(v.i()) < e->values.size()) return widen(e->values[size_t(v.i())].str());
        return formatInt(v.i());
    case Kind::Object:
    case Kind::Class: return widen(v.o() ? v.o()->path() : "None");
    case Kind::Struct: {
        StructType* st = p->structType();
        if (!st || !v.isStruct() || depth > 8) return String();
        String out = u"(";
        bool first = true;
        for (Prop* f : st->layout()) {
            if (!first) out += u",";
            first = false;
            out += widen(f->name.str()) + u"=" + exportText(f, v.st().f[size_t(f->slot)], depth + 1);
        }
        return out + u")";
    }
    default:
        return String();
    }
}

void divzero(NativeCall& c) {
    c.vm.write("ScriptWarning", (c.caller ? c.caller->where() : std::string("?")) + " Divide by zero");
}

char16_t up(char16_t c) {
    if ((c >= u'a' && c <= u'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return char16_t(c - 0x20);
    return c;
}

char16_t down(char16_t c) {
    if ((c >= u'A' && c <= u'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) return char16_t(c + 0x20);
    return c;
}

String caps(String s) {
    for (char16_t& c : s) c = up(c);
    return s;
}

String locs(String s) {
    for (char16_t& c : s) c = down(c);
    return s;
}

// FString::Mid: the range is clamped to the string, never an error.
String mid(const String& s, int64_t start, int64_t count) {
    int64_t len = int64_t(s.size());
    int64_t end = start + count;
    start = std::clamp<int64_t>(start, 0, len);
    end = std::clamp<int64_t>(end, start, len);
    return s.substr(size_t(start), size_t(end - start));
}

struct V3 {
    float x, y, z;
};

V3 vec(NativeCall& c, size_t k) {
    V3 v{};
    c.vm.unvector(c.get(k), v.x, v.y, v.z);
    return v;
}

Value mk(NativeCall& c, V3 v) { return c.vm.vector(v.x, v.y, v.z); }

struct R3 {
    int32_t p, y, r;
};

R3 rot(NativeCall& c, size_t k) {
    R3 v{};
    c.vm.unrotator(c.get(k), v.p, v.y, v.r);
    return v;
}

Value mk(NativeCall& c, R3 r) { return c.vm.rotator(r.p, r.y, r.r); }

V3 transform(V3 v, const float m[3][3], bool inverse) {
    if (inverse)    // world to local: the components along each axis
        return {v.x * m[0][0] + v.y * m[0][1] + v.z * m[0][2],
                v.x * m[1][0] + v.y * m[1][1] + v.z * m[1][2],
                v.x * m[2][0] + v.y * m[2][1] + v.z * m[2][2]};
    return {v.x * m[0][0] + v.y * m[1][0] + v.z * m[2][0],   // local to world
            v.x * m[0][1] + v.y * m[1][1] + v.z * m[2][1],
            v.x * m[0][2] + v.y * m[1][2] + v.z * m[2][2]};
}

float frand(NativeCall& c) {
    return float(std::uniform_real_distribution<double>(0.0, 1.0)(c.vm.rng));
}

}  // namespace

void registerCoreNatives(VM& vm) {
    auto& n = vm.natives;

    // ------------------------------------------------------------- bool
    n["object.not_prebool"] = [](NativeCall& c) { return B(!c.b(0)); };
    n["object.equalequal_boolbool"] = [](NativeCall& c) { return B(c.b(0) == c.b(1)); };
    n["object.notequal_boolbool"] = [](NativeCall& c) { return B(c.b(0) != c.b(1)); };
    n["object.andand_boolbool"] = [](NativeCall& c) { return B(c.b(0) && c.b(1)); };
    n["object.xorxor_boolbool"] = [](NativeCall& c) { return B(c.b(0) != c.b(1)); };
    n["object.oror_boolbool"] = [](NativeCall& c) { return B(c.b(0) || c.b(1)); };

    // ------------------------------------------------------------- byte
    n["object.multiplyequal_bytebyte"] = [](NativeCall& c) {
        c.out(0, I((c.i(0) * c.i(1)) & 0xFF));
        return c.get(0);
    };
    n["object.divideequal_bytebyte"] = [](NativeCall& c) {
        if (!c.i(1)) divzero(c);
        c.out(0, I(c.i(1) ? (c.i(0) / c.i(1)) & 0xFF : 0));
        return c.get(0);
    };
    n["object.addequal_bytebyte"] = [](NativeCall& c) {
        c.out(0, I((c.i(0) + c.i(1)) & 0xFF));
        return c.get(0);
    };
    n["object.subtractequal_bytebyte"] = [](NativeCall& c) {
        c.out(0, I((c.i(0) - c.i(1)) & 0xFF));
        return c.get(0);
    };
    n["object.addadd_prebyte"] = [](NativeCall& c) {
        c.out(0, I((c.i(0) + 1) & 0xFF));
        return c.get(0);
    };
    n["object.subtractsubtract_prebyte"] = [](NativeCall& c) {
        c.out(0, I((c.i(0) - 1) & 0xFF));
        return c.get(0);
    };
    n["object.addadd_byte"] = [](NativeCall& c) {
        int32_t old = c.i(0);
        c.out(0, I((old + 1) & 0xFF));
        return I(old);
    };
    n["object.subtractsubtract_byte"] = [](NativeCall& c) {
        int32_t old = c.i(0);
        c.out(0, I((old - 1) & 0xFF));
        return I(old);
    };

    // -------------------------------------------------------------- int
    n["object.complement_preint"] = [](NativeCall& c) { return I(~c.i(0)); };
    n["object.subtract_preint"] = [](NativeCall& c) { return I(wrap(-int64_t(c.i(0)))); };
    n["object.multiply_intint"] = [](NativeCall& c) { return I(wrap(int64_t(c.i(0)) * c.i(1))); };
    n["object.divide_intint"] = [](NativeCall& c) {
        int32_t a = c.i(0), b = c.i(1);
        if (!b) {
            divzero(c);
            return I(0);
        }
        if (a == INT32_MIN && b == -1) return I(INT32_MIN);
        return I(a / b);
    };
    n["object.add_intint"] = [](NativeCall& c) { return I(wrap(int64_t(c.i(0)) + c.i(1))); };
    n["object.subtract_intint"] = [](NativeCall& c) { return I(wrap(int64_t(c.i(0)) - c.i(1))); };
    n["object.lessless_intint"] = [](NativeCall& c) {
        return I(int32_t(uint32_t(c.i(0)) << (c.i(1) & 31)));
    };
    n["object.greatergreater_intint"] = [](NativeCall& c) { return I(c.i(0) >> (c.i(1) & 31)); };
    n["object.greatergreatergreater_intint"] = [](NativeCall& c) {
        return I(int32_t(uint32_t(c.i(0)) >> (c.i(1) & 31)));
    };
    n["object.less_intint"] = [](NativeCall& c) { return B(c.i(0) < c.i(1)); };
    n["object.greater_intint"] = [](NativeCall& c) { return B(c.i(0) > c.i(1)); };
    n["object.lessequal_intint"] = [](NativeCall& c) { return B(c.i(0) <= c.i(1)); };
    n["object.greaterequal_intint"] = [](NativeCall& c) { return B(c.i(0) >= c.i(1)); };
    n["object.equalequal_intint"] = [](NativeCall& c) { return B(c.i(0) == c.i(1)); };
    n["object.notequal_intint"] = [](NativeCall& c) { return B(c.i(0) != c.i(1)); };
    n["object.and_intint"] = [](NativeCall& c) { return I(c.i(0) & c.i(1)); };
    n["object.xor_intint"] = [](NativeCall& c) { return I(c.i(0) ^ c.i(1)); };
    n["object.or_intint"] = [](NativeCall& c) { return I(c.i(0) | c.i(1)); };
    n["object.multiplyequal_intfloat"] = [](NativeCall& c) {
        c.out(0, I(toInt(float(c.i(0)) * c.f(1))));
        return c.get(0);
    };
    n["object.divideequal_intfloat"] = [](NativeCall& c) {
        if (c.f(1) == 0.0f) divzero(c);
        c.out(0, I(c.f(1) != 0.0f ? toInt(float(c.i(0)) / c.f(1)) : 0));
        return c.get(0);
    };
    n["object.addequal_intint"] = [](NativeCall& c) {
        c.out(0, I(wrap(int64_t(c.i(0)) + c.i(1))));
        return c.get(0);
    };
    n["object.subtractequal_intint"] = [](NativeCall& c) {
        c.out(0, I(wrap(int64_t(c.i(0)) - c.i(1))));
        return c.get(0);
    };
    n["object.addadd_preint"] = [](NativeCall& c) {
        c.out(0, I(wrap(int64_t(c.i(0)) + 1)));
        return c.get(0);
    };
    n["object.subtractsubtract_preint"] = [](NativeCall& c) {
        c.out(0, I(wrap(int64_t(c.i(0)) - 1)));
        return c.get(0);
    };
    n["object.addadd_int"] = [](NativeCall& c) {
        int32_t old = c.i(0);
        c.out(0, I(wrap(int64_t(old) + 1)));
        return I(old);
    };
    n["object.subtractsubtract_int"] = [](NativeCall& c) {
        int32_t old = c.i(0);
        c.out(0, I(wrap(int64_t(old) - 1)));
        return I(old);
    };
    n["object.rand"] = [](NativeCall& c) {
        int32_t m = c.i(0);
        return I(m > 0 ? std::uniform_int_distribution<int32_t>(0, m - 1)(c.vm.rng) : 0);
    };
    n["object.min"] = [](NativeCall& c) { return I(std::min(c.i(0), c.i(1))); };
    n["object.max"] = [](NativeCall& c) { return I(std::max(c.i(0), c.i(1))); };
    n["object.clamp"] = [](NativeCall& c) { return I(std::max(c.i(1), std::min(c.i(0), c.i(2)))); };

    // ------------------------------------------------------------ float
    n["object.subtract_prefloat"] = [](NativeCall& c) { return F(-c.f(0)); };
    n["object.multiplymultiply_floatfloat"] = [](NativeCall& c) {
        return F(std::pow(c.f(0), c.f(1)));
    };
    n["object.multiply_floatfloat"] = [](NativeCall& c) { return F(c.f(0) * c.f(1)); };
    n["object.divide_floatfloat"] = [](NativeCall& c) {
        if (c.f(1) == 0.0f) divzero(c);
        return F(c.f(0) / c.f(1));
    };
    n["object.percent_floatfloat"] = [](NativeCall& c) {
        if (c.f(1) == 0.0f) divzero(c);
        return F(std::fmod(c.f(0), c.f(1)));
    };
    n["object.add_floatfloat"] = [](NativeCall& c) { return F(c.f(0) + c.f(1)); };
    n["object.subtract_floatfloat"] = [](NativeCall& c) { return F(c.f(0) - c.f(1)); };
    n["object.less_floatfloat"] = [](NativeCall& c) { return B(c.f(0) < c.f(1)); };
    n["object.greater_floatfloat"] = [](NativeCall& c) { return B(c.f(0) > c.f(1)); };
    n["object.lessequal_floatfloat"] = [](NativeCall& c) { return B(c.f(0) <= c.f(1)); };
    n["object.greaterequal_floatfloat"] = [](NativeCall& c) { return B(c.f(0) >= c.f(1)); };
    n["object.equalequal_floatfloat"] = [](NativeCall& c) { return B(c.f(0) == c.f(1)); };
    n["object.notequal_floatfloat"] = [](NativeCall& c) { return B(c.f(0) != c.f(1)); };
    n["object.complementequal_floatfloat"] = [](NativeCall& c) {
        return B(std::fabs(c.f(0) - c.f(1)) < 1.0e-4f);
    };
    n["object.multiplyequal_floatfloat"] = [](NativeCall& c) {
        c.out(0, F(c.f(0) * c.f(1)));
        return c.get(0);
    };
    n["object.divideequal_floatfloat"] = [](NativeCall& c) {
        if (c.f(1) == 0.0f) divzero(c);
        c.out(0, F(c.f(0) / c.f(1)));
        return c.get(0);
    };
    n["object.addequal_floatfloat"] = [](NativeCall& c) {
        c.out(0, F(c.f(0) + c.f(1)));
        return c.get(0);
    };
    n["object.subtractequal_floatfloat"] = [](NativeCall& c) {
        c.out(0, F(c.f(0) - c.f(1)));
        return c.get(0);
    };
    n["object.abs"] = [](NativeCall& c) { return F(std::fabs(c.f(0))); };
    n["object.sin"] = [](NativeCall& c) { return F(std::sin(c.f(0))); };
    n["object.cos"] = [](NativeCall& c) { return F(std::cos(c.f(0))); };
    n["object.tan"] = [](NativeCall& c) { return F(std::tan(c.f(0))); };
    n["object.atan"] = [](NativeCall& c) {
        return F(c.has(1) ? std::atan2(c.f(0), c.f(1)) : std::atan(c.f(0)));
    };
    n["object.exp"] = [](NativeCall& c) { return F(std::exp(c.f(0))); };
    n["object.loge"] = [](NativeCall& c) { return F(std::log(c.f(0))); };
    n["object.sqrt"] = [](NativeCall& c) { return F(c.f(0) > 0 ? std::sqrt(c.f(0)) : 0.0f); };
    n["object.square"] = [](NativeCall& c) { return F(c.f(0) * c.f(0)); };
    n["object.frand"] = [](NativeCall& c) { return F(frand(c)); };
    n["object.fmin"] = [](NativeCall& c) { return F(std::min(c.f(0), c.f(1))); };
    n["object.fmax"] = [](NativeCall& c) { return F(std::max(c.f(0), c.f(1))); };
    n["object.fclamp"] = [](NativeCall& c) { return F(std::max(c.f(1), std::min(c.f(0), c.f(2)))); };
    n["object.lerp"] = [](NativeCall& c) { return F(c.f(1) + c.f(0) * (c.f(2) - c.f(1))); };
    n["object.smerp"] = [](NativeCall& c) {
        float a = c.f(0);
        return F(c.f(1) + (3.0f * a * a - 2.0f * a * a * a) * (c.f(2) - c.f(1)));
    };

    // ----------------------------------------------------------- vector
    n["object.subtract_prevector"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        return mk(c, V3{-a.x, -a.y, -a.z});
    };
    n["object.multiply_vectorfloat"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        float s = c.f(1);
        return mk(c, V3{a.x * s, a.y * s, a.z * s});
    };
    n["object.multiply_floatvector"] = [](NativeCall& c) {
        V3 a = vec(c, 1);
        float s = c.f(0);
        return mk(c, V3{a.x * s, a.y * s, a.z * s});
    };
    n["object.multiply_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return mk(c, V3{a.x * b.x, a.y * b.y, a.z * b.z});
    };
    n["object.divide_vectorfloat"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        float s = c.f(1);
        if (s == 0.0f) divzero(c);
        return mk(c, V3{a.x / s, a.y / s, a.z / s});
    };
    n["object.add_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return mk(c, V3{a.x + b.x, a.y + b.y, a.z + b.z});
    };
    n["object.subtract_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return mk(c, V3{a.x - b.x, a.y - b.y, a.z - b.z});
    };
    // vector << rotator turns a world vector into the rotation's local frame,
    // vector >> rotator turns a local one out into the world.
    n["object.lessless_vectorrotator"] = [](NativeCall& c) {
        R3 r = rot(c, 1);
        float m[3][3];
        rotationAxes(r.p, r.y, r.r, m);
        return mk(c, transform(vec(c, 0), m, true));
    };
    n["object.greatergreater_vectorrotator"] = [](NativeCall& c) {
        R3 r = rot(c, 1);
        float m[3][3];
        rotationAxes(r.p, r.y, r.r, m);
        return mk(c, transform(vec(c, 0), m, false));
    };
    n["object.equalequal_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return B(a.x == b.x && a.y == b.y && a.z == b.z);
    };
    n["object.notequal_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return B(a.x != b.x || a.y != b.y || a.z != b.z);
    };
    n["object.dot_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return F(a.x * b.x + a.y * b.y + a.z * b.z);
    };
    n["object.cross_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        return mk(c, V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x});
    };
    n["object.multiplyequal_vectorfloat"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        float s = c.f(1);
        c.out(0, mk(c, V3{a.x * s, a.y * s, a.z * s}));
        return c.get(0);
    };
    n["object.multiplyequal_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        c.out(0, mk(c, V3{a.x * b.x, a.y * b.y, a.z * b.z}));
        return c.get(0);
    };
    n["object.divideequal_vectorfloat"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        float s = c.f(1);
        if (s == 0.0f) divzero(c);
        c.out(0, mk(c, V3{a.x / s, a.y / s, a.z / s}));
        return c.get(0);
    };
    n["object.addequal_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        c.out(0, mk(c, V3{a.x + b.x, a.y + b.y, a.z + b.z}));
        return c.get(0);
    };
    n["object.subtractequal_vectorvector"] = [](NativeCall& c) {
        V3 a = vec(c, 0), b = vec(c, 1);
        c.out(0, mk(c, V3{a.x - b.x, a.y - b.y, a.z - b.z}));
        return c.get(0);
    };
    n["object.vsize"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        return F(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z));
    };
    n["object.normal"] = [](NativeCall& c) {
        V3 a = vec(c, 0);
        float sq = a.x * a.x + a.y * a.y + a.z * a.z;
        if (sq < 1.0e-8f) return mk(c, V3{0, 0, 0});
        float k = 1.0f / std::sqrt(sq);
        return mk(c, V3{a.x * k, a.y * k, a.z * k});
    };
    n["object.vrand"] = [](NativeCall& c) {
        V3 v;
        float sq;
        do {
            v = {frand(c) * 2 - 1, frand(c) * 2 - 1, frand(c) * 2 - 1};
            sq = v.x * v.x + v.y * v.y + v.z * v.z;
        } while (sq > 1.0f || sq < 1.0e-4f);
        float k = 1.0f / std::sqrt(sq);
        return mk(c, V3{v.x * k, v.y * k, v.z * k});
    };
    n["object.mirrorvectorbynormal"] = [](NativeCall& c) {
        V3 v = vec(c, 0), nm = vec(c, 1);
        float sq = nm.x * nm.x + nm.y * nm.y + nm.z * nm.z;
        if (sq > 1.0e-8f) {
            float k = 1.0f / std::sqrt(sq);
            nm = {nm.x * k, nm.y * k, nm.z * k};
        }
        float d = 2.0f * (v.x * nm.x + v.y * nm.y + v.z * nm.z);
        return mk(c, V3{v.x - nm.x * d, v.y - nm.y * d, v.z - nm.z * d});
    };
    n["object.getaxes"] = [](NativeCall& c) {
        R3 r = rot(c, 0);
        float m[3][3];
        rotationAxes(r.p, r.y, r.r, m);
        for (int i = 0; i < 3; ++i) c.out(size_t(i + 1), mk(c, V3{m[i][0], m[i][1], m[i][2]}));
        return Value();
    };
    n["object.getunaxes"] = [](NativeCall& c) {
        R3 r = rot(c, 0);
        float m[3][3];
        rotationAxes(r.p, r.y, r.r, m);
        for (int i = 0; i < 3; ++i) c.out(size_t(i + 1), mk(c, V3{m[0][i], m[1][i], m[2][i]}));
        return Value();
    };

    // ---------------------------------------------------------- rotator
    n["object.equalequal_rotatorrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 0), b = rot(c, 1);
        return B(a.p == b.p && a.y == b.y && a.r == b.r);
    };
    n["object.notequal_rotatorrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 0), b = rot(c, 1);
        return B(a.p != b.p || a.y != b.y || a.r != b.r);
    };
    n["object.multiply_rotatorfloat"] = [](NativeCall& c) {
        R3 a = rot(c, 0);
        float s = c.f(1);
        return mk(c, R3{toInt(a.p * s), toInt(a.y * s), toInt(a.r * s)});
    };
    n["object.multiply_floatrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 1);
        float s = c.f(0);
        return mk(c, R3{toInt(a.p * s), toInt(a.y * s), toInt(a.r * s)});
    };
    n["object.divide_rotatorfloat"] = [](NativeCall& c) {
        R3 a = rot(c, 0);
        float s = c.f(1);
        if (s == 0.0f) divzero(c);
        return mk(c, R3{toInt(a.p / s), toInt(a.y / s), toInt(a.r / s)});
    };
    n["object.multiplyequal_rotatorfloat"] = [](NativeCall& c) {
        R3 a = rot(c, 0);
        float s = c.f(1);
        c.out(0, mk(c, R3{toInt(a.p * s), toInt(a.y * s), toInt(a.r * s)}));
        return c.get(0);
    };
    n["object.divideequal_rotatorfloat"] = [](NativeCall& c) {
        R3 a = rot(c, 0);
        float s = c.f(1);
        if (s == 0.0f) divzero(c);
        c.out(0, mk(c, R3{toInt(a.p / s), toInt(a.y / s), toInt(a.r / s)}));
        return c.get(0);
    };
    n["object.add_rotatorrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 0), b = rot(c, 1);
        return mk(c, R3{wrap(int64_t(a.p) + b.p), wrap(int64_t(a.y) + b.y), wrap(int64_t(a.r) + b.r)});
    };
    n["object.subtract_rotatorrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 0), b = rot(c, 1);
        return mk(c, R3{wrap(int64_t(a.p) - b.p), wrap(int64_t(a.y) - b.y), wrap(int64_t(a.r) - b.r)});
    };
    n["object.addequal_rotatorrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 0), b = rot(c, 1);
        c.out(0, mk(c, R3{wrap(int64_t(a.p) + b.p), wrap(int64_t(a.y) + b.y), wrap(int64_t(a.r) + b.r)}));
        return c.get(0);
    };
    n["object.subtractequal_rotatorrotator"] = [](NativeCall& c) {
        R3 a = rot(c, 0), b = rot(c, 1);
        c.out(0, mk(c, R3{wrap(int64_t(a.p) - b.p), wrap(int64_t(a.y) - b.y), wrap(int64_t(a.r) - b.r)}));
        return c.get(0);
    };
    n["object.rotrand"] = [](NativeCall& c) {
        auto r16 = [&] { return std::uniform_int_distribution<int32_t>(0, 65535)(c.vm.rng); };
        int32_t p = r16(), y = r16();
        return mk(c, R3{p, y, c.b(0) ? r16() : 0});
    };
    n["object.normalize"] = [](NativeCall& c) {
        auto norm = [](int32_t a) {
            a &= 0xFFFF;
            return a > 32767 ? a - 65536 : a;
        };
        R3 a = rot(c, 0);
        return mk(c, R3{norm(a.p), norm(a.y), norm(a.r)});
    };

    // ----------------------------------------------------------- string
    n["object.concat_strstr"] = [](NativeCall& c) { return S(c.s(0) + c.s(1)); };
    n["object.at_strstr"] = [](NativeCall& c) { return S(c.s(0) + u" " + c.s(1)); };
    // == and the orderings compare exactly; ~= ignores case.
    n["object.less_strstr"] = [](NativeCall& c) { return B(c.s(0) < c.s(1)); };
    n["object.greater_strstr"] = [](NativeCall& c) { return B(c.s(0) > c.s(1)); };
    n["object.lessequal_strstr"] = [](NativeCall& c) { return B(c.s(0) <= c.s(1)); };
    n["object.greaterequal_strstr"] = [](NativeCall& c) { return B(c.s(0) >= c.s(1)); };
    n["object.equalequal_strstr"] = [](NativeCall& c) { return B(c.s(0) == c.s(1)); };
    n["object.notequal_strstr"] = [](NativeCall& c) { return B(c.s(0) != c.s(1)); };
    n["object.complementequal_strstr"] = [](NativeCall& c) { return B(caps(c.s(0)) == caps(c.s(1))); };
    n["object.len"] = [](NativeCall& c) { return I(int32_t(c.s(0).size())); };
    n["object.instr"] = [](NativeCall& c) {
        size_t at = c.s(0).find(c.s(1));
        return I(at == String::npos ? -1 : int32_t(at));
    };
    n["object.mid"] = [](NativeCall& c) {
        return S(mid(c.s(0), c.i(1), c.has(2) ? c.i(2) : INT32_MAX));
    };
    n["object.left"] = [](NativeCall& c) { return S(mid(c.s(0), 0, std::max(0, c.i(1)))); };
    n["object.right"] = [](NativeCall& c) {
        String s = c.s(0);
        int64_t k = std::clamp<int64_t>(c.i(1), 0, int64_t(s.size()));
        return S(mid(s, int64_t(s.size()) - k, k));
    };
    n["object.caps"] = [](NativeCall& c) { return S(caps(c.s(0))); };
    n["object.locs"] = [](NativeCall& c) { return S(locs(c.s(0))); };
    n["object.chr"] = [](NativeCall& c) { return S(String(1, char16_t(c.i(0)))); };
    n["object.asc"] = [](NativeCall& c) {
        String s = c.s(0);
        return I(s.empty() ? 0 : int32_t(s[0]));
    };
    n["object.repl"] = [](NativeCall& c) {
        String src = c.s(0), match = c.s(1), with = c.s(2), out;
        if (match.empty()) return S(src);
        bool exact = c.b(3);
        String hay = exact ? src : caps(src), needle = exact ? match : caps(match);
        size_t i = 0;
        while (true) {
            size_t at = hay.find(needle, i);
            if (at == String::npos) break;
            out += src.substr(i, at - i) + with;
            i = at + match.size();
        }
        return S(out + src.substr(i));
    };

    // ----------------------------------------------------- object, name
    n["object.equalequal_objectobject"] = [](NativeCall& c) { return B(c.o(0) == c.o(1)); };
    n["object.notequal_objectobject"] = [](NativeCall& c) { return B(c.o(0) != c.o(1)); };
    n["object.equalequal_namename"] = [](NativeCall& c) { return B(c.n(0) == c.n(1)); };
    n["object.notequal_namename"] = [](NativeCall& c) { return B(c.n(0) != c.n(1)); };
    n["object.log"] = [](NativeCall& c) {
        Name tag = c.n(1);
        c.vm.write(tag.isNone() ? "ScriptLog" : tag.str(), utf8(c.s(0)));
        return Value();
    };
    n["object.warn"] = [](NativeCall& c) {
        c.vm.write("ScriptWarning", (c.caller ? c.caller->where() : std::string("?")) + " " + utf8(c.s(0)));
        return Value();
    };
    // SetPropertyText(PropName, PropValue) and GetPropertyText(PropName): a
    // variable of the object by name, as text. KnowWonder's cutscene actions
    // set their arguments so: PlayAnim's BaseAnim from "PlayAnim SipDrink".
    n["object.setpropertytext"] = [](NativeCall& c) {
        if (!c.self || !c.self->cls) return Value();
        Prop* p = c.self->cls->findProp(Name(utf8(c.s(0))));
        Value v;
        if (p && importText(c.vm, c.self, p, c.s(1), v)) *c.vm.slot(c.self, p) = p->coerce(v);
        return Value();
    };
    n["object.getpropertytext"] = [](NativeCall& c) {
        if (!c.self || !c.self->cls) return S(String());
        Prop* p = c.self->cls->findProp(Name(utf8(c.s(0))));
        return S(p ? exportText(p, *c.vm.slot(c.self, p)) : String());
    };
    n["object.localize"] = [](NativeCall& c) {
        String out;
        if (c.vm.localize && c.vm.localize(utf8(c.s(0)), utf8(c.s(1)), utf8(c.s(2)), out)) return S(out);
        // what the engine returns for a key it cannot find
        return S(u"<?int?" + c.s(2) + u"." + c.s(0) + u"." + c.s(1) + u"?>");
    };
    n["object.isa"] = [](NativeCall& c) {
        Name want = c.n(0);
        for (Class* k = c.self ? c.self->cls : nullptr; k; k = k->super)
            if (k->name == want) return B(true);
        return B(false);
    };
    n["object.classischildof"] = [](NativeCall& c) {
        Object* a = c.o(0);
        Object* b = c.o(1);
        return B(a && b && a->isClass() && b->isClass() &&
                 static_cast<Class*>(a)->isChildOf(static_cast<Class*>(b)));
    };
    n["object.getenum"] = [](NativeCall& c) {
        auto* e = dynamic_cast<EnumType*>(c.o(0));
        int32_t i = c.i(1);
        if (!e || i < 0 || size_t(i) >= e->values.size()) return Value::Nm(Name());
        return Value::Nm(e->values[size_t(i)]);
    };
    n["object.dynamicloadobject"] = [](NativeCall& c) {
        std::string path = utf8(c.s(0));
        Object* o = c.vm.linker.findObject(path);
        Object* want = c.o(1);
        if (!o && c.vm.loadObject)
            o = c.vm.loadObject(path, want && want->isClass() ? static_cast<const Class*>(want) : nullptr);
        if (o && want && want->isClass()) {
            bool ok = o->isA(static_cast<Class*>(want)) || (o->isClass() && want->name == Name("Class"));
            if (!ok) o = nullptr;
        }
        if (!o && !c.b(2)) c.vm.write("ScriptWarning", "DynamicLoadObject: cannot load " + path);
        return Value::Obj(o);
    };
    n["object.saveconfig"] = [](NativeCall&) { return Value(); };
    n["object.staticsaveconfig"] = [](NativeCall&) { return Value(); };
    n["object.resetconfig"] = [](NativeCall&) { return Value(); };

    // ------------------------------------------------------------ states
    n["object.gotostate"] = [](NativeCall& c) {
        c.vm.gotoState(c.self, c.n(0), c.n(1));
        return Value();
    };
    n["object.isinstate"] = [](NativeCall& c) {
        Name want = c.n(0);
        for (State* s = c.self->state; s; s = s->super)
            if (s->name == want) return B(true);
        return B(false);
    };
    n["object.getstatename"] = [](NativeCall& c) {
        // In no state an object runs as its class, whose name this gives.
        if (c.self->state) return Value::Nm(c.self->state->name);
        return Value::Nm(c.self->cls ? c.self->cls->name : Name());
    };
    n["object.enable"] = [](NativeCall& c) {
        c.self->disabled.erase(c.n(0));
        return Value();
    };
    n["object.disable"] = [](NativeCall& c) {
        c.self->disabled.insert(c.n(0));
        return Value();
    };
}

}  // namespace ffa
