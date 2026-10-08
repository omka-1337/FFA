// Tests of the VM on the packages tests/fixtures.py writes. Each function there
// exercises one part of the language and has a result known in advance.
//
// Usage: test-vm <fixtures dir>
#include <cmath>
#include <cstdio>
#include <memory>

#include "script/VM.h"

using namespace ffa;

namespace {

int failures = 0, passes = 0;

void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (ok) {
        ++passes;
        return;
    }
    ++failures;
    std::printf("FAIL  %s %s\n", what.c_str(), detail.c_str());
}

void expect(const Value& got, const Value& want, const std::string& what) {
    check(got == want, what, "got " + describe(got) + ", want " + describe(want));
}

bool logged(VM& vm, const std::string& tag, const std::string& needle) {
    for (auto& [t, text] : vm.log)
        if (t == tag && text.find(needle) != std::string::npos) return true;
    return false;
}

template <class F>
std::string throws(F f) {
    try {
        f();
    } catch (const ScriptError& e) {
        return e.what();
    }
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test-vm <fixtures dir>\n");
        return 2;
    }
    std::string dir = argv[1];
    Linker lk({dir + "/Core.u", dir + "/Game.u"});
    VM vm(lk);
    vm.sink = [](const std::string&, const std::string&) {};
    vm.strict = true;

    // An iterator yields rows of its out parameters; a latent function makes
    // state code wait until its poll says it is done.
    vm.natives["object.counter"] = [](NativeCall& c) {
        for (int i = 1; i <= c.i(0); ++i) c.yield({Value::Int(i)});
        return Value();
    };
    vm.natives["object.wait"] = [](NativeCall& c) {
        auto left = std::make_shared<float>(c.f(0));
        c.self->latent = [left](float dt) { return (*left -= dt) <= 0.0f; };
        return Value();
    };

    try {
        Class* base = vm.findClass("Base");
        Class* child = vm.findClass("Child");
        Object* b = vm.spawn(base);
        Object* c = vm.spawn(child);
        auto I = Value::Int;
        auto S = [](const char* s) { return Value::Str(widen(s)); };

        // -------------------------------------------------- types and defaults
        check(child->super == base && base->super && base->super->name == Name("Object"),
              "class chain crosses packages");
        auto var = [&](Object* o, const char* name) { return o->props[size_t(o->cls->findProp(Name(name))->slot)]; };
        expect(var(c, "Count"), I(6), "child overrides a default");
        expect(var(c, "Ratio"), Value::Float(0.5f), "child inherits a float default");
        expect(var(c, "Label"), S("hi"), "string default");
        expect(var(c, "Flag"), Value::Bool(true), "bool default");
        expect(var(c, "Kind"), I(1), "enum default");
        check(var(c, "Items").isArr() && var(c, "Items").arr().size() == 3 && var(c, "Items").arr()[2] == I(3),
              "array default", describe(var(c, "Items")));
        expect(c->props[size_t(child->findProp(Name("Slots"))->slot + 1)], I(7), "static array element default");
        float x, y, z;
        vm.unvector(var(c, "Pos"), x, y, z);
        check(x == 1 && y == 2 && z == 3, "vector default, read as raw memory", describe(var(c, "Pos")));
        expect(var(b, "Name"), Value::Nm(b->name), "Object.Name is the object's name");
        check(lk.problems.empty(), "defaults decode without problems",
              lk.problems.empty() ? "" : lk.problems[0].second);

        // ----------------------------------------------------------- calls
        expect(vm.callStatic("Base", "Sum", {I(5)}), I(10), "loop with locals and jumps");
        expect(vm.call(b, "Virt"), I(1), "virtual call");
        expect(vm.call(c, "Virt"), I(11), "override calling super");
        expect(vm.callStatic("Base", "Parse", {S("?Name=Bob?Class=X"), S("Name")}), S("Bob"),
               "string natives, optional parameter left out");
        expect(vm.callStatic("Base", "Parse", {S("?Name=Bob?Class=X"), S("Class")}), S("X"), "parse last option");
        expect(vm.callStatic("Base", "Parse", {S("?Name=Bob"), S("Missing")}), S(""), "parse missing option");
        expect(vm.call(b, "SwitchTest", {I(1)}), I(10), "switch: first case");
        expect(vm.call(b, "SwitchTest", {I(2)}), I(23), "switch: fall through into the next case");
        expect(vm.call(b, "SwitchTest", {I(3)}), I(3), "switch: later case");
        expect(vm.call(b, "SwitchTest", {I(7)}), I(99), "switch: default");
        expect(vm.call(b, "UseSwap"), I(21), "out parameters copied back");
        expect(vm.call(b, "StructCopy"), Value::Float(1.0f), "struct assignment copies");
        expect(vm.call(b, "StructEqTest"), Value::Bool(true), "struct comparison");
        expect(vm.call(b, "VecTest"), Value::Float(5.0f), "vector natives");
        expect(vm.call(b, "NameTest"), Value::Bool(true), "names compare without case");
        expect(vm.call(b, "CastTest"), S("42,1.500000,True,-17"), "conversions to and from strings");
        expect(vm.call(b, "ObjCastTest"), I(11), "new, and casts between classes");
        expect(vm.call(b, "CtxArgs"), I(103), "arguments are read from the caller, members from the context");

        vm.log.clear();
        expect(vm.call(vm.spawn(base), "ArrayTest"), I(675), "dynamic arrays grow, shrink, and read out of range");
        check(logged(vm, "ScriptWarning", "out of bounds (10/6)"), "reading past a dynamic array warns");
        vm.log.clear();
        expect(vm.call(vm.spawn(base), "InsertRemoveTest"), I(9234), "dynamic array Insert and Remove");
        expect(vm.call(vm.spawn(base), "StaticArrayTest"), I(57), "static array, index clamped");
        check(logged(vm, "ScriptWarning", "out of bounds (9/4)"), "a static array index out of range warns");
        expect(vm.call(b, "SkipTest"), I(1), "&& and || evaluate their right side only when needed");
        vm.log.clear();
        expect(vm.call(b, "NoneTest"), I(0), "member of none reads as zero");
        check(logged(vm, "ScriptWarning", "Accessed None"), "Accessed None is reported");
        expect(vm.call(b, "IterTest"), I(8), "foreach with break and continue");
        expect(vm.call(b, "IterAll"), I(10), "foreach to the end");
        expect(vm.call(b, "DefaultTest"), I(553), "default., class'X'.default. and static calls");
        expect(vm.call(b, "DelegateTest"), I(17), "delegate: own body, then the assigned function");

        std::string e = throws([&] { vm.call(b, "Loop"); });
        check(e.find("runaway") != std::string::npos, "an endless loop is stopped", e);
        e = throws([&] { vm.call(b, "Rec", {I(0)}); });
        check(e.find("recursion") != std::string::npos, "endless recursion is stopped", e);

        // ---------------------------------------------------------- states
        Class* actorish = vm.findClass("Actorish");
        Object* a = vm.spawn(actorish);
        vm.log.clear();
        check(vm.gotoState(a, Name("Auto")), "GotoState Auto");
        check(a->state && a->state->name == Name("Idle"), "the auto state is entered");
        check(logged(vm, "ScriptLog", "enter Idle"), "BeginState runs");
        expect(vm.call(a, "Virt"), I(2), "a state's function overrides the class's");
        expect(vm.call(a, "GlobalTest"), I(21), "global. skips the state");
        check(vm.gotoState(a, Name("Walking")), "GotoState Walking");
        check(logged(vm, "ScriptLog", "leave Idle"), "EndState runs");
        int countSlot = actorish->findProp(Name("Count"))->slot;
        vm.processState(a, 0.0f);
        expect(a->props[size_t(countSlot)], I(6), "state code runs to the latent call");
        check(a->latent != nullptr, "state code waits on the latent action");
        vm.processState(a, 0.5f);
        expect(a->props[size_t(countSlot)], I(6), "still waiting half way");
        vm.processState(a, 0.6f);
        expect(a->props[size_t(countSlot)], I(116), "resumes after the wait, follows goto, stops");
        check(!a->code, "Stop ends state code");
        vm.gotoState(a, Name("Walking"), Name("Finish"));
        vm.processState(a, 0.0f);
        expect(a->props[size_t(countSlot)], I(216), "GotoState to a label in the same state");
    } catch (const ScriptError& ex) {
        std::printf("FAIL  uncaught: %s\n", ex.full().c_str());
        ++failures;
    } catch (const std::exception& ex) {
        std::printf("FAIL  uncaught: %s\n", ex.what());
        ++failures;
    }

    std::printf("%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
