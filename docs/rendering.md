# Drawing a level

`ffa-play <System> <map.unr>` plays a level in a window: SDL2 for the window
and input, OpenGL ES 2 to draw, what every PortMaster handheld has. The level
begins as the engine begins it (`src/world/Session.cpp`, see
docs/script-vm.md), the world ticks once a frame drawn, by the time the frame
took, as the engine does (a fixed thirty a second drew every second frame
twice, characters and camera stepping), and the
keys held reach the player's controller through the game's own bindings in
DefUser.ini. `--shot <png> <seconds>` runs that long in a hidden window, saves
the frame and quits, which is how the pictures here were checked.

## Input in the window

Keys and mouse buttons go through their bindings in DefUser.ini, aliases
within expanded: an Axis adds its speed to an input variable while held, a
Button holds a bool, a Count counts, and any other command runs once on the
press as an exec function of the controller, else of its pawn, as RightMouse's
Jump does. The mouse moves MouseX and MouseY, `Count bXAxis | Axis aMouseX
Speed=6.0`: the count matters, as PlayerInput's SmoothMouse divides by it.
How many counts the engine's input system gives a pixel is in its native code;
here a frame's movement is taken over the frame's time, scaled so that 600
pixels a second turn Shrek's camera some 90 degrees a second, a value tuned
and not measured. From there the game's script does it: ShPlayerInput gives
savedATurn, and the camera, ShCam in StateStandardCam, turns its destination
by it times its set's fRotSpeed, 8 for Shrek. Shrek turns with it, as in the
game, where the camera does not go round him freely: KWHeroController's
UpdateRotation sets the controller's DesiredRotation to the camera's every
frame, and the engine turns the pawn toward its controller's DesiredRotation
at its RotationRate, 80000 a second in yaw for a KWPawn (world/Physics.cpp).
Tab lets the mouse go.

## The camera

The view is the one the game asks for: the controller's PlayerCalcView, its
out parameters read back (`VM::eventOut`). KnowWonder's controller takes it
from its BaseCam, ShCam on Shrek's levels, and updates it only when its
bViewUpdated differs from bTicked, a flag the engine flips on every actor it
ticks each frame; the world does that now. Unreal's X ahead, Y right and Z up
become GL's eye space as right, up and back. The field of view is the
controller's FovAngle, horizontal.

## What is drawn

Everything decoded is the same data the Python viewer draws, and drawn the
same way, so what was confirmed by eye there holds here, but for the light,
which was measured since against the game's frames (below); the viewer still
adds the ambient to the baked light:

- **The BSP**, its render sections' polygons, a texel position being the
  offset from the surface's base point along its texture vectors over the
  texture's size; the lightmap coordinates are the section vertices'. Lit as
  texture × lightmap × 2, the zone's ambient being in the lightmap already
  (below); unlit surfaces take the texture alone.
- **Terrains**, from the engine's copy of the grid, each layer laid over the
  ones before by its weight from its alpha map, its texture coordinates the
  world position against its TerrainMatrix; lit by the baked vertex light × 2,
  which holds the zone's ambient too.
- **Static meshes**, at Location + R S (v - PrePivot), a section a material,
  the actor's Skins over the mesh's Materials; lit by the StaticMeshInstance's
  vertex colours and the zone's ambient as the BSP holds it, together × 2;
  bUnlit full bright. Where those colours are black
  throughout although lights reach the mesh, they are computed from the lights
  and their masks, by the formula the stored colours follow.

- **Skeletal meshes**, each actor drawn as a mesh skinned on the CPU every
  frame from its pose, its faces by material, the actor's Skins over the
  mesh's materials. Lit by its MaxLights lights strongest where it stands,
  with the same falloff as the static meshes' baked light, but evenly, as the
  game's frames show its characters: three times the zone's ambient, the
  AmbientGlow once, and by half Lambert a tenth of a Sunlight's brightness
  and 1.2 of a point light's. Tuned to two sets of frames, not taken from the
  data. On the swamp, lit by its ambient and two suns, Shrek's shirt is 160
  and 150 on its two sides and his skirt 178, about 0.8 of their texture in
  every channel; this model gives 157 and 177, and 174, where plain N.L, as
  the static meshes take light, gave 130 and 226, and yellow. In the Fairy
  Godmother's office, with no ambient and point lights of 150 to 255, the
  citizens' red clothes are about their texture's own brightness; this gives
  1.0 to 1.3, where a tenth for every light, as first tuned on the swamp alone,
  drew them black. bUnlit full bright. The normals come from the posed faces,
  turned to face away from the mesh's middle, as the winding is not settled. Holding forward on the swamp
  (`--hold W`), Shrek runs into the pond past the lily pads, arms swinging.

## The ambient, in the baked light and out of it

**The lightmaps and the terrains' light hold the zone's ambient.** Where no
light reaches, a terrain's light is flat, and its value follows the zone's
AmbientBrightness on every level that has a terrain: 64 leaves 52 of 255, 32
leaves 36, 16 leaves 25, and none leaves 0, while Hamlet's 64 of hue 150 and
saturation 222 leaves (45, 46, 50). The ambient's colour at 0.41
sqrt(AmbientBrightness / 255) gives 52, 37, 26, 0 and (45, 48, 52). The
lightmaps' darkest texels, through DXT1, follow the same: 16 leaves 21 to 24,
32 leaves 32. So the BSP and the terrains are drawn as their light doubled,
nothing added; with the ambient added again, as first drawn, the swamp's
shadows were washed out: the ground in the shade of its trees and the trunks
around the outhouse came out nearly twice as bright as the game shows them.

**A static mesh's colours do not hold it.** Where no light reaches, they are
0 (docs/package-format.md has the formula they follow). The ambient they are
drawn with is the one the BSP holds, 0.41 sqrt(brightness) in the ambient's
colour, added to the colour and doubled with it, so a mesh in shade is as dark
as the ground in shade beside it. Every part is lit so, the blended foliage
too: drawn at the texture's own brightness, as it was before the ambient was
settled, the swamp's bushes and hanging moss were bright where the game shows
them dark green.

A mesh takes the ambient of the zone its Region names, which the engine wrote.
Where that is the LevelInfo, which has no ambient, the mesh's origin is in
solid, sunk into the ground, for most of them: 92 of the swamp's 221 have the
zone of a ZoneInfo around their middle, among them the two bushes by the pond,
which the game shows dark green, not black. Those take the zone around their
middle.

## The order of a frame

What is opaque first: the BSP, the terrains, the characters and the static
meshes; then the projectors, on that; then what blends, the BSP's, the
characters' and the static meshes', which writes no depth. A blended part
drawn before an opaque thing behind it has that thing drawn over it: the
Fairy Godmother factory's workers, whose suits are textures with an alpha
channel, Elf's RandSkins HazMat1 to HazMat6, had the conveyors and the floor
show through them when the characters came before the static meshes.

## Projectors and shadows

A projector lays its texture over what it covers: the BSP, the terrains and the
opaque static meshes, drawn again after the opaque world and before what
blends over it. AttachProjector and DetachProjector say which are attached;
a placed Projector attaches itself in PostBeginPlay. Its frame is its
Rotation's axes from its Location, as far as MaxTraceDistance; at its location
it is DrawScale times half its texture across, and with a FOV the apex is
behind it where the frustum narrows to nothing. FrameBufferBlendingOp
PB_Modulate multiplies the frame, PB_AlphaBlend blends by the texture's alpha,
and bGradient fades it with depth. Shrek's swamp has one placed projector, a
tree's shadow (a TexOscillator over Tree_shadow, PB_AlphaBlend, FOV 45).

A character's shadow is KWPawn's ShadowProjector: it follows its ShadowActor,
points along the light, the Sunlight's direction where there is one, and asks
for its texture, a ShadowBitmapMaterial, the actor seen from the light. Its
frustum is the one KWPawn's InitShadow sets: the apex LightDistance, 380, back
towards the light, a FOV wide enough for the actor's bounding sphere and 160
more, and a DrawScale that makes it LightDistance tan(FOV / 2) across at the
actor with the texture's 128 texels: so the shadow on the ground is larger than
the actor and spreads away from the light, as in the game. Here
that texture is drawn each frame: the posed mesh's silhouette from the apex,
in the same perspective, grey on white, once, 32 square, and the projector
softens it as it samples it, five taps two texels apart. How the game softened its
shadows is not known; a small texture filtered is what its time could afford,
and its shadows are soft. GetRenderBoundingSphere, which sizes it, is the posed mesh's box's
sphere.

## Textures and materials

`src/render/Texture.cpp` decodes every texture of the game, P8, RGBA8, DXT1,
DXT3, DXT5, L8 and G16, and agrees with tools/utexture.py byte for byte on all
2369, compared by an FNV-1a hash of each one's RGBA at its largest mip no side
of which is over 64 (`ffa-script textures`).

A material is walked to its base texture as tools/umaterial.py does, and on the
way it says how the surface goes onto the frame. Most of the game's foliage is
a FinalBlend over a texture: FrameBufferBlending 2, FB_AlphaBlend, with
AlphaTest and an AlphaRef of 100, cut and blended at once, writing depth by
FinalBlend's ZWrite, True by default. A texture alone cuts at half for bMasked
and blends for bAlphaTexture; a Shader blends by its OutputBlending. What wraps
decides over what it wraps.

## Not yet

- The sky: the sky zone drawn first from the SkyZoneInfo, behind the surfaces
  flagged PF_FakeBackdrop, which show the clear colour now.
- The Spotlight's cone in the vertex light computed for black instances
  (docs/package-format.md, StaticMeshInstance) and on characters.
- Sprites and emitters, fog, translucency sorted by depth, water's cubemap.
