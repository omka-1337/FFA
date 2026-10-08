# Contributing

The code here is small. The method is the valuable part, and it is strict for a
reason: a reader for an undocumented binary format fails silently. A wrong field
width does not crash, it returns plausible numbers, and the mistake surfaces
weeks later in something built on top. So a reader is not finished when it looks
right. It is finished when it is proven.

## The rule

**Every layout must be proven against the whole corpus before it is committed,
by a check that a wrong guess cannot pass.**

Three such checks are in use here, in order of strength.

**1. Exact end alignment.** Every export record has a known length from the
export table. Parse it and the position must land on the last byte, not near it.
One wrong field width desynchronises everything after it, so a layout that lands
exactly across thousands of records is almost certainly right. This is the
preferred proof and most of the project rests on it.

**2. A second, independent measure.** Where one exists, use it as well. A
function's declared `ScriptSize` counts the bytecode in memory while the file
holds it compressed, so summing the token sizes and comparing against that
number tests the memory model, which end alignment cannot see. Two unrelated
checks passing together is much stronger evidence than one passing twice.

**3. Semantic validation.** When a record's tail is not decoded, alignment is
unavailable. Then check that the data means something: every property name in a
default block must resolve to a real property of the class or an ancestor, every
index in a mesh must be inside its vertex array, an index buffer length must be
a multiple of three. Run it over everything, not a sample.

Report the numbers. "8599 of 8638 functions" belongs in the commit message or
the pull request, and so does what the remainder are. A reader that works on the
file you happened to open is not a result.

## Two mistakes this project already made

Both passed a casual look. Both were caught only by the rule above.

**The plausible heuristic.** The default property block sits at the end of a
class record, so it can be found by scanning for an offset that parses cleanly
to the end. Several offsets per record do that, and taking the earliest is the
obvious move. It is right about a third of the time. Only checking the property
names against the class's real properties separated the true block from the
coincidences.

**The field that looks like the previous one.** In a static mesh, a bounding box
is followed by a byte that flags whether it is valid. A bounding sphere follows,
and the byte after it looks like the same flag. It is the section count. Read as
a flag, everything after it shifts by one byte and still parses into numbers
that look like geometry.

## Parsers must be bounded

A desynchronised parse reads garbage as structure and keeps going, off the end
of the record and into the rest of the file. A parser that allocates while it
does this will allocate without limit. That happened here and took a machine
down hard.

Carry the end of the record as a hard limit, raise as soon as it is passed, keep
a node budget per record, and run bulk passes under a cap:

```sh
( ulimit -v 3000000; python3 tools/udecompile.py $SYS SHGame.u --all out )
```

### A Python trap worth knowing

`r.p += r.idx() * 4` does not do what it says. An augmented assignment loads
`r.p` before evaluating the right hand side, so the byte that `r.idx()` consumes
is overwritten straight away and the parse silently falls one byte behind. Read
the count into a variable first. This one shifted every field after a mesh
header and was caught only because the texture count came back empty.

## Other implementations

Do not copy code from other readers of these formats. Use them as oracles
instead: export the same mesh with UE Viewer and compare the counts, and if the
numbers disagree, one of you is wrong and now you know. Two independent
implementations catching each other is worth more than one implementation
inherited twice.

If code ever is taken from elsewhere, it keeps its copyright notice and the
licence comes with it.

## Practical

- **Corpus.** Never commit game data or decompiled output. Both are gitignored.
  The game's script is its author's, not ours. The README explains how to get a
  legal corpus.
- **Language.** Code, comments, documentation and tool output in English.
- **Comments.** Explain why a field is where it is, not what the line does. The
  reasoning behind a layout is the thing nobody can recover later.
- **Documentation.** A new format goes in `docs/package-format.md` in the same
  pass as the reader, including what remains unknown about it. A finding that
  only exists in code is a finding that will be made again.
- **Dependencies.** Standard library only.
- **Commits.** A single subject line, no body, no trailers.

## TODO

What is left, roughly in order of how much it unblocks. The aim is a format
understood well enough for the engine to run the original data, so what a
running game needs comes first. Say which one you are taking before you start,
so two people do not decode the same record twice, and tick it off in the
commit that finishes it.

Formats not read yet, from a census of every export in the game: the classes
whose records carry native data after their properties.

- [x] **Level** (29). The live actor list, the URL and the level's Model, to
  the end of every record.
- [x] **Sound** (3769). Bink Audio and WAV files stored whole; FFmpeg decodes
  every one.
- [ ] **Lip sync**, version 2, after 2681 of the sounds, nearly all the
  dialogue: curves of floats over time that move the characters' mouths.
  Versions 0 and 1 are read.
- [x] **Font** (120). Both layouts, by package version, drawn as text.
- [x] `StaticMeshInstance` (9260), each placed mesh's baked vertex light.
- [x] BSP render sections, and the last three fields of a BSP node.
- [x] **BSP lightmaps** and their DXT1 textures. Every BSP Model reads to its
  end. Left over: one u32 per lightmap, and three levels whose lightmap
  texture was never baked.
- [x] The native part of `TerrainInfo`: the engine's copy of the grid, its
  normals, frames and baked light.
- [ ] The rest of `TerrainSector` beyond its box.
- [ ] `KMeshProps` (320), Karma physics; `ConvexVolume` (156), KnowWonder's
  volumes; `VertMesh` (4), vertex animated meshes.
- [ ] `Polys` (3039), the editor's brush polygons; probably not needed to run.
- [x] Text: all 199 `.ini` and `.int` files, subtitles, bump sets, menus and
  cutscene scripts.
- [ ] The cutscene script language: some fifty commands, Cue, Say, FlyTo,
  PlayAnim and the rest, which the engine has to run. Which script classes
  interpret them is the place to start.
- [ ] Script: the 39 functions that still fail end alignment, 33 of them in
  GUI.u, and the class header fields between the struct and the defaults.

Not formats of our own: music is plain Ogg Vorbis, and the cutscenes are Bink
video, which FFmpeg decodes.

Engine:

- [x] Run the VM on a real corpus. `ffa-script check` loads all 2002 classes
  of Shrek 2 and compiles 7961 of 8145 functions and all 968 states, whose
  tails are 22 bytes in every one. The first run crashed, and the cause was the
  readers': the field record layout, now fixed in both.
- [ ] Which structs are stored raw. Range, Plane and Scale are tagged in Shrek
  2, not raw as the readers assume: `Range reads 8 bytes of 13`, and 1858
  default objects report it. Measure every struct property's size against
  both readings.
- [ ] The functions the VM does not load: 121 with a LocalVariable that names
  no variable, 6 InstanceVariable, 3 StateVariable, the 39 known misaligned
  ones, and about 15 jumps that land inside a statement.
- [ ] LabelTableOffset in a state's tail agrees with the label table found by
  the bytecode walk in 216 states and not in 752.
- [ ] Engine natives: Actor, Level, spawning, timers, the tick.
- [ ] Loading a level: its actors as objects, with their state frames.
- [ ] Collision queries, a line trace and a swept box, on the static mesh
  triangle trees and collision models and on the BSP's leaves and hulls, all
  decoded. Checked against where the game's own actors stand.
- [ ] Config and localisation, probe masks, replication, garbage collection.

Open questions, written up in the format document with the measurements so
far:

- [ ] PrePivot on skeletal meshes, and the props that float.
- [ ] Which of a static mesh's two collision forms the engine uses for what.
- [ ] Which side of a mesh faces out, so that only bTwoSided textures are drawn
  from both sides.

Tools:

- [ ] Control flow in the decompiler: `if`, `while` and `for` in place of
  labels and gotos. Only for reading the script; the VM runs the jumps.
- [ ] Breadth: other UE2 games. Harry Potter and the Prisoner of Azkaban is the
  same engine build as Shrek 2 (a repack is in `roms/`); UT2003 and UT2004 are
  other package versions.
