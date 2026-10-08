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

Current state: all 8638 functions pass both end alignment and the size check.

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

Every object record starts with its tagged properties, and for a field they are
always empty, so a State, Struct or Function record opens with a single None.
A Class is the exception: its properties are its defaults, at the far end of
the record, so it starts straight with its links.

```
index  None                not in a Class record: the empty property block
index  SuperField          parent class, or the overridden parent function
index  Next                next member in the owner's declaration chain
index  ScriptText          TextBuffer export, always named "ScriptText"
index  Children            first member, head of the Next chain
index  FriendlyName        name index; for operators this is the symbol, "+"
index  unused              zero in every record inspected
u32    Line
u32    TextPos
u32    ScriptSize          size of the bytecode IN MEMORY, see below
bytes  bytecode
```

**This layout was wrong here for a long time, in a way that hid itself.** The
first reading had no None and a CppText instead, which gives the same seven
indices before Line, so every function's bytecode still parsed to its exact
end. What it got wrong were the links: a function's SuperField was taken for
its Next, and a class's Children was read one index late, where it lands on
another class's member chain. The decompiler printed most classes with some
other class's variables, BitmapMaterial with Actor's; the class defaults still
validated, because the polluted lists still held the real names. It was found
when the engine's linker, a port of the same reading, laid out BitmapMaterial
with 207 variables, one of them Inventory. Read as above, the chain from every
class, function, state and struct stays inside its owner, 11720 of 11720, and
reaches every field it owns; what a class owns off the chain is the subobjects
of its defaults, emitters and GUI controls.

A function then ends with a tail:

```
u16  iNative            native index, zero for script functions
u8   OperPrecedence
u32  FunctionFlags
u16  extra              present on some functions, see the discriminator below
```

**Reading the tail.** The tail is 7 or 9 bytes: the extra u16 is RepOffset,
there exactly when the flags carry FUNC_Net. Read the u32 at `end - 6`; when it
has FUNC_Net and exactly one access specifier, the tail is 9 bytes. Otherwise it
is 7, the flags at `end - 4`.

The first rule here tested the 7 byte position for any access bit and took the
9 byte one only when that failed. At `end - 4` the u32 takes its top half from
RepOffset, and in 121 net functions, PlayerController.ServerMove among them,
RepOffset has bits where the access specifiers are. Those were read with a 7
byte tail, which left the RepOffset's two zero bytes in the bytecode as a stray
local variable reference after the final return. The script sizes caught it:
dropping that token makes all 121 agree.

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
index  None                 the empty property block, as for every field
index  SuperField
index  Next
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
Next is the third index in every field record, property or otherwise, because
the empty property block's None comes first; only a Class, which has no such
block, has it second.

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
0x39 PrimitiveCast u8 expr  the byte is the conversion, 0x39 .. 0x5F
0x40 DynArrayInsert e e e   array, index, count
0x41 DynArrayRemove e e e   array, index, count
0x43 DelegateFunction obj name parms
                            a call through a delegate property
0x44 DelegateProperty name  a function, as a delegate value
0x45 LetDelegate expr expr  delegate assignment
other 0x3A .. 0x5F          conversions as tokens of their own, 53 in all
```

**These five closed the last misaligned functions.** For as long as the token
table had 0x39 to 0x5F as one run of casts, 39 functions failed to end on their
record, 33 of them in GUI.u, the package that uses delegates for every button.
0x43 was found by hand on GUIListBox.InternalOnChange, `43 10 0a 17 16 04 0b`:
the delegate property, the name OnChange, the parameter self, then return,
13 bytes in memory against a declared 13. Every other layout was chosen by
measurement over the whole corpus. Read as DelegateFunction, 24 more functions
align; with 0x44 taking a single name, all 8638 functions of the game end on
their record and agree with their declared size, the first time with no
exception. The 94 names 0x44 carries are all functions of their class, bar 3
None; the property 0x43 carries is a DelegateProperty in all 54.

Three of the five cannot be told apart by length. A token that takes three
expressions read as taking one leaves the other two behind as statements of
their own, and the function still ends where it should. They show as bare
expressions instead, `pris.Remove` followed by the statements `0;` and
`pris.Length;`, and the right count is the one that leaves none: three for
0x40 and 0x41, two for 0x45, 262 such statements gone in all.

0x39 is a prefix, EX_PrimitiveCast: the byte after it says which conversion,
0x3A ByteToInt, 0x3F IntToFloat, 0x53 IntToString and so on, with 0x39 itself
meaning RotatorToVector, and one expression follows. Of the 8208 casts in the
game's script 7778 carry a code from 0x3A up and 215 carry 0x39; the codes
stand alone as tokens only 53 times, all at statement level.

The first reading took all of 0x39 to 0x5F as cast tokens of one expression
each. That parses to the same length, prefix and code reading as two nested
tokens, so alignment and size never objected, but it means something else:
`(Level.TimeSeconds - Pawn.LastStartTime) > 1` came out as `> vector(float(1))`,
and a VM running it would cast twice. The engine's check settled it, by the
declared type of what each conversion is handed: read as a prefix, every code
is given only its own source type, RotatorToVector 263 Rotators, IntToString
238 ints, NameToString 668 names; read the first way, 0x39 was handed floats,
ints, strings, rotators and vectors alike.

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

Type codes: 1 Byte, 2 Int, 3 Bool, 4 Float, 5 Object, 6 Name, 7 Delegate,
8 Class, 9 Array, 10 Struct, 11 Vector, 12 Rotator, 13 Str, 14 Map,
15 FixedArray. Code 7 is UE1's string, and was first read as one here; in these
packages it holds a delegate, an object reference and a function name, 3 or 4
bytes, on 75 DelegateProperty values such as a GUI button's OnClick.

Three structs hold plain binary: Vector and Rotator, 12 bytes, and Color, 4.
Every other struct is a nested tagged list that ends on its size, Plane, Range
and Scale included. This was measured over every struct value in the game's
defaults and levels, 51835 Vectors, 12417 Rotators and 362 Colors at their
binary sizes, and everything else parsing as a tagged list to its end. An
earlier list here took Plane, Range and Scale as binary too; the sizes do not
agree, Range at 13 bytes against 8 and Scale at 25 against 17, and the engine's
first run against the game reported it on 1858 default objects. An array holds a compact count and then its
elements: an object reference is a compact index, and a struct is a tagged
list of its own, ending in None. A byte value whose property refers to an enum
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

**Names are not enough; the tag types have to agree too.** An entry counts
towards a candidate only when its name is a variable of the class and its tag
type is one that variable is written with. Six classes needed it. In each, a
start a few bytes early read stray bytes as one extra entry with a real name,
EFFECT_DOME_SAINTPAULS's an array called DecayHFRatio, which is a float, and
since every name still resolved, the tie went to the longer list. GUITabControl
was worse: its whole block came out as one such entry. With the types checked,
the six bogus entries were all first in their lists, and all six classes now
start where they should. A ClassProperty is tagged as an object, 590 times,
since underneath it is one.

The variables of a class include those its parents declare, and a subclass may
declare one again: AppleTree has an array of names called ThrowAnimName over
KWPawn's single name. The subclass's declaration is the one its defaults are
written for.

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

### The Level object

Which of those actors exist is the Level's business. Its record has an empty
property block, then:

```
u32 x2    the actor count, twice: the array's count and its capacity
index     that many actors, LevelInfo first
FURL      Protocol, Host, Map, Portal as FStrings, an array of option strings,
          i32 Port, i32 Valid
index     the level's Model
f32       a time in seconds
18 bytes  zero
```

Read on all 29 maps, it lands on the end of every record: count equals
capacity, no actor slot is empty, LevelInfo comes first, the 18 bytes are zero,
and the Model it names is in every map the one found before by elimination, the
Model no Brush refers to. The URL is `unreal:HP-Test.unr` on port 7777 in 28
of the 29; only Entry says `Index.unr`. That is the default URL from the game's
own configuration: Default.ini's [URL] section still names HP-Test.unr as its
Map, a leftover from KnowWonder's Harry Potter games, and a level saved in the
editor carries the URL it was opened with.

**A package keeps actors its level has deleted.** 8572 actors in the game's maps
are not on any Level's list, and 8537 of them carry bDeleteMe, most with
bPendingDelete as well; the 35 others are not actors at all but materials and
emitter parts. Of the 22584 listed actors, not one carries either flag. The
deleted ones include nearly all the AutoLadders, 8158 of 8190, and 67 coins, 59
of them on Shrek's swamp, which the viewer drew until it read the list. Only
the listed actors are the level.

**And lists some the game never loads.** The export's object flags say who
loads it. 20115 of the 22606 listed actors carry 0x02070001: the state frame
flag 0x02000000, LoadForClient 0x10000, LoadForServer 0x20000, LoadForEdit
0x40000, and Transactional 0x1. The other 2491 have neither load bit and carry
NotForClient 0x100000 and NotForServer 0x200000 in their place: all 2056 Brush
actors, the editor's builder brushes the BSP was made from (0x02340001), and
all 435 Camera actors, the editor's viewports (0x02340000), each with its
RendMap, ShowFlags and OrthoZoom saved. Volumes, Brush subclasses, load like any
other actor. The bit values are the engine family's; that exactly the editor's
objects lack the load bits is what confirms them. The engine loads an actor of
the level only with both load bits.

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
index     CollisionModel: a Model export, or 0
index     collision triangle count, then each: u16 x3 vertex indices and an
          index, the section its material comes from
index     collision node count, then 20 bytes each:
            u16 x4  triangle, coplanar node, front node, back node; 65535 none
            i16 x6  bounding box, min xyz then max xyz, quantised
FVector   quantisation scale
lazy      raw triangles: u32 offset of the array's end, a count, then each:
            FVector x3 corners, u32 NumUVs, NumUVs x 3 FVector2D,
            FColor x3, i32 section, u32 smoothing mask
u32       InternalVersion, 9 in every mesh
index     KarmaProps: a KMeshProps export, or 0
u32       0 in every mesh
```

The trap is the byte after the bounding sphere. By analogy with the box it looks
like the sphere's valid flag, but it is the section count, and reading it as a
flag puts every later field one byte out. The section count predicting exactly
where the next bounding box begins is what confirmed it: 413 of 413 meshes.

With the tail decoded the walk can be checked the strong way, by landing on the
end of the record, and it does: all 835 meshes in the StaticMeshes packages and
869 of the game's 870 counting those in texture packages and maps, with every
index in range, for 122011 vertices and 102414 triangles. The exception is the
editor's material preview sphere in Editor.u.

### Collision

A static mesh carries its collision in two forms.

**The triangle tree.** The collision triangles are a copy of the render
triangles, the same three vertex indices into the same vertex array, in 837
meshes; 29 have a set of their own, and 4 have none at all, posters and the like
with collision switched off. The material index is the section: in 108292 of
the 108294 cases where a collision triangle matches a render triangle, it names
the section that triangle is drawn in.

The nodes form a BSP over those triangles. Each holds one triangle. The coplanar
node continues a chain of triangles in the same plane: all 64765 coplanar links
in the game lie in their node's plane. The front node holds the triangles that
reach the front of the node's plane, as its winding makes the normal, and the
back node those that reach behind it, so a triangle that crosses the plane is
listed under both. Over every subtree of every node, 99.8 percent of the
triangles under a front node reach the front and 99.7 percent of those under a
back node reach the back; the few hundredths of a percent left over sit within a
hair of the plane. Two crossed quads, a cattail, show it plainly: the root holds
one quad, its coplanar node the other half of it, and both halves of the second
quad appear under both children, since that quad passes through the first.

A node's box bounds its whole subtree. The six i16s are multiplied by the
scale, and the scale on each axis is the largest absolute collision coordinate
divided by 32767, to 0.0015 percent in every mesh. No box falls short of its
subtree by more than 0.07 units, within one quantisation step.

**The collision model.** 312 meshes also point at a Model export of their own,
a small BSP of the kind levels are made of, usually a box: 6 nodes and 6
polygons. All 312 read with the level BSP reader and pass its checks. Every one
of those meshes also has its triangle tree. Which of the two the engine uses
for what is up to its native code; StaticMesh has no script class, and none of
the game's meshes stores any property but Materials.

**The raw triangles** are the editor's source: per triangle its three corners,
which are mesh vertices in 108697 of 108739 cases, the UVs of each corner for
every UV stream, NumUVs matching the mesh's stream count in all of them, three
colours, a section and a smoothing mask. KarmaProps, set on 320 meshes, are the
physics properties for Karma.

### Sections and their materials

A section is one material's share of the triangles:

```
i32   not decoded
u16   FirstIndex     where the section starts in the index buffer
u16   FirstVertex
u16   LastVertex
u16   a copy of the face count
u16   NumFaces
```

A material slot with no triangles still gets a section, with 65535 for both
vertex bounds and no faces. Skipping those, the sections of a mesh tile its
index buffer exactly, end to end in order, which is the check that fixes the
field order.

The materials themselves are in the tagged block, as `Materials`, an array of
structs `{Material, EnableCollision}` with one element per section. An array of
structs is a compact count and then each element as its own tagged list ending
in None; the whole array lands exactly on the size its tag gives.

`umaterial.py <game dir>` checks all of this on every static mesh in the game,
870 of them counting the 28 kept in texture packages, 6 embedded in maps and 1
in Editor.u. The Materials array parses for all 870, there is one material per
section in all 870, and the sections tile the index buffer in 869. The one
exception, and a 871st mesh that does not parse at all, are the editor's
material preview shapes in Editor.u, TexPropSphere and TexPropCube. The sphere
claims 960 faces over an index buffer of 9 entries, so the editor's own meshes
are stored differently or not stored at all. Nothing in a level uses them.

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
i32       NumSharedSides
i32       NumZones, then that many zones:
            index ZoneActor (its ZoneInfo, or 0), u64 Connectivity,
            u64 Visibility, f32 LastRenderTime
index     Polys, the editor's polygons
index     bounds count, then FBox each; a node's iRenderBound points here
index     leaf hull count, then i32 each; see collision below
index     leaf count, then each: index iZone, index iPermeating,
          index iVolumetric, u64 VisibleZones
index     light count, then index each: Light actors, in lists ending in 0
u32       RootOutside
u32       Linked
index     render section count, then each section:
            index  vertex count, then 40 bytes a vertex: position, texture
                   u v, lightmap u v, normal
            u32    revision, equal to the node count in every section
            index  Material
            u32    node count
            u32    poly flags
            i32    lightmap texture, or -1
index     lightmap count, then the lightmaps (see lightmaps below)
index     lightmap texture count, then the lightmap textures
```

Every Model in the game, 2727 in levels and 312 behind static meshes, reads to
the exact end of its record with every reference in range. The collision and
brush models end in three empty arrays where a level model has its render
sections, lightmaps and lightmap textures.

A surface is also variable length:

```
index     material, usually an import, resolving to a texture name
u32       poly flags
index x6  pBase into Points, vNormal, vTextureU, vTextureV into Vectors,
          iLightMap, iBrushPoly
FPlane    16 bytes
f32       lightmap scale
```

The last float is 32, the UE2 default lightmap scale, on 15355 of the game's
15361 surfaces and 16 on the other six. There is no panning field: panning is
folded into the base point.

**Texture coordinates** come from pBase, vTextureU and vTextureV. A point's texel
coordinates are its offset from the base projected on the two texture vectors,
and dividing by the texture's size gives the 0 to 1 range of the image:

```
u = (P - Base) . TextureU / USize
v = (P - Base) . TextureV / VSize
```

USize and VSize are those of the texture the material leads to (see
materials). The vectors were checked before the formula was trusted: on all
15361 surfaces of the 26 levels, the normal is unit length, TextureU and
TextureV are perpendicular in 15317, and their lengths are plain texture scales,
1 on 7777 surfaces, then 0.5, 0.727, 2, 4, 1.25, 0.75, 0.25 and 8. Most lie in
the plane of their surface; 406 are tilted 45 degrees out of it, a projection
the editor allows. Whether a texture comes out mirrored or turned is not in the
numbers, so that part was checked by eye against the game, on brick, planks and
lettering.

A node is variable length, because seven of its fields are compact indices:

```
FPlane    16 bytes, normal and distance
u64       zone mask
u8        node flags
index x7  iVertPool, iSurf, iBack, iFront, iPlane, iCollisionBound, iRenderBound
FSphere   16 bytes, the node's bounding sphere
16 bytes  zero in all 29512 nodes of the game, meaning unknown
u8        zone behind the plane
u8        zone in front of the plane
u8        vertex count of the node's polygon
i32 x2    iLeaf behind and in front of the plane, -1 for none
i32       render section, -1 in a model that is not rendered
i32       first vertex of the node's polygon in its section
i32       lightmap, or -1
```

The two leaf indices were settled by the zones: wherever a node names a leaf on
a side, that leaf's zone is the node's zone byte for the same side, 292 of 292
behind and 5732 of 5732 in front.

### Zones

A level is divided into zones, rooms joined by portals, and every node carries
the zone on each side of its plane. A point's zone is found by walking the tree:
from the root, go to the front or back child by the side of the plane the point
is on, until there is no child on that side, and the zone byte for that side is
the answer. A child index of 0 means none, since the root is nobody's child.

The check is independent of the BSP: every actor stores a `Region`, a tagged
PointRegion struct of Zone, iLeaf and ZoneNumber, written by the engine when it
saved the level. `ubsp.py --zones <maps>` walks the tree for every actor's
location and compares. Over the live actors, 22146 of 22269 agree. Of the 123
left, 56 are cutscene cameras and 52 brushes, whose location is an editing
handle, not a place they stand. Counted over every actor in the packages, the
deleted ones included, the match falls to 22662 of 30795, nearly all of the
difference AutoLadders: deleted actors keep whatever Region they last had, so
740 of them in the Fairy Godmother factory, standing all over the level, name
the same leaf.

The zone table settles the walk independently. Each zone names its ZoneInfo, and
for all 40 ZoneInfos in the game the zone the table gives it is the zone the
walk finds at its location. A zone's Connectivity always has its own bit set,
and the zones it shares a portal with: on the swamp, zone 2 is 0x204, joined to
zone 9.

### Collision

The BSP is the level's collision too, and the same walk decides it: a point is
solid when the walk leaves the tree into leaf -1, and empty otherwise. Which
side the walk leaves by is not enough. Behind a plane that bounds no solid, a
sheet or a non solid brush, the walk leaves by the back into a leaf like any
other, and the outdoor levels are full of them: The Hunt part 1 sends 810 of
its 852 actors out by the back, into proper leaves and zones. Leaving by the
front without a leaf never happens.

`ubsp.py --solid <maps>` puts 18113 of 19580 live actors in empty space. Of the
1467 in solid, 1245 are StaticMeshActors whose origin sits inside the wall or
floor they decorate, 62 anti portals, 39 cutscene cameras, 38 lights set into
walls and 25 movers, gates and doors standing closed. What has to be in the
open is: all 1193 coins, and 1117 of 1122 path nodes.

A node's iCollisionBound points into the leaf hulls: a list of node indices,
the planes of a convex hull, ending in -1 and followed by the hull's box as six
floats. Bit 0x40000000 on an index means the plane is used flipped. All 3039
Models' hulls read this way, every index a node and every box ordered.

Light lists are the other run-length array: a leaf's iPermeating and
iVolumetric each start a list of Light actors ending in 0, or are -1 for none.

### The sky

A level's sky is BSP like the rest, built around its SkyZoneInfo far from the
playable space: a box of six polygons, with a sky dome and distant treelines as
static meshes inside it. The engine draws that zone first, as a background, from
the SkyZoneInfo's location with the player camera's rotation only, so the sky
stays at infinity however far the player walks. The sky zone is simply the zone
the SkyZoneInfo stands in.

The level shows it through surfaces with poly flag 0x80, PF_FakeBackdrop. The
value is from the engine family, and the data agrees with it: the flag is on
surfaces in all 17 levels that have a SkyZoneInfo and in none of the 12 that do
not, every such surface faces a playable zone, and almost all of them back onto
zone 0, the outside. They are the outer walls a player looks at the sky through.

Several other zones on Shrek's swamp stand near the sky and look like part of
it, ringed with the same treeline texture. They are not: they hold
CutCamPosition, CSPeasant and the potion bottles, sets for the cutscenes, and one
is a tavern interior with pool balls and a shelf of skull candles.

### Render sections

A level Model carries its polygons a second time, laid out for drawing: one
section per material and set of flags, its vertices in one buffer. Each node
names its section and the first of its vertices there, and on all 29512 nodes
of the 29 levels those vertices are exactly the node's polygon, corner for
corner; the sections' node counts and vertex counts add up to the number of
polygons and of corners. Brush and collision models have no sections, and their
nodes hold -1, 0 and -1 in the three fields. Section flags are the surfaces'
poly flags with the bits that do not matter to drawing dropped, 0x08 and 0x80
among them.

The fifth i32 of a node is its lightmap: the Donkey prison has 4007, the count
the lightmap array declares, and its nodes name 0 to 4006. Shrek's swamp has
none, its nodes all -1 and its sections all flagged 0x400000. A surface's own
iLightMap field is something else: the swamp's surfaces name 32 values while it
has no lightmap at all.

### Lightmaps

A level's static light on its BSP is in lightmaps, one for each lit surface,
packed into 512 by 512 DXT1 textures:

```
lightmap
  index x7  lightmap texture, surface, zone, OffsetX, OffsetY, SizeX, SizeY
  FMatrix   world to texel, used as a row vector
  FVector   base, then the world step of one texel in X, then in Y
  index     light count, then each light:
              index  the light actor
              index  shadow bitmap length, then the bitmap, a bit per texel
              i32    width, height, row pitch in bytes, MinX, MinY, MaxX, MaxY
  index     the Level
  u32       not understood yet

lightmap texture
  index     the Level
  index     count, then i32 each: the lightmaps packed into it
  u64       cache id
  u32       revision
  lazy x2   two mips, 512 and 256 square: u32 offset of the end, compact
            count, data
  u8        format, 3, DXT1
  i32 x3    width, height, and the revision again
```

All 29 level Models read to their end this way, the Donkey prison's 4007
lightmaps and 9 textures among them. A node's last field names its lightmap,
and that lightmap's surface is the node's own in all 4007 of the prison's. Every
lightmap is listed by exactly one texture, every light of a lightmap is a
Light, Sunlight or Spotlight, and each shadow bitmap's length is its pitch
times its height, the pitch a byte per eight texels. The first field after the
lights is a compact reference to the Level, which is why it reads as 2 in the
prison, whose Level is export 2, and as 44 in Hamlet; read as a fixed byte, it
derails every level whose Level is past export 63.

`ubsp.py --lightmaps <maps>` checks what the fields mean, on every polygon with
a lightmap in the game:

- The matrix takes each corner into its lightmap's texels: all 134302 corners
  land inside 0 to SizeX by 0 to SizeY, with a texel of border, so a 40 by 40
  lightmap is used from 1 to 39.
- The base plus u times the X step plus v times the Y step gives the corner
  back, within 0.23 units for 99.9 percent of them; the rest is the distance
  off the plane, which the two steps do not describe.
- The lightmap coordinates in the render section's vertices are the texel's
  place in the texture, (Offset + texel) / 512, to within 4.5e-8.
- The 13419 rectangles all lie inside their texture, and no two overlap.

Decoded as DXT1, a texture is plainly a lightmap atlas: the prison's shows cold
blue walls, the orange glow of torches, and the hard shadows of bars.

How the engine combines it is not in the data. The viewer draws a lit surface
as its texture times the light, the lightmap texel or the vertex colour, plus
its zone's ambient, and compared by eye with the game the light looks right
doubled rather than as stored. The shadows in the lightmaps are baked ones, cast
when the level's lighting was built, among them the shadows of the bars in the
Donkey prison, and the game shows them the same way: compared with gameplay
footage, they are there, as darker patches of the surface, with no shadow drawn
at run time.

Three levels, the Fairy Godmother battle, Hamlet and the Hamlet mine, have
their lightmaps with all their shadow bitmaps but a single texture with both
mips empty, its format and size fields holding garbage. Their light was never
baked into texels; an engine has to compute it from the lights and the shadow
bitmaps.

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

**PrePivot** moves the drawn mesh relative to the actor, and it is subtracted
in the mesh's own coordinates, before scale and rotation:

```
Location + R S (v - PrePivot)
```

This was found from a mossy rock hanging in the air on Shrek's swamp. Every
rock_step there has a PrePivot of about (0, 0, 200) and a mesh 405 units tall
with its origin at the bottom. The sign and the space were settled on all 26
static mesh actors with a PrePivot that stand on terrain, comparing the
lowest point of the mesh with the terrain height beneath it:

| PrePivot | floating | worst |
|--|--|--|
| ignored | 21 of 26 | 391 above the ground |
| subtracted in world space | 2 of 26 | 191 above |
| subtracted in mesh space | 0 of 26 | every one 9 to 763 into the ground |

Bushes in the Beanstalk bonus levels, at scale 1 where the two spaces agree, go
from 53 to 84 units in the air to 16 to 47 units planted, which is how bushes
are placed everywhere else. Grates and potion wheelbarrows in the Fairy
Godmother factory levels have a PrePivot from their class.

### Properties come from the class too

An actor's properties are its class's effective defaults overlaid with its own.
The class's defaults block stores only what differs from its parent, so the
effective values come from walking the chain from the root down and letting
each class override the last. That matters three ways for placement:

- Actors that never set a mesh of their own still have one. Coins, crates,
  barrels and chains take theirs from the class: 217 of the prison's 892 placed
  meshes.
- DrawType decides whether the actor is drawn as a static mesh at all, and is
  usually inherited.
- Scale and rotation can be inherited, though none of the prison's actors
  changed when defaults were merged in.

### Movers are static meshes in UE2

In UE2, `Mover` extends `Actor`, not `Brush`, in Epic's UnrealEngine2 Runtime as
much as in Shrek 2, so this is the engine's design and not a licensee change.
Movers are drawn like any other actor, which in practice means as static meshes.
It is worth stating because the opposite is what most writing on Unreal
internals describes: in UE1, `Mover` extends `Brush` and carries brush geometry,
and a reader built from that knowledge will go looking for mover brushes that are
not there.

Across all Shrek 2 maps, 235 movers draw as DT_StaticMesh and are placed like any
other mesh, 18 are invisible logic movers drawn as sprites, and none uses a
brush. The 401 actors in
the prison that do carry a brush are the CSG brushes already baked into the
level's BSP, and invisible volumes.

### A package is a file name, not a file type

Unreal does not tie object types to file types. Shrek 2 keeps some static meshes
in texture packages: the beanstalk bonus maps import
`Beanstalk.StaticEnviroment.fence_1`, which lives in `Textures/Beanstalk.utx`.
Looking only in `.usx` files left 45 actors unplaced. Resolving the import's top
level name against every package file in the game, whatever its extension,
places all of them.

Across all 29 maps, 11448 actors draw as static meshes: 11429 are placed, 19 are
hidden, none fails to resolve.

Not placed yet: anything drawn with a skeletal mesh, such as the prison's
swinging maces, `BallSpiked`, from the `.ukx` packages.

## Terrain

A level's ground is a `TerrainInfo` actor. Its record is one of those with a
native payload after the properties, so its properties are read up to their
terminator rather than to the end of the record. They give:

```
TerrainMap             the heightmap texture, usually a local export of the map
TerrainScale           grid spacing in X and Y, height scale in Z
Location
QuadVisibilityBitmap   one bit per quad, set where the ground is drawn
EdgeTurnBitmap         one bit per quad, set where it splits on the other diagonal
```

Both bitmaps are arrays of u32 words; quad (x, y) is bit `x + y * USize`.

### The heightmap texture

The heightmap is a texture in format 10, G16: one unsigned 16 bit height per
pixel. Its native payload, to the byte on the heightmaps seen:

```
index   mip count
per mip:
  u32     skip offset: the absolute file offset just past the data
  index   data length in bytes
  bytes   pixel data
  u32     USize, u32 VSize, u8 UBits, u8 VBits
```

The skip offset makes a free consistency check: it must equal the position the
data ends at. Only enough of the texture format is read here to take a G16
heightmap's first mip; textures proper are separate work.

### From heightmap to world

For grid point (x, y) with height h:

```
X = Location.X + (x - USize / 2) * TerrainScale.X
Y = Location.Y + (y - VSize / 2) * TerrainScale.Y
Z = Location.Z + (h - 32767) * TerrainScale.Z / 256
```

and the actor's Rotation is not applied.

This went through two stages and the second is the one to trust. First a fit:
counting how many ground placed static meshes come to rest on the surface under
each candidate formula. That settled what it could. No transpose and no flip,
since every such variant fell below 10 percent. A Z divisor of 256, which
doubles the count against 128. And no rotation: applying a 15 degree yaw cut the
count on that terrain from 25 percent to 8. What the fit could not settle was a
half cell offset in the centring; the anchors are too noisy for it, and the two
terrains of the swamp even disagreed.

Then an exact check against the engine's own data. Each TerrainSector records
its quad range and a bounding box the editor computed:

```
u8        property terminator, the block is empty
index     the TerrainInfo it belongs to
u32 x4    QuadsX, QuadsY, OffsetX, OffsetY
FBox      6 floats and a valid byte
```

Recomputing every sector's box from the formula and comparing, over all 1360
sectors of all 22 terrains in the game: 0.0005 units of disagreement
horizontally, which fixes the centring at USize / 2, and 0.0007 vertically. The
vertical match needs a height zero of 32767. With 32768 every one of the 1360
sectors came out lower by exactly one height step, at every height scale, which
is the signature of an off by one and not of noise.

The bitmaps' bit order is not something the sector boxes can check. It was
confirmed against the game instead: the holes fall where the ground is cut away
in play.

A TerrainInfo's Rotation is not part of the transform. Shrek's swamp has a
terrain turned 2776 units, about 15 degrees, and its sector boxes still agree to
0.0004 units with the rotation left out.

### The engine's copy

After its properties a TerrainInfo carries the engine's own copy of the grid,
read to the exact end of all 22 terrains:

```
index       sector count, then TerrainSector references
index       vertex count, then FVector each: the grid in world space
i32 x2      SectorsX, SectorsY
index       count, one per vertex, then two FVector normals each: the two
            triangles of the quad that vertex is the corner of; the last row
            and column are unused
FCoords x2  ToWorld and ToHeightmap, an origin and three axes each
i32 x2      HeightmapX, HeightmapY
index       vertex count, then u8 R, G, B, A per vertex: the baked light
```

It is a second proof of the transform, independent of the sector boxes: the
stored vertices agree with the formula at every vertex of every terrain, within
0.0007 units, and the frames say the same thing in other words, ToWorld's axes
being TerrainScale X and Y and Z / 256 and ToHeightmap's origin Location minus
half the grid.

**The normals settle the quad split, and showed it had been wrong.** Read the
obvious way, the edge turn bit cuts a quad from (x, y) to (x+1, y+1) when set.
The stored normals agree with the opposite: on every quad of every terrain,
exactly, they are the normals of triangles cut along that diagonal when the bit
is clear and along the other one when it is set, while the first reading misses
by up to 1.3. The viewer drew every terrain with its quads split the wrong way
until this was read.

The light is a colour per vertex, alpha 255 throughout, and its channel order is
R G B, settled as for static meshes by the sun: on all five levels with a
coloured Sunlight the light's hue read as R G B is the sun's, Castle Siege's
170 exactly. Drawn like the rest, the texture times the light doubled plus the
zone's ambient, the terrains look as they do in the game, by the memory of
someone who knows it well.

### Layers

A terrain is painted with up to 32 layers, drawn in order, each blended over
what is below it by its alpha map. `Layers` is a fixed array of TerrainLayer
structs, and each element is a tagged list of its own:

```
Texture          the layer's texture
AlphaMap         an RGBA8 texture; the weight is its alpha channel, 0 to 255
UScale, VScale   texture repeat, in quads
UPan, VPan, TextureRotation, LayerRotation    zero on every layer in the game
TextureMapAxis   which plane the texture is projected on
TerrainMatrix    the editor's world to texture transform
KFriction, KRestitution, LayerWeightMap
```

TerrainMatrix is a Matrix struct of four Planes, XPlane to WPlane, and here,
unlike Planes held directly, each Plane is itself a tagged list of X, Y, Z and
W. It is used as a row vector:

```
u = [x y z 1] . (XPlane.X, YPlane.X, ZPlane.X, WPlane.X)
v = [x y z 1] . (XPlane.Y, YPlane.Y, ZPlane.Y, WPlane.Y)
```

The 22 terrains have 73 layers, every one of which parses to the exact end of
its struct, and 1 to 6 layers each. On all 70 layers with a texture the matrix is
1 / (TerrainScale * UScale) on the axes TextureMapAxis picks, so a texture
repeats once every UScale quads: 69 project on the ground plane, and one in the
Carriage Hijack, TextureMapAxis 2, takes u from the height, for a cliff. The
other three layers have an all zero matrix and a UScale of 0, and are not
drawn. The WPlane row, the offset, is small, a fraction of one repeat, and it is
applied as stored; whether it is meant for world or terrain coordinates is not
settled, and at that size it does not show.

Alpha maps hold a constant 127 in red, green and blue and the weight in alpha.
66 are as large as the heightmap, a texel per vertex, and 4 are half its size.
The first layer's map is 255 nearly everywhere, the ground the others are
painted over.

**The spikes on Shrek's swamp are in the data.** On the tutorial terrain four
vertices in a diagonal row, between the first lily pads and a punching bag,
stand 190 to 300 units above their neighbours, painted at full weight with the
cliff layer. The engine's own sector box for that part of the grid reaches the
same height, 244, so they were there when the level was saved. Whatever hides
them in play, if anything does, is not in the terrain.

## Skeletal meshes

Characters and other animated objects are SkeletalMesh records, in the `.ukx`
packages along with their MeshAnimation sets. Shrek 2 has 141 skeletal meshes,
from Shrek, Donkey and Fiona down to a knight's hubcap.

The record opens like every primitive, a bounding box and sphere after an empty
property block, then the LodMesh header:

```
u32       Version            4 throughout Shrek 2
u32       VertexCount
index     packed vertex count, then 4 bytes each, empty here
index     texture count, then that many references
FVector   MeshScale
FVector   MeshOrigin
i32 x3    RotOrigin, Pitch Yaw Roll
```

RotOrigin is (0, -16384, 16384) on 140 of the 141 meshes. The raw points are Y
up; turning them by RotOrigin with the same rotation formula as actors stands a
character upright and facing along X, Unreal's forward axis. That was checked by
eye: Shrek comes out standing in his rigging pose.

Further on are the skeleton and four LOD models. A bone is a name index, a u32
of flags, a unit quaternion, a position, a length, three sizes, a child count
and a parent index; the root is its own parent. What matters for drawing is that
each LOD model keeps its own copy of the geometry in four lazy arrays:

```
u32       absolute file offset just past the array
index     element count
bytes     elements
```

always in this order:

```
influences   8 bytes each: f32 weight, u16 point, u16 bone
wedges      10 bytes each: u16 point, f32 U, f32 V
faces        8 bytes each: u16 wedge x3, u16 material
points      12 bytes each: FVector
```

A wedge is a corner: a point plus the texture coordinates it has in one face.
The skip offset in front of each array is a check that cannot pass by accident,
so the arrays are found by scanning the record rather than by decoding the
sections and index buffers that lie between them. Every one of the 141 meshes
yields exactly four LOD models, and in every one of them all wedge and face
indices fall inside their arrays.

### Placing them in a level

1031 actors across the maps draw with a skeletal mesh, effective DrawType
DT_Mesh, and every one resolves. A mesh's reference pose goes into actor space
as

```
RotOrigin applied to (point - MeshOrigin) * MeshScale
```

then through the actor's transform like any static mesh. The sign of MeshOrigin
was settled against the game's own placement rather than guessed: with it
subtracted, the lowest point of 54 placed characters sits on the bottom of their
collision cylinders, median -0.0 units and a spread of 6.6; added, they float 72
units above. The artists' intent is visible in the numbers, Knight's MeshOrigin
is (0, -33, 0) and its CollisionHeight 33.

**Open question: props that float.** Actors whose mesh has a zero MeshOrigin,
mushrooms, trees, pumpkins and the like, come out floating, and the gap is close
to their CollisionHeight: 26.7 for a CollisionHeight of 30, 24.4 for 25, 97.9 for
100, 78.6 for 80. In play they stand on the ground, so something lowers them
that this reconstruction does not do. Four places were checked and ruled out,
each by measurement:

- **Script.** None of their class chain, ShProps, shpawn, KWPawn, assigns PrePivot
  from the collision height, and the collision and location changes in shpawn
  belong to the shrink potion, not to level start.
- **Class variables.** Nothing in KWPawnNative, KWPawn, shpawn or ShProps reads
  like a flag for drawing from the feet.
- **Animation.** The obvious suspect, and wrong. The root bone in the first frame
  of each prop's idle animation sits exactly where it sits in the reference
  pose, a difference of 0.0 for every one of them, while characters move it by
  about a unit, which is breathing.
- **Mesh data.** Each LOD model holds positions twice, in the lazy point array and
  in a 16 byte skinning stream. The two are identical to the last bit, for props
  and characters alike.

What is left is native code, KnowWonder's KWPawnNative or the engine's own C++,
which this project does not reconstruct. The behaviour can still be matched by
observation, but it is not in the data.

**Open question: PrePivot on skeletal meshes.** The rule proven for static
meshes is not applied to skeletal ones, because the one usable measurement
points the other way. Five KnightMelee in the Shrek prison carry a PrePivot of
(0, 0, -20) and stand on BSP floor. With PrePivot ignored their feet are 16.4
units above the floor; with the static mesh rule, 40.4 above; with the opposite
sign, 7.6 below. That is one hand placed setup copied five times. The 4
VentSlimes with a PrePivot sit in vents with no floor to measure against, and
the 10 BounceLeaves have theirs along an axis that does not move them
vertically. The props that float are not explained by it either: none of them
has a PrePivot.

Not decoded yet: the sections and index buffers inside a LOD model, most of the
skeleton's surroundings, and the animations, which live in MeshAnimation
records.

## Skeletons

A skeletal mesh's reference skeleton is found by its signature rather than by
decoding everything before it: a compact count, then per bone a name index, u32
flags, a unit quaternion, a position, a length, three sizes, a child count and a
parent index, the root being its own parent. The reference to the mesh's
default animation follows the last bone. That gives a check: on 121 of the 141
meshes, the bones are the same names in the same order as the default
animation's. Of the 17 that differ, most share an animation with a different
bone order or carry extra bones in the animation, such as Knight's 33 against 38
with the weapon; Unreal matches animation bones to mesh bones by name.

## Animations

MeshAnimation records sit next to the meshes in the `.ukx` packages. Proven on
all 134 by landing on the exact end of each:

```
u32       Version, 0 or 4
index     bone count; per bone: name index, u32 flags, i32 parent
index     motion chunk count; per chunk:
  FVector   RootSpeed3D
  f32       TrackTime
  i32       StartBone
  u32       Flags
  index     bone index count, then that many i32
  index     track count, then that many tracks
  track     the root track
  index     one more field, version 4 only, zero in all 1543 chunks
index     sequence count; per sequence:
  f32       unknown, between 0 and 1
  index     name
  index     group count, then that many name indices
  i32       StartFrame
  i32       NumFrames
  index     notify count; per notify: f32 time, name index, object index
  f32       Rate, frames per second
```

A track is a u32 of flags and three arrays: rotation keys as quaternions, 16
bytes each, position keys as FVector, and key times as f32. A sequence's chunk
holds one track per animated bone; with bone indices present, track i animates
bone `BoneIndices[i]`, otherwise bone i.

The per chunk field is the one that took finding. In a record with a single
chunk it looks like a stray byte after all the motion data; only a record with
two chunks shows where it belongs, because the second chunk starts exactly one
byte after the first one ends. It is absent in version 0.

Of 1709743 rotation keys, 418 are not unit length, and all 418 are exactly
(0, 0, 0, 0). They are the exporter's mark for a key with no rotation, in
character animations, and a player has to treat them as such rather than
normalise them.

### Playing them back

Key times count in frames: a four frame sequence has keys at 0, 1, 2 and 3.
Between keys, rotations interpolate as normalised linear blends and positions
linearly. A bone the sequence does not animate keeps its reference transform.

Two facts about rotations had to be established, both by measurement.

First, keys and the reference skeleton store quaternions the same way. Where a
bone is not animated its first key should equal its reference rotation, and
39008 such keys on non root bones equal it exactly while none equals its
conjugate; roots likewise, 944 to none.

Second, composing the hierarchy takes the non root rotations conjugated and the
root's as stored. Each of the four possible readings places the joints
differently, and only this one puts them on the skin: the median distance from
a joint to the nearest skin point is 1.8 units on Shrek, 1.3 on Donkey and 1.1 on
Fiona, against 7.6 to 20 for the other three.

A point is skinned by the weighted sum of its bones' change from the reference
pose, `current * inverse(reference)`, with the weights normalised per point.
Skinning a sequence whose frame equals the reference pose must then give the
mesh back unchanged, and on 45 of the 48 meshes with a Static sequence it does,
to under 0.01 units; the other three are posed differently in that sequence.
Played forward, Shrek's idle stands with his arms at his sides and his run
leans into the stride, and the whole set was checked against the game itself:
watched in the gallery, the animations move as they do in play.

### What the animation data says about how it was made

There are 1574 sequences, 32.9 minutes of animation in all: 3.5 minutes over 135
sequences for Shrek alone, 3.1 for Donkey, 2.9 for the peasant, 2.7 for Puss in
Boots.

None of it is inverse kinematics. The data holds rotations per bone and nothing
else, and 42620 of the animated tracks carry exactly one key per frame, with
another 9449 keyed more sparsely. Whatever rigs the animators worked with were
baked down to plain per frame rotations on export, so playback needs no IK
solver. The same baking means the data cannot say whether a sequence was keyed by
hand or captured: both come out as one key per frame.

## Textures

A Texture record is a tagged property block, Format, USize, VSize, Palette and
the rest, followed by the mip chain described under terrain: a mip count, then
per mip a skip offset, the data length, the pixel data, and USize, VSize, UBits,
VBits. Proven on all 2121 textures in the game across every package type: the
chain ends exactly at the end of each record, every skip offset points just past
its data, and every mip's data is exactly as long as its format requires at its
size, which for the block compressed formats means `ceil(w/4) * ceil(h/4)` blocks
of 8 or 16 bytes.

| Format | Value | Textures | Notes |
|--|--|--|--|
| DXT5 | 8 | 1656 | nearly everything |
| RGBA8 | 5 | 229 | 4 bytes per pixel, B G R A |
| P8 | 0 | 189 | one byte per pixel into a Palette |
| G16 | 10 | 22 | terrain heightmaps |
| DXT3 | 7 | 20 | |
| DXT1 | 3 | 5 | |

DXT1, DXT3 and DXT5 are the published S3TC formats and decode as such. A
Palette record is an empty property block, a compact count and that many four
byte colours; all 177 hold 256.

**The two colour orders differ**, and both were settled by eye on textures whose
colours are not in doubt. Direct RGBA8 pixels are B G R A: read that way a
redwood's bark is red brown and a level's title card, "Stealing the Potion,
Level 5", is cream with red and dark blue lettering, while the other order turns
the bark blue. Palette entries are R G B A: read that way a menu hourglass has a
golden wooden frame on the classic magenta mask colour and the palette named
Jred is red, while the other order turns both blue. The trap is that green
survives either order, and green is what Shrek 2 has most of.

Package names are matched without regard to case, as on Windows: Shrek's mesh
imports `ShCharacters.Shrek` from the file `SHCharacters.utx`.

All 2121 textures decode, the paletted ones finding their palettes across
packages.

## Materials

A mesh section or a BSP surface names a material, and a material is rarely a
Texture. Between them stand shaders, blends and modifiers, each keeping the
material it wraps under its own property name. Surveyed over every package,
these are the links that lead to the texture that gives a surface its colour:

| Class | Followed through |
|--|--|
| Shader | Diffuse, else FallbackMaterial |
| Combiner | Material1, else FallbackMaterial |
| FinalBlend, TexPanner, TexOscillator, TexRotator, TexScaler, TexEnvMap, ColorModifier, OpacityModifier, MaterialSwitch | Material, else FallbackMaterial |
| MaterialSequence | the first Material in SequenceItems |
| Cubemap | nothing: a reflection, not a colour |

An actor's `Skins` array overrides the mesh's materials slot by slot where it
is set. It is a compact count and that many compact object references: all 133
Skins arrays in the levels, 221 references, end exactly on the size their tag
gives.

Of the 1263 material slots of the 870 static meshes, 1164 lead to a texture and
74 are empty. The other 25 point at textures that are not in the shipped
packages at all: the package exists, the object does not. `Ambush_TX.rock_moss`
and 20 more come from `Ambush_SM.usx`, which is package version 127 while the
rest of the game is 129, and which no level or script package names; the other four,
`2_Carriage_Hijack_Tex.red_bush` and `AmbCreaturesTX.pillar`, are named in
meshes that are also never placed. Leftovers, then. A name close to the missing
one usually does exist, `bush_red` beside the missing `red_bush`.

Over the 11429 static meshes placed in the 26 levels, every section with
triangles finds its texture except 7, and those 7 have an empty material slot.

## Sounds

A Sound record is a property block, empty in every sound of the game, then the
sound file whole:

```
index       FileType, a name: "bik" or "WAV"
lazy array  u32 offset of the array's end, a compact count, that many bytes
...         KnowWonder's lip sync data
```

The block's None is a name like any other, so its index is whatever None has in
that package's name table; in AllDialog.uax index 0 is "bik". Reading the end of
the property block as a zero byte is the mistake to avoid.

All 3769 sounds read this way, 3353 in `.uax` packages and 4 each in `.u` and
`.ukx`, for 136.5 MB, every lazy offset landing on the end of its data. The
files are 3353 Bink Audio, 22050 Hz mono, and 416 RIFF WAV, 16 bit PCM. Bink is
RAD's own format, but FFmpeg decodes it: all 3769, extracted and decoded in
full, with not one decoder error. Music is not in packages at all; it is plain
Ogg Vorbis in `Music/`.

**Lip sync.** After every sound comes a block starting with an i32 version,
each checked against the sound's length as FFmpeg decodes it:

| Version | Sounds | Layout |
|--|--|--|
| 0 | 818 | nothing more (25), or one i32: -1 (442) or the sound's length as 16 bit PCM in bytes |
| 1 | 270 | a compact count, that many amplitude bytes, then the length as 16 bit PCM in bytes |
| 2 | 2681 | i32 length in milliseconds, i32 30, i32 -9 to -13, then curves of floats over time |

Version 1 reads to the exact end of all 270. Its amplitudes come 50 to the
second, 49.6 on median over the dialogue, one per 20 ms, and the length field
runs at 43546 bytes per second of decoded sound against 44100 for 22050 Hz 16
bit mono, the difference being Bink's padding to whole frames. Version 2's
length is 98.5 percent of the decoded duration on median, for the same reason.
The rest of version 2, nearly all the dialogue, is not decoded: what follows the
three fields is floats starting two bytes out of alignment, with channels that
ramp smoothly over the frames, and two further i32s whose meaning is unknown,
the second of which is not the body's size.

## Fonts

A Font is a set of glyph rectangles over texture pages, after an empty property
block, in one of two layouts. Which one is decided by the package version, and
Shrek 2 ships both: its own fonts are in version 129 packages, and GUIFONTS.utx
is 122, but UT2003Fonts.utx is 120, WarfareFonts.utx 121 and UWindowFonts.utx
99, brought over from Epic's earlier games.

Version 122 and later:

```
index     character count, 256 in every font
per char  i32 StartU, StartV, USize, VSize, u8 page
index     page count, then Texture references
i32       0 or 1
index     character remap count, then u16 pairs
i32       0
```

Version 121 and earlier:

```
index     page count
per page  index Texture, index character count, i32 x4 per character
i32       characters per page: 32, 128 or 256
i32       0, 1 or 2; not there in version 99
index     character remap count, then u16 pairs
i32       0
```

All 120 fonts read to the exact end of their records with the layout their
version picks, every page is a Texture, and every glyph's rectangle lies inside
its page. The remap is empty in all of them, so a character's code is its
glyph's index. Version 99 was found by its records overrunning by exactly the
four bytes of the i32 after the pages. Drawn, a line of text comes out
readable in both layouts, which the rectangles alone could not show:
`ufont.py <package> <font> <text> <out.png>`.

**One font points at the wrong pages.** SHHugeInkFont, in SH_Fonts.utx and in
its copy in SHGame.u, lists four pages, but the third and fourth are PageA
again, while its PageC and PageD sit in the package unreferenced. The glyphs on
those two pages are codes 164 and up, the accented letters, so as stored they
draw fragments of PageA. Putting PageC and PageD in those slots brings some of
them right, Ö, ß and ü, and leaves others blank, so the slots are not simply
swapped either. The English game has no use for those letters, which is likely
why it shipped like this. The engine should draw what is stored.

## Static mesh lighting

Each placed static mesh carries its own baked light, in the StaticMeshInstance
record its actor names:

```
index      colour count, one per vertex of the mesh
per colour u8 R, G, B, A, with A 255
u32        revision of the colour stream
index      light count, then each: index the light actor, index mask length
           and the mask, a bit per vertex set where that light reaches it,
           then u32 applied
```

All 9260 records read to their exact end. Every mask is ceil(vertices / 8)
bytes, 73611 of them, and every light is a Light, Sunlight or Spotlight.

The channel order is R G B A, the opposite of RGBA8 texture pixels, and it was
settled by the data rather than by eye: over the 402 meshes a single coloured
light reaches, the hue of their colours read as R G B matches the light's
LightHue in 400. In Castle Siege a blue light of hue 170 leaves colours such as
(12, 12, 49), whose hue as R G B is 170 exactly.

The colour count is the mesh's vertex count for 9117 of the 9161 live instances
whose mesh resolves. The other 44 are one mesh, 11_FGM_Battle_SM.bush_wall,
lit when it had 143 vertices and shipped with 79: stale light an engine has to
throw away. 1563 instances are black throughout with no light reaching them,
lit, presumably, by their zone's ambient alone; 544 more are black throughout
although lights reach them, which is not explained yet.

## Text files

Configuration (`.ini`), localisation (`.int`) and KnowWonder's cutscene scripts
(`System/Cutscenes/*.int`) share one layout: `[Section]` lines, `key=value`
lines, and `;` comments. A key may repeat, and every line counts, in order.
Most files are single byte text, Windows-1252; a file starting with FF FE is
UTF-16 little endian, and four are: the subtitles, the bump line sets and the
menu text, the ones with typographic quotes and accented names in them.

All 199 text files in System parse with no line left over: 1376 sections and
19511 key=value lines. What they hold, and how it was checked:

- **Subtitles.** HpDialog.int and BumpDialog.int, one section each, map a
  sound's name to its line: `pc_nar_StoryBook1_16=[_Calm]Once upon a time, in a
  kingdom far, far away...`. 2859 of HpDialog's 2863 keys and 2024 of
  BumpDialog's 2028 are sounds in AllDialog.uax, and 2859 of that package's
  2947 sounds have a subtitle. The bracket starts the line with an emotion:
  `_Calm`, `surprised`, `mad`, `Question`, `sad`, `sneer`, and on 170 lines
  `* NULL VALUE *`, a field nobody filled in.
- **Bump lines.** BumpSet.int's 496 sections are the sets a character picks
  from when bumped into, `Line0=pc_bbs_bumpline_85` and on; 2645 of 2647 name
  a sound, and every one has a subtitle. GameData.int decodes the speaker codes
  in the names, BBS for Bandit_Boss and BND for Bandit.
- **Cutscenes.** 180 scripts, a section per sequence and `line_N=` commands:
  Cue, WaitForCue, Sleep, FlyTo, Say, WaitForSay, PlayAnim, Trigger, Teleport,
  PlayMusic and some forty more. Every PlayMusic names a track in `Music/`, 52 of
  52. Of 462 Say commands 430 name a sound; the other 32, all in the early
  swamp and hunt cutscenes, name sounds that are not in the game and have no
  subtitle either, lines cut from the game and left in the script.
- **Menus.** HpMenu.int, 701 lines of interface text.
- **Engine.int is Epic's.** Its sections are classes and its keys localised
  properties, but 33 of the 43 such keys, Console's messages and Weapon's
  DeathMessage, name properties Shrek 2's script no longer has: the file was
  never brought in line with KnowWonder's changes.

## A warning about parsers

A desynchronised parse will happily read garbage as opcodes and walk off the end
of the record into the rest of the file. If the parser builds nodes while it
does so, it allocates without bound and can take a machine down. Always carry
the record end as a hard limit, raise as soon as it is passed, and keep a node
budget per record. Run bulk passes under an external memory cap.

## What is known to be incomplete

- The editor's material preview meshes in Editor.u, TexPropSphere and
  TexPropCube, do not follow the static mesh layout. No level uses them.
- Whether and how PrePivot applies to skeletal meshes; see placing them.
- The sky dome is drawn lit like the rest of the scene; the game probably
  draws it unlit.
- The u32 at the end of each BSP lightmap.
- Which of a static mesh's two collision forms, its triangle tree or its
  collision model, the engine uses for which kind of check.
- Version 2 lip sync after a sound, the curves that move the characters'
  mouths in the dialogue.

- 0x42, standing alone five times, is read with one expression; nothing yet
  says what it is.
- The seventh index of struct-like records is zero everywhere seen, so its
  meaning is unknown.
- The fields between a class's struct header and its defaults block are still
  unknown: state masks, class flags, GUID, dependency and import arrays. The
  block is found by scanning instead.
