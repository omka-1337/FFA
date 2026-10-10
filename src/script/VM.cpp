#include "script/VM.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ffa {

// ---------------------------------------------------------------- errors
std::string ScriptError::full() const {
    std::string out = what();
    for (const auto& t : trace) out += "\n  in " + t;
    return out;
}

// ----------------------------------------------------------- conversions
int32_t toInt(float f) {
    if (!(std::fabs(f) < 2147483648.0f)) return INT32_MIN;
    return int32_t(f);
}

int32_t parseInt(const String& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    int64_t sign = 1, v = 0;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) sign = s[i++] == '-' ? -1 : 1;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) v = (v * 10 + (s[i] - '0')) & 0xFFFFFFFFLL;
    return int32_t(uint32_t(sign * v));
}

float parseFloat(const String& s) {
    std::string narrow;
    for (char16_t c : s) {
        if (c > 0x7F) break;
        narrow += char(c);
    }
    return std::strtof(narrow.c_str(), nullptr);
}

String formatInt(int32_t i) { return widen(std::to_string(i)); }

String formatFloat(float f) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%f", double(f));
    return widen(buf);
}

// ------------------------------------------------------------------ LRef
Value* LRef::resolve() const {
    Value* v = root ? root : temp.get();
    for (const Step& s : steps) {
        if (!v) return nullptr;
        if (s.element) {
            if (!v->isArr() || s.i < 0 || size_t(s.i) >= v->arr().size()) return nullptr;
            v = &v->arr()[size_t(s.i)];
        } else {
            if (!v->isStruct() || s.i < 0 || size_t(s.i) >= v->st().f.size()) return nullptr;
            v = &v->st().f[size_t(s.i)];
        }
    }
    return v;
}

Value LRef::get() const {
    Value* v = resolve();
    if (length) return Value::Int(v && v->isArr() ? int32_t(v->arr().size()) : 0);
    if (v) return *v;
    return prop ? prop->zero() : Value();
}

void LRef::set(Value x) const {
    Value* v = resolve();
    if (!v) return;
    if (length) {
        if (!v->isArr()) return;
        int32_t n = std::max(0, x.i());
        Array& a = v->arr();
        if (size_t(n) < a.size()) {
            a.resize(size_t(n));
        } else {
            while (a.size() < size_t(n)) a.push_back(prop ? prop->zero() : Value());
        }
        return;
    }
    *v = prop ? prop->coerce(std::move(x)) : std::move(x);
}

LRef LRef::temporary(Value v, const Prop* p) {
    LRef r;
    r.temp = std::make_shared<Value>(std::move(v));
    r.prop = p;
    return r;
}

// ------------------------------------------------------------ NativeCall
Value NativeCall::get(size_t k) {
    Arg& a = args.at(k);
    if (a.skip) return vm.ev(*a.skip, *caller, caller->self);
    if (a.isRef) return a.ref.get();
    return a.v;
}

void NativeCall::out(size_t k, Value v) {
    if (k < args.size() && args[k].isRef) args[k].ref.set(std::move(v));
}

// -------------------------------------------------------------------- VM
VM::VM(Linker& lk) : linker(lk) {
    auto builtin = [&](const char* name, Kind kind, std::initializer_list<const char*> fields) {
        StructType* st = linker.findStruct(name);
        if (st) return st;
        auto s = std::make_unique<StructType>();
        s->name = Name(name);
        for (const char* f : fields) {
            auto p = std::make_unique<Prop>();
            p->name = Name(f);
            p->kind = kind;
            s->own.push_back(p.get());
            builtinProps_.push_back(std::move(p));
        }
        builtin_.push_back(std::move(s));
        return builtin_.back().get();
    };
    vector_ = builtin("Vector", Kind::Float, {"X", "Y", "Z"});
    rotator_ = builtin("Rotator", Kind::Int, {"Pitch", "Yaw", "Roll"});
    const char* vn[] = {"X", "Y", "Z"};
    const char* rn[] = {"Pitch", "Yaw", "Roll"};
    for (int i = 0; i < 3; ++i) {
        if (Prop* p = vector_->field(Name(vn[i]))) vslot_[i] = p->slot;
        if (Prop* p = rotator_->field(Name(rn[i]))) rslot_[i] = p->slot;
    }
    registerCoreNatives(*this);
}

void VM::write(const std::string& tag, const std::string& text) {
    log.emplace_back(tag, text);
    if (sink)
        sink(tag, text);
    else
        std::fprintf(stdout, "%s: %s\n", tag.c_str(), text.c_str());
}

void VM::warn(const Frame* f, const Ins* at, const std::string& msg) {
    write("ScriptWarning", (f ? f->where(at) : std::string("?")) + " " + msg);
}

std::vector<std::string> VM::trace() const {
    std::vector<std::string> out;
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) out.push_back((*it)->where());
    return out;
}

ScriptError VM::error(const std::string& msg) const { return ScriptError(msg, trace()); }

Class* VM::findClass(std::string_view name) {
    Class* c = linker.findClass(name);
    if (!c) throw error("class " + std::string(name) + " not found");
    return c;
}

Object* VM::spawn(Class* c, Name name, Object* outer) {
    auto o = std::make_unique<Object>();
    o->cls = c;
    o->name = name.isNone() ? Name(c->name.str() + std::to_string(objects.size())) : name;
    o->outer = outer;
    o->props = c->defaults()->props;
    Object* raw = o.get();
    objects.push_back(std::move(o));
    linker.setIntrinsics(raw);
    return raw;
}

Value VM::vector(float x, float y, float z) const {
    Value v = vector_->make();
    v.st().f[size_t(vslot_[0])] = Value::Float(x);
    v.st().f[size_t(vslot_[1])] = Value::Float(y);
    v.st().f[size_t(vslot_[2])] = Value::Float(z);
    return v;
}

Value VM::rotator(int32_t p, int32_t y, int32_t r) const {
    Value v = rotator_->make();
    v.st().f[size_t(rslot_[0])] = Value::Int(p);
    v.st().f[size_t(rslot_[1])] = Value::Int(y);
    v.st().f[size_t(rslot_[2])] = Value::Int(r);
    return v;
}

void VM::unvector(const Value& v, float& x, float& y, float& z) const {
    x = y = z = 0;
    if (!v.isStruct() || v.st().f.size() < 3) return;
    x = v.st().f[size_t(vslot_[0])].f();
    y = v.st().f[size_t(vslot_[1])].f();
    z = v.st().f[size_t(vslot_[2])].f();
}

void VM::unrotator(const Value& v, int32_t& p, int32_t& y, int32_t& r) const {
    p = y = r = 0;
    if (!v.isStruct() || v.st().f.size() < 3) return;
    p = v.st().f[size_t(rslot_[0])].i();
    y = v.st().f[size_t(rslot_[1])].i();
    r = v.st().f[size_t(rslot_[2])].i();
}

String VM::toString(const Value& v) {
    if (v.isInt()) return formatInt(v.i());
    if (v.isFloat()) return formatFloat(v.f());
    if (v.isBool()) return widen(v.b() ? "True" : "False");
    if (v.isName()) return widen(v.n().str());
    if (v.isStr()) return v.s();
    if (v.isObj()) return widen(v.o() ? v.o()->path() : "None");
    if (v.isStruct() && v.st().type == rotator_) {
        int32_t p, y, r;
        unrotator(v, p, y, r);
        return formatInt(p) + u"," + formatInt(y) + u"," + formatInt(r);
    }
    if (v.isStruct()) {
        float x, y, z;
        unvector(v, x, y, z);
        return formatFloat(x) + u"," + formatFloat(y) + u"," + formatFloat(z);
    }
    return String();
}

Value* VM::slot(Object* o, const Prop* p, const Frame* f, const Ins* at) {
    if (!o) throw error("variable " + p->name.str() + " read with no object");
    if (o->isClass() && o->props.empty()) linker.initClassObject(static_cast<Class*>(o));
    // A variable belongs to one class; reaching it through an object of an
    // unrelated class would silently read another variable at that slot.
    Class* layoutOf = o->cls;
    if (o->isClass() && !layoutOf) {
        layoutOf = static_cast<Class*>(o);
        while (layoutOf->super) layoutOf = layoutOf->super;
    }
    if (!layoutOf || !layoutOf->hasProp(p) || size_t(p->slot) >= o->props.size()) {
        (void)f;
        (void)at;
        throw error(o->path() + " has no variable " + p->name.str());
    }
    return &o->props[size_t(p->slot)];
}

// =============================================================== compiling
namespace {
std::unordered_map<Name, uint32_t>* labelsOf(Function*) { return nullptr; }
std::unordered_map<Name, uint32_t>* labelsOf(State* s) { return &s->labels; }
}  // namespace

void VM::compile(Function* fn) { compileCode(fn, fn->pkg, fn->idx); }
void VM::compile(State* st) { compileCode(st, st->pkg, st->idx); }

template <class Owner>
void VM::compileCode(Owner* o, int pkg, int idx) {
    if (o->compiled) return;
    ParsedCode pc;
    try {
        pc = linker.bytecode(pkg, idx);
        for (Ins& n : pc.stmts) resolve(n, pkg);
    } catch (const FormatError& e) {
        throw error(o->qualname() + " does not load: " + e.what());
    }
    o->parseOk = pc.aligned && pc.sized;
    std::unordered_map<uint32_t, int> index;
    for (size_t i = 0; i < pc.stmts.size(); ++i) index[pc.stmts[i].mem] = int(i);
    index[pc.mem] = int(pc.stmts.size());
    for (Ins& n : pc.stmts) {
        bool jumps = n.op == Op::Jump || n.op == Op::JumpIfNot || n.op == Op::Iterator ||
                     (n.op == Op::Case && n.raw[0] != 0xFFFF);
        if (jumps) {
            auto it = index.find(uint32_t(n.raw[0]));
            if (it == index.end()) {
                char buf[96];
                std::snprintf(buf, sizeof buf, ": %s at %04X jumps to %04X, inside a statement",
                              opName(n.op), n.mem, unsigned(n.raw[0]));
                throw error(o->qualname() + buf);
            }
            n.target = it->second;
        } else if (n.op == Op::LabelTable && labelsOf(o)) {
            const Package& p = *linker.packages[size_t(pkg)];
            for (size_t i = 0; i + 1 < n.raw.size(); i += 2) {
                Name nm = p.name(int(n.raw[i]));
                if (!nm.isNone()) (*labelsOf(o))[nm] = uint32_t(n.raw[i + 1]);
            }
        }
    }
    o->code = std::move(pc.stmts);
    o->index = std::move(index);
    o->compiled = true;
}


void VM::resolve(Ins& n, int pkg) {
    for (Ins& k : n.kids) resolve(k, pkg);
    const Package& p = *linker.packages[size_t(pkg)];
    auto ref = [&]() -> int32_t {
        if (n.raw.empty()) throw FormatError(std::string(opName(n.op)) + " without its operand");
        return int32_t(n.raw[0]);
    };
    switch (n.op) {
    case Op::LocalVariable:
    case Op::InstanceVariable:
    case Op::DefaultVariable:
    case Op::StateVariable:
    case Op::NativeParm:
    case Op::StructMember:
        n.prop = linker.propRef(pkg, ref());
        if (!n.prop) throw FormatError(std::string(opName(n.op)) + " does not name a variable");
        break;
    case Op::VirtualFunction:
    case Op::GlobalFunction:
    case Op::NameConst:
    case Op::InstanceDelegate:
    case Op::DelegateProperty:
        n.name = p.name(ref());
        break;
    case Op::DelegateFunction:
        if (n.raw.size() < 2) throw FormatError("DelegateFunction without its operands");
        n.prop = linker.propRef(pkg, int32_t(n.raw[0]));
        if (!n.prop) throw FormatError("DelegateFunction does not name a delegate");
        n.name = p.name(int32_t(n.raw[1]));
        break;
    case Op::FinalFunction:
        n.fn = linker.functionRef(pkg, ref());
        if (!n.fn) throw FormatError("final call to something that is not a function");
        break;
    case Op::NativeCall:
        n.fn = linker.native(n.native);
        if (!n.fn) throw FormatError("native " + std::to_string(n.native) + " is not declared in any package");
        break;
    case Op::ObjectConst:
        n.obj = linker.objectRef(pkg, ref());
        break;
    case Op::DynamicCast:
    case Op::Metacast:
        n.obj = linker.classRef(pkg, ref());
        break;
    case Op::StructCmpEq:
    case Op::StructCmpNe:
        n.st = linker.structRef(pkg, ref());
        break;
    default:
        break;
    }
}

// ================================================================= calling
Function* VM::findVirtual(Object* self, Name name) {
    // The current state and the states it extends first, then the classes.
    for (State* s = self->state; s; s = s->super) {
        auto it = s->funcs.find(name);
        if (it != s->funcs.end()) return it->second;
    }
    return self->cls ? self->cls->findFunction(name) : nullptr;
}

std::pair<Function*, Object*> VM::bindDelegate(Function* fn, Object* self) {
    // A delegate call goes to the function assigned to it, held in a hidden
    // variable named after it; with none assigned, the delegate's own body runs.
    if (self && self->cls) {
        Prop* p = self->cls->findProp(Name("__" + fn->name.str() + "__Delegate"));
        if (p && size_t(p->slot) < self->props.size()) {
            Delegate d = self->props[size_t(p->slot)].d();
            if (d.obj && !d.obj->deleted)
                if (Function* f2 = findVirtual(d.obj, d.func)) return {f2, d.obj};
        }
    }
    return {fn, self};
}

namespace {
void zeroLocals(Function* fn, std::vector<Value>& locals) {
    locals.resize(size_t(fn->slots));
    auto fill = [&](Prop* p) {
        for (int i = 0; i < p->dim; ++i) locals[size_t(p->slot + i)] = p->zero();
    };
    for (Prop* p : fn->params) fill(p);
    if (fn->ret) fill(fn->ret);
    for (Prop* p : fn->locals) fill(p);
}
}  // namespace

Value VM::invoke(Function* fn, Object* self, const std::vector<Ins>& args, Frame& f, bool bind) {
    // A delegate called goes where it is bound; called by name through super
    // it is the parent's own body, as ShInGameMenuGUIPage's Internal_OnDraw
    // ends with super.OnDraw(Canvas).
    if (bind && (fn->flags & FUNC_Delegate)) std::tie(fn, self) = bindDelegate(fn, self);
    if (fn->isNative()) return callNative(fn, self, args, f);
    if ((fn->flags & FUNC_Singular) && self->singular) return fn->ret ? fn->ret->zero() : Value();
    compile(fn);
    Frame callee;
    callee.fn = fn;
    callee.self = self;
    callee.code = &fn->code;
    zeroLocals(fn, callee.locals);
    std::vector<std::pair<Prop*, LRef>> outs;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        Prop* p = fn->params[i];
        if (i >= args.size() || args[i].op == Op::Nothing) continue;
        if (p->isOut()) {
            LRef r = lv(args[i], f, f.self, true);
            callee.locals[size_t(p->slot)] = p->coerce(r.get());
            outs.emplace_back(p, std::move(r));
        } else {
            callee.locals[size_t(p->slot)] = p->coerce(ev(args[i], f, f.self));
        }
    }
    Value result = execute(callee);
    // Out parameters are copied back when the call returns, as the engine does.
    for (auto& [p, r] : outs) r.set(callee.locals[size_t(p->slot)]);
    return result;
}

Value VM::execute(Frame& callee) {
    if (stack_.size() >= recursion)
        throw error("infinite script recursion (" + std::to_string(recursion) + " calls)");
    bool singular = callee.fn && (callee.fn->flags & FUNC_Singular);
    if (singular) callee.self->singular = true;
    stack_.push_back(&callee);
    try {
        run(callee);
    } catch (...) {
        stack_.pop_back();
        if (singular) callee.self->singular = false;
        throw;
    }
    stack_.pop_back();
    if (singular) callee.self->singular = false;
    return std::move(callee.result);
}

void VM::prepareNative(NativeCall& c, const std::vector<Ins>& args, Frame& f) {
    Function* fn = c.fn;
    for (size_t i = 0; i < fn->params.size(); ++i) {
        Prop* p = fn->params[i];
        NativeCall::Arg a;
        if (i >= args.size() || args[i].op == Op::Nothing) {
            a.omitted = true;
            a.v = p->zero();
            if (p->isOut()) {
                a.isRef = true;
                a.ref = LRef::temporary(p->zero(), p);
            }
        } else if (args[i].op == Op::Skip) {
            a.skip = &args[i].kids[0];
        } else if (p->isOut()) {
            a.isRef = true;
            a.ref = lv(args[i], f, f.self, true);
        } else {
            a.v = ev(args[i], f, f.self);
        }
        c.args.push_back(std::move(a));
    }
}

Value VM::callNative(Function* fn, Object* self, const std::vector<Ins>& args, Frame& f) {
    NativeCall c(*this, self, fn, &f);
    prepareNative(c, args, f);
    return runNative(c);
}

NativeFn VM::nativeOf(Function* fn) {
    // Looked up by name once, then kept on the function: operators are
    // natives, and are called more than anything else.
    if (fn->impl) return reinterpret_cast<NativeFn>(fn->impl);
    auto it = natives.find(fn->nativeKey());
    if (it == natives.end()) return nullptr;
    fn->impl = reinterpret_cast<void (*)()>(it->second);
    return it->second;
}

Value VM::runNative(NativeCall& c) {
    NativeFn impl = nativeOf(c.fn);
    if (!impl) {
        // A latent function the VM lacks waits one tick, so that state code
        // looping on it gives way each tick, as it would, and does not spin.
        if ((c.fn->flags & FUNC_Latent) && c.self) c.self->latent = [](float) { return true; };
        return noNative(c.fn);
    }
    Value r = impl(c);
    if (c.fn->ret && !(c.fn->flags & FUNC_Iterator)) return c.fn->ret->coerce(std::move(r));
    return r;
}

Value VM::noNative(Function* fn) {
    if (strict) throw NativeMissing("native " + fn->qualname() + " is not implemented", trace());
    missingCalls[fn->qualname()]++;
    if (missing.insert(fn->nativeKey()).second) write("Error", "native " + fn->qualname() + " is not implemented");
    return fn->ret ? fn->ret->zero() : Value();
}

Value VM::call(Object* self, std::string_view name, std::vector<Value> args) {
    Function* fn = findVirtual(self, Name(name));
    if (!fn) throw error(self->path() + " has no function " + std::string(name));
    return callFunction(fn, self, std::move(args));
}

Value VM::callStatic(std::string_view cls, std::string_view name, std::vector<Value> args) {
    return call(findClass(cls)->defaults(), name, std::move(args));
}

Value VM::callFunction(Function* fn, Object* self, std::vector<Value> args) {
    if (fn->flags & FUNC_Delegate) std::tie(fn, self) = bindDelegate(fn, self);
    if (fn->isNative()) {
        Frame outside;
        outside.self = self;
        NativeCall c(*this, self, fn, &outside);
        for (size_t i = 0; i < fn->params.size(); ++i) {
            Prop* p = fn->params[i];
            NativeCall::Arg a;
            if (i < args.size()) {
                if (p->isOut()) {
                    a.isRef = true;
                    a.ref = LRef::temporary(p->coerce(args[i]), p);
                } else {
                    a.v = args[i];
                }
            } else {
                a.omitted = true;
                a.v = p->zero();
                if (p->isOut()) {
                    a.isRef = true;
                    a.ref = LRef::temporary(p->zero(), p);
                }
            }
            c.args.push_back(std::move(a));
        }
        return runNative(c);
    }
    compile(fn);
    Frame callee;
    callee.fn = fn;
    callee.self = self;
    callee.code = &fn->code;
    zeroLocals(fn, callee.locals);
    for (size_t i = 0; i < fn->params.size() && i < args.size(); ++i)
        callee.locals[size_t(fn->params[i]->slot)] = fn->params[i]->coerce(args[i]);
    return execute(callee);
}

Value VM::event(Object* self, std::string_view name, std::vector<Value> args) {
    Name n(name);
    if (self->deleted || self->disabled.count(n)) return Value();
    Function* fn = findVirtual(self, n);
    if (!fn) return Value();
    return callFunction(fn, self, std::move(args));
}

Value VM::eventOut(Object* self, std::string_view name, std::vector<Value>& args) {
    Name n(name);
    if (self->deleted || self->disabled.count(n)) return Value();
    Function* fn = findVirtual(self, n);
    if (!fn) return Value();
    if (fn->flags & FUNC_Delegate) std::tie(fn, self) = bindDelegate(fn, self);
    if (fn->isNative()) return callFunction(fn, self, args);
    compile(fn);
    Frame callee;
    callee.fn = fn;
    callee.self = self;
    callee.code = &fn->code;
    zeroLocals(fn, callee.locals);
    for (size_t i = 0; i < fn->params.size() && i < args.size(); ++i)
        callee.locals[size_t(fn->params[i]->slot)] = fn->params[i]->coerce(args[i]);
    Value r = execute(callee);
    for (size_t i = 0; i < fn->params.size() && i < args.size(); ++i)
        if (fn->params[i]->isOut()) args[i] = callee.locals[size_t(fn->params[i]->slot)];
    return r;
}

// ============================================================== statements
void VM::run(Frame& f) {
    const std::vector<Ins>& code = *f.code;
    size_t steps = 0;
    while (!f.done && f.pc < code.size()) {
        const Ins& n = code[f.pc++];
        stmt(n, f);
        if (++steps > runaway) throw error("runaway loop: more than " + std::to_string(runaway) + " statements");
    }
}

void VM::stmt(const Ins& n, Frame& f) {
    switch (n.op) {
    case Op::DynArrayInsert:
    case Op::DynArrayRemove: {
        // array.Insert(Index, Count) and array.Remove(Index, Count). The
        // engine refuses an index or count out of range, with a warning.
        LRef b = lv(n.kids[0], f, f.self, true);
        Prop* inner = b.prop ? b.prop->inner() : nullptr;
        int32_t at = ev(n.kids[1], f, f.self).i(), count = ev(n.kids[2], f, f.self).i();
        Value* a = b.resolve();
        if (!a || !a->isArr()) return;
        Array& arr = a->arr();
        bool insert = n.op == Op::DynArrayInsert;
        if (at < 0 || count < 0 || size_t(at) > arr.size() ||
            (!insert && size_t(at) + size_t(count) > arr.size())) {
            warn(&f, &n, std::string("Attempt to ") + (insert ? "insert " : "remove ") +
                             std::to_string(count) + " elements at " + std::to_string(at) +
                             " in an " + std::to_string(arr.size()) + "-element array");
            return;
        }
        if (insert)
            arr.insert(arr.begin() + at, size_t(count), inner ? inner->zero() : Value());
        else
            arr.erase(arr.begin() + at, arr.begin() + at + count);
        return;
    }
    case Op::Let:
    case Op::LetBool:
    case Op::LetDelegate: {
        // The value is computed before the variable is located, so that a
        // call on the right that grows an array cannot move the target away.
        Value v = ev(n.kids[1], f, f.self);
        lv(n.kids[0], f, f.self, true).set(std::move(v));
        return;
    }
    case Op::Return: {
        Prop* ret = f.fn ? f.fn->ret : nullptr;
        Value v;
        if (n.kids.empty() || n.kids[0].op == Op::Nothing)
            v = ret ? f.locals[size_t(ret->slot)] : Value();
        else
            v = ev(n.kids[0], f, f.self);
        f.result = ret ? ret->coerce(std::move(v)) : std::move(v);
        f.done = true;
        return;
    }
    case Op::Jump:
        f.pc = size_t(n.target);
        return;
    case Op::JumpIfNot:
        if (!ev(n.kids[0], f, f.self).b()) f.pc = size_t(n.target);
        return;
    case Op::Switch:
        doSwitch(n, f);
        return;
    case Op::Case:              // reached by falling through: no test
    case Op::Nothing:
    case Op::LabelTable:
    case Op::EndParmValue:
    case Op::IteratorNext:
    case Op::IteratorPop:
        return;
    case Op::Stop:
        f.done = true;
        return;
    case Op::GotoLabel: {
        Name label = ev(n.kids[0], f, f.self).n();
        if (!gotoLabel(f.self, label)) warn(&f, &n, "goto " + label.str() + ": label not found");
        f.done = true;
        return;
    }
    case Op::Assert:
        if (!ev(n.kids.back(), f, f.self).b())
            throw error("assertion failed, line " + std::to_string(n.raw[0]));
        return;
    case Op::Iterator:
        iterate(n, f);
        return;
    default:
        ev(n, f, f.self);
        return;
    }
}

void VM::doSwitch(const Ins& n, Frame& f) {
    // The value is compared with each case in turn. A case that does not match
    // sends control on to the next case; a match, or the default, continues
    // into the statements that follow it.
    Value v = ev(n.kids[0], f, f.self);
    const std::vector<Ins>& code = *f.code;
    while (f.pc < code.size()) {
        const Ins& c = code[f.pc];
        if (c.op != Op::Case) return;
        if (c.raw[0] == 0xFFFF || v == ev(c.kids[0], f, f.self)) {
            ++f.pc;
            return;
        }
        f.pc = size_t(c.target);
    }
}

void VM::iterate(const Ins& n, Frame& f) {
    // foreach. The engine's iterator natives drive the loop body themselves:
    // each element is stored through the out arguments, then the statements
    // run up to IteratorNext, which fetches the next element, or IteratorPop,
    // which `break` and `return` emit to leave early. When the elements run
    // out, control continues past the IteratorPop at the end offset.
    const std::vector<Ins>& code = *f.code;
    auto finish = [&] {
        size_t end = size_t(n.target);
        f.pc = end < code.size() && code[end].op == Op::IteratorPop ? end + 1 : end;
    };
    const Ins* call = &n.kids[0];
    Object* ctx = f.self;
    while (call->op == Op::Context) {
        ctx = ev(call->kids[0], f, ctx).o();
        if (!ctx) {
            warn(&f, call, "Accessed None");
            finish();
            return;
        }
        call = &call->kids[1];
    }
    Function* fn = nullptr;
    if (call->op == Op::NativeCall || call->op == Op::FinalFunction)
        fn = call->fn;
    else if (call->op == Op::VirtualFunction || call->op == Op::GlobalFunction)
        fn = findVirtual(ctx, call->name);
    if (!fn || !fn->isNative()) throw error("foreach over something that is not an iterator");
    NativeCall c(*this, ctx, fn, &f);
    prepareNative(c, call->kids, f);
    if (NativeFn impl = nativeOf(fn))
        impl(c);
    else
        noNative(fn);
    size_t body = f.pc, steps = 0;
    for (auto& row : c.rows) {
        // a row holds one value per out parameter, left out or not
        size_t k = 0;
        for (auto& a : c.args) {
            if (!a.isRef) continue;
            if (k < row.size() && !a.omitted) a.ref.set(row[k]);
            ++k;
        }
        f.pc = body;
        while (true) {
            if (f.pc >= code.size()) throw error("foreach ran past the end of the code");
            const Ins& s = code[f.pc++];
            if (s.op == Op::IteratorNext) break;
            if (s.op == Op::IteratorPop) return;
            stmt(s, f);
            if (f.done) return;
            if (++steps > runaway) throw error("runaway foreach");
        }
    }
    // Run to its end, the engine's iterators leave their object out None, as
    // they clear it before looking for each next one: KWCutController finds
    // its pawn by tag with a foreach that stops when one matches, and a
    // cutscene's MAIN, which matches none, took the last pawn instead, the
    // camera's target, and the camera stood still while it played.
    for (auto& a : c.args)
        if (a.isRef && !a.omitted && a.ref.get().isObj()) a.ref.set(Value::Obj(nullptr));
    finish();
}

// ================================================================== states
bool VM::gotoState(Object* o, Name state, Name label) {
    // Leave the current state through EndState and enter the new one through
    // BeginState, then resume its code at the label, Begin when none is given.
    // Either notification may itself change state, and then the change in
    // progress gives way to it.
    if (!o->cls) return false;
    Name current = o->state ? o->state->name : o->cls->name;
    if (state != current) {
        State* next = nullptr;
        if (state == Name("Auto")) {
            next = o->cls->autoState();
        } else if (!state.isNone()) {
            next = o->cls->findState(state);
            if (!next) {
                write("ScriptWarning", o->path() + ": GotoState " + state.str() + ": state not found");
                return false;
            }
        }
        State* old = o->state;
        uint32_t mark = ++o->stateChanges;
        if (old && old != next) {
            event(o, "EndState");
            if (o->stateChanges != mark) return false;
        }
        o->state = next;
        o->code.reset();
        o->latent = nullptr;
        if (next && next != old) {
            event(o, "BeginState");
            if (o->stateChanges != mark) return false;
        }
    }
    if (!gotoLabel(o, label.isNone() ? Name("Begin") : label) && !label.isNone())
        write("ScriptWarning", o->path() + ": GotoState " + state.str() + " " + label.str() +
                                   ": label not found");
    return true;
}

bool VM::gotoLabel(Object* o, Name label) {
    // A label is looked for through the state's parents, so a state that
    // extends another can resume in the parent's code.
    o->latent = nullptr;
    o->code.reset();
    if (!o->state || label.isNone()) return false;
    for (State* s = o->state; s; s = s->super) {
        compile(s);
        auto it = s->labels.find(label);
        if (it == s->labels.end()) continue;
        auto j = s->index.find(it->second);
        if (j == s->index.end()) throw error(s->qualname() + ": label " + label.str() + " is not a statement");
        auto fr = std::make_shared<Frame>();
        fr->st = s;
        fr->self = o;
        fr->code = &s->code;
        fr->pc = size_t(j->second);
        o->code = fr;
        return true;
    }
    return false;
}

void VM::processState(Object* o, float dt) {
    // One tick of state code: poll the latent action it waits on, then run
    // until it waits again, stops, runs out, or changes state too often.
    if (o->latent && o->latent(dt)) o->latent = nullptr;
    State* node = nullptr;
    int changes = 0;
    size_t steps = 0;
    while (true) {
        std::shared_ptr<Frame> f = o->code;     // kept alive while it runs
        if (!f || o->latent || o->deleted) return;
        if (f->st != node) {
            if (node && ++changes > 4) return;
            node = f->st;
        }
        if (f->pc >= f->code->size()) {
            o->code.reset();
            return;
        }
        const Ins& n = (*f->code)[f->pc++];
        stack_.push_back(f.get());
        try {
            stmt(n, *f);
        } catch (...) {
            stack_.pop_back();
            throw;
        }
        stack_.pop_back();
        if (f->done && o->code == f) o->code.reset();
        if (++steps > runaway) throw error("runaway state code in " + o->path());
    }
}

// ============================================================= expressions
namespace {
bool isA(Object* o, Object* cls) {
    if (!o || o->deleted || !cls || !cls->isClass()) return false;
    if (o->isA(static_cast<Class*>(cls))) return true;
    return o->isClass() && cls->name == Name("Class");
}
}  // namespace

Value VM::ev(const Ins& n, Frame& f, Object* ctx) {
    switch (n.op) {
    case Op::LocalVariable:
    case Op::NativeParm:
        return f.locals.at(size_t(n.prop->slot));
    case Op::InstanceVariable:
    case Op::StateVariable:
        return *slot(ctx, n.prop, &f, &n);
    case Op::DefaultVariable:
        if (!ctx || !ctx->cls) throw error("default of an object with no class");
        return *slot(ctx->cls->defaults(), n.prop, &f, &n);
    case Op::BoolVariable:
    case Op::Skip:
        return ev(n.kids[0], f, ctx);
    case Op::Self:
        return Value::Obj(ctx);
    case Op::Nothing:
    case Op::EndFunctionParms:
        return Value();
    case Op::NoObject:
        return Value::Obj(nullptr);
    case Op::NoDelegate:
        return Value::Dlg(Delegate());
    case Op::IntZero:
        return Value::Int(0);
    case Op::IntOne:
        return Value::Int(1);
    case Op::True:
        return Value::Bool(true);
    case Op::False:
        return Value::Bool(false);
    case Op::IntConst:
        return Value::Int(int32_t(uint32_t(n.raw[0])));
    case Op::IntConstByte:
    case Op::ByteConst:
        return Value::Int(int32_t(n.raw[0]));
    case Op::FloatConst:
        return Value::Float(n.fv[0]);
    case Op::StringConst:
    case Op::UnicodeStringConst:
        return Value::Str(n.str);
    case Op::NameConst:
        return Value::Nm(n.name);
    case Op::ObjectConst:
        return Value::Obj(n.obj);
    case Op::VectorConst:
        return vector(n.fv[0], n.fv[1], n.fv[2]);
    case Op::RotationConst:
        return rotator(int32_t(uint32_t(n.raw[0])), int32_t(uint32_t(n.raw[1])),
                       int32_t(uint32_t(n.raw[2])));
    case Op::Context: {
        Object* o = ev(n.kids[0], f, ctx).o();
        if (!o || o->deleted) {
            warn(&f, &n, "Accessed None");
            return zeroOf(n.kids[1]);
        }
        return ev(n.kids[1], f, o);
    }
    case Op::ClassContext: {
        Object* c = ev(n.kids[0], f, ctx).o();
        if (!c) {
            warn(&f, &n, "Accessed None");
            return zeroOf(n.kids[1]);
        }
        if (!c->isClass()) throw error("class context of " + c->path() + ", which is not a class");
        return ev(n.kids[1], f, static_cast<Class*>(c)->defaults());
    }
    case Op::ArrayElement:
    case Op::DynArrayElement:
    case Op::DynArrayLength:
    case Op::StructMember:
        return lv(n, f, ctx, false).get();
    case Op::EatString:
        ev(n.kids[0], f, ctx);
        return Value();
    case Op::VirtualFunction: {
        if (!ctx) throw error("call of " + n.name.str() + " with no object");
        Function* fn = findVirtual(ctx, n.name);
        if (!fn) throw error(ctx->path() + " has no function " + n.name.str());
        return invoke(fn, ctx, n.kids, f);
    }
    case Op::GlobalFunction: {
        Function* fn = ctx && ctx->cls ? ctx->cls->findFunction(n.name) : nullptr;
        if (!fn) throw error("no global function " + n.name.str());
        return invoke(fn, ctx, n.kids, f);
    }
    case Op::FinalFunction:
        return invoke(n.fn, ctx, n.kids, f, false);
    case Op::NativeCall:
        return callNative(n.fn, ctx, n.kids, f);
    case Op::InstanceDelegate:
    case Op::DelegateProperty:
        return Value::Dlg(Delegate{ctx, n.name});
    case Op::DelegateFunction: {
        // Call through the delegate property: its bound function on its
        // object, or, unbound, the delegate's own body on this object.
        if (!ctx) throw error("delegate call " + n.name.str() + " with no object");
        Delegate d = slot(ctx, n.prop, &f, &n)->d();
        Object* self = d.obj && !d.obj->deleted ? d.obj : ctx;
        Name fname = d.obj && !d.obj->deleted ? d.func : n.name;
        Function* fn = findVirtual(self, fname);
        if (!fn) throw error(self->path() + " has no function " + fname.str());
        return invoke(fn, self, n.kids, f);
    }
    case Op::DynamicCast: {
        Object* o = ev(n.kids[0], f, f.self).o();
        return Value::Obj(isA(o, n.obj) ? o : nullptr);
    }
    case Op::Metacast: {
        Object* o = ev(n.kids[0], f, f.self).o();
        bool ok = o && o->isClass() && n.obj && n.obj->isClass() &&
                  static_cast<Class*>(o)->isChildOf(static_cast<Class*>(n.obj));
        return Value::Obj(ok ? o : nullptr);
    }
    case Op::StructCmpEq:
        return Value::Bool(ev(n.kids[0], f, f.self) == ev(n.kids[1], f, f.self));
    case Op::StructCmpNe:
        return Value::Bool(ev(n.kids[0], f, f.self) != ev(n.kids[1], f, f.self));
    case Op::New: {
        // new(Outer, Name, Flags) Class, each part optional
        Value parts[4];
        for (size_t i = 0; i < 4 && i < n.kids.size(); ++i)
            if (n.kids[i].op != Op::Nothing) parts[i] = ev(n.kids[i], f, f.self);
        Object* c = parts[3].o();
        if (!c || !c->isClass()) throw error("new of something that is not a class");
        return Value::Obj(spawn(static_cast<Class*>(c), parts[1].n(), parts[0].o()));
    }
    case Op::Cast:
        return cast(n, ev(n.kids[0], f, f.self));
    default:
        throw error(std::string("cannot evaluate ") + opName(n.op));
    }
}

Value VM::zeroOf(const Ins& n) {
    // The value an expression has when its context is none: its type's
    // default, which takes knowing its type.
    switch (n.op) {
    case Op::LocalVariable:
    case Op::InstanceVariable:
    case Op::DefaultVariable:
    case Op::StateVariable:
    case Op::NativeParm:
    case Op::StructMember:
        return n.prop->zero();
    case Op::FinalFunction:
    case Op::NativeCall:
        return n.fn->ret ? n.fn->ret->zero() : Value();
    case Op::VirtualFunction:
    case Op::GlobalFunction: {
        Function* fn = anyFunction(n.name);
        return fn && fn->ret ? fn->ret->zero() : Value();
    }
    case Op::ArrayElement:
    case Op::Context:
    case Op::ClassContext:
    case Op::BoolVariable:
        return zeroOf(n.kids.back());
    case Op::DynArrayElement: {
        const Ins& base = n.kids[1];
        if (base.prop && base.prop->inner()) return base.prop->inner()->zero();
        return Value();
    }
    case Op::DynArrayLength:
        return Value::Int(0);
    default:
        return Value();
    }
}

Function* VM::anyFunction(Name name) {
    // Overrides share a signature, so any declaration gives the return type.
    if (!anyFnBuilt_) {
        anyFnBuilt_ = true;
        for (int k = 0; k < int(linker.packages.size()); ++k) {
            const Package& p = *linker.packages[size_t(k)];
            for (int i = 1; i <= int(p.exports.size()); ++i)
                if (p.classOf(i) == "Function") anyFn_.emplace(Name(p.exp(i).name), nullptr);
        }
    }
    auto it = anyFn_.find(name);
    if (it == anyFn_.end()) return nullptr;
    if (!it->second) {
        for (int k = 0; k < int(linker.packages.size()) && !it->second; ++k) {
            const Package& p = *linker.packages[size_t(k)];
            for (int i = 1; i <= int(p.exports.size()); ++i)
                if (p.classOf(i) == "Function" && Name(p.exp(i).name) == name) {
                    it->second = linker.functionAt(k, i);
                    break;
                }
        }
    }
    return it->second;
}

// ================================================================= lvalues
LRef VM::lv(const Ins& n, Frame& f, Object* ctx, bool write) {
    switch (n.op) {
    case Op::LocalVariable:
    case Op::NativeParm: {
        LRef r;
        r.root = &f.locals.at(size_t(n.prop->slot));
        r.prop = n.prop;
        return r;
    }
    case Op::InstanceVariable:
    case Op::StateVariable: {
        LRef r;
        r.root = slot(ctx, n.prop, &f, &n);
        r.prop = n.prop;
        return r;
    }
    case Op::DefaultVariable: {
        if (!ctx || !ctx->cls) throw error("default of an object with no class");
        LRef r;
        r.root = slot(ctx->cls->defaults(), n.prop, &f, &n);
        r.prop = n.prop;
        return r;
    }
    case Op::BoolVariable:
        return lv(n.kids[0], f, ctx, write);
    case Op::ArrayElement: {
        // A static array is consecutive variables. The engine clamps an index
        // out of range, with a warning, and carries on.
        int32_t i = ev(n.kids[0], f, f.self).i();
        LRef b = lv(n.kids[1], f, ctx, write);
        int dim = b.prop ? b.prop->dim : 1;
        if (i < 0 || i >= dim) {
            warn(&f, &n, "Accessed array out of bounds (" + std::to_string(i) + "/" + std::to_string(dim) + ")");
            i = std::clamp(i, 0, dim - 1);
        }
        if (b.temp) return LRef::temporary(b.prop ? b.prop->zero() : Value(), b.prop);
        if (b.steps.empty())
            b.root += i;
        else
            b.steps.back().i += i;
        return b;
    }
    case Op::DynArrayElement: {
        // Writing past the end of a dynamic array grows it, which script uses
        // to append: A[A.Length] = X. Reading past it is a warning and zero.
        int32_t i = ev(n.kids[0], f, f.self).i();
        LRef b = lv(n.kids[1], f, ctx, write);
        Prop* inner = b.prop ? b.prop->inner() : nullptr;
        Value* a = b.resolve();
        if (!a || !a->isArr()) return LRef::temporary(inner ? inner->zero() : Value(), inner);
        Array& arr = a->arr();
        if (i < 0 || size_t(i) >= arr.size()) {
            if (write && i >= 0) {
                while (arr.size() <= size_t(i)) arr.push_back(inner ? inner->zero() : Value());
            } else {
                warn(&f, &n, "Accessed array out of bounds (" + std::to_string(i) + "/" +
                                 std::to_string(arr.size()) + ")");
                return LRef::temporary(inner ? inner->zero() : Value(), inner);
            }
        }
        b.steps.push_back({true, i});
        b.prop = inner;
        return b;
    }
    case Op::DynArrayLength: {
        LRef b = lv(n.kids[0], f, ctx, write);
        b.length = true;
        b.prop = b.prop ? b.prop->inner() : nullptr;
        return b;
    }
    case Op::StructMember: {
        LRef b = lv(n.kids[0], f, ctx, write);
        Value* s = b.resolve();
        if (!s || !s->isStruct()) return LRef::temporary(n.prop->zero(), n.prop);
        n.prop->structType();       // nothing to resolve; the field's slot is laid out
        b.steps.push_back({false, n.prop->slot});
        b.prop = n.prop;
        return b;
    }
    case Op::Context: {
        Object* o = ev(n.kids[0], f, ctx).o();
        if (!o || o->deleted) {
            warn(&f, &n, "Accessed None");
            return LRef::temporary(zeroOf(n.kids[1]));
        }
        return lv(n.kids[1], f, o, write);
    }
    case Op::ClassContext: {
        Object* c = ev(n.kids[0], f, ctx).o();
        if (!c || !c->isClass()) {
            warn(&f, &n, "Accessed None");
            return LRef::temporary(zeroOf(n.kids[1]));
        }
        return lv(n.kids[1], f, static_cast<Class*>(c)->defaults(), write);
    }
    default:
        return LRef::temporary(ev(n, f, ctx));
    }
}

// =================================================================== casts
// The conversion tokens 0x39 to 0x59. Which conversion each number is follows
// the engine's published token list; `ffa-script check` tallies the declared
// type of every operand each token is given across a corpus, which confirms or
// refutes the mapping. 0x46 is not in that list and is refused, not guessed.
namespace {
void axes(int32_t pitch, int32_t yaw, int32_t roll, float out[3][3]) {
    const double k = M_PI / 32768.0;
    double p = pitch * k, y = yaw * k, r = roll * k;
    double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y),
           sr = std::sin(r), cr = std::cos(r);
    double m[3][3] = {{cp * cy, cp * sy, sp},
                      {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
                      {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = float(m[i][j]);
}

std::vector<String> splitComma(const String& s) {
    std::vector<String> out(1);
    for (char16_t c : s) {
        if (c == u',')
            out.emplace_back();
        else
            out.back() += c;
    }
    while (out.size() < 3) out.emplace_back();
    return out;
}
}  // namespace

void rotationAxes(int32_t pitch, int32_t yaw, int32_t roll, float out[3][3]) {
    axes(pitch, yaw, roll, out);
}

Value VM::cast(const Ins& n, Value v) {
    switch (n.code) {
    case 0x39: {    // RotatorToVector
        int32_t p, y, r;
        unrotator(v, p, y, r);
        float m[3][3];
        axes(p, y, r, m);
        return vector(m[0][0], m[0][1], m[0][2]);
    }
    case 0x3A: return Value::Int(v.i());                    // ByteToInt
    case 0x3B: return Value::Bool(v.i() != 0);              // ByteToBool
    case 0x3C: return Value::Float(float(v.i()));           // ByteToFloat
    case 0x3D: return Value::Int(v.i() & 0xFF);             // IntToByte
    case 0x3E: return Value::Bool(v.i() != 0);              // IntToBool
    case 0x3F: return Value::Float(float(v.i()));           // IntToFloat
    case 0x40:                                              // BoolToByte
    case 0x41: return Value::Int(v.b() ? 1 : 0);            // BoolToInt
    case 0x42: return Value::Float(v.b() ? 1.0f : 0.0f);    // BoolToFloat
    case 0x43: return Value::Int(toInt(v.f()) & 0xFF);      // FloatToByte
    case 0x44: return Value::Int(toInt(v.f()));             // FloatToInt
    case 0x45: return Value::Bool(v.f() != 0.0f);           // FloatToBool
    case 0x47: return Value::Bool(v.o() != nullptr);        // ObjectToBool
    case 0x48: return Value::Bool(!v.n().isNone());         // NameToBool
    case 0x49: return Value::Int(parseInt(v.s()) & 0xFF);   // StringToByte
    case 0x4A: return Value::Int(parseInt(v.s()));          // StringToInt
    case 0x4B: {                                            // StringToBool
        String s = v.s();
        return Value::Bool(iequals(utf8(s), "true") || parseInt(s) != 0);
    }
    case 0x4C: return Value::Float(parseFloat(v.s()));      // StringToFloat
    case 0x4D: {                                            // StringToVector
        auto p = splitComma(v.s());
        return vector(parseFloat(p[0]), parseFloat(p[1]), parseFloat(p[2]));
    }
    case 0x4E: {                                            // StringToRotator
        auto p = splitComma(v.s());
        return rotator(parseInt(p[0]), parseInt(p[1]), parseInt(p[2]));
    }
    case 0x4F: {                                            // VectorToBool
        float x, y, z;
        unvector(v, x, y, z);
        return Value::Bool(x != 0 || y != 0 || z != 0);
    }
    case 0x50: {                                            // VectorToRotator
        float x, y, z;
        unvector(v, x, y, z);
        const double k = 32768.0 / M_PI;
        return rotator(toInt(float(std::atan2(z, std::sqrt(double(x) * x + double(y) * y)) * k)),
                       toInt(float(std::atan2(y, x) * k)), 0);
    }
    case 0x51: {                                            // RotatorToBool
        int32_t p, y, r;
        unrotator(v, p, y, r);
        return Value::Bool(p || y || r);
    }
    case 0x52:                                              // ByteToString
    case 0x53: return Value::Str(formatInt(v.i()));         // IntToString
    case 0x54: return Value::Str(widen(v.b() ? "True" : "False"));
    case 0x55: return Value::Str(formatFloat(v.f()));       // FloatToString
    case 0x56:                                              // ObjectToString
        return Value::Str(widen(v.o() ? v.o()->path() : "None"));
    case 0x57: return Value::Str(widen(v.n().str()));       // NameToString
    case 0x58: {                                            // VectorToString
        float x, y, z;
        unvector(v, x, y, z);
        return Value::Str(formatFloat(x) + u"," + formatFloat(y) + u"," + formatFloat(z));
    }
    case 0x59: {                                            // RotatorToString
        int32_t p, y, r;
        unrotator(v, p, y, r);
        return Value::Str(formatInt(p) + u"," + formatInt(y) + u"," + formatInt(r));
    }
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "cast token %02X has no known meaning", n.code);
    throw error(buf);
}

}  // namespace ffa
