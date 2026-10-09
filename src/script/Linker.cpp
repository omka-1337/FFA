#include "script/Linker.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <unordered_set>

namespace ffa {

namespace {

bool endsWith(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int refsFor(const std::string& cls) {
    if (cls == "ClassProperty") return 2;
    if (cls == "ObjectProperty" || cls == "StructProperty" || cls == "ByteProperty" ||
        cls == "ArrayProperty" || cls == "DelegateProperty")
        return 1;
    return 0;
}

Kind kindFor(const std::string& cls) {
    static const std::unordered_map<std::string, Kind> k = {
        {"ByteProperty", Kind::Byte},     {"IntProperty", Kind::Int},
        {"BoolProperty", Kind::Bool},     {"FloatProperty", Kind::Float},
        {"NameProperty", Kind::Name},     {"StrProperty", Kind::Str},
        {"StringProperty", Kind::Str},    {"ObjectProperty", Kind::Object},
        {"ClassProperty", Kind::Class},   {"StructProperty", Kind::Struct},
        {"ArrayProperty", Kind::Array},   {"DelegateProperty", Kind::Delegate},
        {"PointerProperty", Kind::Pointer}, {"MapProperty", Kind::Map},
        {"FixedArrayProperty", Kind::FixedArray},
    };
    auto it = k.find(cls);
    return it == k.end() ? Kind::Unknown : it->second;
}

// Structs the engine writes as raw memory rather than a nested tagged list:
// Vector, Rotator and Color, measured over every struct value in Shrek 2's
// defaults and levels. Plane, Range and Scale are tagged lists like the rest.
bool atomic(Name n) {
    static const std::unordered_set<std::string> a = {"vector", "rotator", "color"};
    return a.count(lower(n.str())) > 0;
}

uint32_t le32(const Package& p, size_t at) {
    uint32_t v;
    std::memcpy(&v, p.data.data() + at, 4);
    return v;
}

}  // namespace

Linker::Linker(const std::vector<std::string>& paths) {
    std::vector<std::string> sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    for (const auto& path : sorted) packages.push_back(std::make_unique<Package>(path));
    owned_.resize(packages.size());
    paths_.resize(packages.size());
    for (int k = 0; k < int(packages.size()); ++k) {
        const Package& p = *packages[size_t(k)];
        byStem_.emplace(lower(p.stem), k);
        for (int i = 1; i <= int(p.exports.size()); ++i) {
            const Export& e = p.exp(i);
            if (e.outer > 0) owned_[size_t(k)][e.outer].push_back(i);
            std::string cls = p.classOf(e);
            if (cls == "Class") {
                classes_.emplace(lower(e.name), std::make_pair(k, i));
            } else if (cls == "Function") {
                FunctionTail t = readTail(p, e);
                if ((t.flags & FUNC_Native) && t.native) natives_.emplace(t.native, std::make_pair(k, i));
            }
        }
    }
}

std::vector<std::string> Linker::packageFiles(const std::string& dir,
                                              const std::vector<std::string>& exts) {
    std::vector<std::string> out;
    for (const auto& ent : std::filesystem::directory_iterator(dir)) {
        if (!ent.is_regular_file()) continue;
        std::string ext = lower(ent.path().extension().string());
        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) out.push_back(ent.path().string());
    }
    return out;
}

int Linker::packageIndex(std::string_view stem) const {
    auto it = byStem_.find(lower(stem));
    return it == byStem_.end() ? -1 : it->second;
}

Object* Linker::adopt(std::unique_ptr<Object> o) {
    owned_objects_.push_back(std::move(o));
    return owned_objects_.back().get();
}

// ------------------------------------------------------------------ records
FieldInfo Linker::field(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = fields_.find(key);
    if (it != fields_.end()) return it->second;
    const Package& p = *packages[size_t(pkg)];
    const Export& e = p.exp(idx);
    FieldInfo d;
    d.cls = p.classOf(e);
    d.name = e.name;
    try {
        Reader r = p.record(idx);
        // A class starts with its links. Every other field starts with the
        // None of an empty property block, then the same links; read without
        // it, a function's Super passes for its Next and the member chain
        // wanders into the parent class.
        if (d.cls != "Class") r.idx();
        d.super = r.idx();
        d.next = r.idx();
        if (endsWith(d.cls, "Property")) {
            d.dim = r.u16();
            r.u16();                                // ElementSize
            d.flags = r.u32();
            r.idx();                                // Category
            if (d.flags & CPF_Net) r.u16();         // RepOffset
            for (int n = refsFor(d.cls); n > 0; --n) d.refs.push_back(r.idx());
        } else if (d.cls == "Class" || d.cls == "State" || d.cls == "Function" ||
                   d.cls == "Struct") {
            r.idx();                                // ScriptText
            d.children = r.idx();
        }
    } catch (const FormatError&) {
    }
    fields_.emplace(key, d);
    return d;
}

std::vector<int> Linker::members(int pkg, int idx) {
    // Declaration order is Children then the Next chain. Whatever the export
    // table says the record owns and the chain does not reach comes after.
    std::vector<int> out;
    std::unordered_set<int> seen;
    int32_t cur = field(pkg, idx).children;
    int n = int(packages[size_t(pkg)]->exports.size());
    while (cur > 0 && cur <= n && !seen.count(cur)) {
        seen.insert(cur);
        out.push_back(cur);
        cur = field(pkg, cur).next;
    }
    auto it = owned_[size_t(pkg)].find(idx);
    if (it != owned_[size_t(pkg)].end())
        for (int m : it->second)
            if (!seen.count(m)) out.push_back(m);
    return out;
}

std::vector<std::string> Linker::enumValues(int pkg, int idx) {
    const Package& p = *packages[size_t(pkg)];
    std::vector<std::string> out;
    try {
        Reader r = p.record(idx);
        r.idx();
        r.idx();
        r.idx();
        int32_t n = r.idx();
        for (int32_t i = 0; i < n; ++i) {
            int32_t v = r.idx();
            out.push_back(p.validName(v) ? p.names[size_t(v)] : "?");
        }
        if (r.p != r.limit) return {};
    } catch (const FormatError&) {
        return {};
    }
    return out;
}

std::string Linker::exportPath(int pkg, int idx) const {
    const Package& p = *packages[size_t(pkg)];
    std::vector<const std::string*> parts;
    for (int guard = 0; idx > 0 && guard < 64; ++guard) {
        parts.push_back(&p.exp(idx).name);
        idx = p.exp(idx).outer;
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += (out.empty() ? "" : ".") + **it;
    return out;
}

int Linker::findExport(int pkg, std::string_view path) {
    auto& index = paths_[size_t(pkg)];
    if (index.empty()) {
        int n = int(packages[size_t(pkg)]->exports.size());
        for (int i = 1; i <= n; ++i) index.emplace(lower(exportPath(pkg, i)), i);
    }
    auto it = index.find(lower(path));
    return it == index.end() ? 0 : it->second;
}

std::vector<std::string> Linker::importParts(int pkg, int32_t ref, std::string* cls) const {
    const Package& p = *packages[size_t(pkg)];
    std::vector<std::string> parts;
    int32_t i = -ref - 1;
    if (cls) *cls = p.imports.at(size_t(i)).clsName;
    for (int guard = 0; guard < 64; ++guard) {
        const Import& im = p.imports.at(size_t(i));
        parts.push_back(im.name);
        if (im.outer >= 0) break;
        i = -im.outer - 1;
    }
    std::reverse(parts.begin(), parts.end());
    return parts;
}

std::optional<std::pair<int, int>> Linker::resolve(int pkg, int32_t ref) {
    if (ref > 0) return std::make_pair(pkg, int(ref));
    if (ref == 0) return std::nullopt;
    auto parts = importParts(pkg, ref);
    int k = packageIndex(parts[0]);
    if (k < 0 || parts.size() == 1) return std::nullopt;
    std::string rest;
    for (size_t i = 1; i < parts.size(); ++i) rest += (i > 1 ? "." : "") + parts[i];
    int idx = findExport(k, rest);
    if (!idx) return std::nullopt;
    return std::make_pair(k, idx);
}

std::string Linker::kindOf(int pkg, int idx) const {
    std::string c = packages[size_t(pkg)]->classOf(idx);
    if (endsWith(c, "Property")) return "Property";
    return c;
}

// --------------------------------------------------------------------- types
void Linker::setIntrinsics(Object* o) {
    // Name, Class and Outer are declared in Object and read by script.
    if (!o->cls) return;
    static const Name nName("Name"), nClass("Class"), nOuter("Outer");
    if (Prop* p = o->cls->findProp(nName)) o->props[size_t(p->slot)] = Value::Nm(o->name);
    if (Prop* p = o->cls->findProp(nClass)) o->props[size_t(p->slot)] = Value::Obj(o->cls);
    if (Prop* p = o->cls->findProp(nOuter)) o->props[size_t(p->slot)] = Value::Obj(o->outer);
}

Prop* Linker::propAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = props_.find(key);
    if (it != props_.end()) return it->second.get();
    FieldInfo d = field(pkg, idx);
    auto p = std::make_unique<Prop>();
    p->name = Name(d.name);
    p->kind = kindFor(d.cls);
    p->dim = std::max(1, d.dim);
    p->flags = d.flags;
    p->linker = this;
    p->pkg = pkg;
    p->ref = d.refs.size() > 0 ? d.refs[0] : 0;
    p->ref2 = d.refs.size() > 1 ? d.refs[1] : 0;
    Prop* raw = p.get();
    props_.emplace(key, std::move(p));
    return raw;
}

StructType* Linker::structAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = structs_.find(key);
    if (it != structs_.end()) return it->second.get();
    auto s = std::make_unique<StructType>();
    StructType* st = s.get();
    structs_.emplace(key, std::move(s));
    FieldInfo d = field(pkg, idx);
    st->name = Name(d.name);
    if (d.super) st->super = structRef(pkg, d.super);
    for (int m : members(pkg, idx))
        if (kindOf(pkg, m) == "Property") st->own.push_back(propAt(pkg, m));
    return st;
}

EnumType* Linker::enumAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = objects_.find(key);
    if (it != objects_.end()) return dynamic_cast<EnumType*>(it->second);
    auto e = std::make_unique<EnumType>();
    e->name = Name(packages[size_t(pkg)]->exp(idx).name);
    for (const auto& v : enumValues(pkg, idx)) e->values.emplace_back(v);
    EnumType* raw = e.get();
    objects_.emplace(key, adopt(std::move(e)));
    return raw;
}

Function* Linker::functionAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = functions_.find(key);
    if (it != functions_.end()) return it->second.get();
    const Package& p = *packages[size_t(pkg)];
    const Export& e = p.exp(idx);
    auto f = std::make_unique<Function>();
    Function* fn = f.get();
    functions_.emplace(key, std::move(f));
    fn->name = Name(e.name);
    fn->pkg = pkg;
    fn->idx = idx;
    FunctionTail t = readTail(p, e);
    fn->flags = t.flags;
    fn->native = t.native;
    for (int m : members(pkg, idx)) {
        if (kindOf(pkg, m) != "Property") continue;
        Prop* pr = propAt(pkg, m);
        if (pr->flags & CPF_ReturnParm)
            fn->ret = pr;
        else if (pr->flags & CPF_Parm)
            fn->params.push_back(pr);
        else
            fn->locals.push_back(pr);
    }
    int slot = 0;
    for (Prop* pr : fn->params) { pr->slot = slot; slot += pr->dim; }
    if (fn->ret) { fn->ret->slot = slot; slot += fn->ret->dim; }
    for (Prop* pr : fn->locals) { pr->slot = slot; slot += pr->dim; }
    fn->slots = slot;
    if (e.outer > 0) {
        std::string k = kindOf(pkg, e.outer);
        if (k == "State") {
            fn->state = stateAt(pkg, e.outer);
            fn->cls = fn->state->cls;
        } else if (k == "Class") {
            fn->cls = classAt(pkg, e.outer);
        }
    }
    return fn;
}

State* Linker::stateAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = states_.find(key);
    if (it != states_.end()) return it->second.get();
    const Package& p = *packages[size_t(pkg)];
    const Export& e = p.exp(idx);
    auto s = std::make_unique<State>();
    State* st = s.get();
    states_.emplace(key, std::move(s));
    st->name = Name(e.name);
    st->pkg = pkg;
    st->idx = idx;
    // StateFlags is taken as the record's last word: the state tail is read as
    // ProbeMask, IgnoreMask, LabelTableOffset and StateFlags. `ffa-script check`
    // tests that reading against the label table the bytecode walk finds.
    if (e.size >= 4) st->flags = le32(p, size_t(e.off + e.size - 4));
    if (e.outer > 0 && kindOf(pkg, e.outer) == "Class") st->cls = classAt(pkg, e.outer);
    FieldInfo d = field(pkg, idx);
    if (d.super) st->super = stateRef(pkg, d.super);
    for (int m : members(pkg, idx))
        if (kindOf(pkg, m) == "Function") {
            Function* f = functionAt(pkg, m);
            st->funcs.emplace(f->name, f);
        }
    return st;
}

Class* Linker::classAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = objects_.find(key);
    if (it != objects_.end()) return dynamic_cast<Class*>(it->second);
    const Package& p = *packages[size_t(pkg)];
    auto c = std::make_unique<Class>();
    Class* cls = c.get();
    objects_.emplace(key, adopt(std::move(c)));
    cls->name = Name(p.exp(idx).name);
    cls->pkg = pkg;
    cls->idx = idx;
    cls->linker = this;
    cls->outer = findObject(p.stem);
    FieldInfo d = field(pkg, idx);
    if (d.super) cls->super = classRef(pkg, d.super);
    for (int m : members(pkg, idx)) {
        std::string k = kindOf(pkg, m);
        if (k == "Property") {
            cls->own.push_back(propAt(pkg, m));
        } else if (k == "Function") {
            Function* f = functionAt(pkg, m);
            cls->funcs.emplace(f->name, f);
        } else if (k == "State") {
            State* s = stateAt(pkg, m);
            cls->states.emplace(s->name, s);
        } else if (k == "Struct") {
            StructType* s = structAt(pkg, m);
            cls->structs.emplace(s->name, s);
        }
    }
    // A class is an object too, of class Class where the game declares it.
    // Its variables are set up on first use, by initClassObject: doing it here
    // would lay out the metaclass while it may still be loading.
    cls->cls = findClass("Class");
    return cls;
}

void Linker::initClassObject(Class* c) {
    if (!c->props.empty()) return;
    Class* layoutOf = c->cls;
    if (!layoutOf) {
        layoutOf = c;
        while (layoutOf->super) layoutOf = layoutOf->super;
    }
    c->props.assign(size_t(layoutOf->slots()), Value());
    for (Prop* pr : layoutOf->layout())
        for (int i = 0; i < pr->dim; ++i) c->props[size_t(pr->slot + i)] = pr->zero();
    Class* saved = c->cls;
    c->cls = layoutOf;
    setIntrinsics(c);
    c->cls = saved;
}

size_t Linker::propertiesStart(const Package& p, const Export& e) {
    size_t start = size_t(e.off), end = size_t(e.off + e.size);
    if (e.flags & 0x02000000) {        // RF_HasStack: the state frame comes first
        Reader r(p.data, start, end);
        int32_t node = r.idx();
        r.idx();                        // StateNode
        r.p += 12;                      // ProbeMask, LatentAction
        if (node) r.idx();              // Offset
        start = r.p;
    }
    return start;
}

Object* Linker::instanceAt(int pkg, int idx) {
    auto key = std::make_pair(pkg, idx);
    auto it = objects_.find(key);
    if (it != objects_.end()) return it->second;
    const Package& p = *packages[size_t(pkg)];
    const Export& e = p.exp(idx);
    auto o = std::make_unique<Object>();
    Object* obj = o.get();
    objects_.emplace(key, adopt(std::move(o)));
    obj->name = Name(e.name);
    obj->cls = e.cls ? classRef(pkg, e.cls) : nullptr;
    obj->outer = e.outer > 0 ? objectRef(pkg, e.outer) : findObject(p.stem);
    if (!obj->cls) return obj;
    obj->props = obj->cls->defaults()->props;
    setIntrinsics(obj);
    if (e.size <= 0) return obj;
    size_t start = 0, end = size_t(e.off + e.size);
    try {
        start = propertiesStart(p, e);
    } catch (const FormatError&) {
        return obj;
    }
    std::vector<TagEntry> entries;
    size_t pos = 0;
    if (parseTagged(p, start, end, entries, pos))
        applyTagged(pkg, obj->cls, obj->props, entries, obj->path());
    return obj;
}

// ------------------------------------------------------- by reference
Class* Linker::classRef(int pkg, int32_t ref) {
    auto hit = resolve(pkg, ref);
    if (!hit && ref < 0) {
        // a class of the engine's own, imported by name
        std::string cls;
        auto parts = importParts(pkg, ref, &cls);
        return cls == "Class" && !parts.empty() ? nativeClass(parts.back()) : nullptr;
    }
    if (!hit || kindOf(hit->first, hit->second) != "Class") return nullptr;
    return classAt(hit->first, hit->second);
}

Prop* Linker::propRef(int pkg, int32_t ref) {
    auto hit = resolve(pkg, ref);
    if (!hit || kindOf(hit->first, hit->second) != "Property") return nullptr;
    return propAt(hit->first, hit->second);
}

Function* Linker::functionRef(int pkg, int32_t ref) {
    auto hit = resolve(pkg, ref);
    if (!hit || kindOf(hit->first, hit->second) != "Function") return nullptr;
    return functionAt(hit->first, hit->second);
}

State* Linker::stateRef(int pkg, int32_t ref) {
    auto hit = resolve(pkg, ref);
    if (!hit || kindOf(hit->first, hit->second) != "State") return nullptr;
    return stateAt(hit->first, hit->second);
}

StructType* Linker::structRef(int pkg, int32_t ref) {
    auto hit = resolve(pkg, ref);
    if (!hit || kindOf(hit->first, hit->second) != "Struct") return nullptr;
    return structAt(hit->first, hit->second);
}

EnumType* Linker::enumRef(int pkg, int32_t ref) {
    auto hit = resolve(pkg, ref);
    if (!hit || kindOf(hit->first, hit->second) != "Enum") return nullptr;
    return enumAt(hit->first, hit->second);
}

Object* Linker::objectRef(int pkg, int32_t ref) {
    if (!ref) return nullptr;
    auto hit = resolve(pkg, ref);
    if (!hit) return stub(pkg, ref);
    auto [k, i] = *hit;
    std::string kind = kindOf(k, i);
    if (kind == "Class") return classAt(k, i);
    if (kind == "Enum") return enumAt(k, i);
    if (kind == "Function" || kind == "State" || kind == "Struct" || kind == "Property") {
        // a type used as an object value: named, without variables
        auto key = std::make_pair(k, i);
        auto it = objects_.find(key);
        if (it != objects_.end()) return it->second;
        auto o = std::make_unique<Object>();
        o->name = Name(packages[size_t(k)]->exp(i).name);
        o->outer = findObject(packages[size_t(k)]->stem);
        Object* raw = adopt(std::move(o));
        objects_.emplace(key, raw);
        return raw;
    }
    return instanceAt(k, i);
}

Object* Linker::stub(int pkg, int32_t ref) {
    // An object in a package that is not loaded: named, of its class where
    // the class is known, without variables.
    auto key = std::make_pair(pkg, ref);
    auto it = stubs_.find(key);
    if (it != stubs_.end()) return it->second;
    std::string cls;
    auto parts = importParts(pkg, ref, &cls);
    Object* outer = nullptr;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        auto o = std::make_unique<Object>();
        o->name = Name(parts[i]);
        o->outer = outer;
        outer = adopt(std::move(o));
    }
    // a class of the engine's own, such as Mesh, is that class
    if (cls == "Class")
        if (Class* nc = nativeClass(parts.back())) {
            stubs_.emplace(key, nc);
            return nc;
        }
    auto o = std::make_unique<Object>();
    o->name = Name(parts.back());
    o->outer = outer;
    if (cls != "Package") o->cls = findClass(cls);
    if (o->cls) {
        o->props = o->cls->defaults()->props;
    }
    Object* raw = adopt(std::move(o));
    setIntrinsics(raw);
    stubs_.emplace(key, raw);
    return raw;
}

Class* Linker::findClass(std::string_view name) {
    auto it = classes_.find(lower(name));
    return it == classes_.end() ? nativeClass(name) : classAt(it->second.first, it->second.second);
}

// Classes of the engine's C++ that no package holds, as no script declares
// them, though script names them: in casts, Mesh(DynamicLoadObject(..., class'
// Mesh')) as KWPawn's SetActorMeshes does, and as objects' classes. These are
// the ones the game's packages import without any exporting them, but for the
// property and function types and the subsystems, each under its parent in the
// engine's hierarchy; each is made once, with no variables of its own.
Class* Linker::nativeClass(std::string_view name) {
    static const std::pair<const char*, const char*> table[] = {
        {"Primitive", "Object"},          {"Mesh", "Primitive"},          {"VertMesh", "Mesh"},
        {"SkeletalMesh", "Mesh"},         {"StaticMesh", "Primitive"},    {"Model", "Primitive"},
        {"ConvexVolume", "Primitive"},    {"TerrainPrimitive", "Primitive"},
        {"FluidSurfacePrimitive", "Primitive"},                           {"MeshInstance", "Object"},
        {"MeshAnimation", "Object"},      {"StaticMeshInstance", "Object"},
        {"TerrainSector", "Object"},      {"Sound", "Object"},            {"Font", "Object"},
        {"Level", "Object"},
    };
    std::string key = lower(name);
    auto it = native_.find(key);
    if (it != native_.end()) return it->second;
    for (const auto& [n, parent] : table) {
        if (lower(n) != key) continue;
        Class* super = findClass(parent);
        auto c = std::make_unique<Class>();
        Class* cls = c.get();
        cls->name = Name(n);
        cls->linker = this;
        cls->super = super;
        cls->outer = findObject("Engine");
        adopt(std::move(c));
        native_.emplace(key, cls);
        cls->cls = findClass("Class");
        return cls;
    }
    return nullptr;
}

StructType* Linker::findStruct(std::string_view name) {
    for (int k = 0; k < int(packages.size()); ++k) {
        const Package& p = *packages[size_t(k)];
        for (int i = 1; i <= int(p.exports.size()); ++i)
            if (iequals(p.exp(i).name, name) && p.classOf(i) == "Struct") return structAt(k, i);
    }
    return nullptr;
}

Object* Linker::findObject(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dot = path.find('.', start);
        parts.emplace_back(path.substr(start, dot == std::string_view::npos ? dot : dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    int k = packageIndex(parts[0]);
    if (k < 0) return nullptr;
    if (parts.size() == 1) {
        auto key = std::make_pair(k, 0);
        auto it = objects_.find(key);
        if (it != objects_.end()) return it->second;
        auto o = std::make_unique<Object>();
        o->name = Name(packages[size_t(k)]->stem);
        Object* raw = adopt(std::move(o));
        objects_.emplace(key, raw);
        return raw;
    }
    std::string rest(path.substr(parts[0].size() + 1));
    int idx = findExport(k, rest);
    return idx ? objectRef(k, idx) : nullptr;
}

Function* Linker::native(int index) {
    auto it = natives_.find(index);
    return it == natives_.end() ? nullptr : functionAt(it->second.first, it->second.second);
}

ParsedCode Linker::bytecode(int pkg, int idx) {
    const Package& p = *packages[size_t(pkg)];
    BytecodeParser bp(p);
    if (p.classOf(idx) == "Function") return bp.function(p.exp(idx));
    return bp.structCode(p.exp(idx));
}

// ------------------------------------------------------------------ defaults
Object* Linker::buildDefault(Class* c) {
    if (c->defaultObject) return c->defaultObject;
    auto o = std::make_unique<Object>();
    Object* obj = o.get();
    adopt(std::move(o));
    obj->cls = c;
    obj->name = Name("Default__" + c->name.str());
    obj->outer = c->outer;
    if (c->super) obj->props = c->super->defaults()->props;
    obj->props.resize(size_t(c->slots()));
    for (Prop* p : c->own)
        for (int i = 0; i < p->dim; ++i) obj->props[size_t(p->slot + i)] = p->zero();
    setIntrinsics(obj);
    c->defaultObject = obj;
    std::vector<TagEntry> entries;
    if (c->pkg >= 0 && locateDefaults(c, entries)) applyTagged(c->pkg, c, obj->props, entries, c->name.str());
    return obj;
}

bool Linker::locateDefaults(Class* c, std::vector<TagEntry>& out) {
    // The fields between the struct header and the defaults are not decoded,
    // so the block is found by scanning for an offset that parses exactly to
    // the end of the record, and choosing among those by how many of the
    // names are variables of the class. The second test is what makes it
    // right: several offsets usually parse cleanly to the end.
    const Package& p = *packages[size_t(c->pkg)];
    const Export& e = p.exp(c->idx);
    size_t start = size_t(e.off), end = size_t(e.off + e.size);
    // An entry counts when its name is a variable of the class and its tag
    // type is one that variable is written with; names alone let a start a
    // few bytes early read stray bytes as a tag with a real name.
    auto fits = [](int type, Kind k) {
        switch (type) {
        case T_Byte: return k == Kind::Byte;
        case T_Int: return k == Kind::Int;
        case T_Bool: return k == Kind::Bool;
        case T_Float: return k == Kind::Float;
        case T_Object: return k == Kind::Object || k == Kind::Class;
        case T_Name: return k == Kind::Name;
        case T_Delegate: return k == Kind::Delegate;
        case T_Class: return k == Kind::Class;
        case T_Array: return k == Kind::Array;
        case T_Struct: case T_Vector: case T_Rotator: return k == Kind::Struct;
        case T_Str: return k == Kind::Str;
        default: return false;
        }
    };
    std::vector<TagEntry> entries;
    long best = -1;
    double bestScore = -1;
    size_t bestN = 0;
    for (size_t s = start; s < end; ++s) {
        size_t pos = 0;
        if (!parseTagged(p, s, end, entries, pos) || pos != end) continue;
        size_t hits = 0;
        for (const TagEntry& t : entries) {
            const Prop* pr = c->findProp(p.name(t.name));
            hits += pr && fits(t.type, pr->kind);
        }
        double score = entries.empty() ? 1.0 : double(hits) / double(entries.size());
        if (score > bestScore || (score == bestScore && entries.size() > bestN)) {
            best = long(s);
            bestScore = score;
            bestN = entries.size();
        }
    }
    if (best < 0) return false;
    size_t pos = 0;
    parseTagged(p, size_t(best), end, out, pos);
    return true;
}

void Linker::applyTagged(int pkg, Class* c, std::vector<Value>& props,
                         const std::vector<TagEntry>& entries, const std::string& where) {
    const Package& p = *packages[size_t(pkg)];
    for (const TagEntry& t : entries) {
        Name n = p.name(t.name);
        Prop* pr = c->findProp(n);
        if (!pr) {
            problems.emplace_back(where, "no variable " + n.str());
            continue;
        }
        try {
            Value v = pr->coerce(decode(pkg, pr, t));
            if (pr->dim > 1) {
                if (t.index >= 0 && t.index < pr->dim) props[size_t(pr->slot + t.index)] = std::move(v);
            } else {
                props[size_t(pr->slot)] = std::move(v);
            }
        } catch (const std::exception& ex) {
            problems.emplace_back(where, n.str() + ": " + ex.what());
        }
    }
}

String Linker::readString(int pkg, Reader& r) {
    // A compact length counting the terminator, negative for UTF-16.
    int32_t n = r.idx();
    String s;
    if (n >= 0) {
        r.need(size_t(n));
        for (int32_t i = 0; i + 1 < n; ++i) s += char16_t(r.b[r.p + size_t(i)]);
        r.p += size_t(n);
    } else {
        for (int32_t i = 0; i < -n; ++i) {
            uint16_t c = r.u16();
            if (i + 1 < -n) s += char16_t(c);
        }
    }
    (void)pkg;
    return s;
}

Value Linker::decode(int pkg, const Prop* pr, const TagEntry& t) {
    const Package& p = *packages[size_t(pkg)];
    Reader r(p.data, t.at, t.at + t.size);
    switch (t.type) {
    case T_Bool: return Value::Bool(t.boolValue);
    case T_Byte: return Value::Int(r.u8());
    case T_Int: return Value::Int(r.i32());
    case T_Float: return Value::Float(r.f32());
    case T_Object:
    case T_Class: return Value::Obj(objectRef(pkg, r.idx()));
    case T_Name: return Value::Nm(p.name(r.idx()));
    case T_Str: return Value::Str(readString(pkg, r));
    case T_Delegate: {
        // An object and a function name. Code 7 was UE1's string; in these
        // packages every value with it is a DelegateProperty's.
        Delegate d;
        d.obj = objectRef(pkg, r.idx());
        d.func = p.name(r.idx());
        return Value::Dlg(d);
    }
    case T_Struct: {
        StructType* st = pr->kind == Kind::Struct ? pr->structType() : nullptr;
        if (!st && t.structName >= 0) st = findStruct(p.names[size_t(t.structName)]);
        return structValue(pkg, st, t.at, t.size);
    }
    case T_Vector: return structValue(pkg, findStruct("Vector"), t.at, t.size);
    case T_Rotator: return structValue(pkg, findStruct("Rotator"), t.at, t.size);
    case T_Array: return arrayValue(pkg, pr, t.at, t.size);
    }
    throw FormatError("tag type " + std::to_string(t.type) + " not decoded");
}

Value Linker::structValue(int pkg, StructType* st, size_t at, size_t size) {
    if (!st) throw FormatError("unknown struct type");
    const Package& p = *packages[size_t(pkg)];
    if (atomic(st->name)) {
        Reader r(p.data, at, at + size);
        Value v = binaryStruct(pkg, st, r);
        if (r.p != at + size)
            throw FormatError(st->name.str() + " reads " + std::to_string(r.p - at) + " bytes of " +
                              std::to_string(size));
        return v;
    }
    std::vector<TagEntry> entries;
    size_t pos = 0;
    if (!parseTagged(p, at, at + size, entries, pos) || pos != at + size)
        throw FormatError(st->name.str() + ": tagged list does not fill its size");
    return taggedStruct(pkg, st, entries);
}

Value Linker::taggedStruct(int pkg, StructType* st, const std::vector<TagEntry>& entries) {
    const Package& p = *packages[size_t(pkg)];
    Value v = st->make();
    for (const TagEntry& t : entries) {
        Prop* f = st->field(p.name(t.name));
        if (!f) throw FormatError(st->name.str() + " has no field " + p.names[size_t(t.name)]);
        Value x = f->coerce(decode(pkg, f, t));
        int at = f->slot + (f->dim > 1 ? t.index : 0);
        if (at >= f->slot && at < f->slot + f->dim) v.st().f[size_t(at)] = std::move(x);
    }
    return v;
}

Value Linker::binaryStruct(int pkg, StructType* st, Reader& r) {
    // laid out as memory: each field at its natural size, parents first
    Value v = st->make();
    for (Prop* f : st->layout()) {
        for (int i = 0; i < f->dim; ++i) {
            Value x;
            switch (f->kind) {
            case Kind::Float: x = Value::Float(r.f32()); break;
            case Kind::Int: x = Value::Int(r.i32()); break;
            case Kind::Byte: x = Value::Int(r.u8()); break;
            case Kind::Struct:
                if (!f->structType()) throw FormatError("unresolved struct field");
                x = binaryStruct(pkg, f->structType(), r);
                break;
            default: throw FormatError(std::string(kindName(f->kind)) + " field in a binary struct");
            }
            v.st().f[size_t(f->slot + i)] = std::move(x);
        }
    }
    return v;
}

Value Linker::arrayValue(int pkg, const Prop* pr, size_t at, size_t size) {
    Prop* inner = pr->kind == Kind::Array ? pr->inner() : nullptr;
    if (!inner) throw FormatError("array without an element type");
    const Package& p = *packages[size_t(pkg)];
    size_t end = at + size;
    Reader r(p.data, at, end);
    Array out;
    int32_t n = r.idx();
    for (int32_t i = 0; i < n; ++i) {
        switch (inner->kind) {
        case Kind::Object:
        case Kind::Class: out.push_back(Value::Obj(objectRef(pkg, r.idx()))); break;
        case Kind::Name: out.push_back(Value::Nm(p.name(r.idx()))); break;
        case Kind::Int: out.push_back(Value::Int(r.i32())); break;
        case Kind::Float: out.push_back(Value::Float(r.f32())); break;
        case Kind::Byte: out.push_back(Value::Int(r.u8())); break;
        case Kind::Str: out.push_back(Value::Str(readString(pkg, r))); break;
        case Kind::Struct: {
            StructType* st = inner->structType();
            if (!st) throw FormatError("array of an unresolved struct");
            if (atomic(st->name)) {
                out.push_back(binaryStruct(pkg, st, r));
            } else {
                std::vector<TagEntry> entries;
                size_t pos = 0;
                if (!parseTagged(p, r.p, end, entries, pos)) throw FormatError("array element does not parse");
                out.push_back(taggedStruct(pkg, st, entries));
                r.p = pos;
            }
            break;
        }
        default: throw FormatError(std::string("array of ") + kindName(inner->kind) + " not decoded");
        }
    }
    if (r.p != end) throw FormatError("array ends short of its size");
    return Value::Arr(std::move(out));
}

}  // namespace ffa
