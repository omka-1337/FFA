// A level: its Level record, and its live actors loaded as objects.
//
// The Level object of a .unr lists the actors that exist; the package also
// keeps actors the editor deleted, every one of them marked bDeleteMe, and
// those are not loaded. The record, after an empty property block:
//
//   u32 x2   actor count, twice (count and capacity)
//   index    that many actors, LevelInfo first
//   FURL     Protocol, Host, Map, Portal, an array of option strings,
//            i32 Port, i32 Valid
//   index    the level's Model
//   f32      a time in seconds
//   18 bytes zero
//
// docs/package-format.md records how this was established; tools/umap.py
// reads the same record.
#pragma once

#include <string>
#include <vector>

#include "script/Linker.h"

namespace ffa {

struct LevelRecord {
    int idx = 0;                        // the Level export
    std::vector<int32_t> actors;        // export indices, LevelInfo first
    std::string protocol, host, map, portal;
    std::vector<std::string> options;
    int32_t port = 0, valid = 0;
    int32_t model = 0;
    float time = 0;
};

// The package's Level record. Throws FormatError when there is none, or when it
// does not read to the end of its record.
LevelRecord readLevel(const Package& p);

// Every live actor of the level, built with its properties, in list order.
std::vector<Object*> loadActors(Linker& lk, int pkg, const LevelRecord& level);

}  // namespace ffa
