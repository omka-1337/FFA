// Fonts: glyph rectangles over texture pages (tools/ufont.py has the layout
// and how it was found). A Font record has an empty property block, then, by
// the package's version:
//
//   122 and later: index count, per character i32 StartU, StartV, USize,
//                  VSize and u8 page; index page count, Texture references
//   121 and before: index page count, per page a Texture reference and its
//                  characters, i32 StartU, StartV, USize, VSize each; then
//                  i32 characters a page
//
// then an i32 (absent in version 99), the character remap (index count, u16
// pairs) and an i32. Every one of the game's 120 fonts reads to its end.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Library.h"

namespace ffa {

struct FontGlyph {
    int page = 0;
    int u = 0, v = 0, width = 0, height = 0;
};

struct FontData {
    std::vector<FontGlyph> glyphs;      // by character code
    std::vector<ObjectRef> pages;       // the textures
    // A character's glyph, or null for one the font has not.
    const FontGlyph* glyph(uint32_t code) const {
        return code < glyphs.size() && glyphs[code].width > 0 ? &glyphs[code] : nullptr;
    }
};

// Throws FormatError when the record does not read to its end.
FontData readFont(Library& lib, const ObjectRef& font);

}  // namespace ffa
