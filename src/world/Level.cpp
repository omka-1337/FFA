#include "world/Level.h"

#include "script/Tagged.h"

namespace ffa {

namespace {

// An FString: a compact length counting the terminator, then that many
// bytes; a negative length means as many UTF-16 characters.
std::string fstring(Reader& r) {
    int32_t n = r.idx();
    std::string out;
    if (n < 0) {
        for (int32_t i = 0; i < -n; ++i) {
            uint16_t c = r.u16();
            if (c) out += c < 0x80 ? char(c) : '?';
        }
    } else {
        for (int32_t i = 0; i < n; ++i) {
            char c = char(r.u8());
            if (c) out += c;
        }
    }
    return out;
}

}  // namespace

LevelRecord readLevel(const Package& p) {
    LevelRecord lv;
    for (int i = 1; i <= int(p.exports.size()); ++i)
        if (p.classOf(i) == "Level" && p.exp(i).size > 0) {
            lv.idx = i;
            break;
        }
    if (!lv.idx) throw FormatError(p.stem + " has no Level");
    const Export& e = p.exp(lv.idx);
    size_t start = size_t(e.off), end = size_t(e.off + e.size);
    std::vector<TagEntry> props;
    size_t pos = 0;
    if (!parseTagged(p, start, end, props, pos) || !props.empty())
        throw FormatError("the Level's property block is not empty");
    Reader r(p.data, pos, end);
    uint32_t n = r.u32(), capacity = r.u32();
    if (n != capacity) throw FormatError("the Level's actor count and capacity differ");
    for (uint32_t i = 0; i < n; ++i) lv.actors.push_back(r.idx());
    lv.protocol = fstring(r);
    lv.host = fstring(r);
    lv.map = fstring(r);
    lv.portal = fstring(r);
    for (int32_t i = 0, k = r.idx(); i < k; ++i) lv.options.push_back(fstring(r));
    lv.port = r.i32();
    lv.valid = r.i32();
    lv.model = r.idx();
    lv.time = r.f32();
    for (int i = 0; i < 18; ++i)
        if (r.u8()) throw FormatError("the Level's last 18 bytes are not zero");
    if (r.p != end) throw FormatError("the Level does not end on its record");
    return lv;
}

std::vector<Object*> loadActors(Linker& lk, int pkg, const LevelRecord& level) {
    std::vector<Object*> out;
    out.reserve(level.actors.size());
    for (int32_t idx : level.actors)
        if (idx > 0) out.push_back(lk.instanceAt(pkg, idx));
    return out;
}

}  // namespace ffa
