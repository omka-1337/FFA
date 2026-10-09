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
same way, so what was confirmed by eye there holds here:

- **The BSP**, its render sections' polygons, a texel position being the
  offset from the surface's base point along its texture vectors over the
  texture's size; the lightmap coordinates are the section vertices'. Lit as
  texture × (lightmap × 2 + the zone's ambient); unlit surfaces take the
  texture alone.
- **Terrains**, from the engine's copy of the grid, each layer laid over the
  ones before by its weight from its alpha map, its texture coordinates the
  world position against its TerrainMatrix; lit by the baked vertex light × 2
  plus the zone's ambient.
- **Static meshes**, at Location + R S (v - PrePivot), a section a material,
  the actor's Skins over the mesh's Materials; lit by the StaticMeshInstance's
  vertex colours × 2 plus the zone's ambient. Where those colours are black
  throughout although lights reach the mesh, they are computed from the lights
  and their masks, by the formula the stored colours follow.

- **Skeletal meshes**, each actor drawn as a mesh skinned on the CPU every
  frame from its pose, its faces by material, the actor's Skins over the
  mesh's materials. Lit as the engine lights an actor: its MaxLights lights
  strongest where it stands, by the same falloff and N.L as the static meshes'
  baked light, without shadows, plus the zone's ambient and the actor's
  AmbientGlow; bUnlit full bright. The normals come from the posed faces,
  turned to face away from the mesh's middle, as the winding is not settled. Holding forward on the swamp
  (`--hold W`), Shrek runs into the pond past the lily pads, arms swinging.

## How meshes are lit: measured against the game's frames

The first reading, the baked colour doubled plus the zone's ambient, drew the
swamp's foliage nearly black, where the game draws it bright green (frames from
play, of the swamp's start). Two changes bring it near the game, both found by
comparing frames, not settled by the data:

- **Colour and ambient doubled together**, light = 2 (colour + ambient): the
  STAY OUT sign comes to 0.77, 0.70, 0.61 of its texture against the game's
  0.69, 0.62, 0.53, where it was 0.51, 0.45, 0.36.
- **Blended materials unlit.** Every mesh part drawn blended, the FinalBlend
  foliage, grass, cattails, hanging moss and light beams, is drawn at the
  texture's own brightness. Their baked colours are dark, a mean of 10 to 30
  of 255, and some stand in the LevelInfo's zone with no ambient, yet in the
  game they are as bright as the texture, while the opaque meshes, the sign,
  the trunks, the rocks, are shaded.

A mesh takes the ambient of the zone its Region names, which the engine wrote;
a walk from its origin, which can sit inside the ground, found the zone outside
the level for some.

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
for its texture, a ShadowBitmapMaterial, the actor seen from the light. Here
that texture is drawn each frame: the posed mesh's silhouette from along the
projector, grey on white, twelve copies shifted round a small circle each
taking its share of the darkness away, so that the edges are soft as the
game's are. GetRenderBoundingSphere, which sizes it, is the posed mesh's box's
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
