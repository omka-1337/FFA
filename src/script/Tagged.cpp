#include "script/Tagged.h"

namespace ffa {

bool parseTagged(const Package& p, size_t start, size_t end, std::vector<TagEntry>& out,
                 size_t& pos) {
    out.clear();
    try {
        Reader r(p.data, start, end);
        while (true) {
            if (r.p >= end) return false;
            int32_t nm = r.idx();
            if (nm == p.noneIndex) {
                pos = r.p;
                return true;
            }
            if (!p.validName(nm) || r.p >= end) return false;
            TagEntry e;
            e.name = nm;
            uint8_t info = r.u8();
            e.type = info & 0x0F;
            if (e.type == 0 || e.type > T_FixedArray) return false;
            if (e.type == T_Struct) {
                int32_t sn = r.idx();
                if (!p.validName(sn)) return false;
                e.structName = sn;
            }
            switch ((info >> 4) & 0x07) {
            case 0: e.size = 1; break;
            case 1: e.size = 2; break;
            case 2: e.size = 4; break;
            case 3: e.size = 12; break;
            case 4: e.size = 16; break;
            case 5: e.size = r.u8(); break;
            case 6: e.size = r.u16(); break;
            case 7: e.size = r.u32(); break;
            }
            if ((info & 0x80) && e.type != T_Bool) {
                uint8_t b0 = r.u8();
                if (b0 & 0x80) {
                    if (b0 & 0x40) {
                        uint32_t v = uint32_t(b0 & 0x3F) << 24;
                        v |= uint32_t(r.u8()) << 16;
                        v |= uint32_t(r.u8()) << 8;
                        v |= r.u8();
                        e.index = int32_t(v);
                    } else {
                        e.index = ((b0 & 0x7F) << 8) | r.u8();
                    }
                } else {
                    e.index = b0;
                }
            }
            if (e.type == T_Bool) {
                e.boolValue = info & 0x80;
                e.at = r.p;
            } else {
                e.at = r.p;
                if (r.p + e.size > end) return false;
                r.p += e.size;
            }
            out.push_back(e);
            if (out.size() > 4000) return false;
        }
    } catch (const FormatError&) {
        return false;
    }
}

}  // namespace ffa
