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
run out, control continues past the IteratorPop at the loop's end offset,
and an object out parameter is left None, as the engine's iterators clear it
before looking for each next element. KWCutController finds the pawn its
sequence plays by tag with a loop that breaks on a match; a sequence for MAIN,
which matches none, took the last pawn looked at instead, the camera's
BaseCamTarget, which stops following the hero while a cutscene has it: on the
swamp, Hamlet and Shrek's prison the camera stood still while a cutscene that
only talks or turns it played.

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

## Loading a level

`ffa-script level <System> <map.unr>` opens the game's script packages and the
map together, reads the Level record (`src/world/Level.cpp`, the layout in
docs/package-format.md) and builds every actor it lists as an object, its
properties being its class's defaults with its own block applied. Actors the
package keeps but the Level does not list are deleted ones and are not loaded.

**Measured.** All 29 levels of Shrek 2 load, 22606 actors, every one with its
class and no property that fails to decode. Read by the engine and by
tools/umap.py, each actor's name, class, Location and Tag agree, all 22606:
two readers written apart, one in each language, reading the same thing.

Every live actor carries a state frame, and in all of them both its node and
its state node are the actor's own class, with no code position: the level
was saved before anything ran. An actor's first state comes from the start up
sequence, SetInitialState, not from the file.

## Starting a level

`ffa-script start <System> <map.unr>` begins play on a level the way the
engine does when a map loads, in `src/world/World.cpp`:

1. Load the actors the Level lists and the game loads: both load bits set,
   which leaves out the editor's builder brushes and viewport cameras
   (docs/package-format.md, the Level object).
2. Spawn the game. Its class is a `Game=` option of the URL, else Default.ini's
   `[Engine.Engine] DefaultGame`, ShGame.ShGame. No level sets
   LevelInfo.DefaultGameType. LevelInfo.Game points at it.
3. Set LevelInfo's bBegunPlay and bStartup, and call the game's InitGame with
   the URL's options.
4. Send PreBeginPlay to every actor, then BeginPlay to every actor, then
   PostBeginPlay and PostNetBeginPlay, then SetInitialState, then clear
   bStartup.

5. Log the local player in: the game's Login, with the URL's portal and
   options, returns a PlayerController; a Player object becomes its Player,
   standing for the engine's Viewport, whose class has no script; then the
   game's PostLogin.

KnowWonder's Login does not spawn the player's pawn. The pawn is placed in the
level: Login looks for the KWPawn with bIsMainPlayer, spawns its
DefaultPlayerControllerClass and has it possess the pawn. PostLogin gives the
controller its HUD.

Then KnowWonder's engine puts the level in its game state and restores what was
saved of it. Nothing in script calls Actor.FilterForCurrentGameState,
PrePersistentDataRestored or PostPersistentDataRestored but their overrides'
supers, yet the game depends on them: FilterForCurrentGameState hides and stops
the actors of other game states, CutSceneTrigger turns itself off outside its
own, and KWPawn's PostPersistentDataRestored gives its upper channels their
bones and alphas. So every actor takes the three in turn after
SetInitialState, and a spawned actor takes the filter after its own start up
events. The game state comes from the LevelInfo's WorldInfo, an object the
engine makes of the class Default.ini names, ShGame.ShWorldInfo, whose
defaults hold the game states: with it KWGame's InitGame sets GSTATE000 on
every level, and no actor is out of it at the start.

From step 3 a spawned actor takes Spawned, PreBeginPlay, BeginPlay,
PostBeginPlay, PostNetBeginPlay and SetInitialState in its Spawn, and
Actor.SetInitialState marks it bScriptInitialized; the passes of step 4 skip
such an actor, so none takes an event twice. The order is the engine's
published behaviour, not something the data says.

Spawn names the actor after its class with the next free number, places it at
the spawner's Location and Rotation unless given others, copies the spawner's
Instigator, and sets its Tag to the class name unless given one: 1359 of the
swamp's 1650 placed actors still carry that Tag, which the editor's spawn gave
them. It refuses a bStatic or bNoDelete class. What it does not do yet is
collision: the engine moves a new actor out of whatever it would sit in, or
refuses it when there is no room. Destroy refuses bStatic and bNoDelete
actors, sends Destroyed, tells the owner LostChild, frees what the actor owned,
and marks it bDeleteMe; it stays in the list, and the iterators skip it.

**Corpus check.** Across all 29 levels, 20115 actors are loaded and 2491 are
the editor's. InitGame and the start up passes run 100749 events with no
script error, spawn 6603 actors, coins, AI controllers, potions, shadow
projectors, and destroy 2. On Shrek's swamp 222 actors end in a state, pickups
Pickup, triggers NormalTrigger. Two errors came up on the way, both the VM's.
BanditBoss declares an int MaxHealth over KWPawn's float one, and KWPawn's
PostBeginPlay sets its own: both variables live in the object, and each
class's code reaches its own. And with no game, every actor not
bGameRelevant destroyed itself in Actor.PreBeginPlay, asking a mutator that
did not exist, and script retried what it could not spawn: 6780 Spawn and 1644
Destroy calls on the swamp alone, where with the game there are 131 and 2.

Every level but Entry, which has no actors to play, logs its player in, with
no script error, and each controller is in PlayerWalking with its pawn as view
target:

| Levels | Controller | Pawn |
|---|---|---|
| 1, 2, 4_FGM_Office, 6 (all three), the beanstalk bonuses, the books, Credits | ShrekController | Shrek |
| 3_The_Hunt, all four parts; 5_FGM_Donkey | DonkeyController | Donkey |
| 4_FGM_PIB, 7_Prison_Donkey, 8_Prison_PIB | ShrekController | PIB |
| 9_Prison_Shrek, 11_FGM_Battle | ShrekController | ShrekHuman |
| 10_Castle_Siege | ShrekController | Mongo |
| SH2_Preamble | KWHeroController | DummyPlayer |

The natives still missing on the way are animation (AnimBlendParams, HasAnim,
LinkSkelAnim), attachment (AttachToBone, SetRelativeLocation and Rotation),
projectors, ParticleEmitter.Trigger, and those that need collision, SetLocation
and Move.

## Running a level

`ffa-script run <System> <map.unr> <seconds>` begins play as above, then runs
frames of a thirtieth of a second (`World::tick`). A frame scales its time by
LevelInfo.TimeDilation and advances TimeSeconds, then visits every actor that
is not bStatic, in list order:

1. the local controller's PlayerTick;
2. Tick;
3. its state code, run on to its next wait (`VM::processState`);
4. its timer: TimerCounter counts while TimerRate is set, and on reaching the
   rate Timer fires once, however many periods the frame covered, keeping the
   remainder when bTimerLoop is set and clearing the rate when not;
5. its LifeSpan, which destroys it on running out.

An actor spawned during a frame first ticks in the next. Physics is not run:
it needs collision.

Of the eight latent natives, Sleep waits its seconds. The other seven,
FinishAnim, FinishInterpolation, MoveTo, MoveToward, FinishRotation,
WaitForLanding and WaitToSeeEnemy, need animation or physics. Until they exist
each waits one frame and is counted as missing: state code that loops on one
then gives way each frame, as it would in the game, where without it the loop
would spin within a single frame.

Traces go through the level's collision (`src/world/Collision.cpp`): the BSP,
the terrains, and the static mesh actors that block traces, with the actors'
cylinders when asked for: those a projectile would hit, bProjTarget or blocking
both actors and players, as the game's own traces need. A cutscene's trigger
over the swamp's pond, which collides but blocks nothing, was hit before, and
Shrek's trace for what he stands in (TraceMaterial) found it and not the
water. Trace returns the actor hit, the LevelInfo for the BSP, and zero
vectors when nothing is hit; its out Material is the BSP surface's, or on a
terrain the layer that shows most at the hit, each layer over those before by
the weight of its alpha map, with the MaterialType its package gives it. The
swamp's pond bed is its gravel layer, MaterialType 6, water: ShHeroController
reads it every frame to wade, 150 a second where Shrek runs 550, its
MovementAnims the WadeAnims. TraceActors yields the actors along
the line nearest first and then the BSP's hit as the LevelInfo, which the
data asks for: the camera's script, BaseCam.bShouldBlock, tests a hit with
IsA('LevelInfo'). FastTrace counts world geometry only. SetLocation moves the
actor and updates its Region, sending ActorLeaving, ZoneChange and
ActorEntered when the zone changes. Two things are approximated and counted as
such: a trace with an extent is traced as a line, and an actor that collides is
moved by SetLocation without testing that it fits. In twenty seconds of every
level that happens 650 and 45 times.

The camera shows the collision at work. KnowWonder's camera, ShCam, is an actor
that traces from Shrek back to where it wants to be; at the swamp's start that
place is inside the bank behind him, the terrain at 49 against the camera's
-93, so it settles 115 units from him instead of 200.

Physics runs after the timer, in `src/world/Physics.cpp`, on boxes swept
through the collision (`Collision::boxCheck`): the BSP's faces that bound solid
(one unit behind the polygon's middle is solid, one unit in front is not), the
terrains' triangles, the static meshes' triangles, the colliding brushes, and
the cylinders of actors that block, grown by the box: its half width added to
the radius and its half height to the height, round in plan as the engine
clips them (the script's `LineIntersectCylinder` is that clip, its variables
the same); as squares, the swamp's rock_step, a cylinder of 200, kept Shrek
from the four leaf clover in its lee. A box against a triangle is
exact: the segment against their Minkowski sum, on the 13 axes that can
separate them. Path nodes check it: a box of a node's size dropped from the
node sinks a median 2.50 before it touches, the same as the line trace says.

The modes are the engine family's, with its constants, none of them in the
data: Walking with friction, acceleration up to GroundSpeed, a step up of 35,
sliding along walls by their face as it stands, their slope left out, and the
floor kept 1.9 to 2.4 below, or Falling when it is gone; a pawn whose box
touches a steep bank at its edge while its middle stands on the ground keeps
standing. Pressed into the swamp's steep banks, Shrek slid up the slope's own
normal onto it, fell, landed and walked into it again, a round every four
frames; now he stands, and jumping onto the bank he slides back down in the
air, as the game does; Falling with gravity from the PhysicsVolume, TerminalVelocity, and
Landed on a floor whose normal has Z of 0.7 or more, HitWall otherwise, each
offered to the controller first; Flying, Rotating, Projectile and Trailer.
Swimming, Karma, MovingBrush and the rest are counted as missing. A move ends
in Touch and UnTouch with the actors whose cylinders it begins or stops
overlapping, keeping each actor's Touching array, which TouchingActors reads.
A player's pawn is stopped by what has bBlockPlayers, anything else by
bBlockActors, and two touch unless each stops the other: pickups, the wanted
poster and the save fairy block actors and not players, and drawn as solid
against Shrek, the waffle could not be taken, the poster not torn down and
the fairy not touched to save. A static mesh actor with bUseCylinderCollision
collides by its cylinder or box only, not also by its triangles, which still
stopped Shrek at the energy bars before he picked them up.

Over twenty seconds of every level, no pawn falls through the world. Three
fall more than 500 units, and they are not collision: Puss on The Hunt's
parts 2 and 3 waits on a cutscene mark right over a BouncePad, whose Touch
throws him to its target. Before the colliding brushes were in, Shrek on
6_Hamlet_Mine and Puss on The Hunt part 4 fell, standing on BlockingVolumes.

**Movers.** Nearly every mover in the game is a static mesh: 241 of the 262
movers the levels load are drawn as one, and they collide as one, their transform
taken again at each query. The script does the planning, InterpolateTo
setting OldPos, OldRot, KeyNum, PhysRate and bInterpolating; MovingBrush
physics moves the mover from OldPos to BasePos + KeyPos[KeyNum] as PhysAlpha
goes from 0 to 1 at PhysRate, eased in and out as 3a^2 - 2a^3 for
MV_GlideByTime and straight for MV_MoveByTime, its rotation the same way, the
short way round with bUseShortestRotation. What stands on the mover, its Base,
goes with it. At the end bInterpolating is cleared and FinishedInterpolation
sent, and the latent FinishInterpolation waits for that. KnowWonder's
MV_SpringByTime moves straight for now. Movers wait to be triggered, so
`run ... --event LightMover1` sends an event first, through the game's own
TriggerEvent: the five swinging lights of the first story book then go
through their three keys in 7 seconds each, round again at 21.

**Animation.** `src/world/Animator.cpp` runs each actor's channels; how a
frame becomes a pose is not done yet, but the timing script waits on is. A
MeshAnimation is read in the engine to its exact end (layout in
tools/uanim.py) for its sequences: name, groups, frame count and frames a
second. An actor's sequences are those LinkSkelAnim added, newest first, then
its mesh's default animation, which a SkeletalMesh names right after its
reference skeleton: 138 of the game's 141 do, 126 of them one of the same
name. A path names more than one object here, Shrek being both a SkeletalMesh
and a MeshAnimation in ShrekCharacters.ukx, so a lookup by path matches the
class as well. DynamicLoadObject reaches every package of the game, not only
the linked ones, through `VM::loadObject`, and takes the object of the class
it is asked for: CitFemale is both a mesh and an animation.

Some of the engine's classes have no script at all, Mesh, SkeletalMesh,
StaticMesh, MeshAnimation, Sound and Font among them: the game's packages
import them, and none exports them. Script names them all the same, in casts
above all, and a cast to a class that is not there gives None. KWPawn's
SetActorMeshes loads a mesh as `Mesh(DynamicLoadObject(name, class'Mesh'))`
and links it, which is how the citizens in the Fairy Godmother's office take
the mesh their citizenType names, two women and two kinds of men; with the
cast giving None they all kept their class's default, the same man in red.
The linker makes each of these classes once, empty, under its parent in the
engine's own hierarchy (SkeletalMesh under Mesh under Primitive under Object),
for a cast, an import and an object's class alike.

GetAnimParams and GetAnimFrame give the frame counted in frames, as every
script of the game uses it: an attack hits between the frames its AttackInfo
names, human Shrek's punch1 from 6 to 12, FoodThrow throws past frame 17, and
SuperSoaker indexes its frames with it. With the frame given from 0 to 1 no
attack in the game ever hit.

PlayAnim and LoopAnim run a channel's frame from 0 to 1 at Rate times the
sequence's frames a second over its frame count; looping a sequence that
already loops changes only its rate. A tween holds the first frame for its
time; PlayAnim's and LoopAnim's own tween time does not hold anything but
blends the new sequence in from the channel's pose before, while it already
plays, and the engine's movement animation blends over BlendChangeTime the same
way. A looping sequence goes from its last key back to its first by its end.
At the end of a play or of a round AnimEnd(channel) goes to the actor
when the channel's notify is on, as every channel's is from the start; no
script of the game turns one on, yet the cutscenes play on channels 20 and 21
and wait for their AnimEnd, and KWPawn's AnimEnd passes over the channels it
does not want. With channel 0 alone notifying, Shrek's attacks also froze on
their first frame, and play through with every channel, which someone who
knows the game confirmed in the window. It goes to the pawn's controller instead while the controller
has bControlAnimations, which a cutscene's PlayAnim sets. A channel above
0 adds nothing until AnimBlendParams gives it an alpha: KWPawn blinks on
channels 34 to 39, set to its lid and brow bones, and with an alpha of 1 by
default the blink took the whole skeleton to the reference pose for a few
frames, a T pose. FinishAnim waits
for the channel to stop, ending a loop at the end of its round, and the data
settles one thing about it: KnowWonder's BounceController plays its pawn's
idle and finishes it in a loop, Pawn.PlayAnim(IdleAnim); FinishAnim(); goto
'Begin', which spins forever unless a controller's FinishAnim waits on its
pawn's animation; so it does. The game's animations carry no notifies of their
own; script adds them with AddNotify, in AddAnimNotifys, which script only
ever calls as a super: the engine sends it, and here it is sent when an
actor's animation is first set up. That is how Shrek's footsteps sound.

Pawns with bPhysicsAnimUpdate are animated by the engine as they move: walking
faster than 10 units a second, the one of their four MovementAnims for the way
they go against the way they face, looped on channel 0 and blended in over
BlendChangeTime; stopped, KnowWonder's IdleAnimName, which their
ChangeAnimation keeps up to date, apparently for this. Leaving the ground
going up is a jump: the engine sends PlayJump, which no script calls, and
KWPawn plays its take-off on channel 1 over the whole body; walking off an
edge is the Falling event instead. Without it the jump had no animation, only
the landing on channel 1 when it came down.

KnowWonder's pawns climb ledges by their script, KWPawn.Mount(Delta, A) and
its states Mounting and MountFinish, once the engine has found a ledge and
called Mount with the move to its top and what is under it: none for the
level, which a MountVolume around the pawn must make climbable, else an actor
with bIsMountable. The engine's search is not in the data; it is modelled
(world/Physics.cpp, tryMount): in the air, or on the ground when the player
pressed jump, something in the way, and on it a floor higher than a step and
no higher than MaxMountHeight (256 for the heroes) above the feet, where the
pawn fits. The script then plays the climb by how high it is (climb32,
climb64, the big climb, or for a lily pad, MA_StepUpOnlyMount, stepup) and
moves the pawn up by GetAnimTime, the channel's way through its sequence, 0
to 1. Wading on the swamp, where ShHeroPawn's DoJump will not jump, Shrek
pressed against a lily pad steps up onto it with stepup2 and stands on it,
from 249 below to 208. The level's own geometry is given as the LevelInfo, which Mount lets be
climbed only where the pawn touches a MountVolume; None is anywhere at all,
which only the ladders' Mount asks for, and given None for the level Shrek
climbed every rock and dock of the swamp. A volume is touched where its brush
is, inside every face's plane by the pawn's reach, not as a cylinder about its
middle. What the pawn runs or jumps into counts, from any side: met from below,
the top looked for is right above. Volumes are passed through on the way down
to it, as the swamp's vine has a blocking volume over it, and what is only
hung from (MA_UnAbleFinishMount) needs no room on top. A static mesh is world
geometry, yet one with bIsMountable is still the actor Mount is given, else
the vine counted as the level and wanted a MountVolume. Once a pawn has let go
of something it does not take hold of it again until it has walked: letting
go of the vine, Shrek fell past it and caught it at once. With GetAnimNumFrames,
by which the hang lifts the pawn over the first eighth of jumptohang, Shrek
jumping from the swamp's stump catches the vine (jumptohang2, hangidle2),
shimmies along it with the strafe keys (shimmyright2, 120 a second), and lets
go with back. The top must be close in front, within 24 of the pawn's face:
the climb carries the pawn only some 22 forward, the script keeping the
height of the move it is given and not its length, and from a top found 80 in
Shrek rose, stayed short of the edge, fell back and caught it again, a round
every three seconds by the swamp's mount volumes.

Karma is done only for what hangs (`src/world/Karma.cpp`): an actor in
PHYS_Karma held to the world by a KBSJoint, through more joints if it hangs
from another, swings about that joint as one rigid body, the punching bag with
its rope. KAddImpulse turns it about the joint by the impulse's moment over
the members' inertia as points, the impulse taken at a sixteenth of its size
over KMass, which is not in the data: tuned so that Shrek's punch,
ForceFromHit 10000, swings a bag of KMass 2 some 30 degrees. Gravity brings
it back and KAngularDamping slows it; still and hanging straight, it sleeps
(KIsAwake). A free body is left where it is. A static mesh that SetCollision
turns off, or that is destroyed, no longer blocks: the bag broken by its last
punch kept blocking where it hung.

The vine itself is the ShimmyVine that the ShimmyStatVine spawns and sizes by
SetDrawScale3D, 1.2 by 1.75 by 1.75 on the swamp; without it the vine stopped
short of its stump. When the engine switches
is modelled, not measured. Holding forward on the swamp, Shrek runs `run`,
falls on channel 1 in `jumploop`, lands with `jumplandtorun`, idles in `Idle`,
and blinks on channel 34. Twenty seconds of every level start 10605 sequences,
send 89681 AnimEnd and 1147 notifies, with no script error; the 384 sequences
not found are asked for by name None, or are not in the data.

**Poses.** `src/world/SkeletalMesh.cpp` reads a skeletal mesh's header,
reference skeleton, default animation and first LOD model, and poses it the way
tools/uanim.py measured: keys interpolated linearly, a zero rotation key the
reference's, the root's rotation taken as it is and every other bone's
conjugated, each point moved by the weighted sum of its bones' change from the
reference pose. `ffa-script poses` checks it against the Python: every
skeletal mesh with a default animation, 138 of 141, at three sequences each,
skinned 37 percent of the way through, every 17th point, 5941 points, all
within 0.0004 units of tools/uanim.py's skin(), the rounding of the print.

An actor's pose is its channel 0 sequence, with each channel above that has an
alpha laid over it, from its blend bone down or over the whole skeleton.
GetBoneCoords and GetBoneRotation give a bone in the world, through the mesh's
RotOrigin, MeshOrigin and MeshScale and the actor's Location, Rotation and
scale, and its PrePivot, added as it is: the characters whose meshes stand
high in their collision cylinders carry one that takes them to its bottom,
within three units for human Shrek (-19), Puss (-9.5), the Steed (-17.5) and
the exploding pumpkin Donkey (-7). A pawn whose mesh has no MeshOrigin is
taken down by its CollisionHeight: those meshes have their feet at their
origin, and every pawn with one stood exactly its cylinder's height in the
air, Bandit 40, FatKnight 33, the rats 27 and 34, the prisoners 150, and the
factory's HazMatShrek 38, whose mesh is Shrek's, the same skeleton and
points, with a MeshOrigin of 0 where Shrek's is -49; the game's frames show
him on the floor. That rule is inferred from those, not read.
AttachToBone makes the actor the attachment's Base with its AttachmentBone
set, and each frame, after the animation, an attachment is put on its bone at
its RelativeLocation and RelativeRotation. A rotator is made from axes by the
inverse of rotationAxes, which the tests take round trips through.

**Input.** `run ... --hold MoveForward` holds a key for the whole run, by its
alias in DefUser.ini: `Axis aBaseY Speed=+1200`. Each frame, before the
controller's PlayerTick, every controller variable declared `input` is set to
zero, the variables with property flag 0x4, which are exactly the axes and
buttons (aBaseY, aForward, aStrafe, bLook and the rest, and KnowWonder's
aArrowUp and aArrowRight), and each held axis then gets its speed. A held
key's axis is its speed, not scaled by the frame: that much is assumed. The
controller's InitInputSystem is called when it gets its Player, as the engine
does for a local player, or it has no PlayerInput object. From there the
game's own script does the rest: ShPlayerInput turns aBaseY into aForward,
PlayerWalking.PlayerMove makes the acceleration along the pawn's facing, and
ProcessMove gives it to the pawn, which the walking physics bounds by its
AccelRate. After a walking move the velocity is what the pawn actually moved.

Holding forward on Shrek's swamp, Shrek walks south at 550, touches two
cutscene triggers, picks up a coin, which goes with its twirl effect, steps
off a ledge, falls 43 units and lands, and stops against the collision
cylinder of a lily pad, AmbientLily8, at the pond. Ten seconds of it on every
level run 4051526 events with no script error.

**Cutscenes.** KnowWonder's cutscenes are script: a KWCutScene, triggered,
reads its sequences from System/Cutscenes/<name>.int through Localize, one
sequence for each actor it takes over, and a KWCutControllerII possesses each
and runs its lines, `PlayAnim SipDrink`, `Cue _CAMERA_4`, `WaitForCue
StartPart2`, `Say pc_shk_SwampIntro3_9`, `Sleep 1.5`, as actions; its exit
lines run when it ends, `SaveGame 1` or `Donkey Follow Shrek`. What the engine
gives them: Localize, which reads the System directory's localisation files,
case not minded, UTF-16 with its byte order mark or Latin-1; SetPropertyText,
which the actions set their arguments with, a name read up to its first space
as the engine reads one, for `PlayAnim IdleStart Loop` hands BaseAnim
"IDLESTART LOOP" and the game plays IdleStart; the controller's AnimEnd and
every channel's, above; and the moves of the AI, below, as the actors walk to
their marks with MoveToward. With these the opening cutscene of every level
plays to its end: the swamp's NEWSwampIntro in 68 seconds of level time, the
carriage's in 38, the hunt's in 33, Hamlet's in 49, and the player has the
pawn back after it. Space bypasses one, BypassCutscene, an exec function of
the HUD, which the console reaches after the controller and its pawn.
A line lasts as its sound does: GetSoundDuration counts what a Bink file's
packets say they decode to, without decoding it, and DeliverLocalizedDialog
and WaitForSay wait on that.

**Sound.** `src/world/Audio.cpp` takes PlaySound, PlayOwnedSound and
DemoPlaySound, StopSound and the music natives, filling what a call leaves
out from the actor's TransientSoundVolume, TransientSoundRadius and
TransientSoundPitch, and hands them to the world's audio sink. ffa-play's is
a mixer on SDL (`apps/Mixer.h`): each sound a voice from its actor, falling
off linearly to its radius and panned against the camera's right; a slot
other than SLOT_None replaced, or kept with bNoOverride; and every actor's
AmbientSound looping within SoundRadius times 25, as the engine family
measures it (the swamp's water has 50 to 90, its birds 400), at SoundVolume
over 255 and SoundPitch over 64. Sounds are decoded when first played and
kept (`src/audio/SoundBank.cpp`); a line of a few seconds takes a few
milliseconds. Music, Ogg Vorbis, is not played yet.

**The console.** ConsoleCommand answers what the game asks of it: open,
start and travel go to a level, exit and quit end the game, getcurrentres
tells the screen; get <class> <variable> reads the configuration,
Default.ini then DefUser.ini, ini:Engine.Engine.ViewportManager naming the
class an engine setting names, WinDrv.WindowsClient, whose
FullscreenViewportX, 800, the options page sizes its tabs and the preamble
chooses its movies by; set changes one for the session, the class Input being
the key bindings; keyname gives a key's name by its number in EInputKey and
keybinding what it is bound to, as the options page lists the controls.
What is set is not written back yet.

**Movies and levels.** The game begins as Default.ini's LocalMap says,
SH2_Preamble.unr: its CutFactory sends its PlayerStart's event, MovieManager,
and the level's SHMovieManager plays the logos on the HUD's Movie, one each
time the last ends, and then LoadLevel goes to Book_FrontEnd.unr, the menu.
The engine gives every HUD its Movie, an object HUD's PostBeginPlay finds
there (`src/world/Movie.cpp`). A movie is a Bink file of Movies/, its name's
case not minded, as long as its header's frames over its frame rate say:
DW_LOGO 21.7 seconds, ACTIVSN 10.4, KWlogo 12. At its end, or at StopNow,
which Space and Escape ask for while one plays, the Movie's MovieEnded
relays it to the MovieManager. ffa-play shows its pictures over the frame,
fitted and in black, decoding up to the frame its time has reached
(`src/audio/BinkVideo.cpp`), and plays its stereo sound from the same file. ServerTravel leaves the next level in
Level.NextURL, KWGame adding the game state, Book_FrontEnd.unr?GameState=
GSTATE000; once NextSwitchCountdown runs out the world asks for it, as do
ClientTravel and the console's open, and ffa-play loads it in place of the
one playing, the URL's options over the level's own.

**The GUI.** `src/world/Gui.cpp` is the engine's side of GUI.u. After the
login the engine makes the player's GUIController, of Default.ini's class
(ShGame.ShGUIController), with the Player as its ViewportOwner, and its
InitializeController registers the styles and fonts in script. Pages open
through OpenMenu: the menu level's cutscene does GotoMenu SHGame.ShFEGUIPage
once its book has turned its first page, and the in-game menu opens on
Escape, the game's EscHandler. A page's controls are not in its data as an
array: its variables hold templates, GUIButton cBtnOptions and the rest, made
in the page's package with only their flags and delegates, and
InitializeControls gives the page a copy of each, the template's delegates
bound to the page, as its Controls, in the order the variables are declared;
the page's script places them (ShFEGUIPage from its SHMenuBook's sizes).
WinLeft, WinTop, WinWidth and WinHeight of 1 or less are parts of the screen,
or with bBoundToParent and bScaleToParent of the control they are on, and
more are pixels. Each frame, after the HUD and on its Canvas, the pages of
the menu stack are drawn: a page's Background, then its OnDraw (the in-game
menu draws its book there, through Canvas's DrawActor, over the depth
cleared), then each control by RenderWeight, a button through its style's
Draw and DrawText, a label its caption, an image its image, and last the
mouse cursor. A style's Draw and DrawText set the Canvas to the state's
colour, render style and font, the font of the largest resolution not over the
screen's width, before its OnDraw and OnDrawText, which Shrek's styles answer
themselves and leave the font as it is; the controls call them as the
engine's own, past the script functions of the same name some styles declare.
The mouse watches the control under it and clicks it with OnClick; the keys
go to the page's OnKeyEvent, then Escape closes the menu, Enter clicks what
is watched, and the arrows, the pad's among them, move what is watched to the
nearest control that way, Shrek's buttons being all bNeverFocus. Calling a
delegate by name through super runs the parent's own body, not the delegate,
as ShInGameMenuGUIPage's Internal_OnDraw ends with super.OnDraw(Canvas); and
a delegate set to None is unbound, its own body run, as Tab_OptionsSound sets
each control's OnActivate. A tab control draws its tab buttons and the
active tab's panel only, not the other panels it holds: Shrek's sound tab
shows its controls again whenever it is drawn. Its buttons go in a row along
its top, TabHeight high, each as wide as its caption in its style's font with
half the height either side; the panels keep their places, and the control's
own style is not drawn, only a BackgroundStyle or BackgroundImage.
While Level.Pauser is set, as the in-game menu's SetPause sets it, the level's
time stands and only the player's controller and what is bAlwaysTick tick and
animate, the menu's book among them; the LevelInfo keeps the clock, which the
menus' fades time themselves by.

**Lip sync.** A sound with lip sync (docs/package-format.md, Sounds) moves
the face of the actor that plays it, from when it is played until its last
frame or StopSound. The Animator lays the face over the body's pose: each
viseme and blink pose a weight's way from its rest frame to its full one,
applied as the change from the rest frame, so the head stays where the body's
sequence has it. A phoneme's weight goes to its viseme, summed and held to 1:
0 and 1 to E, 2 and 3 to AI, 4 to O, 5 to WQ, 8 to FV, 11 to MBP, 14 to L, the
rest to CDGKNRSthYZ; a mesh without WQ takes U for it and without L,
CDGKNRSthYZ. Version 1's loudness opens the mouth as AI. The head and eye
channels and the emotion a subtitle starts with are not used yet.

**AI.** `src/world/AI.cpp` does the engine's part of what controllers do. A
controller other than the player's turns its pawn toward its Focus, or its
FocalPoint, which KnowWonder's TurnToActor and TurnToPoint set: its
DesiredRotation is the way to it, and the pawn's physics turns to that, in
yaw, at its RotationRate; a controller looking at nothing leaves the pawn to
its own rotation. MoveTo and MoveToward accelerate the pawn at its AccelRate
toward the place, or the actor wherever it goes, until it is within its
radius, half the pawn's, across and its height and a step up or down, which
sets bMoveToSuccess, or until MoveTimer, one second and a third over the
time the distance takes at the pawn's speed, runs out. FinishRotation waits
until the pawn is within 2000 of its controller's yaw, WaitForLanding until
it does not fall, each no more than a few seconds. actorReachable and
pointReachable walk the way there in steps of the pawn's radius with its box,
a step up, across and down to a floor, which must be within a drop of 160 and
walkable; nothing farther than 1200 is reachable at once. FindPathToward
gives the actor itself when it is reachable, else the first node of the
cheapest route over the level's own paths: each NavigationPoint's PathList of
ReachSpecs, from the nearest nodes the pawn can walk to, to the node itself or
the nodes within 1200 that see the goal, along specs the pawn's cylinder fits,
leaving out ladders, specials and proscribed ones; the route goes into
RouteCache with RouteGoal and RouteDist. A move that cannot be made, its pawn
or target gone, still waits a tick, as every latent call does. A pawn that
has hardly moved for three frames while it means to steps round: to the
nearer side, then the farther, at one and a half, three and five times its
radius, where its box is free and free ahead, and on from there; stuck again,
it tries again, the other side first, as Donkey and Puss walking out of the
factory's elevator in its cutscene step round each other. That is this
engine's stand-in for the AI's adjusting round walls. LineOfSightTo looks from the pawn's
eyes at the other's middle and head; CanSee adds its SightRadius and its
PeripheralVision. The distances and the reach test are this engine's own, the
behaviour the published UE2's.

**Corpus check.** Twenty seconds of every level run 8014835 events with no
script error. Thirty seconds of Shrek's swamp take 2.4 s. Gnats and dragonflies
spawn and wander, 32 KWCutScene objects come up, the camera ShCam runs in
StateStandardCam, and Shrek, Donkey and Fiona idle. With collision in, what the
frames call most without having it is animation, projectors and attachment to
bones. Twenty seconds of the slowest level, 6_Hamlet, take 10.4 s.
