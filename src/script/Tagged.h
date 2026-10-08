// The tagged property list: defaultproperties, struct values in defaults, and
// the variables of every object instance in a level. Port of the Tagged reader
// in tools/udefaults.py; the format is in docs/package-format.md.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Package.h"

namespace ffa {

enum TagType : uint8_t {
    T_Byte = 1, T_Int = 2, T_Bool = 3, T_Float = 4, T_Object = 5, T_Name = 6,
    T_String = 7, T_Class = 8, T_Array = 9, T_Struct = 10, T_Vector = 11,
    T_Rotator = 12, T_Str = 13, T_Map = 14, T_FixedArray = 15,
};

struct TagEntry {
    int name = 0;               // name index
    uint8_t type = 0;
    int32_t index = 0;          // static array element
    int structName = -1;        // name index, for struct values
    uint32_t size = 0;
    size_t at = 0;              // where the value's bytes start
    bool boolValue = false;     // a bool's value is the array bit of its info byte
};

// Parses one list from `start`. Returns false on anything that is not a valid
// list, which callers scanning for a list's start rely on; `pos` is the offset
// after the terminating None.
bool parseTagged(const Package& p, size_t start, size_t end, std::vector<TagEntry>& out,
                 size_t& pos);

}  // namespace ffa
