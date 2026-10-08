// The UnrealScript virtual machine.
//
// Bytecode is executed as the token trees the parser builds, after a compile
// pass that resolves every reference a token carries once: a variable
// reference to its Prop, a function reference to its Function, a name to its
// Name. Jump targets are memory offsets, and each must land on the start of a
// statement; the compile pass maps them to statement indices and refuses one
// that does not, which is itself a check on the parse.
//
// Like the engine's interpreter, every evaluation knows two objects: the
// frame's own object, and the context an expression such as `A.B` sets up.
// Only Context and ClassContext change the context, and only member access
// passes it down to its base. Function arguments, array indices and the rest
// are evaluated against the frame's object, which is what makes `A.Foo(B)`
// read B from the caller.
#pragma once

#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "script/Linker.h"
#include "script/Types.h"

namespace ffa {

class VM;

struct ScriptError : std::runtime_error {
    ScriptError(const std::string& msg, std::vector<std::string> trace = {})
        : std::runtime_error(msg), trace(std::move(trace)) {}
    std::vector<std::string> trace;
    std::string full() const;
};

struct NativeMissing : ScriptError {
    using ScriptError::ScriptError;
};

// A variable's location, found again each time it is used rather than held as
// a pointer, so that a dynamic array growing in between cannot leave it
// dangling. The root is a slot in an object, a frame or a temporary, which do
// not move; the steps index into struct fields and dynamic array elements.
struct LRef {
    struct Step {
        bool element;           // dynamic array element, or else struct field
        int32_t i;
    };
    Value* root = nullptr;
    std::shared_ptr<Value> temp;    // when the location is not a variable
    std::vector<Step> steps;
    const Prop* prop = nullptr;     // declared type of what is stored here
    bool length = false;            // a dynamic array's Length

    Value* resolve() const;
    Value get() const;
    void set(Value v) const;
    static LRef temporary(Value v, const Prop* p = nullptr);
};

// What a native implementation sees of its call.
class NativeCall {
public:
    struct Arg {
        Value v;
        LRef ref;
        bool isRef = false;
        bool omitted = false;
        const Ins* skip = nullptr;  // evaluated only on demand: && and ||
    };

    NativeCall(VM& vm, Object* self, Function* fn, Frame* caller)
        : vm(vm), self(self), fn(fn), caller(caller) {}

    VM& vm;
    Object* self;
    Function* fn;
    Frame* caller;
    std::vector<Arg> args;
    std::vector<std::vector<Value>> rows;   // what an iterator yields

    bool has(size_t k) const { return k < args.size() && !args[k].omitted; }
    Value get(size_t k);                     // evaluates a skipped argument
    int32_t i(size_t k, int32_t def = 0) { return has(k) ? get(k).i() : def; }
    float f(size_t k, float def = 0) { return has(k) ? get(k).f() : def; }
    bool b(size_t k, bool def = false) { return has(k) ? get(k).b() : def; }
    Name n(size_t k, Name def = Name()) { return has(k) ? get(k).n() : def; }
    String s(size_t k, String def = String()) { return has(k) ? get(k).s() : def; }
    Object* o(size_t k) { return has(k) ? get(k).o() : nullptr; }
    void out(size_t k, Value v);            // write an out parameter
    void yield(std::vector<Value> outs) { rows.push_back(std::move(outs)); }
};

using NativeFn = Value (*)(NativeCall&);

// Conversions between script types, as the engine's casts make them.
int32_t toInt(float f);                 // truncation; out of range is INT32_MIN
int32_t parseInt(const String& s);      // appAtoi
float parseFloat(const String& s);      // appAtof
String formatInt(int32_t i);
String formatFloat(float f);            // %f

class VM {
public:
    explicit VM(Linker& linker);

    Linker& linker;
    std::unordered_map<std::string, NativeFn> natives;   // "class.function"
    bool strict = false;            // a missing native is an error, not a warning
    size_t runaway = 1000000;       // statements one call may run
    size_t recursion = 250;         // script call depth
    std::mt19937 rng{0};

    std::function<void(const std::string& tag, const std::string& text)> sink;
    std::vector<std::pair<std::string, std::string>> log;
    std::unordered_set<std::string> missing;    // natives called but not implemented
    std::unordered_map<std::string, size_t> missingCalls;   // and how often each was
    std::vector<std::unique_ptr<Object>> objects;
    void* host = nullptr;           // what the engine's natives act on: a World

    void write(const std::string& tag, const std::string& text);
    void warn(const Frame* f, const Ins* at, const std::string& msg);

    // objects
    Object* spawn(Class* c, Name name = Name(), Object* outer = nullptr);
    Class* findClass(std::string_view name);
    StructType* vectorType() const { return vector_; }
    StructType* rotatorType() const { return rotator_; }
    Value vector(float x, float y, float z) const;
    Value rotator(int32_t p, int32_t y, int32_t r) const;
    void unvector(const Value& v, float& x, float& y, float& z) const;
    void unrotator(const Value& v, int32_t& p, int32_t& y, int32_t& r) const;
    String toString(const Value& v);    // what the to-string casts give
    Value* slot(Object* o, const Prop* p, const Frame* f = nullptr, const Ins* at = nullptr);

    // calls from the engine's side, the way native code sends events
    Value call(Object* self, std::string_view name, std::vector<Value> args = {});
    Value callStatic(std::string_view cls, std::string_view name, std::vector<Value> args = {});
    Value callFunction(Function* fn, Object* self, std::vector<Value> args);
    Value event(Object* self, std::string_view name, std::vector<Value> args = {});
    // An event whose out parameters the engine reads back: args holds the
    // values going in, and after the call what the function left in them.
    Value eventOut(Object* self, std::string_view name, std::vector<Value>& args);
    Function* findVirtual(Object* self, Name name);

    // states
    bool gotoState(Object* o, Name state, Name label = Name());
    bool gotoLabel(Object* o, Name label);
    void processState(Object* o, float dt = 0.0f);

    // compile
    void compile(Function* fn);
    void compile(State* st);

    std::vector<std::string> trace() const;
    ScriptError error(const std::string& msg) const;

private:
    friend class NativeCall;
    template <class Owner>
    void compileCode(Owner* owner, int pkg, int idx);
    void resolve(Ins& n, int pkg);

    Value ev(const Ins& n, Frame& f, Object* ctx);
    LRef lv(const Ins& n, Frame& f, Object* ctx, bool write);
    void stmt(const Ins& n, Frame& f);
    void run(Frame& f);
    Value execute(Frame& callee);
    Value invoke(Function* fn, Object* self, const std::vector<Ins>& args, Frame& f);
    Value callNative(Function* fn, Object* self, const std::vector<Ins>& args, Frame& f);
    void prepareNative(NativeCall& c, const std::vector<Ins>& args, Frame& f);
    Value runNative(NativeCall& c);
    NativeFn nativeOf(Function* fn);
    Value noNative(Function* fn);
    std::pair<Function*, Object*> bindDelegate(Function* fn, Object* self);
    void iterate(const Ins& n, Frame& f);
    void doSwitch(const Ins& n, Frame& f);
    Value zeroOf(const Ins& n);
    Value cast(const Ins& n, Value v);
    Function* anyFunction(Name name);

    StructType* vector_ = nullptr;
    StructType* rotator_ = nullptr;
    int vslot_[3] = {0, 1, 2};
    int rslot_[3] = {0, 1, 2};
    std::vector<std::unique_ptr<StructType>> builtin_;
    std::vector<std::unique_ptr<Prop>> builtinProps_;
    std::vector<Frame*> stack_;
    std::unordered_map<Name, Function*> anyFn_;
    bool anyFnBuilt_ = false;
};

void registerCoreNatives(VM& vm);
// Rows are the X, Y and Z axes of a rotation in the engine's units.
void rotationAxes(int32_t pitch, int32_t yaw, int32_t roll, float out[3][3]);

}  // namespace ffa
