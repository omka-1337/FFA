# Unreal Engine 2 package format, build 2226 (Shrek 2 PC)

Everything here was recovered from the retail game files, not from any source or
specification. Shrek 2 PC was built by KnowWonder on Unreal Engine 2 build 2226,
the same code drop Epic shipped publicly as the UnrealEngine2 Runtime. Package
file version is **129**, licensee version 0.

Where this document differs from published descriptions of UE2 packages, the
difference is real and was measured. Published notes mostly describe UT2003 and
UT2004, which are different builds.

## How anything here was verified

Two independent checks, both exact:

1. **End alignment.** Every export record has a known length from the export
   table. A correct parse consumes exactly that many bytes, no more and no less.
   A single wrong field width desynchronises and the record stops lining up, so
   a layout that aligns across thousands of records is almost certainly right.
2. **Size agreement.** A function's declared `ScriptSize` must equal the sum of
   the memory sizes of the tokens walked. This catches errors that end alignment
   cannot, because it checks the memory model rather than the disk layout.

The class reader was additionally confirmed against the outside world: the
recovered signatures of `Actor.Spawn`, `Actor.Trace`, `Actor.PlaySound` and
`Actor.PlayAnim` match the published UE2 API exactly.

Current state: 8599 of 8638 functions pass end alignment, 8426 pass both checks.

## Primitives

### Compact index

Signed variable length integer, used for every object reference and name index.

```
byte 0:  bit 0x80 = sign, bit 0x40 = another byte follows, bits 0x3F = value
byte n:  bit 0x80 = another byte follows,                  bits 0x7F = value
```

Bits accumulate 6 from the first byte then 7 per following byte.

### Object references

A reference is a compact index. Positive `n` means export `n - 1`, negative `n`
means import `-n - 1`, zero means none. Important: on disk a reference is 1 to 5
bytes, in memory it is a 4 byte pointer. This distinction matters for bytecode.

## File header

```
u32  tag = 0x9E2A83C1
u16  file version = 129
u16  licensee version = 0
u32  package flags
u32  name count,   u32 name offset
u32  export count, u32 export offset
u32  import count, u32 import offset
     version >= 68: 16 byte GUID, then u32 generation count and that many
     pairs of (u32 export count, u32 name count)
```

Name table entry: compact index length, that many bytes of text including a
trailing NUL, then u32 flags. Name 0 is `None` in every package seen.

Import entry: class package name index, class name index, s32 package index,
object name index.

Export entry: class reference, super reference, s32 package index, object name
index, u32 object flags, compact index serial size, and a compact index serial
offset when the size is non zero.

**The export table's package index is the owner object.** This is the reliable
way to enumerate what a class contains without parsing any record body. Its
order is serialisation order, which is *not* declaration order: for some
functions the parameters come out forward and for others reversed, so it must
not be used to build signatures.

## Record layouts

### Class, State, Struct, Function

These start directly with the field links. There is no leading tagged property
list, which is the first thing that trips up a parser written from UE1 notes.

```
index  SuperField          parent class, or the overridden parent function
index  Next                next member in the owner's declaration chain
index  ScriptText          TextBuffer export, always named "ScriptText"
index  CppText             second TextBuffer slot, zero in this build
index  Children            first member, head of the Next chain
index  FriendlyName        name index; for operators this is the symbol, "+"
index  unused              zero in every record inspected
u32    Line
u32    TextPos
u32    ScriptSize          size of the bytecode IN MEMORY, see below
bytes  bytecode
```

A function then ends with a tail:

```
u16  iNative            native index, zero for script functions
u8   OperPrecedence
u32  FunctionFlags
u16  extra              present on some functions, see the discriminator below
```

**Reading the tail.** The tail is 7 or 9 bytes and the length cannot be taken
from the flags, because reading the flags at the wrong offset returns a word
shifted by 16 bits. Every real `FunctionFlags` word carries exactly one access
specifier, so those bits are the discriminator: read a u32 at `end - 4`, and if
no access bit is set, read at `end - 6` instead.

```
FUNC_Final      0x00000001   FUNC_Defined   0x00000002   FUNC_PreOperator 0x00000010
FUNC_Net        0x00000040   FUNC_Simulated 0x00000100   FUNC_Exec        0x00000200
FUNC_Native     0x00000400   FUNC_Event     0x00000800   FUNC_Operator    0x00001000
FUNC_Static     0x00002000
access bits     0x000E0000   Public | Private | Protected
```

Note `FUNC_Defined` is *not* set on native declarations, which have no body.

### Property records

All property classes share a base and differ only in trailing references.

```
index  (three indices; the chain link Next is the THIRD one, not the second)
u16    ArrayDim
u16    ElementSize
u32    PropertyFlags
index  Category
u16    RepOffset            only when CPF_Net (0x20) is set
index  ...                  trailing references, see table
```

| Property class | trailing references |
|---|---|
| IntProperty, FloatProperty, BoolProperty, StrProperty, NameProperty | none |
| ObjectProperty, StructProperty, ByteProperty, ArrayProperty, DelegateProperty | 1 |
| ClassProperty | 2 (property class, then meta class) |

Verified at 100 percent across all 21274 property records in the game.

```
CPF_Edit     0x00000001   CPF_Const     0x00000002   CPF_Optional 0x00000010
CPF_Net      0x00000020   CPF_Parm      0x00000080   CPF_OutParm  0x00000100
CPF_Return   0x00000400   CPF_Coerce    0x00000800   CPF_Native   0x00001000
CPF_Transient 0x00002000  CPF_Config    0x00004000   CPF_Localized 0x00008000
```

Member order for signatures must come from `Children` then the `Next` chain.
Remember that `Next` sits at a different index position for properties than for
struct-like records.

## Bytecode

### ScriptSize is a memory size

This is the single most important thing about the bytecode. `ScriptSize` counts
the bytes the script occupies in the engine's in-memory array, where references
and names are 4 bytes each. On disk they are compact indices, usually 2 bytes.
A script therefore cannot be skipped by its declared size; it must be walked
token by token. Jump offsets are memory offsets into the same array, so a
decompiler has to track both offsets per token to resolve labels.

### Native calls

```
opcode 0x70 .. 0xFF   native index is the opcode itself
opcode 0x60 .. 0x6F   extended: index = ((opcode - 0x60) << 8) + next byte
```

### Opcode table

Operand kinds: `expr` nested expression, `obj` object reference, `name` name
index, `parms` expressions terminated by EndFunctionParms (0x16).

```
0x00 LocalVariable obj      0x01 InstanceVariable obj   0x02 DefaultVariable obj
0x03 StateVariable obj      0x04 Return expr            0x05 Switch u8 expr
0x06 Jump u16               0x07 JumpIfNot u16 expr     0x08 Stop
0x09 Assert u16 expr        0x0A Case (u16, expr unless 0xFFFF)
0x0B Nothing                0x0C LabelTable (name,u32 pairs until None)
0x0D GotoLabel expr         0x0E EatString expr         0x0F Let expr expr
0x10 DynArrayElement e e    0x11 New e e e e            0x12 ClassContext e u16 u8 e
0x13 Metacast obj expr      0x14 LetBool expr expr      0x15 EndParmValue
0x16 EndFunctionParms       0x17 Self                   0x18 Skip u16 expr
0x19 Context expr u16 u8 e  0x1A ArrayElement e e       0x1B VirtualFunction name parms
0x1C FinalFunction obj parms 0x1D IntConst u32          0x1E FloatConst f32
0x1F StringConst asciiz     0x20 ObjectConst obj        0x21 NameConst name
0x22 RotationConst 3x u32   0x23 VectorConst 3x f32     0x24 ByteConst u8
0x25 IntZero                0x26 IntOne                 0x27 True
0x28 False                  0x29 NativeParm obj         0x2A NoObject
0x2B NoDelegate             0x2C IntConstByte u8        0x2D BoolVariable expr
0x2E DynamicCast obj expr   0x2F Iterator expr u16      0x30 IteratorPop
0x31 IteratorNext           0x32 StructCmpEq obj e e    0x33 StructCmpNe obj e e
0x34 UnicodeStringConst     0x35 InstanceDelegate name  0x36 StructMember obj expr
0x37 DynArrayLength expr    0x38 GlobalFunction name parms
0x39 .. 0x5F                primitive conversions, one expression each
```

The conversion block 0x39 to 0x5F is a single run of cast tokens, each taking
exactly one expression. Not knowing this is what stalls a first parser: those
opcodes look like unrelated unknowns.

## Default properties, and the tagged value format

A class record ends with its `defaultproperties`: a tagged list of name and
value pairs terminated by the name `None`. The same format carries actor
properties inside `.unr` maps, so this is also the way into level data.

```
index  property name            terminator when it is None
u8     info
         bits 0..3   type code
         bits 4..6   size: 0->1, 1->2, 2->4, 3->12, 4->16, 5->u8, 6->u16, 7->u32
         bit  7      array flag; for BoolProperty this bit IS the value
index  struct name               only when the type is StructProperty
       array index               only when the array flag is set and not a bool:
                                 one byte, unless bit 0x80 is set, then a two
                                 byte form, or a four byte form when 0x40 is
                                 also set
bytes  value                     `size` bytes, absent for BoolProperty
```

Type codes: 1 Byte, 2 Int, 3 Bool, 4 Float, 5 Object, 6 Name, 7 String,
8 Class, 9 Array, 10 Struct, 11 Vector, 12 Rotator, 13 Str, 14 Map,
15 FixedArray.

Structs named Vector, Plane, Rotator, Color, Range and Scale hold plain binary
rather than a nested tagged list. Rotations are in UE units, where 65536 is a
full turn.

### Finding the block

The fields between the struct header and the defaults are not decoded yet: state
masks, class flags, a GUID, and variable length dependency and import arrays sit
in between. So the block is located rather than seeked to. Scan for an offset
from which a tagged parse lands exactly on the end of the record, then choose
the candidate whose property names all resolve to real properties of the class
or one of its ancestors.

**The semantic check is not optional.** Several offsets per record usually parse
cleanly to the end, and the earliest of them is the right answer only about a
third of the time. Picking the first match would produce quietly wrong output.
Ancestors must be followed across packages, which is what turns a partial result
into a complete one: restricted to a single package the check validated 162 of
220 classes, and following imports it validates all of them.

Verified on every class of two package versions: 2002 of 2002 in Shrek 2 PC
(version 129) and 607 of 607 in the UnrealEngine2 Runtime (version 126), with
every property name resolving.

## A warning about parsers

A desynchronised parse will happily read garbage as opcodes and walk off the end
of the record into the rest of the file. If the parser builds nodes while it
does so, it allocates without bound and can take a machine down. Always carry
the record end as a hard limit, raise as soon as it is passed, and keep a node
budget per record. Run bulk passes under an external memory cap.

## What is known to be incomplete

- 39 of 8638 functions still fail end alignment, 33 of them in GUI.u.
- 212 functions align but disagree on size, so one token's memory size is still
  wrong somewhere, most likely in a rarely used operand kind.
- The seventh index of struct-like records is zero everywhere seen, so its
  meaning is unknown.
- The fields between a class's struct header and its defaults block are still
  unknown: state masks, class flags, GUID, dependency and import arrays. The
  block is found by scanning instead.
