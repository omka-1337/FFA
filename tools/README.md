# Shrek 2 PC package tools

Readers for the Unreal Engine 2 build 2226 packages that Shrek 2 PC ships in
`System/`. Pure Python, no dependencies. The format they implement is written up
in [../docs/package-format.md](../docs/package-format.md).

| File | What it does |
|---|---|
| `upkg.py` | Package container: header, name table, imports, exports |
| `uscript.py` | Bytecode parser, builds token trees with disk and memory offsets |
| `uclass.py` | Classes, properties, states, function signatures |
| `udecompile.py` | Renders bytecode back to UnrealScript source |
| `udefaults.py` | Default property blocks, and the tagged value format |
| `natives.py` | Which engine natives the game's script actually calls |

## Usage

```sh
SYS=../work/Shrek2/System

python3 upkg.py       $SYS/*.u                  # record counts per package
python3 uscript.py    $SYS/*.u                  # parse rate and both checks
python3 natives.py    $SYS/*.u                  # native surface, writes natives-used.txt
python3 uclass.py     $SYS/SHGame.u Shrek       # one class, declarations only
python3 udecompile.py $SYS SHGame.u ShrekController     # one class with bodies
python3 udecompile.py $SYS SHGame.u --all out/SHGame    # every class in a package
python3 udefaults.py  $SYS SHGame.u Shrek       # one class's defaultproperties
python3 udefaults.py  $SYS                      # validate every class's block
```

`udecompile.py` takes the `System` directory rather than one file because the
native index table has to be built from every package before anything can be
rendered.

## Running these safely

A parser that desynchronises will read garbage as opcodes and walk past the end
of a record. The readers carry a hard limit and a node budget per record so this
raises instead of allocating, but a bulk run is still worth capping:

```sh
( ulimit -v 3000000; python3 udecompile.py $SYS SHGame.u --all out/SHGame )
```

## State

2002 classes decompile. Of 8638 functions, 8599 parse to the exact end of their
record and 8426 also agree with the declared script size. 146 functions are
marked with an incomplete parse warning in the output and 40 fail outright,
most of them in GUI.u.

Control flow is printed as labels and gotos, which is what the bytecode actually
contains. Recovering `if`/`else`/`while` from the jump graph is a separate pass
that has not been written.
