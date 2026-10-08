// The package container: header, name table, imports and exports.
// The layout is the one docs/package-format.md records and tools/upkg.py
// reads.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Name.h"

namespace ffa {

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A bounded cursor. Every read checks the limit, because a desynchronised
// parse otherwise reads garbage as structure and keeps going.
struct Reader {
    const uint8_t* b = nullptr;
    size_t limit = 0;
    size_t p = 0;

    Reader(const std::vector<uint8_t>& data, size_t at, size_t end)
        : b(data.data()), limit(end), p(at) {}

    void need(size_t n) const {
        if (p + n > limit) throw FormatError("read past the end of the record");
    }
    uint8_t u8() { need(1); return b[p++]; }
    uint16_t u16() { need(2); uint16_t v = uint16_t(b[p] | b[p + 1] << 8); p += 2; return v; }
    uint32_t u32() {
        need(4);
        uint32_t v = uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 |
                     uint32_t(b[p + 3]) << 24;
        p += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    float f32() {
        uint32_t u = u32();
        float f;
        static_assert(sizeof f == 4);
        __builtin_memcpy(&f, &u, 4);
        return f;
    }
    // Compact index: sign and continuation in the first byte with six value
    // bits, then seven value bits per further byte.
    int32_t idx() {
        uint8_t b0 = u8();
        bool neg = b0 & 0x80;
        int64_t val = b0 & 0x3F;
        if (b0 & 0x40) {
            int shift = 6;
            for (int n = 0; n < 4; ++n) {
                uint8_t c = u8();
                val |= int64_t(c & 0x7F) << shift;
                shift += 7;
                if (!(c & 0x80)) break;
            }
        }
        return int32_t(neg ? -val : val);
    }
};

struct Import {
    std::string clsPackage, clsName;
    int32_t outer = 0;
    std::string name;
};

struct Export {
    int32_t cls = 0, super = 0, outer = 0;
    std::string name;
    uint32_t flags = 0;
    int32_t size = 0, off = 0;
};

class Package {
public:
    explicit Package(const std::string& path);

    std::string path, file, stem;      // file is the base name, stem without extension
    uint16_t version = 0, licensee = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> data;
    std::vector<std::string> names;
    int noneIndex = 0;
    std::vector<Import> imports;
    std::vector<Export> exports;

    const std::string& refName(int32_t ref) const;
    // Class name of an export; a class record itself has no class reference.
    std::string classOf(const Export& e) const;
    std::string classOf(int idx) const { return classOf(exports[idx - 1]); }
    const Export& exp(int idx) const { return exports.at(idx - 1); }
    Reader record(int idx) const {
        const Export& e = exp(idx);
        return Reader(data, size_t(e.off), size_t(e.off) + size_t(e.size));
    }
    Name name(int i) const;
    bool validName(int64_t i) const { return i >= 0 && i < int64_t(names.size()); }

private:
    mutable std::vector<Name> interned_;
};

}  // namespace ffa
