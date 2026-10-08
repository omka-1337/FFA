#include "script/Types.h"

#include <cmath>
#include <cstdio>

#include "script/Linker.h"

namespace ffa {

// ------------------------------------------------------------------ strings
std::string utf8(const String& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }
    return out;
}

String widen(std::string_view latin1) {
    String out;
    out.reserve(latin1.size());
    for (unsigned char c : latin1) out += char16_t(c);
    return out;
}

String fromUtf8(std::string_view s) {
    String out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6 && i + 1 < s.size()) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14 && i + 2 < s.size()) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30 && i + 3 < s.size()) { cp = c & 0x07; n = 4; }
        else { out += char16_t(c); ++i; continue; }
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (uint8_t(s[i + size_t(k)]) & 0x3F);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += char16_t(0xD800 + (cp >> 10));
            out += char16_t(0xDC00 + (cp & 0x3FF));
        } else {
            out += char16_t(cp);
        }
        i += size_t(n);
    }
    return out;
}

// ------------------------------------------------------------------- values
bool StructVal::operator==(const StructVal& o) const { return f == o.f; }

int32_t Value::i() const {
    if (auto p = std::get_if<int32_t>(&v)) return *p;
    if (auto p = std::get_if<bool>(&v)) return *p ? 1 : 0;
    if (auto p = std::get_if<float>(&v)) {
        float x = *p;
        if (!(std::fabs(x) < 2147483648.0f)) return INT32_MIN;
        return int32_t(x);
    }
    return 0;
}

float Value::f() const {
    if (auto p = std::get_if<float>(&v)) return *p;
    if (auto p = std::get_if<int32_t>(&v)) return float(*p);
    if (auto p = std::get_if<bool>(&v)) return *p ? 1.0f : 0.0f;
    return 0.0f;
}

bool Value::b() const {
    if (auto p = std::get_if<bool>(&v)) return *p;
    if (auto p = std::get_if<int32_t>(&v)) return *p != 0;
    if (auto p = std::get_if<float>(&v)) return *p != 0.0f;
    if (auto p = std::get_if<Object*>(&v)) return *p != nullptr;
    return false;
}

Name Value::n() const {
    if (auto p = std::get_if<Name>(&v)) return *p;
    return Name();
}

String Value::s() const {
    if (auto p = std::get_if<String>(&v)) return *p;
    return String();
}

Object* Value::o() const {
    if (auto p = std::get_if<Object*>(&v)) return *p;
    return nullptr;
}

Delegate Value::d() const {
    if (auto p = std::get_if<Delegate>(&v)) return *p;
    return Delegate();
}

std::string describe(const Value& v) {
    char buf[64];
    switch (v.v.index()) {
    case 0: return "void";
    case 1: return std::to_string(std::get<int32_t>(v.v));
    case 2: std::snprintf(buf, sizeof buf, "%g", double(std::get<float>(v.v))); return buf;
    case 3: return std::get<bool>(v.v) ? "true" : "false";
    case 4: return "'" + std::get<Name>(v.v).str() + "'";
    case 5: return "\"" + utf8(std::get<String>(v.v)) + "\"";
    case 6: {
        Object* o = std::get<Object*>(v.v);
        return o ? o->path() : "None";
    }
    case 7: {
        const StructVal& s = std::get<StructVal>(v.v);
        std::string out = "(";
        const auto* lay = s.type ? &s.type->layout() : nullptr;
        for (size_t i = 0; i < s.f.size(); ++i) {
            if (i) out += ",";
            if (lay) {
                for (Prop* p : *lay)
                    if (size_t(p->slot) <= i && i < size_t(p->slot + p->dim)) {
                        out += p->name.str() + "=";
                        break;
                    }
            }
            out += describe(s.f[i]);
        }
        return out + ")";
    }
    case 8: {
        std::string out = "[";
        const Array& a = std::get<Array>(v.v);
        for (size_t i = 0; i < a.size(); ++i) out += (i ? "," : "") + describe(a[i]);
        return out + "]";
    }
    case 9: {
        Delegate d = std::get<Delegate>(v.v);
        return d.obj ? d.obj->path() + "." + d.func.str() : "None";
    }
    }
    return "?";
}

const char* kindName(Kind k) {
    static const char* n[] = {"byte", "int", "bool", "float", "name", "string", "object",
                              "class", "struct", "array", "delegate", "pointer", "map",
                              "fixedarray", "?"};
    return n[int(k)];
}

// -------------------------------------------------------------------- Prop
void Prop::resolve() const {
    if (resolved_ || !linker) return;
    resolved_ = true;
    switch (kind) {
    case Kind::Struct: struct_ = linker->structRef(pkg, ref); break;
    case Kind::Array: inner_ = linker->propRef(pkg, ref); break;
    case Kind::Byte: enum_ = ref ? linker->enumRef(pkg, ref) : nullptr; break;
    case Kind::Object: class_ = linker->classRef(pkg, ref); break;
    case Kind::Class: class_ = linker->classRef(pkg, ref2); break;
    default: break;
    }
}

StructType* Prop::structType() const { resolve(); return struct_; }
Prop* Prop::inner() const { resolve(); return inner_; }
EnumType* Prop::enumType() const { resolve(); return enum_; }
Class* Prop::objClass() const { resolve(); return class_; }

Value Prop::zero() const {
    switch (kind) {
    case Kind::Byte:
    case Kind::Int:
    case Kind::Pointer: return Value::Int(0);
    case Kind::Float: return Value::Float(0.0f);
    case Kind::Bool: return Value::Bool(false);
    case Kind::Name: return Value::Nm(Name());
    case Kind::Str: return Value::Str(String());
    case Kind::Object:
    case Kind::Class: return Value::Obj(nullptr);
    case Kind::Struct: {
        StructType* st = structType();
        return st ? st->make() : Value::Struct(StructVal());
    }
    case Kind::Array: return Value::Arr(Array());
    case Kind::Delegate: return Value::Dlg(Delegate());
    default: return Value();
    }
}

Value Prop::coerce(Value v) const {
    switch (kind) {
    case Kind::Int:
    case Kind::Pointer: return Value::Int(v.i());
    case Kind::Byte: return Value::Int(v.i() & 0xFF);
    case Kind::Float: return Value::Float(v.f());
    case Kind::Bool: return Value::Bool(v.b());
    case Kind::Name: return Value::Nm(v.n());
    case Kind::Str: return Value::Str(v.s());
    case Kind::Object:
    case Kind::Class: return Value::Obj(v.o());
    case Kind::Struct: return v.isStruct() ? std::move(v) : zero();
    case Kind::Array: return v.isArr() ? std::move(v) : Value::Arr(Array());
    case Kind::Delegate:
        return std::holds_alternative<Delegate>(v.v) ? std::move(v) : Value::Dlg(Delegate());
    default: return v;
    }
}

// --------------------------------------------------------------- StructType
const std::vector<Prop*>& StructType::layout() const {
    if (!built_) {
        built_ = true;
        if (super) {
            layout_ = super->layout();
            slots_ = super->slots();
        }
        for (Prop* p : own) {
            p->slot = slots_;
            slots_ += p->dim;
            layout_.push_back(p);
        }
    }
    return layout_;
}

Prop* StructType::field(Name n) const {
    for (Prop* p : layout())
        if (p->name == n) return p;
    return nullptr;
}

Value StructType::make() const {
    StructVal s;
    s.type = this;
    s.f.reserve(size_t(slots()));
    for (Prop* p : layout())
        for (int i = 0; i < p->dim; ++i) s.f.push_back(p->zero());
    return Value::Struct(std::move(s));
}

bool StructType::isChildOf(const StructType* o) const {
    for (const StructType* s = this; s; s = s->super)
        if (s == o) return true;
    return false;
}

// ---------------------------------------------------------- Function, State
std::string Function::qualname() const {
    std::string c = cls ? cls->name.str() : "?";
    if (state) return c + "." + state->name.str() + "." + name.str();
    return c + "." + name.str();
}

std::string Function::nativeKey() const {
    return lower((cls ? cls->name.str() : std::string("?")) + "." + name.str());
}

std::string State::qualname() const {
    return (cls ? cls->name.str() : std::string("?")) + "." + name.str();
}

// ------------------------------------------------------------------- Object
Object::Object() = default;
Object::~Object() = default;

std::string Object::path() const {
    std::string out = name.str();
    for (const Object* o = outer; o; o = o->outer) out = o->name.str() + "." + out;
    return out;
}

bool Object::isA(const Class* c) const {
    for (const Class* k = cls; k; k = k->super)
        if (k == c) return true;
    return false;
}

// -------------------------------------------------------------------- Class
const std::vector<Prop*>& Class::layout() const {
    if (!built_) {
        built_ = true;
        if (super) {
            layout_ = super->layout();
            slots_ = super->slots();
        }
        for (Prop* p : own) {
            p->slot = slots_;
            slots_ += p->dim;
            layout_.push_back(p);
        }
        // A subclass may declare a variable its parent already has, AppleTree's
        // array<name> ThrowAnimName over KWPawn's name: the subclass's wins.
        // The parent's stays in the layout, and the parent's code still uses it:
        // BanditBoss's int MaxHealth over KWPawn's float, which KWPawn's
        // PostBeginPlay sets. So a name finds the subclass's, but either is a
        // variable of the object.
        for (Prop* p : layout_) {
            propMap_[p->name] = p;
            propSet_.insert(p);
        }
    }
    return layout_;
}

Prop* Class::findProp(Name n) const {
    layout();
    auto it = propMap_.find(n);
    return it == propMap_.end() ? nullptr : it->second;
}

bool Class::hasProp(const Prop* p) const {
    layout();
    return propSet_.count(p) != 0;
}

Function* Class::findFunction(Name n) const {
    auto it = fcache_.find(n);
    if (it != fcache_.end()) return it->second;
    Function* f = nullptr;
    for (const Class* c = this; c && !f; c = c->super) {
        auto j = c->funcs.find(n);
        if (j != c->funcs.end()) f = j->second;
    }
    fcache_.emplace(n, f);
    return f;
}

State* Class::findState(Name n) const {
    auto it = scache_.find(n);
    if (it != scache_.end()) return it->second;
    State* s = nullptr;
    for (const Class* c = this; c && !s; c = c->super) {
        auto j = c->states.find(n);
        if (j != c->states.end()) s = j->second;
    }
    scache_.emplace(n, s);
    return s;
}

State* Class::autoState() const {
    for (const Class* c = this; c; c = c->super)
        for (auto& [n, s] : c->states)
            if (s->flags & STATE_Auto) return findState(n);
    return nullptr;
}

bool Class::isChildOf(const Class* c) const {
    for (const Class* k = this; k; k = k->super)
        if (k == c) return true;
    return false;
}

Object* Class::defaults() {
    if (!defaultObject) defaultObject = linker->buildDefault(this);
    return defaultObject;
}

// -------------------------------------------------------------------- Frame
std::string Frame::where(const Ins* at) const {
    char off[16] = "";
    if (at) std::snprintf(off, sizeof off, ":%04X", at->mem);
    std::string what = fn ? fn->qualname() : st ? st->qualname() : "?";
    return (self ? self->path() : std::string("None")) + " (" + what + off + ")";
}

}  // namespace ffa
