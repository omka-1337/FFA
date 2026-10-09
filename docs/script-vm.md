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
cylinders when asked for. Trace returns the actor hit, the LevelInfo for the
BSP, and zero vectors when nothing is hit. TraceActors yields the actors along
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
the cylinders of actors that block, as boxes. A box against a triangle is
exact: the segment against their Minkowski sum, on the 13 axes that can
separate them. Path nodes check it: a box of a node's size dropped from the
node sinks a median 2.50 before it touches, the same as the line trace says.

The modes are the engine family's, with its constants, none of them in the
data: Walking with friction, acceleration up to GroundSpeed, a step up of 35,
sliding along walls, and the floor kept 1.9 to 2.4 below, or Falling when it
is gone; Falling with gravity from the PhysicsVolume, TerminalVelocity, and
Landed on a floor whose normal has Z of 0.7 or more, HitWall otherwise, each
offered to the controller first; Flying, Rotating, Projectile and Trailer.
Swimming, Karma, MovingBrush and the rest are counted as missing. A move ends
in Touch and UnTouch with the actors whose cylinders it begins or stops
overlapping, keeping each actor's Touching array, which TouchingActors reads.

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
the linked ones, through `VM::loadObject`.

PlayAnim and LoopAnim run a channel's frame from 0 to 1 at Rate times the
sequence's frames a second over its frame count; looping a sequence that
already loops changes only its rate. A tween holds the first frame for its
time; PlayAnim's and LoopAnim's own tween time does not hold anything but
blends the new sequence in from the channel's pose before, while it already
plays, and the engine's movement animation blends over BlendChangeTime the same
way. A looping sequence goes from its last key back to its first by its end.
At the end of a play or of a round AnimEnd(channel) goes to the actor
when the channel's notify is on, channel 0's from the start. A channel above
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
ChangeAnimation keeps up to date, apparently for this. When the engine switches
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
scale; PrePivot is left out, as it is not settled for skeletal meshes.
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

**Corpus check.** Twenty seconds of every level run 8014835 events with no
script error. Thirty seconds of Shrek's swamp take 2.4 s. Gnats and dragonflies
spawn and wander, 32 KWCutScene objects come up, the camera ShCam runs in
StateStandardCam, and Shrek, Donkey and Fiona idle. With collision in, what the
frames call most without having it is animation, projectors and attachment to
bones. Twenty seconds of the slowest level, 6_Hamlet, take 10.4 s.
