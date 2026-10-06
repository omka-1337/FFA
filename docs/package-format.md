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
rather than a nested tagged list. A byte value whose property refers to an enum
is rendered by name, which needs the Enum record:

```
index  three leading indices, as for a property record
index  value count
index  that many name indices
```

Verified on every enum of both corpora, 157 in Shrek 2 and 107 in the Runtime,
and the recovered EPhysics matches the documented UE2 enum, with one value
added by KnowWonder. Rotations are in UE units, where 65536 is a
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

## Levels

A `.unr` map is the same container, same version, same tables. The difference is
what the exports hold: object instances rather than classes and functions. An
instance is the same tagged property block, with one addition in front when the
object carries RF_HasStack (0x02000000), the execution state frame:

```
index  Node            the state's UStruct
index  StateNode
u64    ProbeMask
u32    LatentAction
index  Offset          only when Node is non zero
```

Skip that and every actor parses exactly to the end of its record. Measured over
all 29 Shrek 2 maps: 36111 objects parse, 16406 do not, and the ones that do not
are exactly the engine classes with native C++ serialisation behind their
properties, headed by StaticMeshInstance, Model, Polys and TerrainSector. Model
and Polys are the BSP, so level geometry remains a separate decoding problem,
but actor placement, lights, triggers, path nodes and their properties are all
readable.

## Static meshes

A StaticMesh record is a tagged property block followed by a native payload:

```
FBox      6 floats then a u8 valid flag        bounding box
FSphere   4 floats, centre and radius          no valid flag
index     section count
sections  14 bytes each: i32 then five u16
FBox      25 bytes, the render bounding box
stream    vertices:  24 bytes each, position and normal, then u32 revision
stream    colours:    4 bytes each, then u32 revision
stream    alpha:      4 bytes each, then u32 revision
index     number of UV streams
stream    each UV:    8 bytes each, two floats, then u32 revision
u32       one further word after the UV streams
stream    index buffer:      u16 each, then u32 revision
stream    wireframe buffer:  u16 each, then u32 revision
...       raw triangles and collision data, not decoded
```

The trap is the byte after the bounding sphere. By analogy with the box it looks
like the sphere's valid flag, but it is the section count, and reading it as a
flag puts every later field one byte out. The section count predicting exactly
where the next bounding box begins is what confirmed it: 413 of 413 meshes.

Confidence here is lower than for the script formats, and worth stating. The
tail is undecoded, so a parse cannot be checked by landing exactly on the end of
the record. What is checked instead is consistency: the index buffer length is a
multiple of three, every index is inside the vertex array, and the walk stays
within the record. All 835 static meshes of Shrek 2 pass, for 122011 vertices
and 102414 triangles. Raising confidence further means comparing against an
independent implementation such as UE Viewer, which is MIT licensed and can
export the same meshes.

## BSP, the Model record

A level's brush geometry lives in Model records: a tagged property block, the
same UPrimitive prefix as a static mesh, then the BSP arrays.

```
FBox      6 floats and a u8 valid flag
FSphere   4 floats
index     vector count, then that many FVector   plane normals
index     point count,  then that many FVector   brush corners
index     node count,   then that many FBspNode
index     surf count,   then that many FBspSurf
index     vert count,   then that many FVert, two compact indices each:
          pBase into Points, and iSide
...       zones, lightmaps, bounds and leaves, not decoded yet
```

A surface is also variable length:

```
index     material, usually an import, resolving to a texture name
u32       poly flags
index x6  pBase into Points, vNormal, vTextureU, vTextureV into Vectors,
          iLightMap, iBrushPoly
FPlane    16 bytes
f32       pan
```

A node is variable length, because seven of its fields are compact indices:

```
FPlane    16 bytes, normal and distance
u64       zone mask
u8        node flags
index x7  iVertPool, iSurf, iBack, iFront, iPlane, iCollisionBound, iRenderBound
FSphere   16 bytes, the node's bounding sphere
17 bytes  zero in every node seen, meaning unknown
u8        zone
u8        vertex count of the node's polygon
i32 x5    typically -1, -1, 0, leaf, -1
```

### How the node was found

Variable length records cannot be stepped through until the layout is known, and
the layout cannot be checked without stepping through them. The way out was to
find the node boundaries independently: a node starts with a unit length plane
normal followed by a small zone mask, and scanning a level's node array for that
pattern found exactly 245 candidates where the array declared 245, with no false
positives.

That gives the exact length of every node, which turns the problem into a search:
how many compact indices after the flags byte, and how many fixed bytes at the
end? Over all 244 measurable gaps there is exactly one answer, seven indices and
a 55 byte tail, and it fits every one of them.

Surfaces were found the same way, using a different anchor. Nodes carry an iSurf
index, so the surface array's length is known before it is parsed: the largest
iSurf was 195 and the array declared 196. Nodes also carry iVertPool, so the
array that follows the surfaces must be at least that large, and that constraint
alone narrowed a search over field counts to two candidates. Semantics then
separated them: the right layout gives 196 of 196 unit length plane normals and
196 of 196 indices inside the vector and point arrays, while the wrong one gives
36 and 0 and resolves materials to objects that are not textures.

Verts came last, and they carried two lessons.

The first is about what to measure. A quarter of the vertex pool holds indices
that point outside the point array, which looks like a broken parse. It is not:
those entries are simply unreferenced, left behind by editing. The check has to
be over the vertices a node actually points at, not over the pool.

The second is sharper. With the pool checked properly, 108 of 32690 nodes still
failed, all of them in five large models. The cause was two adjacent bytes in
the node whose meaning I had swapped: a zone and a vertex count. Both readings
parse, both produce plausible polygons, and nothing crashes. Two independent
measurements settle it. With the second byte as the count, no node in the game
indexes outside its point array, against 108 the other way; and the spacing
between consecutive vertex pools matches the second byte 16402 times against 533
for the first.

Also useful as a check on the link fields: following iBack, iFront and iPlane
from the root reaches every node in every model, exactly once each. A wrong
field there would not give a complete traversal.

Checked across all 29 Shrek 2 maps: 2727 Model records, 32690 nodes, 18539
surfaces and 602807 verts, with every reference in range and every plane normal
unit length.

## Putting a level together

A map holds one Model per brush plus one for the level. The brush models are the
shapes the editor's CSG pass started from, subtractive volumes included, so
drawing all of them overlays the building blocks on the result. The level's own
Model is the one no actor points at through its Brush property. In every map
checked there is exactly one, for example 81 of the 82 models in Shreks Swamp
are owned by brushes and volumes, and Model1 is the level.

A polygon is a node's run of verts, `vert_pool` to `vert_pool + num_vertices`,
each pointing into the model's Points, with the material taken from the node's
surface.

Unreal is left handed with Z up. For a right handed, Y up viewer, swapping Y and
Z does both conversions at once. This was confirmed against the game rather
than assumed: in the Donkey prison the hole the cat goes through sits to the
left of the stairs, as it does in play. Mirroring is the one error this kind of
conversion makes silently, so it needs a check against something known.

BSP faces point into the playable space, the side a polygon is meant to be seen
from, and the node's plane normal says which side that is. Drawing BSP from the
front only therefore makes a level transparent from outside, the way the editor
shows it, while still solid from within. Winding each triangle to agree with its
node's plane makes this independent of whatever winding the file uses.

### Placed static meshes

Most of what makes a level look like itself is not BSP. Static mesh actors carry
most of the detail: 675 in the Donkey prison, 1025 in Shreks Swamp.

An actor's StaticMesh property is usually an import. Its outer chain ends at the
package name, which is also the `.usx` file name, so the mesh is found by
package and object name. All 675 prison actors resolve this way.

The transform comes from four properties, each optional with a default:

```
Location      vector, default (0, 0, 0)
Rotation      Pitch, Yaw, Roll in Unreal units, 65536 to the full turn
DrawScale     float, default 1
DrawScale3D   vector, default (1, 1, 1), multiplied with DrawScale per axis
```

The rotation follows Unreal's FRotationMatrix: roll about X, then pitch about Y,
then yaw about Z, giving the images of the local axes as

```
X = ( CP*CY,              CP*SY,              SP     )
Y = ( SR*SP*CY - CR*SY,   SR*SP*SY + CR*CY,   -SR*CP )
Z = ( -(CR*SP*CY + SR*SY), CY*SR - CR*SP*SY,  CR*CP  )
```

and a vertex lands at `Location + x*X*sx + y*Y*sy + z*Z*sz`. A sign error in a
formula like this puts every object in the right place but turned the wrong
way, and passes every numeric check, so it was confirmed against the game: in
the Donkey prison the placed objects face the way they do in play.

For a right handed, Y up viewer the instance matrix is `P A P` with P the Y and
Z swap, and the translation `P Location`. Checked numerically against baking the
transform in Unreal space first: 9560 vertices, worst disagreement 1.7e-14.

Not placed yet: actors that take their mesh from class defaults rather than
their own property (the prison's chains, for one), movers, which carry their own
brush models, and anything drawn with a skeletal mesh, such as the prison's
swinging maces, `BallSpiked`, from the `.ukx` packages.

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
