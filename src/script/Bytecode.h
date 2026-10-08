// UnrealScript bytecode as a tree of tokens, the port of tools/uscript.py.
//
// ScriptSize counts the bytecode as the engine holds it in memory, where object
// references and names are four bytes, while on disk they are compact indices.
// So the script is walked token by token, keeping both offsets: jumps target
// memory offsets, and the memory sizes summing to ScriptSize is the check that
// the walk is right, alongside landing exactly on the end of the record.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Package.h"

namespace ffa {

enum class Op : uint8_t {
    LocalVariable = 0x00, InstanceVariable = 0x01, DefaultVariable = 0x02,
    StateVariable = 0x03, Return = 0x04, Switch = 0x05, Jump = 0x06,
    JumpIfNot = 0x07, Stop = 0x08, Assert = 0x09, Case = 0x0A, Nothing = 0x0B,
    LabelTable = 0x0C, GotoLabel = 0x0D, EatString = 0x0E, Let = 0x0F,
    DynArrayElement = 0x10, New = 0x11, ClassContext = 0x12, Metacast = 0x13,
    LetBool = 0x14, EndParmValue = 0x15, EndFunctionParms = 0x16, Self = 0x17,
    Skip = 0x18, Context = 0x19, ArrayElement = 0x1A, VirtualFunction = 0x1B,
    FinalFunction = 0x1C, IntConst = 0x1D, FloatConst = 0x1E, StringConst = 0x1F,
    ObjectConst = 0x20, NameConst = 0x21, RotationConst = 0x22, VectorConst = 0x23,
    ByteConst = 0x24, IntZero = 0x25, IntOne = 0x26, True = 0x27, False = 0x28,
    NativeParm = 0x29, NoObject = 0x2A, NoDelegate = 0x2B, IntConstByte = 0x2C,
    BoolVariable = 0x2D, DynamicCast = 0x2E, Iterator = 0x2F, IteratorPop = 0x30,
    IteratorNext = 0x31, StructCmpEq = 0x32, StructCmpNe = 0x33,
    UnicodeStringConst = 0x34, InstanceDelegate = 0x35, StructMember = 0x36,
    DynArrayLength = 0x37, GlobalFunction = 0x38,
    DynArrayInsert = 0x40, DynArrayRemove = 0x41, DelegateFunction = 0x43,
    DelegateProperty = 0x44, LetDelegate = 0x45,
    Cast = 0x39,            // 0x39 .. 0x5F, the conversion in `code`
    NativeCall = 0x60,      // 0x60 .. 0xFF, the index in `native`
};

const char* opName(Op op);

struct Prop;
struct Function;
struct StructType;
struct Object;
class Class;

// One token. The raw operands are what the bytecode holds; the resolved ones
// are filled in by the VM's compile pass and are what execution uses.
struct Ins {
    Op op = Op::Nothing;
    uint8_t code = 0;           // the opcode byte
    uint16_t native = 0;        // native index of a NativeCall
    uint32_t mem = 0;           // offset in the in-memory script
    uint32_t msize = 0;         // size in the in-memory script
    std::vector<Ins> kids;
    std::vector<int64_t> raw;   // object refs, name indices, integers, label pairs
    float fv[3] = {0, 0, 0};
    std::u16string str;         // string constants, UTF-16 as the engine holds them

    // resolved
    Prop* prop = nullptr;
    Function* fn = nullptr;
    Object* obj = nullptr;
    StructType* st = nullptr;
    Name name;
    int target = -1;            // statement index a jump goes to
};

struct ParsedCode {
    std::vector<Ins> stmts;
    uint32_t declared = 0;      // ScriptSize
    uint32_t mem = 0;           // memory size the walk found
    size_t end = 0;             // disk offset the walk stopped at
    bool aligned = true;        // landed exactly on the record tail
    bool sized = true;          // memory sizes agree with ScriptSize
};

// FunctionFlags and the length of the tail they end. The tail is iNative u16,
// OperPrecedence u8, FunctionFlags u32 and sometimes a further u16; every real
// flags word carries exactly one access bit, which says which length applies.
struct FunctionTail {
    uint32_t flags = 0;
    int length = 7;
    uint16_t native = 0;
    uint8_t precedence = 0;
};
FunctionTail readTail(const Package& p, const Export& e);

class BytecodeParser {
public:
    explicit BytecodeParser(const Package& p) : p_(p) {}

    // A function's bytecode, ending where its tail begins.
    ParsedCode function(const Export& e);
    // Any struct-like record's bytecode, bounded by ScriptSize alone. States
    // and classes carry fields after it whose layout is not established.
    ParsedCode structCode(const Export& e);

    static const int kMaxNodes = 20000;

private:
    Ins token(Reader& r, uint32_t mem);
    uint32_t parms(Reader& r, uint32_t mem, std::vector<Ins>& out);
    Reader header(const Export& e, uint32_t& declared);

    const Package& p_;
    int budget_ = 0;
};

}  // namespace ffa
