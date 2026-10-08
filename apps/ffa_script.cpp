// ffa-script: run a game's UnrealScript, and check the VM against its corpus.
//
//   ffa-script check <System dir>                 load and compile everything, report
//   ffa-script call  <System dir> Class.Function [args...]
//   ffa-script smoke <System dir>                 call every static function once
//   ffa-script level <System dir> <map> [dump]    load a level's live actors
//   ffa-script start <System dir> <map>           and run its start up sequence
//
// `check` is the corpus-wide proof the VM rests on, in the manner of the
// Python readers in tools/: every class loads, every function and state compiles with
// every reference resolved and every jump landing on a statement, every
// default block decodes, and the state tail and cast table are tested
// against what the bytecode itself says.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "script/VM.h"
#include "world/Level.h"

using namespace ffa;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ffa-script check <System dir>\n"
                 "       ffa-script call <System dir> Class.Function [args...]\n"
                 "       ffa-script smoke <System dir>\n"
                 "       ffa-script level <System dir> <map.unr> [dump.tsv]\n"
                 "       ffa-script start <System dir> <map.unr>\n");
    return 2;
}

// The declared type of an expression, where the bytecode says it.
std::string staticType(const Ins& n) {
    auto ofProp = [](const Prop* p) -> std::string {
        if (!p) return "?";
        if (p->kind == Kind::Struct && p->structType()) return "struct " + p->structType()->name.str();
        return kindName(p->kind);
    };
    switch (n.op) {
    case Op::LocalVariable:
    case Op::InstanceVariable:
    case Op::DefaultVariable:
    case Op::StateVariable:
    case Op::StructMember:
        return ofProp(n.prop);
    case Op::ArrayElement:
    case Op::Context:
    case Op::ClassContext:
        return staticType(n.kids.back());
    case Op::DynArrayElement: {
        const Ins& b = n.kids[1];
        return b.prop && b.prop->inner() ? ofProp(b.prop->inner()) : "?";
    }
    case Op::DynArrayLength:
    case Op::IntConst:
    case Op::IntConstByte:
    case Op::IntZero:
    case Op::IntOne:
        return "int";
    case Op::ByteConst: return "byte";
    case Op::FloatConst: return "float";
    case Op::StringConst:
    case Op::UnicodeStringConst: return "string";
    case Op::NameConst: return "name";
    case Op::ObjectConst:
    case Op::NoObject:
    case Op::Self:
    case Op::DynamicCast: return "object";
    case Op::Metacast: return "class";
    case Op::True:
    case Op::False:
    case Op::BoolVariable: return "bool";
    case Op::VectorConst: return "struct Vector";
    case Op::RotationConst: return "struct Rotator";
    case Op::NativeCall:
    case Op::FinalFunction: return n.fn && n.fn->ret ? ofProp(n.fn->ret) : "?";
    case Op::Cast: {
        // the target type of each conversion, as the VM reads the token
        static const std::map<int, const char*> target = {
            {0x39, "struct Vector"}, {0x3A, "int"}, {0x3B, "bool"}, {0x3C, "float"},
            {0x3D, "byte"}, {0x3E, "bool"}, {0x3F, "float"}, {0x40, "byte"}, {0x41, "int"},
            {0x42, "float"}, {0x43, "byte"}, {0x44, "int"}, {0x45, "bool"}, {0x47, "bool"},
            {0x48, "bool"}, {0x49, "byte"}, {0x4A, "int"}, {0x4B, "bool"}, {0x4C, "float"},
            {0x4D, "struct Vector"}, {0x4E, "struct Rotator"}, {0x4F, "bool"},
            {0x50, "struct Rotator"}, {0x51, "bool"}};
        auto it = target.find(n.code);
        if (it != target.end()) return it->second;
        return n.code >= 0x52 && n.code <= 0x59 ? "string" : "?";
    }
    default: return "?";
    }
}

struct Census {
    std::map<std::string, size_t> nativeCalls;
    std::map<int, std::map<std::string, size_t>> casts;
    std::map<std::string, size_t> ops;
};

void walk(const Ins& n, Census& c) {
    c.ops[opName(n.op)]++;
    if ((n.op == Op::NativeCall || n.op == Op::FinalFunction) && n.fn && n.fn->isNative())
        c.nativeCalls[n.fn->nativeKey()]++;
    if (n.op == Op::Cast) c.casts[n.code][staticType(n.kids[0])]++;
    for (const Ins& k : n.kids) walk(k, c);
}

// A message reduced to its kind, so that failures group: names of objects
// and functions, which contain dots, and numbers are blanked out.
std::string pattern(const std::string& msg, size_t words = 10) {
    std::string out, word;
    size_t n = 0;
    auto flush = [&] {
        if (word.empty()) return;
        bool named = word.find('.') != std::string::npos || word.rfind("Default__", 0) == 0;
        if (n++ < words) out += (out.empty() ? "" : " ") + (named ? std::string("<x>") : word);
        word.clear();
    };
    for (char ch : msg) {
        if (ch == '\n') break;
        if (ch == ' ') {
            flush();
            continue;
        }
        word += (ch >= '0' && ch <= '9') ? '#' : ch;
    }
    flush();
    return out;
}

int check(const std::string& dir) {
    Linker lk(Linker::packageFiles(dir));
    VM vm(lk);
    vm.sink = [](const std::string&, const std::string&) {};
    Census census;
    size_t classes = 0, classFail = 0, funcs = 0, funcOk = 0, funcClean = 0;
    size_t states = 0, stateOk = 0, labelAgree = 0, labelDisagree = 0, defaults = 0;
    std::map<std::string, size_t> failures;
    std::map<long, size_t> stateTails;
    std::map<uint32_t, size_t> stateFlags;
    std::set<std::string> declaredNatives;

    for (int k = 0; k < int(lk.packages.size()); ++k) {
        const Package& p = *lk.packages[size_t(k)];
        for (int i = 1; i <= int(p.exports.size()); ++i) {
            std::string cls = p.classOf(i);
            try {
                if (cls == "Class") {
                    ++classes;
                    Class* c = lk.classAt(k, i);
                    c->defaults();
                    ++defaults;
                } else if (cls == "Function") {
                    Function* fn = lk.functionAt(k, i);
                    if (fn->isNative()) {
                        declaredNatives.insert(fn->nativeKey());
                        continue;
                    }
                    ++funcs;
                    vm.compile(fn);
                    ++funcOk;
                    funcClean += fn->parseOk;
                    for (const Ins& n : fn->code) walk(n, census);
                } else if (cls == "State") {
                    ++states;
                    State* st = lk.stateAt(k, i);
                    vm.compile(st);
                    ++stateOk;
                    for (const Ins& n : st->code) walk(n, census);
                    // The state tail, read as ProbeMask, IgnoreMask, LabelTableOffset
                    // and StateFlags, must be 22 bytes after the bytecode, and its
                    // LabelTableOffset must point at the label table the walk found.
                    ParsedCode pc = lk.bytecode(k, i);
                    const Export& e = p.exp(i);
                    long tail = long(e.off + e.size) - long(pc.end);
                    stateTails[tail]++;
                    if (tail == 22) {
                        const uint8_t* q = p.data.data() + pc.end + 16;
                        uint16_t lto = uint16_t(q[0] | q[1] << 8);
                        uint32_t sf;
                        std::memcpy(&sf, q + 2, 4);
                        stateFlags[sf]++;
                        // LabelTableOffset points at the table's entries, one
                        // byte past its token, or is 0xFFFF for no table.
                        uint16_t found = 0xFFFF;
                        for (const Ins& n : st->code)
                            if (n.op == Op::LabelTable) found = uint16_t(n.mem + 1);
                        (lto == found ? labelAgree : labelDisagree)++;
                    }
                }
            } catch (const ScriptError& ex) {
                if (cls == "Class") ++classFail;
                failures[cls + ": " + pattern(ex.what())]++;
            } catch (const std::exception& ex) {
                if (cls == "Class") ++classFail;
                failures[cls + ": internal: " + pattern(ex.what())]++;
            }
        }
    }

    std::printf("packages            %zu\n", lk.packages.size());
    std::printf("classes             %zu loaded, %zu failed\n", classes - classFail, classFail);
    std::printf("default objects     %zu built, %zu problems\n", defaults, lk.problems.size());
    std::printf("script functions    %zu, %zu compile, %zu of them parse cleanly\n", funcs, funcOk, funcClean);
    std::printf("states              %zu, %zu compile\n", states, stateOk);
    std::printf("state tails         ");
    for (auto& [len, n] : stateTails) std::printf("%ld bytes: %zu  ", len, n);
    std::printf("\nlabel table offset  %zu agree, %zu disagree\n", labelAgree, labelDisagree);
    std::printf("state flags         ");
    for (auto& [f, n] : stateFlags) std::printf("%08X: %zu  ", f, n);
    std::printf("\n");

    size_t implemented = 0;
    for (const auto& key : declaredNatives) implemented += vm.natives.count(key);
    std::printf("natives declared    %zu, %zu implemented\n", declaredNatives.size(), implemented);
    std::vector<std::pair<size_t, std::string>> missing;
    size_t calledImpl = 0;
    for (auto& [key, n] : census.nativeCalls) {
        if (vm.natives.count(key))
            ++calledImpl;
        else
            missing.emplace_back(n, key);
    }
    std::printf("natives called      %zu, %zu implemented (by index or final call)\n",
                census.nativeCalls.size(), calledImpl);
    std::sort(missing.rbegin(), missing.rend());
    std::printf("most called of the missing:\n");
    for (size_t i = 0; i < missing.size() && i < 40; ++i)
        std::printf("  %7zu  %s\n", missing[i].first, missing[i].second.c_str());

    std::printf("cast operands, by token:\n");
    for (auto& [code, types] : census.casts) {
        std::printf("  %02X ", code);
        for (auto& [t, n] : types) std::printf(" %s:%zu", t.c_str(), n);
        std::printf("\n");
    }
    if (!failures.empty()) {
        std::printf("failures:\n");
        std::vector<std::pair<size_t, std::string>> f;
        for (auto& [m, n] : failures) f.emplace_back(n, m);
        std::sort(f.rbegin(), f.rend());
        for (size_t i = 0; i < f.size() && i < 40; ++i) std::printf("  %6zu  %s\n", f[i].first, f[i].second.c_str());
    }
    if (!lk.problems.empty()) {
        std::printf("default problems, first 20:\n");
        for (size_t i = 0; i < lk.problems.size() && i < 20; ++i)
            std::printf("  %s: %s\n", lk.problems[i].first.c_str(), lk.problems[i].second.c_str());
    }
    return 0;
}

Value parseArg(const Prop* p, const std::string& s) {
    switch (p->kind) {
    case Kind::Byte:
    case Kind::Int: return Value::Int(parseInt(fromUtf8(s)));
    case Kind::Float: return Value::Float(parseFloat(fromUtf8(s)));
    case Kind::Bool: return Value::Bool(iequals(s, "true") || s == "1");
    case Kind::Name: return Value::Nm(Name(s));
    case Kind::Str: return Value::Str(fromUtf8(s));
    default: return p->zero();
    }
}

int call(const std::string& dir, const std::string& what, const std::vector<std::string>& args) {
    size_t dot = what.find('.');
    if (dot == std::string::npos) return usage();
    Linker lk(Linker::packageFiles(dir));
    VM vm(lk);
    try {
        Class* c = vm.findClass(what.substr(0, dot));
        Function* fn = c->findFunction(Name(what.substr(dot + 1)));
        if (!fn) {
            std::fprintf(stderr, "%s has no function %s\n", c->name.str().c_str(), what.substr(dot + 1).c_str());
            return 1;
        }
        std::vector<Value> vals;
        for (size_t i = 0; i < args.size() && i < fn->params.size(); ++i)
            vals.push_back(parseArg(fn->params[i], args[i]));
        Value r = vm.callFunction(fn, c->defaults(), vals);
        std::printf("%s\n", describe(r).c_str());
    } catch (const ScriptError& ex) {
        std::fprintf(stderr, "error: %s\n", ex.full().c_str());
        return 1;
    }
    return 0;
}

int smoke(const std::string& dir) {
    // Every static script function whose parameters are plain values, called
    // once with defaults. Script errors and missing natives are expected; an
    // internal error is a VM bug.
    Linker lk(Linker::packageFiles(dir));
    VM vm(lk);
    vm.runaway = 100000;
    size_t warnings = 0;
    vm.sink = [&](const std::string&, const std::string&) { ++warnings; };
    size_t calls = 0, ok = 0;
    std::map<std::string, size_t> errors;
    for (int k = 0; k < int(lk.packages.size()); ++k) {
        const Package& p = *lk.packages[size_t(k)];
        for (int i = 1; i <= int(p.exports.size()); ++i) {
            if (p.classOf(i) != "Function") continue;
            Function* fn;
            try {
                fn = lk.functionAt(k, i);
            } catch (const std::exception&) {
                continue;
            }
            if (!(fn->flags & FUNC_Static) || fn->isNative() || !fn->cls || fn->state) continue;
            bool plain = true;
            for (Prop* pr : fn->params)
                plain = plain && (pr->kind == Kind::Int || pr->kind == Kind::Float || pr->kind == Kind::Bool ||
                                  pr->kind == Kind::Byte || pr->kind == Kind::Name || pr->kind == Kind::Str);
            if (!plain) continue;
            ++calls;
            try {
                vm.callFunction(fn, fn->cls->defaults(), {});
                ++ok;
            } catch (const NativeMissing& ex) {
                errors["missing native"]++;
            } catch (const ScriptError& ex) {
                errors["script: " + pattern(ex.what())]++;
            } catch (const std::exception& ex) {
                errors["INTERNAL: " + pattern(ex.what())]++;
            }
        }
    }
    std::printf("static functions called  %zu, returned %zu, warnings logged %zu\n", calls, ok, warnings);
    std::printf("natives missing on the way  %zu\n", vm.missing.size());
    std::vector<std::pair<size_t, std::string>> e;
    for (auto& [m, n] : errors) e.emplace_back(n, m);
    std::sort(e.rbegin(), e.rend());
    for (auto& [n, m] : e) std::printf("  %6zu  %s\n", n, m.c_str());
    return 0;
}

}  // namespace

// Load a level's live actors with their properties, and report. With a dump
// file, write each actor's name, class, Location and Tag, to compare with what
// tools/umap.py reads from the same map.
int level(const std::string& dir, const std::string& map, const char* dump) {
    std::vector<std::string> paths = Linker::packageFiles(dir);
    paths.push_back(map);
    Linker lk(paths);
    std::string stem = map.substr(map.find_last_of("/\\") + 1);
    stem = stem.substr(0, stem.find_last_of('.'));
    int pkg = lk.packageIndex(stem);
    if (pkg < 0) throw std::runtime_error("the level " + stem + " did not load");
    const Package& p = *lk.packages[size_t(pkg)];
    LevelRecord lv = readLevel(p);
    size_t before = lk.problems.size();
    std::vector<Object*> actors = loadActors(lk, pkg, lv);
    std::map<std::string, size_t> byClass;
    size_t noClass = 0;
    for (Object* a : actors) {
        if (a->cls) byClass[a->cls->name.str()]++;
        else ++noClass;
    }
    std::printf("level               %s, URL %s:%s port %d\n", p.file.c_str(), lv.protocol.c_str(),
                lv.map.c_str(), lv.port);
    std::printf("actors              %zu listed, %zu loaded, %zu without a class\n", lv.actors.size(),
                actors.size(), noClass);
    std::printf("first actor         %s\n", actors.empty() ? "-" : actors[0]->path().c_str());
    std::vector<std::pair<size_t, std::string>> top;
    for (auto& [c, n] : byClass) top.emplace_back(n, c);
    std::sort(top.rbegin(), top.rend());
    std::printf("classes             %zu, most common:", byClass.size());
    for (size_t i = 0; i < top.size() && i < 6; ++i) std::printf(" %s %zu", top[i].second.c_str(), top[i].first);
    std::printf("\nproperty problems   %zu\n", lk.problems.size() - before);
    for (size_t i = before; i < lk.problems.size() && i < before + 10; ++i)
        std::printf("  %s: %s\n", lk.problems[i].first.c_str(), lk.problems[i].second.c_str());
    if (dump) {
        FILE* out = std::fopen(dump, "w");
        if (!out) throw std::runtime_error(std::string("cannot write ") + dump);
        for (Object* a : actors) {
            float x = 0, y = 0, z = 0;
            std::string tag;
            if (a->cls) {
                if (Prop* lp = a->cls->findProp(Name("Location"))) {
                    const Value& v = a->props[size_t(lp->slot)];
                    if (v.isStruct() && v.st().f.size() == 3) {
                        x = v.st().f[0].f();
                        y = v.st().f[1].f();
                        z = v.st().f[2].f();
                    }
                }
                if (Prop* tp = a->cls->findProp(Name("Tag"))) tag = a->props[size_t(tp->slot)].n().str();
            }
            std::fprintf(out, "%s\t%s\t%.4f\t%.4f\t%.4f\t%s\n", a->name.str().c_str(),
                         a->cls ? a->cls->name.str().c_str() : "-", x, y, z, tag.c_str());
        }
        std::fclose(out);
    }
    return 0;
}

// Load a level and send its actors the start up events, the way the engine
// does when a map begins: PreBeginPlay to every actor, then BeginPlay to
// every actor, then PostBeginPlay, then SetInitialState. The order is the
// engine's, from its published behaviour, not from the data. Reports what
// ran, what failed, and the natives the sequence needed that do not exist.
int start(const std::string& dir, const std::string& map) {
    std::vector<std::string> paths = Linker::packageFiles(dir);
    paths.push_back(map);
    Linker lk(paths);
    std::string stem = map.substr(map.find_last_of("/\\") + 1);
    stem = stem.substr(0, stem.find_last_of('.'));
    int pkg = lk.packageIndex(stem);
    if (pkg < 0) throw std::runtime_error("the level " + stem + " did not load");
    LevelRecord lv = readLevel(*lk.packages[size_t(pkg)]);
    std::vector<Object*> actors = loadActors(lk, pkg, lv);
    VM vm(lk);
    vm.sink = [](const std::string&, const std::string&) {};
    std::map<std::string, size_t> failures;
    for (const char* ev : {"PreBeginPlay", "BeginPlay", "PostBeginPlay", "SetInitialState"}) {
        size_t ok = 0, failed = 0;
        for (Object* a : actors) {
            if (a->deleted) continue;
            try {
                vm.event(a, ev);
                ++ok;
            } catch (const std::exception& ex) {
                ++failed;
                failures[std::string(ev) + ": " + pattern(ex.what())]++;
            }
        }
        std::printf("%-19s %zu ran, %zu failed\n", ev, ok, failed);
    }
    std::map<std::string, size_t> states;
    for (Object* a : actors)
        if (a->state) states[a->state->name.str()]++;
    std::vector<std::pair<size_t, std::string>> top;
    for (auto& [s, n] : states) top.emplace_back(n, s);
    std::sort(top.rbegin(), top.rend());
    size_t inState = 0;
    for (auto& [n, s] : top) inState += n;
    std::printf("in a state          %zu of %zu actors:", inState, actors.size());
    for (size_t i = 0; i < top.size() && i < 8; ++i) std::printf(" %s %zu", top[i].second.c_str(), top[i].first);
    std::printf("\n");
    std::vector<std::pair<size_t, std::string>> miss;
    for (auto& [k, n] : vm.missingCalls) miss.emplace_back(n, k);
    std::sort(miss.rbegin(), miss.rend());
    std::printf("missing natives     %zu, most called:\n", miss.size());
    for (size_t i = 0; i < miss.size() && i < 25; ++i) std::printf("  %7zu  %s\n", miss[i].first, miss[i].second.c_str());
    if (!failures.empty()) {
        std::vector<std::pair<size_t, std::string>> f;
        for (auto& [m, n] : failures) f.emplace_back(n, m);
        std::sort(f.rbegin(), f.rend());
        std::printf("failures:\n");
        for (size_t i = 0; i < f.size() && i < 20; ++i) std::printf("  %7zu  %s\n", f[i].first, f[i].second.c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1], dir = argv[2];
    try {
        if (cmd == "check") return check(dir);
        if (cmd == "smoke") return smoke(dir);
        if (cmd == "start" && argc >= 4) return start(dir, argv[3]);
        if (cmd == "level" && argc >= 4) return level(dir, argv[3], argc >= 5 ? argv[4] : nullptr);
        if (cmd == "call" && argc >= 4)
            return call(dir, argv[3], std::vector<std::string>(argv + 4, argv + argc));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return usage();
}
