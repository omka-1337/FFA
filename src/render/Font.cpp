#include "render/Font.h"
#include "script/Tagged.h"

namespace ffa {

FontData readFont(Library& lib, const ObjectRef& font) {
    std::vector<TagEntry> tags;
    size_t at = 0;
    if (!Library::properties(font, tags, at)) throw FormatError("a font without its property block");
    const Package& p = *font.pkg;
    const Export& e = font.exp();
    Reader r(p.data, at, size_t(e.off + e.size));
    FontData f;
    std::vector<int32_t> refs;
    if (p.version > 121) {
        int32_t n = r.idx();
        if (n < 0 || n > 65536) throw FormatError("a font's character count is out of range");
        for (int32_t i = 0; i < n; ++i) {
            FontGlyph g;
            g.u = r.i32();
            g.v = r.i32();
            g.width = r.i32();
            g.height = r.i32();
            g.page = r.u8();
            f.glyphs.push_back(g);
        }
        int32_t pages = r.idx();
        for (int32_t i = 0; i < pages && i < 256; ++i) refs.push_back(r.idx());
        r.i32();
    } else {
        int32_t pages = r.idx();
        if (pages < 0 || pages > 256) throw FormatError("a font's page count is out of range");
        for (int32_t k = 0; k < pages; ++k) {
            refs.push_back(r.idx());
            int32_t n = r.idx();
            if (n < 0 || n > 65536) throw FormatError("a font page's character count is out of range");
            for (int32_t i = 0; i < n; ++i) {
                FontGlyph g;
                g.page = k;
                g.u = r.i32();
                g.v = r.i32();
                g.width = r.i32();
                g.height = r.i32();
                f.glyphs.push_back(g);
            }
        }
        r.i32();                                // characters a page
        if (p.version > 99) r.i32();
    }
    int32_t remap = r.idx();
    if (remap < 0 || remap > 65536) throw FormatError("a font's remap count is out of range");
    r.need(size_t(remap) * 4);
    r.p += size_t(remap) * 4;
    r.i32();
    if (r.p != size_t(e.off + e.size)) throw FormatError("a font does not end on its record");
    for (int32_t ref : refs) f.pages.push_back(lib.resolve(p, ref));
    return f;
}

}  // namespace ffa
