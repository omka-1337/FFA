# ue2tools

Readers for Unreal Engine 2 packages, written from scratch against the game
files. They open the container, decompile UnrealScript back to source, read class
defaults, list the actors of a level, and pull static mesh geometry out.

Nothing here was taken from an existing implementation. Every format was
recovered by measurement, and every reader has to prove itself against the whole
corpus before it is committed. How that is done is the subject of
[CONTRIBUTING.md](CONTRIBUTING.md), and it is the part of this project worth
copying even if the code is not.

## What it reads

| Tool | What it does |
|--|--|
| `tools/upkg.py` | Package container: header, name table, imports, exports |
| `tools/uscript.py` | UnrealScript bytecode, as token trees with disk and memory offsets |
| `tools/uclass.py` | Classes, properties, states, function signatures, enums |
| `tools/udefaults.py` | Default property blocks, and the tagged value format |
| `tools/udecompile.py` | Renders a class back to readable UnrealScript |
| `tools/umap.py` | Actors of a level and their properties |
| `tools/umesh.py` | Static mesh geometry: vertices, normals, UVs, colours, triangles |
| `tools/ubsp.py` | BSP: level vectors, points, nodes, surfaces and verts |
| `tools/ulevel.py` | Assembles a level's geometry, BSP polygons so far |
| `tools/uview.py` | Writes a level as a self contained HTML viewer |
| `tools/natives.py` | Which engine natives a game's script actually calls |

Pure Python, standard library only, no dependencies.

## Where it stands

Measured against Shrek 2 PC (package version 129) and the freely available
UnrealEngine2 Runtime (version 126):

- **Script.** 8599 of 8638 functions parse to the exact end of their record,
  8426 of those also agree with the declared script size. 2002 classes
  decompile, with signatures, bodies and defaults.
- **Defaults.** Every class of both corpora, 2002 and 607, with every property
  name resolving to a real inherited property.
- **Levels.** Across all 29 Shrek 2 maps, 36111 objects parse. The 16406 that do
  not are exactly the engine classes with native payloads: Model and Polys (the
  BSP), StaticMeshInstance, TerrainSector.
- **Meshes.** All 835 static meshes, 122011 vertices and 102414 triangles.
- **BSP.** All 2727 Model records, with 32690 nodes, 18539 surfaces and 602807
  verts, every reference in range. Zones, lightmaps and bounds are still
  undecoded.

Not done yet: the rest of the BSP record, terrain, textures, and structuring the
decompiler's control flow into `if` and `for` rather than labels and gotos.

## Getting a corpus

The tools need package files to run against, and this repository ships none.

Epic's UnrealEngine2 Runtime is free to download and makes a legal corpus anyone
can obtain:

```sh
curl -LO https://archive.org/download/ue2runtime_inst/UE2Runtime-22262002_Demo.exe
7z e -ocorpus UE2Runtime-22262002_Demo.exe 'System/*.u'
```

For a game you own, point the tools at its `System`, `Maps` and `StaticMeshes`
directories.

## Running

```sh
cd tools
SYS=../corpus

python3 upkg.py       $SYS/*.u                       # record counts per package
python3 uscript.py    $SYS/*.u                       # parse rate and both checks
python3 uclass.py     $SYS/Engine.u Actor            # one class, declarations
python3 udecompile.py $SYS Engine.u Pawn             # one class with bodies
python3 udefaults.py  $SYS Engine.u Pawn             # one class's defaults
python3 umap.py       ../Maps                        # object counts per level
python3 umesh.py      ../StaticMeshes                # geometry totals
python3 uview.py      ../Maps/7_Prison_Donkey.unr ../out/prison.html
```

The viewer output contains the game's own geometry, so it goes to the gitignored
`out/` and is for looking at your own copy only.

A bulk run is worth capping, for reasons [CONTRIBUTING.md](CONTRIBUTING.md)
explains:

```sh
( ulimit -v 3000000; python3 udecompile.py $SYS SHGame.u --all out )
```

## The format

[docs/package-format.md](docs/package-format.md) documents what was recovered:
compact indices, the file header, class, property, function and enum records,
the bytecode and its opcode table, default properties, levels, and static
meshes. It also lists what is still unknown.

Facts about a file format are not anyone's property, so that document is free to
use whatever licence the code carries.

## Credits

UE Viewer by Konstantin Nosov covers UE1 to UE4 assets and is MIT licensed. No
code from it is used here, but it is a far more complete asset exporter and an
excellent independent check on anything this project claims about meshes.

## Licence

MIT. See [LICENSE](LICENSE).

Permissive on purpose: the point of a format reader is that other projects can
use it, and the ones most likely to want it, such as SurrealEngine and UE
Viewer, are not copyleft.
