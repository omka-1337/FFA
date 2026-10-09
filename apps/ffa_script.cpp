// ffa-script: run a game's UnrealScript, and check the VM against its corpus.
//
//   ffa-script check <System dir>                 load and compile everything, report
//   ffa-script call  <System dir> Class.Function [args...]
//   ffa-script smoke <System dir>                 call every static function once
//   ffa-script level <System dir> <map> [dump]    load a level's live actors
//   ffa-script start <System dir> <map>           and run its start up sequence
//   ffa-script run <System dir> <map> <seconds>   then that long of level time
//   ffa-script collide <System dir> <packages...> check the BSP as collision
//   ffa-script textures <game dir> <out.tsv>      decode every texture, for comparing
//
// `check` is the corpus-wide proof the VM rests on, in the manner of the
// Python readers in tools/: every class loads, every function and state compiles with
// every reference resolved and every jump landing on a statement, every
// default block decodes, and the state tail and cast table are tested
// against what the bytecode itself says.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

#include "script/VM.h"
#include "render/Texture.h"
#include "world/Bsp.h"
#include "world/SkeletalMesh.h"
#include "world/Collision.h"
#include "world/Level.h"
#include "world/Physics.h"
#include "world/Session.h"
#include "world/World.h"

using namespace ffa;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: ffa-script check <System dir>\n"
                 "       ffa-script call <System dir> Class.Function [args...]\n"
                 "       ffa-script smoke <System dir>\n"
                 "       ffa-script level <System dir> <map.unr> [dump.tsv]\n"
                 "       ffa-script start <System dir> <map.unr>\n"
                 "       ffa-script run <System dir> <map.unr> <seconds> [--hold <key>] [--axis <var>=<value>] [--event <tag>] [--log <word>] [--exec <seconds>=<command>]...\n"
                 "       ffa-script collide <System dir> <map.unr or .usx>...\n");
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

// A key of an .ini file in the System directory, or empty. Enough for the
// few settings the engine reads before any script runs.
std::string iniValue(const std::string& file, const std::string& section, const std::string& key) {
    std::ifstream in(file);
    std::string line, at;
    auto lower = [](std::string x) {
        for (char& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return x;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && line[0] == '[') {
            at = lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (at == lower(section) && eq != std::string::npos && lower(line.substr(0, eq)) == lower(key))
            return line.substr(eq + 1);
    }
    return std::string();
}

// Load a level and begin play the way the engine does when a map loads: spawn
// the game, InitGame it, then send every actor PreBeginPlay, then BeginPlay,
// then PostBeginPlay and PostNetBeginPlay, then SetInitialState. The order is
// the engine's, from its published behaviour, not from the data. Reports what
// ran, what failed, and the natives the sequence needed that do not exist.
int start(const std::string& dir, const std::string& map, float seconds, const std::vector<std::string>& hold = {},
          const std::vector<std::string>& events = {}, const std::vector<std::string>& logs = {},
          std::vector<std::pair<float, std::string>> execs = {}) {
    Session session(dir, map);
    VM& vm = *session.vm;
    World& w = *session.world;
    // --log: the script's log lines that hold any of the words, as they come
    if (!logs.empty())
        vm.sink = [&w, logs](const std::string& tag, const std::string& text) {
            for (const std::string& l : logs)
                if (text.find(l) != std::string::npos || tag == l) {
                    std::printf("  %7.2fs  %-13s %s\n", w.time, tag.c_str(), text.c_str());
                    return;
                }
        };
    Collision& col = *session.collision;
    (void)col;
    size_t loaded = w.actors.size();
    std::string dgt = utf8(w.var(w.info, "DefaultGameType").s());
    std::printf("game                %s%s, the level's DefaultGameType %s\n", session.gameName.c_str(),
                session.gameClass ? "" : " (not found)", dgt.empty() ? "empty" : dgt.c_str());
    session.begin();
    Object* pc = session.controller;
    // Keys held for the whole run, by a key or an alias of DefUser.ini.
    for (const std::string& want : hold) {
        if (want[0] == '=') {
            // --axis name=value: an input variable held at a value as it is
            size_t eq = want.find('=', 1);
            w.held.emplace_back(want.substr(1, eq - 1), std::stof(want.substr(eq + 1)));
            std::printf("holding             %s\n", want.substr(1).c_str());
            continue;
        }
        auto axes = session.axesOf(want);
        if (axes.empty()) throw std::runtime_error("no axis bound to " + want + " in DefUser.ini");
        for (auto& [axis, speed] : axes) {
            w.held.emplace_back(axis, speed);
            std::printf("holding             %s: %s at %g\n", want.c_str(), axis.c_str(), speed);
        }
    }
    // Events sent at the start, as a trigger would: the game's own
    // TriggerEvent, which triggers every actor with that Tag.
    for (const std::string& e : events) {
        Object* pawn = pc ? w.obj(pc, "Pawn") : nullptr;
        vm.event(w.info, "TriggerEvent", {Value::Nm(Name(e)), Value::Obj(w.info), Value::Obj(pawn)});
        std::printf("event               %s triggered\n", e.c_str());
    }
    // Where every mover starts, to see which move.
    std::map<Object*, Vec3> moverStart;
    if (Class* moverClass = vm.findClass("Mover"))
        for (Object* a : w.actors)
            if (!a->deleted && a->isA(moverClass)) {
                Vec3 v;
                vm.unvector(w.var(a, "Location"), v.x, v.y, v.z);
                moverStart[a] = v;
            }
    // Where every pawn starts, to see where physics takes it.
    std::map<Object*, float> startZ;
    std::map<Object*, Vec3> startAt;
    for (Object* a : w.actors)
        if (!a->deleted && a->isA(w.pawnClass)) {
            float x, y, z;
            vm.unvector(w.var(a, "Location"), x, y, z);
            startZ[a] = z;
            startAt[a] = {x, y, z};
        }
    // Then time: frames of a thirtieth of a second, with the player's and its
    // pawn's state changes as they happen.
    std::vector<std::string> timeline;
    auto stateOf = [](Object* o) { return o && o->state ? o->state->name.str() : std::string("none"); };
    Object* pawn0 = pc ? w.obj(pc, "Pawn") : nullptr;
    std::string pcState = stateOf(pc), pawnState = stateOf(pawn0);
    std::set<Object*> touched;
    std::set<Object*> gone, wasGone;
    for (Object* a : w.actors)
        if (a->deleted) wasGone.insert(a);
    int pawnPhysics = pawn0 ? w.var(pawn0, "Physics").i() : -1;
    for (int f = 0; f < int(seconds * 30.0f + 0.5f); ++f) {
        // --exec: a command run once at its time, as a key bound to it does
        for (auto& [t, command] : execs)
            if (!command.empty() && w.time >= t) {
                std::printf("  %7.2fs  exec %s\n", w.time, command.c_str());
                session.exec(command);
                command.clear();
            }
        w.tick(1.0f / 30.0f);
        if (!pc) continue;
        Object* pawn = w.obj(pc, "Pawn");
        char at[32];
        std::snprintf(at, sizeof at, "%7.2fs  ", w.time);
        if (stateOf(pc) != pcState) timeline.push_back(at + std::string("controller ") + (pcState = stateOf(pc)));
        if (pawn && !w.held.empty() && (f + 1) % 30 == 0) {
            float x, y, z, vx, vy, vz;
            vm.unvector(w.var(pawn, "Location"), x, y, z);
            vm.unvector(w.var(pawn, "Velocity"), vx, vy, vz);
            char buf[160];
            float ax, ay, az;
            vm.unvector(w.var(pawn, "Acceleration"), ax, ay, az);
            Object* input = w.obj(pc, "PlayerInput");
            std::snprintf(buf, sizeof buf, "pawn at (%.0f, %.0f, %.0f), speed %.0f, acceleration %.0f, physics %d, "
                          "aForward %.0f, input %s", x, y, z, std::sqrt(vx * vx + vy * vy + vz * vz),
                          std::sqrt(ax * ax + ay * ay + az * az), w.var(pawn, "Physics").i(), w.var(pc, "aForward").f(),
                          input ? input->cls->name.str().c_str() : "none");
            std::string anims;
            for (size_t k = 0; k < session.animator->state(pawn).channels.size(); ++k) {
                const AnimChannel& ch = session.animator->state(pawn).channels[k];
                if (ch.seq) anims += " " + std::to_string(k) + ":" + ch.name + (ch.animating ? "" : "(stopped)");
            }
            std::strncat(buf, (", animating" + anims).c_str(), sizeof buf - std::strlen(buf) - 1);
            timeline.push_back(at + std::string(buf));
        }
        for (size_t i = 0; i < w.actors.size(); ++i) {
            Object* a = w.actors[i];
            if (a->deleted && !w.held.empty() && gone.insert(a).second && !wasGone.count(a))
                timeline.push_back(at + std::string("gone: ") + a->name.str() + " (" + a->cls->name.str() + ")");
        }
        if (pawn) {
            const Value& t = w.var(pawn, "Touching");
            if (t.isArr())
                for (const Value& e : t.arr())
                    if (e.o() && touched.insert(e.o()).second)
                        timeline.push_back(at + std::string("pawn touches ") + e.o()->name.str() + " (" +
                                           e.o()->cls->name.str() + ")");
        }
        if (pawn != pawn0) {
            timeline.push_back(at + std::string("pawn is now ") + (pawn ? pawn->path() : std::string("none")));
            pawn0 = pawn;
        }
        if (stateOf(pawn) != pawnState) timeline.push_back(at + std::string("pawn ") + (pawnState = stateOf(pawn)));
        if (pawn) {
            int ph = w.var(pawn, "Physics").i();
            if (ph != pawnPhysics) {
                float x, y, z;
                vm.unvector(w.var(pawn, "Location"), x, y, z);
                Object* base = w.obj(pawn, "Base");
                char buf[160];
                std::snprintf(buf, sizeof buf, "pawn physics %d at (%.1f, %.1f, %.1f), base %s", ph, x, y, z,
                              base ? base->name.str().c_str() : "none");
                timeline.push_back(at + std::string(buf));
                pawnPhysics = ph;
            }
        }
    }
    for (const char* ev : {"InitGame", "PreBeginPlay", "BeginPlay", "PostBeginPlay", "PostNetBeginPlay",
                           "SetInitialState", "FilterForCurrentGameState",
                           "PrePersistentDataRestored", "PostPersistentDataRestored", "Login", "InitInputSystem", "PostLogin", "PlayerTick", "Tick", "Timer", "tick"})
        std::printf("%-19s %zu ran, %zu failed\n", ev, w.sent[ev], w.failed[ev]);
    if (w.frames) {
        std::printf("ran                 %zu frames, %.2f s of level time\n", w.frames, w.time);
        Animator& an = *session.animator;
        // the posed skin against the collision cylinder: its lowest point
        // should be the cylinder's bottom, its feet on the ground
        std::vector<float> feet;
        for (Object* a : w.actors) {
            if (a->deleted || !a->isA(w.pawnClass)) continue;
            const SkeletalMesh* sk = an.state(a).mesh;
            if (!sk) continue;
            std::vector<Vec3> pts = sk->skin(an.pose(a));
            float m[3][3];
            Vec3 o;
            an.meshToWorld(a, m, o);
            float low = 1e30f;
            for (const Vec3& p : pts) {
                Vec3 q = sk->toActor(p);
                low = std::min(low, o.z + m[2][0] * q.x + m[2][1] * q.y + m[2][2] * q.z);
            }
            feet.push_back(low - (o.z - w.var(a, "CollisionHeight").f()));
        }
        std::sort(feet.begin(), feet.end());
        if (!feet.empty())
            std::printf("posed pawns         %zu: lowest skin point above the cylinder's bottom, 10%% %.1f median %.1f 90%% %.1f\n",
                        feet.size(), feet[feet.size() / 10], feet[feet.size() / 2], feet[feet.size() * 9 / 10]);
        std::printf("animation           %zu sequences started, %zu not found, %zu AnimEnd sent, %zu notifies\n",
                    an.sequencesPlayed, an.notFound, an.animEnds, an.notifiesSent);
        for (auto& [k, n] : an.missing) std::printf("  not found %6zu  %s\n", n, k.c_str());
        for (const std::string& t : timeline) std::printf("  %s\n", t.c_str());
    }
    auto name = [](Object* o) { return o ? o->path() + " (" + o->cls->name.str() + ")" : std::string("none"); };
    std::printf("player controller   %s", name(pc).c_str());
    if (pc) {
        Object* pawn = w.obj(pc, "Pawn");
        std::printf(", state %s\n", pc->state ? pc->state->name.str().c_str() : "none");
        std::printf("  pawn              %s", name(pawn).c_str());
        if (pawn) {
            float x, y, z;
            vm.unvector(w.var(pawn, "Location"), x, y, z);
            std::printf(" at (%.0f, %.0f, %.0f), state %s", x, y, z,
                        pawn->state ? pawn->state->name.str().c_str() : "none");
        }
        std::printf("\n  view target       %s\n", name(w.obj(pc, "ViewTarget")).c_str());
        {
            int32_t cr[3], prr[3] = {0, 0, 0};
            vm.unrotator(w.var(pc, "Rotation"), cr[0], cr[1], cr[2]);
            if (pawn) vm.unrotator(w.var(pawn, "Rotation"), prr[0], prr[1], prr[2]);
            Object* cam = pc->cls->findProp(Name("Camera")) ? w.obj(pc, "Camera") : nullptr;
            if (cam)
                std::printf("  camera flags      sync rotation %d, transitioning %d, player %s, controller bUseBaseCam %d bShouldRotate %d, rDest yaw %d\n",
                            w.flag(cam, "bSyncRotationWithTarget"), w.flag(cam, "bTransitioning"),
                            w.obj(cam, "Player") ? w.obj(cam, "Player")->name.str().c_str() : "none", w.flag(pc, "bUseBaseCam"),
                            w.flag(pc, "bShouldRotate"), [&] { int32_t a2, b2, c2; vm.unrotator(w.var(cam, "rDestRotation"), a2, b2, c2); return b2; }());
            std::printf("  rotations         controller (%d, %d, %d), pawn (%d, %d, %d), savedATurn %.2f aTurn %.2f\n", cr[0], cr[1], cr[2],
                        prr[0], prr[1], prr[2], pc->cls->findProp(Name("savedATurn")) ? w.var(pc, "savedATurn").f() : 0.0f, w.var(pc, "aTurn").f());
        }
        // KnowWonder's camera follows the pawn as an actor of its own.
        for (Object* a : w.actors) {
            if (a->deleted || !a->cls->name.str().ends_with("Cam") || !pawn) continue;
            float x, y, z, px, py, pz;
            vm.unvector(w.var(a, "Location"), x, y, z);
            vm.unvector(w.var(pawn, "Location"), px, py, pz);
            int32_t pitch, yaw, roll;
            vm.unrotator(w.var(a, "Rotation"), pitch, yaw, roll);
            std::printf("  camera            %s (%s) at (%.0f, %.0f, %.0f), %.0f from the pawn, %.0f above; "
                        "rotation (%d, %d, %d), state %s\n",
                        a->path().c_str(), a->cls->name.str().c_str(), x, y, z,
                        std::sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz)), z - pz, pitch,
                        yaw, roll, a->state ? a->state->name.str().c_str() : "none");
        }
        std::printf("  HUD               %s\n", name(w.obj(pc, "myHUD")).c_str());
    } else {
        std::printf("\n");
    }
    size_t live = 0, spawnedLive = 0;
    std::map<std::string, size_t> states, spawned;
    for (size_t i = 0; i < w.actors.size(); ++i) {
        Object* a = w.actors[i];
        if (a->deleted) continue;
        ++live;
        if (a->state) states[a->state->name.str()]++;
        if (i >= loaded) {
            ++spawnedLive;
            spawned[a->cls->name.str()]++;
        }
    }
    auto top = [](const std::map<std::string, size_t>& m, size_t k) {
        std::vector<std::pair<size_t, std::string>> t;
        for (auto& [s, n] : m) t.emplace_back(n, s);
        std::sort(t.rbegin(), t.rend());
        std::string out;
        for (size_t i = 0; i < t.size() && i < k; ++i) out += " " + t[i].second + " " + std::to_string(t[i].first);
        return out;
    };
    std::printf("actors              %zu loaded, %zu for the editor only, %zu spawned, %zu destroyed, %zu live\n",
                loaded, w.editorOnly, w.actors.size() - loaded, w.actors.size() - live, live);
    std::printf("spawned and live    %zu:%s\n", spawnedLive, top(spawned, 8).c_str());
    if (w.frames) {
        size_t still = 0, up = 0, down = 0, fell = 0;
        float worst = 0;
        std::string worstName;
        for (auto& [a, z0] : startZ) {
            if (a->deleted) continue;
            float x, y, z;
            vm.unvector(w.var(a, "Location"), x, y, z);
            float dz = z - z0;
            if (std::fabs(dz) <= 1) ++still;
            else if (dz > 0) ++up;
            else ++down;
            if (dz < -500) ++fell;
            if (dz < worst) {
                worst = dz;
                worstName = a->path() + " (" + a->cls->name.str() + ")";
            }
        }
        size_t moved = 0, interpolating = 0;
        float farthest = 0;
        for (auto& [m, v0] : moverStart) {
            Vec3 v;
            vm.unvector(w.var(m, "Location"), v.x, v.y, v.z);
            float d = length(v - v0);
            moved += d > 1;
            farthest = std::max(farthest, d);
            interpolating += w.flag(m, "bInterpolating");
        }
        if (!moverStart.empty())
            std::printf("movers              %zu: %zu moved, the farthest %.0f; %zu moving at the end\n", moverStart.size(),
                        moved, farthest, interpolating);
        std::printf("pawns               %zu: within a unit of where they started %zu, higher %zu, lower %zu, "
                    "more than 500 lower %zu; the lowest %s by %.0f\n",
                    startZ.size(), still, up, down, fell, worstName.c_str(), -worst);
    }
    // The pawns other controllers drive: their controller's state, and how far
    // they went, the first dozen by distance.
    {
        std::vector<std::tuple<float, Object*, Object*>> ai;
        for (auto& [a, v0] : startAt) {
            if (a->deleted) continue;
            Object* c = w.obj(a, "Controller");
            if (!c || c == pc) continue;
            Vec3 v;
            vm.unvector(w.var(a, "Location"), v.x, v.y, v.z);
            ai.emplace_back(length(v - v0), a, c);
        }
        std::sort(ai.begin(), ai.end(), [](auto& x, auto& y) { return std::get<0>(x) > std::get<0>(y); });
        size_t moved = 0;
        for (auto& t : ai) moved += std::get<0>(t) > 1;
        std::printf("controlled pawns    %zu, %zu moved\n", ai.size(), moved);
        for (size_t i = 0; i < ai.size() && i < 12; ++i) {
            auto [d, a, c] = ai[i];
            std::printf("  %-28s %-26s state %-24s moved %.0f\n", a->name.str().c_str(), c->cls->name.str().c_str(),
                        c->state ? c->state->name.str().c_str() : "none", d);
        }
    }
    // Cutscenes: each KnowWonder cut controller still scripting, where it is
    // in its script and what its action is.
    if (Class* cc = vm.findClass("KWCutController")) {
        size_t n = 0;
        for (Object* a : w.actors) {
            if (a->deleted || !a->isA(cc) || !a->state || a->state->name != Name("Scripting")) continue;
            if (n++ == 0) std::printf("cutscene actions\n");
            const Value& acts = w.var(a, "Actions");
            int k = w.var(a, "ActionNum").i();
            std::string what;
            if (acts.isArr() && k >= 0 && size_t(k) < acts.arr().size() && acts.arr()[size_t(k)].o())
                what = utf8(vm.call(acts.arr()[size_t(k)].o(), "GetActionString").s());
            Object* pawn = w.obj(a, "Pawn");
            std::printf("  %-22s %-18s %s %d of %zu: %s\n", a->name.str().c_str(), pawn ? pawn->name.str().c_str() : "none",
                        utf8(w.var(a, "ScriptFileName").s()).c_str(), k, acts.isArr() ? acts.arr().size() : 0, what.c_str());
        }
    }
    std::map<std::string, size_t> physics;
    static const char* modes[] = {"None", "Walking", "Falling", "Swimming", "Flying", "Rotating", "Projectile",
                                  "Interpolating", "MovingBrush", "Spider", "Trailer", "Ladder", "RootMotion",
                                  "Karma", "KarmaRagDoll", "PushPulled"};
    for (Object* a : w.actors) {
        if (a->deleted || w.flag(a, "bStatic")) continue;
        int m = w.var(a, "Physics").i();
        physics[m >= 0 && m < 16 ? modes[m] : "?"]++;
    }
    std::printf("physics, not static %s\n", top(physics, 16).c_str());
    {
        size_t out = 0;
        for (Object* a : w.actors)
            if (!a->deleted && !w.flag(a, "bInCurrentGameState")) ++out;
        std::printf("game state          %s: %zu actors out of it, hidden and not colliding\n",
                    utf8(w.game ? vm.call(w.game, "GetGameState").s() : String()).c_str(), out);
    }
    {
        std::map<std::string, size_t> movers;
        Class* moverClass = vm.findClass("Mover");
        for (Object* a : w.actors)
            if (!a->deleted && moverClass && a->isA(moverClass))
                movers[a->cls->name.str() + " drawn " + std::to_string(w.var(a, "DrawType").i()) +
                       (w.obj(a, "StaticMesh") ? " with a mesh" : "") + (w.obj(a, "Brush") ? " with a brush" : "")]++;
        if (!movers.empty()) std::printf("movers             %s\n", top(movers, 8).c_str());
    }
    size_t inState = 0;
    for (auto& [s, n] : states) inState += n;
    std::printf("in a state          %zu:%s\n", inState, top(states, 8).c_str());
    std::vector<std::pair<size_t, std::string>> miss;
    for (auto& [k, n] : vm.missingCalls) miss.emplace_back(n, k);
    std::sort(miss.rbegin(), miss.rend());
    std::printf("missing natives     %zu, most called:\n", miss.size());
    for (size_t i = 0; i < miss.size() && i < 25; ++i) std::printf("  %7zu  %s\n", miss[i].first, miss[i].second.c_str());
    if (!w.failures.empty()) {
        std::vector<std::pair<size_t, std::string>> f;
        std::map<std::string, size_t> grouped;
        for (auto& [m, n] : w.failures) grouped[pattern(m)] += n;
        for (auto& [m, n] : grouped) f.emplace_back(n, m);
        std::sort(f.rbegin(), f.rend());
        std::printf("failures:\n");
        for (size_t i = 0; i < f.size() && i < 20; ++i) std::printf("  %7zu  %s\n", f[i].first, f[i].second.c_str());
    }
    return 0;
}

// The level's collision, checked against the data. Every Model and StaticMesh
// in the given packages must read to the end of its record. Every live actor's
// saved Region, which the engine wrote, must be the leaf and zone the BSP walk
// finds. A trace down from what stands in the open, coins and path nodes, must
// hit the BSP on one of its polygons, not on a bare splitting plane; and with
// the static meshes added, it shows how far above the ground they stand.
int collide(const std::string& dir, const std::vector<std::string>& files) {
    size_t models = 0, modelsBad = 0, meshes = 0, meshesBad = 0;
    size_t regions = 0, leafAgree = 0, zoneAgree = 0;
    size_t traces = 0, hits = 0, onPoly = 0, startSolid = 0, meshHits = 0;
    size_t meshActors = 0, meshesMissing = 0;
    size_t terrainCount = 0, terrainHits = 0;
    size_t treeChecks = 0, treeAgree = 0;
    size_t brushes = 0, brushPoints = 0, brushOn = 0, brushAll = 0;
    size_t boxNodes = 0, boxFits = 0, bspFaces = 0, bspSkipped = 0, statics = 0, volumes = 0, volumeTris = 0;
    std::vector<float> boxDrops;
    float worstNormal = 0;
    std::vector<float> drops, bspDrops;
    std::map<std::string, std::vector<float>> byClass, withActors;
    std::map<std::string, size_t> problems;
    std::string gameDir = dir + "/..";
    for (const std::string& file : files) {
        Package pkg(file);
        for (int i = 1; i <= int(pkg.exports.size()); ++i) {
            std::string cls = pkg.classOf(i);
            if ((cls != "Model" && cls != "StaticMesh") || pkg.exp(i).size <= 0) continue;
            bool model = cls == "Model";
            ++(model ? models : meshes);
            try {
                if (model)
                    BspModel m(pkg, i);
                else
                    StaticMeshCollision m(pkg, i);
            } catch (const FormatError& ex) {
                ++(model ? modelsBad : meshesBad);
                problems[cls + ": " + ex.what()]++;
            }
        }
        if (file.size() < 4 || file.compare(file.size() - 4, 4, ".unr") != 0) continue;
        std::vector<std::string> paths = Linker::packageFiles(dir);
        paths.push_back(file);
        Linker lk(paths);
        std::string stem = file.substr(file.find_last_of("/\\") + 1);
        stem = stem.substr(0, stem.find_last_of('.'));
        int k = lk.packageIndex(stem);
        if (k < 0) continue;
        LevelRecord lv = readLevel(*lk.packages[size_t(k)]);
        VM vm(lk);
        World w(vm, k, lv);
        Collision col(w, k, lv.model, gameDir);
        meshActors += col.meshActors;
        terrainCount += col.terrains.size();
        bspFaces += col.bspFaces;
        bspSkipped += col.bspFacesSkipped;
        statics += col.staticTriangles;
        volumes += col.brushActors;
        volumeTris += col.brushTriangles;
        for (auto& t : col.terrains) worstNormal = std::max(worstNormal, t->normalCheck());
        meshesMissing += col.meshesMissing;
        for (auto& [m, n] : col.problems) problems[m] += n;
        const BspModel& bsp = col.bsp;
        // The editor's brushes are what the level's BSP was built from, so
        // carried into the world their corners are corners of the BSP.
        {
            std::map<std::tuple<long, long, long>, int> grid;
            for (const Vec3& p : bsp.points) grid[{std::lround(p.x * 4), std::lround(p.y * 4), std::lround(p.z * 4)}] = 1;
            auto near = [&](Vec3 p) {
                long x = std::lround(p.x * 4), y = std::lround(p.y * 4), z = std::lround(p.z * 4);
                for (long dx = -1; dx <= 1; ++dx)
                    for (long dy = -1; dy <= 1; ++dy)
                        for (long dz = -1; dz <= 1; ++dz)
                            if (grid.count({x + dx, y + dy, z + dz})) return true;
                return false;
            };
            for (Object* b : loadActors(lk, k, lv)) {
                if (b->cls->name != Name("Brush")) continue;
                Object* mo = w.obj(b, "Brush");
                const std::vector<BrushPolygon>* polys = mo ? col.brushPolygons(mo) : nullptr;
                if (!polys || polys->empty()) continue;
                float m[3][3];
                Vec3 o;
                col.brushTransform(b, m, o);
                size_t on = 0, all = 0;
                for (const BrushPolygon& q : *polys)
                    for (const Vec3& p : q.vertices) {
                        ++all;
                        on += near(o + Vec3{m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z,
                                            m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
                                            m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z});
                    }
                ++brushes;
                brushPoints += all;
                brushOn += on;
                brushAll += on == all;
            }
        }
        for (Object* a : w.actors) {
            Vec3 at;
            vm.unvector(w.var(a, "Location"), at.x, at.y, at.z);
            const StructVal& sv = w.var(a, "Region").st();
            Prop* lf = sv.type->field(Name("iLeaf"));
            Prop* zf = sv.type->field(Name("ZoneNumber"));
            BspModel::Region g = bsp.regionAt(at);
            ++regions;
            leafAgree += g.leaf == sv.f[size_t(lf->slot)].i();
            zoneAgree += g.zone == sv.f[size_t(zf->slot)].i();
            std::string cls = a->cls->name.str();
            // A path node is where the editor found room for a pawn of its
            // size, standing on the ground: a box of its extent fits there,
            // and sinks next to nothing before it touches the ground.
            if (cls == "PathNode") {
                Vec3 ext{w.var(a, "CollisionRadius").f(), w.var(a, "CollisionRadius").f(),
                         w.var(a, "CollisionHeight").f()};
                ++boxNodes;
                boxFits += col.fits(at, ext, a);
                TraceHit bh = col.boxCheck(at, at - Vec3{0, 0, 256}, ext, a);
                if (bh) boxDrops.push_back(bh.time * 256);
            }
            bool open = cls.find("Coin") != std::string::npos || cls == "PathNode";
            if (!open || bsp.solidAt(at)) continue;
            ++traces;
            Vec3 down = at + Vec3{0, 0, -8192};
            Hit h = bsp.lineCheck(at, down);
            if (h) {
                ++hits;
                startSolid += h.startSolid;
                onPoly += bsp.onPolygon(h.node, h.location, 0.5f);
                bspDrops.push_back(at.z - h.location.z);
            }
            TraceHit t = col.lineCheck(at, down, a);
            // with the actors that block players standing on them, the
            // beanstalk's leaves among them, which are meshes with cylinders
            TraceHit ta = col.lineCheck(at, down, a, true);
            if (ta) withActors[cls.find("Coin") != std::string::npos ? "Coin" : "PathNode"].push_back(at.z - ta.location.z);
            col.everyTriangle = true;
            TraceHit all = col.lineCheck(at, down, a);
            col.everyTriangle = false;
            ++treeChecks;
            treeAgree += std::fabs(all.time - t.time) * 8192 < 0.01f && all.actor == t.actor;
            if (t) {
                byClass[cls + " (CollisionHeight " + std::to_string(int(w.var(a, "CollisionHeight").f())) + ")"]
                    .push_back(at.z - t.location.z);
                drops.push_back(at.z - t.location.z);
                meshHits += t.actor && t.actor->cls->name != Name("TerrainInfo");
                terrainHits += t.actor && t.actor->cls->name == Name("TerrainInfo");
            }
        }
    }
    std::printf("models              %zu read to the end, %zu not\n", models - modelsBad, modelsBad);
    std::printf("static meshes       %zu read to the end, %zu not\n", meshes - meshesBad, meshesBad);
    std::printf("regions             %zu actors: leaf agrees %zu, zone agrees %zu\n", regions, leafAgree, zoneAgree);
    std::printf("mesh actors         %zu blocking traces, %zu meshes not found\n", meshActors, meshesMissing);
    std::printf("terrains            %zu read to the end; stored normals agree with the split to %.6f\n",
                terrainCount, worstNormal);
    auto q = [](std::vector<float>& v, double f) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0f : v[size_t(f * double(v.size() - 1))];
    };
    std::printf("down from coins and path nodes, %zu\n", traces);
    std::printf("  BSP               %zu hit, %zu on a polygon, %zu start solid; drop 10%% %.1f median %.1f 90%% %.1f\n",
                hits, onPoly, startSolid, q(bspDrops, 0.1), q(bspDrops, 0.5), q(bspDrops, 0.9));
    std::printf("  everything        %zu hit, %zu on a mesh, %zu on terrain; drop 10%% %.1f median %.1f 90%% %.1f\n",
                drops.size(), meshHits, terrainHits, q(drops, 0.1), q(drops, 0.5), q(drops, 0.9));
    for (auto& [c, v] : withActors)
        std::printf("  with actors, %-9s %5zu: 10%% %.1f median %.1f 90%% %.1f 99%% %.1f\n", c.c_str(), v.size(), q(v, 0.1),
                    q(v, 0.5), q(v, 0.9), q(v, 0.99));
    std::printf("  the mesh trees agree with testing every triangle on %zu of %zu\n", treeAgree, treeChecks);
    std::printf("brushes             %zu of the editor's: %zu of their %zu corners are BSP points, to a quarter "
                "unit; every corner for %zu brushes\n",
                brushes, brushOn, brushPoints, brushAll);
    std::printf("boxes               %zu BSP faces bound solid, %zu do not; %zu static triangles; %zu colliding "
                "brushes, %zu triangles\n",
                bspFaces, bspSkipped, statics, volumes, volumeTris);
    std::printf("  path nodes        %zu: a box of their size fits at %zu; it sinks %zu of them, 10%% %.2f median %.2f "
                "90%% %.2f 99%% %.1f\n",
                boxNodes, boxFits, boxDrops.size(), q(boxDrops, 0.1), q(boxDrops, 0.5), q(boxDrops, 0.9),
                q(boxDrops, 0.99));
    for (auto& [c, v] : byClass)
        if (v.size() >= 20)
            std::printf("  %-36s %5zu: 10%% %.1f median %.1f 90%% %.1f\n", c.c_str(), v.size(), q(v, 0.1), q(v, 0.5),
                        q(v, 0.9));
    for (auto& [m, n] : problems) std::printf("  %6zu  %s\n", n, m.c_str());
    return 0;
}

// Every texture of the game decoded, at the largest mip no side of which is
// over 64: per texture its package, name, size and an FNV-1a hash of its RGBA,
// to compare with tools/utexture.py's decoding of the same.
int textures(const std::string& gameDir, const char* out) {
    Library lib(gameDir);
    FILE* f = std::fopen(out, "w");
    if (!f) throw std::runtime_error(std::string("cannot write ") + out);
    size_t n = 0, bad = 0;
    std::map<std::string, size_t> problems;
    for (const std::string& path : lib.files()) {
        std::string stem = std::filesystem::path(path).stem().string();
        const Package* p = lib.package(stem);
        if (!p) continue;
        for (int i = 1; i <= int(p->exports.size()); ++i) {
            if (p->classOf(i) != "Texture" || p->exp(i).size <= 0) continue;
            ++n;
            try {
                Image img = decodeTexture(lib, ObjectRef{p, i}, 64);
                uint64_t h = 1469598103934665603ull;
                for (uint8_t b : img.rgba) h = (h ^ b) * 1099511628211ull;
                std::fprintf(f, "%s\t%s\t%d\t%d\t%016llx\n", p->stem.c_str(), p->exp(i).name.c_str(), img.width,
                             img.height, (unsigned long long)h);
            } catch (const FormatError& ex) {
                ++bad;
                problems[ex.what()]++;
            }
        }
    }
    std::fclose(f);
    std::printf("textures            %zu decoded, %zu not\n", n - bad, bad);
    for (auto& [m, k] : problems) std::printf("  %6zu  %s\n", k, m.c_str());
    return 0;
}

// Every skeletal mesh with a default animation, skinned at three of its
// sequences, the first, the middle and the last, 37 percent of the way
// through: every 17th point in mesh space, to compare with tools/uanim.py's
// skin() of the same.
int poses(const std::string& gameDir, const char* out) {
    Library lib(gameDir);
    FILE* f = std::fopen(out, "w");
    if (!f) throw std::runtime_error(std::string("cannot write ") + out);
    size_t meshes = 0, posed = 0;
    std::map<std::string, size_t> problems;
    for (const std::string& path : lib.files()) {
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".ukx") != 0) continue;
        const Package* p = lib.package(std::filesystem::path(path).stem().string());
        for (int i = 1; p && i <= int(p->exports.size()); ++i) {
            if (p->classOf(i) != "SkeletalMesh" || p->exp(i).size <= 0) continue;
            ++meshes;
            try {
                SkeletalMesh m(*p, i);
                ObjectRef ar = lib.resolve(*p, m.defaultAnim);
                if (!ar || ar.cls() != "MeshAnimation") continue;
                MeshAnimation anim(*ar.pkg, ar.idx);
                size_t n = anim.sequences.size();
                if (!n) continue;
                ++posed;
                for (size_t si : {size_t(0), n / 2, n - 1}) {
                    float frame = float(anim.sequences[si].numFrames) * 0.37f;
                    std::vector<Vec3> pts = m.skin(m.compose(m.locals(anim, si, frame)));
                    for (size_t k = 0; k < pts.size(); k += 17)
                        std::fprintf(f, "%s\t%s\t%zu\t%zu\t%.4f\t%.4f\t%.4f\n", p->stem.c_str(), p->exp(i).name.c_str(),
                                     si, k, pts[k].x, pts[k].y, pts[k].z);
                }
            } catch (const FormatError& ex) {
                problems[ex.what()]++;
            }
        }
    }
    std::fclose(f);
    std::printf("skeletal meshes     %zu, %zu posed with their default animation\n", meshes, posed);
    for (auto& [m, k] : problems) std::printf("  %6zu  %s\n", k, m.c_str());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    std::string cmd = argv[1], dir = argv[2];
    try {
        if (cmd == "check") return check(dir);
        if (cmd == "smoke") return smoke(dir);
        if (cmd == "textures" && argc >= 4) return textures(dir, argv[3]);
        if (cmd == "poses" && argc >= 4) return poses(dir, argv[3]);
        if (cmd == "collide" && argc >= 4) return collide(dir, std::vector<std::string>(argv + 3, argv + argc));
        if (cmd == "start" && argc >= 4) return start(dir, argv[3], 0.0f);
        if (cmd == "run" && argc >= 5) {
            std::vector<std::string> hold, events, logs;
            std::vector<std::pair<float, std::string>> execs;
            for (int i = 5; i + 1 < argc; i += 2) {
                if (std::string(argv[i]) == "--exec") {
                    std::string e = argv[i + 1];
                    size_t eq = e.find('=');
                    if (eq != std::string::npos) execs.emplace_back(std::stof(e.substr(0, eq)), e.substr(eq + 1));
                }
                if (std::string(argv[i]) == "--log") logs.push_back(argv[i + 1]);
                if (std::string(argv[i]) == "--hold") hold.push_back(argv[i + 1]);
                if (std::string(argv[i]) == "--event") events.push_back(argv[i + 1]);
                if (std::string(argv[i]) == "--axis") hold.push_back(std::string("=") + argv[i + 1]);
            }
            return start(dir, argv[3], std::stof(argv[4]), hold, events, logs, execs);
        }
        if (cmd == "level" && argc >= 4) return level(dir, argv[3], argc >= 5 ? argv[4] : nullptr);
        if (cmd == "call" && argc >= 4)
            return call(dir, argv[3], std::vector<std::string>(argv + 4, argv + argc));
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return usage();
}
