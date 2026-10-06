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

## Picking something up

Open work, roughly in order of how much it unblocks:

- The rest of the BSP record: zones, lightmaps, bounds. Zones are what the
  skybox needs, to tell its geometry from the level's.
- Terrain layers: which textures a terrain blends, and with what alpha maps.
- PrePivot on skeletal meshes, and the props that float: both written up as
  open questions in the format document, with the measurements so far.
- `TerrainSector` and `StaticMeshInstance` payloads.
- The static mesh tail: raw triangles and the collision tree.
- Control flow structuring in the decompiler, turning labels and gotos into `if`
  and `for`. No binary work at all, pure analysis of data already parsed.
- Breadth: run the tools against other UE2 games and report where they break.
  Harry Potter: Prisoner of Azkaban is the same engine build as Shrek 2;
  UT2003 and UT2004 are different package versions.

Say which one you are taking before you start, so two people do not decode the
same record twice.
