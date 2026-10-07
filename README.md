# FFA-Engine

The engine that runs a UE2 game's original data, in the way OpenMW runs
Morrowind's. It starts where a game starts, with its script: the first part is
a virtual machine for UnrealScript bytecode, loaded straight from the game's
`.u` packages.

The formats come from [FFA-Tools](https://github.com/omka-1337/FFA-Tools), where
they are worked out and proven against the game files by Python readers. This
repository reimplements what those establish, and FFA-Tools'
`docs/package-format.md` is the contract between the two.

C++20, standard library only, built with CMake.

## Building

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests need Python 3 and a copy of FFA-Tools, by default next to this
repository (`-DFFA_TOOLS_DIR=` points elsewhere): `tests/fixtures.py` writes
small packages whose script has known results, and reads each one back with
FFA-Tools' readers before the engine sees it.

## Running against a game

```sh
build/ffa-script check $SYS                       # load and compile everything, report
build/ffa-script call  $SYS GameInfo.ParseOption '?Name=Bob?Class=X' Name
build/ffa-script smoke $SYS                       # call every static function once
```

`$SYS` is a game's `System` directory, or that of Epic's freely available
UnrealEngine2 Runtime (FFA-Tools' README says how to get it).

`check` is the corpus-wide proof the VM rests on, as end alignment is for the
readers. It loads every class and builds its default object, and compiles every
function and state, which resolves every reference a token carries and requires
every jump to land on the start of a statement. It then tests two readings that
come from knowledge of the engine rather than from measurement, against what
the bytecode itself says:

- **The state tail.** A state's record is taken to end in ProbeMask,
  IgnoreMask, LabelTableOffset and StateFlags, 22 bytes. `check` reports the
  tail length of every state, and whether LabelTableOffset points at the label
  table the bytecode walk found.
- **The conversion tokens.** 0x39 to 0x59 are taken from the engine's
  published token list. `check` tallies the declared type of every operand each
  token is given; IntToString should only ever be handed ints.

It also lists the natives the script calls that are not implemented yet, most
called first, which is the work list for the next step.

## How it is put together

| File | What it does |
|--|--|
| `src/core/Package` | The container: header, names, imports, exports |
| `src/core/Name` | Case blind names, compared as integers |
| `src/script/Bytecode` | Bytecode to token trees, with memory offsets; port of `uscript.py` |
| `src/script/Tagged` | Tagged property lists; port of the reader in `udefaults.py` |
| `src/script/Linker` | Imports resolved by path; classes, functions, states, structs; defaults |
| `src/script/Types` | Values, declared types, objects |
| `src/script/VM` | The interpreter: calls, states, latent actions, iterators |
| `src/script/NativesCore` | Object's natives: operators, maths, strings, states |
| `apps/ffa_script.cpp` | The command line above |

How each part of the language executes, and on what authority, is in
[docs/script-vm.md](docs/script-vm.md).

## Where it stands

The VM runs the language: virtual, final, global, super and static calls,
states with BeginState and EndState, state code with labels, goto and latent
actions, foreach over native iterators with break and continue, switch with
fall through, out and optional parameters, skip parameters for `&&` and `||`,
delegates, structs and static and dynamic arrays with the engine's copy and
bounds behaviour, casts, `new`, and accessing members of none. 55 tests over
the fixture packages cover each of these.

It has not yet been run on a real corpus. That is the next step, and
`ffa-script check` is built for it.

Not done yet, roughly in order:

- Engine natives: Actor, Level, spawning, timers, the tick. Script cannot run a
  level until the Actor natives exist; `check` lists them by how often they are
  called.
- Config and localisation: `.ini` and `.int` files, `config` and `localized`
  variables, `Localize`.
- Loading a level: its actors as objects, with their state frames, from the
  `.unr` records FFA-Tools' `umap.py` already reads.
- Probe masks and `ignores`, replication, and garbage collection.
- Speed: the interpreter walks token trees, which is simple and easy to check
  but slower than the flat bytecode the engine runs.
