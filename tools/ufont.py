"""Fonts of a UE2 package: the glyph rectangles over a set of texture pages.

A Font record has an empty property block, then one of two layouts, chosen by
the package version. Shrek 2 ships both, since some of its font packages came
from older Epic games: UT2003Fonts.utx is version 120, WarfareFonts.utx 121,
UWindowFonts.utx 99, against 122 and 129 for the rest.

Version 122 and later, characters with a page number each:

    index       character count, 256 in every font seen
    per char    i32 StartU, StartV, USize, VSize, u8 page
    index       page count, then that many Texture references
    i32         0 or 1
    index       character remap count, then u16 pairs; empty everywhere
    i32         0

Version 121 and earlier, pages with their characters:

    index       page count
    per page    index Texture, index character count, then i32 StartU,
                StartV, USize, VSize per character
    i32         characters per page: 32, 128 or 256
    i32         0, 1 or 2; absent in version 99
    index       character remap count, then u16 pairs; empty everywhere
    i32         0

All 120 fonts in the game read to the exact end of their records, every page
is a Texture, and every glyph rectangle lies inside its page.

Usage: ufont.py <package>...                     survey
       ufont.py <package> <font> <text> <out.png>  draw a line of text
"""
import sys, os, struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from upkg import Package, R
from umap import Map
from udefaults import Tagged

PAGES_UNTIL = 121       # the last package version with the page layout
NO_KERNING_UNTIL = 99   # the last version without the i32 after the pages


def package_version(pkg):
    return struct.unpack_from('<H', pkg.b, 4)[0]


class Font:
    def __init__(self, pkg, e):
        self.p, self.e, self.name = pkg, e, e['name']
        end = e['off'] + e['size']
        v = Tagged(pkg).parse(Map(pkg).props_start(e), end, want_values=True)
        if not v:
            raise ValueError('%s: no property block' % self.name)
        b, r, ver = pkg.b, R(pkg.b, v[1]), package_version(pkg)
        self.version = ver
        # glyphs: (page index, u, v, width, height), indexed by character code
        self.glyphs, self.pages = [], []
        if ver > PAGES_UNTIL:
            for _ in range(r.idx()):
                u, vv, w, h, page = struct.unpack_from('<4iB', b, r.p)
                r.p += 17
                self.glyphs.append((page, u, vv, w, h))
            self.pages = [r.idx() for _ in range(r.idx())]
            self.per_page = None
            self.kerning = r.i32()
        else:
            for page in range(r.idx()):
                self.pages.append(r.idx())
                for _ in range(r.idx()):
                    self.glyphs.append((page,) + struct.unpack_from('<4i', b, r.p))
                    r.p += 16
            self.per_page = r.i32()
            self.kerning = r.i32() if ver > NO_KERNING_UNTIL else None
        self.remap = []
        for _ in range(r.idx()):
            self.remap.append(struct.unpack_from('<HH', b, r.p))
            r.p += 4
        self.is_remapped = r.i32()
        if r.p != end:
            raise ValueError('%s: font does not end on its record' % self.name)

    def glyph(self, code):
        """(page, u, v, width, height) for a character code, or None."""
        if self.per_page and code < len(self.glyphs):
            return self.glyphs[code]
        return self.glyphs[code] if code < len(self.glyphs) else None


def fonts(path):
    p = Package(path)
    for e in p.exports:
        if p.classof(e) == 'Font' and e['size']:
            yield Font(p, e)


def draw(font, text, resolve_page):
    """RGBA (width, height, bytes) of a line of text, glyph after glyph.
    resolve_page(font, page index) gives a decoded page as (w, h, rgba)."""
    pages = {}
    gl = [font.glyph(ord(c)) for c in text]
    gl = [g for g in gl if g and g[3] > 0 and g[4] > 0]
    width = sum(g[3] for g in gl) or 1
    height = max((g[4] for g in gl), default=1)
    out = bytearray(width * height * 4)
    x0 = 0
    for page, u, v, w, h in gl:
        if page not in pages:
            pages[page] = resolve_page(font, page)
        pw, ph, px = pages[page]
        for y in range(h):
            src = ((v + y) * pw + u) * 4
            dst = (y * width + x0) * 4
            out[dst:dst + w * 4] = px[src:src + w * 4]
        x0 += w
    return width, height, bytes(out)


def main(argv):
    if len(argv) == 4:
        from utexture import Texture, write_png
        p = Package(argv[0])
        font = next(f for f in fonts(argv[0]) if f.name == argv[1])

        def page(f, i):
            t = Texture(p, p.exports[f.pages[i] - 1])
            return t.rgba(0)
        write_png(argv[3], *draw(font, argv[2], page))
        print('%s: "%s" -> %s' % (font.name, argv[2], argv[3]))
        return
    total = 0
    for path in argv:
        for f in fonts(path):
            total += 1
            print('  %-28s version %3d  %3d glyphs on %d page(s)%s'
                  % (f.name, f.version, len(f.glyphs), len(f.pages),
                     ', %d per page' % f.per_page if f.per_page else ''))
    print('%d fonts read to the exact end of their records' % total)


if __name__ == '__main__':
    main(sys.argv[1:])
