// Runtime values, the types script declares, and objects.
//
// UnrealScript has fixed width numbers, case blind names, UTF-16 strings and
// value semantics for structs and dynamic arrays. Value holds each as its own
// alternative, and copying a Value copies a struct or array deeply, which is
// exactly the language's assignment. Byte and int are both int32_t here; the
// declared type of the variable a value is stored into masks it.
//
// Variables live in slots: an object's in a vector laid out by its class, a
// struct's in a vector laid out by its struct type, a call's parameters and
// locals in its frame. A static array takes consecutive slots, as it takes
// consecutive memory in the engine.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "core/Name.h"
#include "script/Bytecode.h"

namespace ffa {

class Linker;
class Class;
struct Object;
struct StructType;
struct Prop;
struct Function;
struct State;
struct EnumType;
struct Frame;

using String = std::u16string;
std::string utf8(const String& s);
String widen(std::string_view latin1);
String fromUtf8(std::string_view s);

// FunctionFlags. Native and the access bits are established in the format
// document; the others are the engine's published values, consistent with them.
enum : uint32_t {
    FUNC_Final = 0x00000001, FUNC_Defined = 0x00000002, FUNC_Iterator = 0x00000004,
    FUNC_Latent = 0x00000008, FUNC_PreOperator = 0x00000010, FUNC_Singular = 0x00000020,
    FUNC_Net = 0x00000040, FUNC_Simulated = 0x00000100, FUNC_Exec = 0x00000200,
    FUNC_Native = 0x00000400, FUNC_Event = 0x00000800, FUNC_Operator = 0x00001000,
    FUNC_Static = 0x00002000, FUNC_Delegate = 0x00100000,
};

// PropertyFlags, established in the format document.
enum : uint32_t {
    CPF_Edit = 0x00000001, CPF_Const = 0x00000002, CPF_OptionalParm = 0x00000010,
    CPF_Net = 0x00000020, CPF_Parm = 0x00000080, CPF_OutParm = 0x00000100,
    CPF_ReturnParm = 0x00000400, CPF_CoerceParm = 0x00000800, CPF_Native = 0x00001000,
    CPF_Transient = 0x00002000, CPF_Config = 0x00004000, CPF_Localized = 0x00008000,
};

enum : uint32_t { STATE_Auto = 0x00000002 };

struct Value;
using Array = std::vector<Value>;

struct StructVal {
    const StructType* type = nullptr;
    std::vector<Value> f;
    bool operator==(const StructVal& o) const;
};

struct Delegate {
    Object* obj = nullptr;
    Name func;
    bool operator==(const Delegate& o) const { return obj == o.obj && func == o.func; }
};

struct Value {
    std::variant<std::monostate, int32_t, float, bool, Name, String, Object*, StructVal, Array,
                 Delegate>
        v;

    Value() = default;
    static Value Int(int32_t x) { Value r; r.v = x; return r; }
    static Value Float(float x) { Value r; r.v = x; return r; }
    static Value Bool(bool x) { Value r; r.v = x; return r; }
    static Value Nm(Name x) { Value r; r.v = x; return r; }
    static Value Str(String x) { Value r; r.v = std::move(x); return r; }
    static Value Obj(Object* x) { Value r; r.v = x; return r; }
    static Value Struct(StructVal x) { Value r; r.v = std::move(x); return r; }
    static Value Arr(Array x) { Value r; r.v = std::move(x); return r; }
    static Value Dlg(Delegate x) { Value r; r.v = x; return r; }

    bool empty() const { return v.index() == 0; }
    bool isInt() const { return std::holds_alternative<int32_t>(v); }
    bool isFloat() const { return std::holds_alternative<float>(v); }
    bool isBool() const { return std::holds_alternative<bool>(v); }
    bool isName() const { return std::holds_alternative<Name>(v); }
    bool isStr() const { return std::holds_alternative<String>(v); }
    bool isObj() const { return std::holds_alternative<Object*>(v); }
    bool isStruct() const { return std::holds_alternative<StructVal>(v); }
    bool isArr() const { return std::holds_alternative<Array>(v); }
    bool isDlg() const { return std::holds_alternative<Delegate>(v); }

    // Lenient readers: a value of a neighbouring type converts as C would,
    // and an empty value reads as the type's zero.
    int32_t i() const;
    float f() const;
    bool b() const;
    Name n() const;
    String s() const;
    Object* o() const;
    Delegate d() const;
    StructVal& st() { return std::get<StructVal>(v); }
    const StructVal& st() const { return std::get<StructVal>(v); }
    Array& arr() { return std::get<Array>(v); }
    const Array& arr() const { return std::get<Array>(v); }

    bool operator==(const Value& o) const { return v == o.v; }
    bool operator!=(const Value& o) const { return !(v == o.v); }
};

std::string describe(const Value& v);       // for logs and tests

enum class Kind : uint8_t {
    Byte, Int, Bool, Float, Name, Str, Object, Class, Struct, Array, Delegate, Pointer,
    Map, FixedArray, Unknown
};
const char* kindName(Kind k);

// A declared variable: class member, struct field, parameter or local. The
// types it refers to are resolved on first use, since they may live in a
// package whose classes are not loaded yet.
struct Prop {
    Name name;
    Kind kind = Kind::Unknown;
    int dim = 1;
    uint32_t flags = 0;
    int slot = -1;              // in the owner's layout

    Linker* linker = nullptr;
    int pkg = -1;
    int32_t ref = 0, ref2 = 0;

    StructType* structType() const;
    Prop* inner() const;        // element of a dynamic array
    EnumType* enumType() const;
    Class* objClass() const;    // class of an object, metaclass of a class

    bool isOut() const { return (flags & CPF_OutParm) && !(flags & CPF_ReturnParm); }

    Value zero() const;         // one element's default
    Value coerce(Value v) const;  // one element made fit to store

private:
    mutable bool resolved_ = false;
    mutable StructType* struct_ = nullptr;
    mutable Prop* inner_ = nullptr;
    mutable EnumType* enum_ = nullptr;
    mutable Class* class_ = nullptr;
    void resolve() const;
};

struct StructType {
    Name name;
    StructType* super = nullptr;
    std::vector<Prop*> own;

    const std::vector<Prop*>& layout() const;
    int slots() const { layout(); return slots_; }
    Prop* field(Name n) const;
    Value make() const;
    bool isChildOf(const StructType* o) const;

private:
    mutable bool built_ = false;
    mutable std::vector<Prop*> layout_;
    mutable int slots_ = 0;
};

struct Function {
    Name name;
    int pkg = -1, idx = 0;
    uint32_t flags = 0;
    uint16_t native = 0;
    Class* cls = nullptr;       // the class it is declared in
    State* state = nullptr;     // the state, when declared in one
    std::vector<Prop*> params;
    Prop* ret = nullptr;
    std::vector<Prop*> locals;
    int slots = 0;
    void (*impl)() = nullptr;   // the native's implementation, once found

    // compiled on first call
    bool compiled = false;
    bool parseOk = true;
    std::vector<Ins> code;
    std::unordered_map<uint32_t, int> index;    // memory offset -> statement

    bool isNative() const { return flags & FUNC_Native; }
    std::string qualname() const;
    std::string nativeKey() const;              // "class.function", lowercase
};

struct State {
    Name name;
    int pkg = -1, idx = 0;
    uint32_t flags = 0;
    Class* cls = nullptr;
    State* super = nullptr;
    std::unordered_map<Name, Function*> funcs;

    bool compiled = false;
    bool parseOk = true;
    std::vector<Ins> code;
    std::unordered_map<uint32_t, int> index;
    std::unordered_map<Name, uint32_t> labels;  // label -> memory offset

    std::string qualname() const;
};

struct Object {
    Class* cls = nullptr;
    Name name;
    Object* outer = nullptr;
    std::vector<Value> props;

    State* state = nullptr;             // none: the object runs as its class
    std::shared_ptr<Frame> code;        // the frame running state code
    std::function<bool(float)> latent;  // the latent action state code waits on
    std::unordered_set<Name> disabled;  // probe functions turned off
    bool deleted = false;
    bool singular = false;
    uint32_t stateChanges = 0;

    Object();
    virtual ~Object();
    std::string path() const;
    bool isA(const Class* c) const;
    virtual bool isClass() const { return false; }
};

struct EnumType : Object {
    std::vector<Name> values;
};

class Class : public Object {
public:
    int pkg = -1, idx = 0;
    Class* super = nullptr;
    std::vector<Prop*> own;
    std::unordered_map<Name, Function*> funcs;
    std::unordered_map<Name, State*> states;
    std::unordered_map<Name, StructType*> structs;
    Linker* linker = nullptr;
    Object* defaultObject = nullptr;

    bool isClass() const override { return true; }
    const std::vector<Prop*>& layout() const;
    int slots() const { layout(); return slots_; }
    Prop* findProp(Name n) const;
    bool hasProp(const Prop* p) const;   // this class's or an ancestor's, shadowed or not
    Function* findFunction(Name n) const;
    State* findState(Name n) const;
    State* autoState() const;
    bool isChildOf(const Class* c) const;
    Object* defaults();

private:
    mutable bool built_ = false;
    mutable std::vector<Prop*> layout_;
    mutable int slots_ = 0;
    mutable std::unordered_map<Name, Prop*> propMap_;
    mutable std::unordered_set<const Prop*> propSet_;
    mutable std::unordered_map<Name, Function*> fcache_;
    mutable std::unordered_map<Name, State*> scache_;
};

struct Frame {
    Function* fn = nullptr;
    State* st = nullptr;
    Object* self = nullptr;
    std::vector<Value> locals;
    const std::vector<Ins>* code = nullptr;
    size_t pc = 0;
    bool done = false;
    Value result;

    std::string where(const Ins* at = nullptr) const;
};

}  // namespace ffa
