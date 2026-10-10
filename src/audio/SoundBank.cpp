#include "audio/SoundBank.h"

#include <vector>

#include "script/Tagged.h"
#include "script/Types.h"

namespace ffa {

bool SoundBank::file(const Object* sound, File& out) {
    if (!sound) return false;
    std::vector<std::string> parts;
    for (const Object* k = sound; k; k = k->outer) parts.insert(parts.begin(), k->name.str());
    if (parts.size() < 2) return false;
    const Package* p = lib_.package(parts[0]);
    if (!p) return false;
    int idx = Library::findByPath(*p, std::vector<std::string>(parts.begin() + 1, parts.end()), "Sound");
    if (!idx) return false;
    ObjectRef r{p, idx};
    std::vector<TagEntry> tags;
    size_t end = 0;
    if (!Library::properties(r, tags, end)) return false;
    try {
        Reader rd(p->data, end, size_t(r.exp().off + r.exp().size));
        int32_t type = rd.idx();
        if (!p->validName(type)) return false;
        rd.u32();                               // where the lazy array ends
        int32_t n = rd.idx();
        rd.need(size_t(n));
        out.ref = r;
        out.type = p->names[size_t(type)];
        out.at = rd.p;
        out.size = size_t(n);
        out.end = size_t(r.exp().off + r.exp().size);
        return true;
    } catch (const FormatError&) {
        return false;
    }
}

std::shared_ptr<const DecodedSound> SoundBank::clip(const Object* sound) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clips_.find(sound);
    if (it != clips_.end()) return it->second;
    std::shared_ptr<const DecodedSound> c;
    File f;
    if (file(sound, f)) {
        try {
            const uint8_t* d = f.ref.pkg->data.data() + f.at;
            bool wav = f.size >= 4 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F';
            c = std::make_shared<const DecodedSound>(wav ? decodeWav(d, f.size) : decodeBink(d, f.size));
            ++decoded;
        } catch (const FormatError&) {
            ++failed;
        }
    }
    clips_[sound] = c;
    return c;
}

std::shared_ptr<const LipSync> SoundBank::lipSync(const Object* sound) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lips_.find(sound);
    if (it != lips_.end()) return it->second;
    std::shared_ptr<const LipSync> l;
    File f;
    if (file(sound, f) && f.end >= f.at + f.size) {
        LipSync ls;
        if (parseLipSync(f.ref.pkg->data.data() + f.at + f.size, f.end - f.at - f.size, ls))
            l = std::make_shared<const LipSync>(std::move(ls));
    }
    lips_[sound] = l;
    return l;
}

float SoundBank::duration(const Object* sound) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = durations_.find(sound);
        if (it != durations_.end()) return it->second;
        auto ct = clips_.find(sound);
        if (ct != clips_.end() && ct->second && ct->second->rate)
            return durations_[sound] = float(ct->second->samples.size()) / float(ct->second->rate * ct->second->channels);
    }
    float d = 0;
    File f;
    std::lock_guard<std::mutex> lock(mutex_);
    if (file(sound, f)) {
        const uint8_t* b = f.ref.pkg->data.data() + f.at;
        auto le32 = [&](size_t o) { return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24; };
        if (f.size >= 64 && b[0] == 'B' && b[1] == 'I' && b[2] == 'K') {
            // each audio packet starts with what it decodes to, in 16 bit bytes
            uint32_t frames = le32(8);
            int rate = b[48] | b[49] << 8;
            size_t table = 56;
            uint64_t bytes = 0;
            for (uint32_t i = 0; i < frames && table + 4 * (i + 1) <= f.size; ++i) {
                size_t at = le32(table + 4 * i) & ~1u;
                if (at + 8 > f.size) break;
                if (le32(at)) bytes += le32(at + 4);
            }
            if (rate) d = float(bytes / 2) / float(rate);
        } else if (f.size >= 44 && b[0] == 'R') {
            for (size_t o = 12; o + 8 <= f.size;) {
                uint32_t len = le32(o + 4);
                if (b[o] == 'f' && b[o + 1] == 'm' && b[o + 2] == 't' && b[o + 3] == ' ') {
                    uint32_t bytesPerSecond = le32(o + 16);
                    for (size_t q = o + 8 + len; q + 8 <= f.size;) {
                        uint32_t l2 = le32(q + 4);
                        if (b[q] == 'd' && b[q + 1] == 'a' && b[q + 2] == 't' && b[q + 3] == 'a' && bytesPerSecond)
                            d = float(l2) / float(bytesPerSecond);
                        q += 8 + l2 + (l2 & 1);
                    }
                    break;
                }
                o += 8 + len + (len & 1);
            }
        }
    }
    return durations_[sound] = d;
}

}  // namespace ffa
