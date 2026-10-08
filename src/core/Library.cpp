#include "core/Library.h"

#include <cctype>
#include <cstring>
#include <filesystem>

#include "script/Tagged.h"

namespace ffa {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

Library::Library(const std::string& gameDir) {
    static const char* dirs[] = {"StaticMeshes", "Textures", "System", "Animations", "Sounds", "Music", "Maps"};
    static const char* exts[] = {".usx", ".utx", ".u", ".ukx", ".uax", ".umx", ".unr"};
    for (const char* d : dirs) {
        std::filesystem::path base = std::filesystem::path(gameDir) / d;
        if (!std::filesystem::is_directory(base)) continue;
        for (const auto& f : std::filesystem::directory_iterator(base)) {
            std::string ext = lower(f.path().extension().string());
            bool known = false;
            for (const char* e : exts) known |= ext == e;
            if (known) files_.emplace(lower(f.path().stem().string()), f.path().string());
        }
    }
}

const Package* Library::package(const std::string& stem) {
    std::string key = lower(stem);
    auto it = open_.find(key);
    if (it != open_.end()) return it->second;
    const Package* p = nullptr;
    auto f = files_.find(key);
    if (f != files_.end()) {
        auto owned = std::make_unique<Package>(f->second);
        p = owned.get();
        owned_[key] = std::move(owned);
    }
    open_[key] = p;
    return p;
}

void Library::adopt(const Package* p) { open_[lower(p->stem)] = p; }

int Library::findByPath(const Package& p, const std::vector<std::string>& parts) {
    for (int i = 1; i <= int(p.exports.size()); ++i) {
        int k = i;
        size_t j = parts.size();
        while (j > 0 && k > 0 && lower(p.exp(k).name) == lower(parts[j - 1])) {
            k = p.exp(k).outer;
            --j;
        }
        if (j == 0 && k == 0) return i;
    }
    return 0;
}

ObjectRef Library::resolve(const Package& p, int32_t ref) {
    if (ref > 0) return ref <= int32_t(p.exports.size()) ? ObjectRef{&p, ref} : ObjectRef{};
    if (ref == 0 || -ref > int32_t(p.imports.size())) return {};
    // the import's path: its name, then its outers', up to the package
    std::vector<std::string> parts;
    int32_t k = ref;
    for (int guard = 0; k < 0 && -k <= int32_t(p.imports.size()) && guard < 64; ++guard) {
        const Import& im = p.imports[size_t(-k - 1)];
        parts.insert(parts.begin(), im.name);
        k = im.outer;
    }
    if (parts.size() < 2) return {};
    const Package* other = package(parts[0]);
    if (!other) return {};
    int idx = findByPath(*other, std::vector<std::string>(parts.begin() + 1, parts.end()));
    if (!idx) return {};
    // the class must agree, as the same path can name different objects
    const Import& im = p.imports[size_t(-ref - 1)];
    if (lower(other->classOf(idx)) != lower(im.clsName)) return {};
    return {other, idx};
}

bool Library::properties(const ObjectRef& o, std::vector<TagEntry>& out, size_t& end) {
    const Export& e = o.exp();
    size_t start = size_t(e.off);
    if (e.flags & 0x02000000) {        // RF_HasStack: the state frame comes first
        Reader r(o.pkg->data, start, size_t(e.off + e.size));
        int32_t node = r.idx();
        r.idx();
        r.p += 12;
        if (node) r.idx();
        start = r.p;
    }
    return parseTagged(*o.pkg, start, size_t(e.off + e.size), out, end);
}

const TagEntry* findTag(const Package& p, const std::vector<TagEntry>& tags, const char* name, int index) {
    for (const TagEntry& t : tags)
        if (t.index == index && p.validName(t.name) && strcasecmp(p.names[size_t(t.name)].c_str(), name) == 0)
            return &t;
    return nullptr;
}

int32_t tagInt(const Package& p, const std::vector<TagEntry>& tags, const char* name, int32_t def) {
    const TagEntry* t = findTag(p, tags, name);
    if (!t) return def;
    Reader r(p.data, t->at, t->at + t->size);
    if (t->type == T_Byte) return r.u8();
    if (t->type == T_Int) return r.i32();
    if (t->type == T_Bool) return t->boolValue;
    return def;
}

float tagFloat(const Package& p, const std::vector<TagEntry>& tags, const char* name, float def) {
    const TagEntry* t = findTag(p, tags, name);
    if (!t || t->type != T_Float) return def;
    Reader r(p.data, t->at, t->at + t->size);
    return r.f32();
}

int32_t tagObject(const Package& p, const std::vector<TagEntry>& tags, const char* name) {
    const TagEntry* t = findTag(p, tags, name);
    if (!t || (t->type != T_Object && t->type != T_Class)) return 0;
    Reader r(p.data, t->at, t->at + t->size);
    return r.idx();
}

}  // namespace ffa
