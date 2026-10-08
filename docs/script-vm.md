# How UnrealScript executes

What the engine's VM does with the bytecode that
[package-format.md](package-format.md) describes. The format document records
what was measured. This one records behaviour, and behaviour cannot be read off
a file the way a layout can, so every point below says where it comes from:

- **Measured**: established from the game files, with the check that proves it.
- **Corpus check**: taken from knowledge of the engine, with a test in
  `ffa-script check` that the corpus can confirm or refute.
- **Language**: how UnrealScript is documented to behave, tested only by the
  fixture packages in `tests` until a game's own script shows otherwise.

A point that is not measured is not yet established, however confident the
source.

## Loading

**Measured.** Classes, functions, states, structs, properties and enums are
the records the format document gives. Members come in declaration order from
Children and the Next chain, then anything the export table says the record
owns that the chain does not reach. An import is resolved by path: its package
name and chain of outers name the export with the same path in that package.

**Measured.** The defaults block is located by scanning, as the Python tools'
`udefaults.py` does: the offset that parses exactly to the end of the record and whose names
are all variables of the class. A class's default object is its parent's, with
its own variables at their type's zero, and then its own block applied. Struct
values of Vector, Rotator and Color are raw memory; the engine reads them field
by field in declaration order and requires the size to come out exact. Every
other struct, Plane, Range and Scale included, is a tagged list of its own.

**Measured.** A state's record ends in ProbeMask (8 bytes), IgnoreMask (8),
LabelTableOffset (2) and StateFlags (4), 22 bytes after the bytecode in all 968
states of Shrek 2. LabelTableOffset points at the label table's entries, one
byte past its 0x0C token, in all 752 states that have one, and is 0xFFFF in the
216 that do not. StateFlags 0x2 marks the auto state. `check` reports both.

## Compiling

Before a function or state first runs, its bytecode is parsed and every
reference a token carries is resolved once. Jump targets are memory offsets;
each is mapped to the statement starting there, and a target that falls inside
a statement is refused. For a correct parse every target is a statement start,
so this is also a check on the bytecode reader, and `check` runs it over every
function and state.

A state's bytecode has no tail that marks its end, so the walk stops when the
memory sizes reach ScriptSize.

## Values

**Language.** int wraps at 32 bits and float is single precision. byte is held
as an int and masked when stored. Names compare without regard to case. Strings
are UTF-16, as TCHAR is in the engine, so `Len`, `Mid` and `Asc` count the
same characters. Structs and dynamic arrays are copied when assigned and when
passed by value.

## Context

**Language.** An expression is evaluated against two objects: the frame's own,
and the context `A.B` sets up. Only Context and ClassContext change the
context, and only member access passes it to its base. Function arguments and
array indices are evaluated against the frame's object, so `A.Foo(B)` reads B
from the caller and runs Foo on A.

A context that is none logs `Accessed None` and the expression gives its
type's zero. Writes through it are dropped. ClassContext evaluates its member
against the class's default object, which is how `class'X'.default.Y` and
`class'X'.static.F()` work.

## Calls

**Language.** A virtual call looks in the current state and the states it
extends, then in the class chain. `global.` skips the states. A final call goes
to the function the token names, which is how `super.` is compiled.

Out parameters are copied in at the call and back when it returns. An optional
parameter left out is the Nothing token, or missing before EndFunctionParms,
and is that type's zero; a native can tell it was left out. The right side of
`&&` and `||` is marked with a Skip token and evaluated only if needed.

A delegate is a function whose call goes to the function assigned to the
hidden variable `__Name__Delegate`, when one is assigned, and to its own body
otherwise.

`Return` followed by Nothing returns the current value of the return variable.

## Statements

**Language.** `switch` compares its value with each Case in turn. A case that
does not match sends control to the next case's offset; a match or the default
(offset 0xFFFF) continues into the following statements. A Case reached by
falling through is not tested.

`foreach` calls a native iterator, which yields one row of out parameter values
per element. For each element the body runs up to IteratorNext. IteratorPop in
the body, which `break` and `return` emit, leaves the loop. When the elements
run out, control continues past the IteratorPop at the loop's end offset.

`Let` evaluates its value before it locates the variable. The engine does it
the other way round, writing straight into the address it found, which can go
wrong when the value's evaluation reallocates the array the address points
into. The difference shows only when both sides have side effects.

## Arrays

**Language.** A static array index out of range is clamped, with a warning.
Writing past the end of a dynamic array grows it (script appends with
`A[A.Length] = X`). Reading past the end warns and gives zero. Assigning
`Length` resizes.

## States

**Language.** GotoState calls EndState in the old state and BeginState in the
new one, and either may change state again, in which case the change in
progress stops there. Then state code resumes at the label, Begin by default,
found through the state and the states it extends. GotoState to the current
state changes nothing but the label. In no state, an object runs as its class,
and GetStateName gives the class's name.

State code runs a statement at a time until it calls a latent function, reaches
Stop, or runs out. A latent function leaves an action the object waits on;
each tick polls it, and when it is done, state code carries on. A tick that
changes state more than four times stops there.

## Conversions

**Corpus check.** The tokens 0x39 to 0x59 are, in order: RotatorToVector,
ByteToInt, ByteToBool, ByteToFloat, IntToByte, IntToBool, IntToFloat,
BoolToByte, BoolToInt, BoolToFloat, FloatToByte, FloatToInt, FloatToBool, an
unused 0x46, ObjectToBool, NameToBool, StringToByte, StringToInt,
StringToBool, StringToFloat, StringToVector, StringToRotator, VectorToBool,
VectorToRotator, RotatorToBool, ByteToString, IntToString, BoolToString,
FloatToString, ObjectToString, NameToString, VectorToString,
RotatorToString. `check` tallies the declared type of each token's operands.
0x46 and anything past 0x59 are refused.

**Language.** Floats print with `%f`, bools as True and False, objects as their
path. Strings convert to numbers the way `atoi` and `atof` read them, from the
start and as far as they make sense.

## Limits

The engine stops a call that runs more than a million statements, and script
recursion deeper than 250 calls, rather than hang.

## Not done

Probe masks and `ignores`; singular functions are honoured but not tested;
replication; config and localisation; garbage collection; and every native
outside Core.
