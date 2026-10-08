#include "core/Package.h"

#include <filesystem>
#include <fstream>

namespace ffa {

namespace {
const uint32_t kTag = 0x9E2A83C1;
const std::string kNone = "None", kUnknown = "?";
}  // namespace

Package::Package(const std::string& p) : path(p) {
    std::filesystem::path fp(p);
    file = fp.filename().string();
    stem = fp.stem().string();
    std::ifstream in(p, std::ios::binary);
    if (!in) throw FormatError("cannot open " + p);
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());

    Reader r(data, 0, data.size());
    if (r.u32() != kTag) throw FormatError(file + ": not an Unreal package");
    version = r.u16();
    licensee = r.u16();
    flags = r.u32();
    uint32_t nameCount = r.u32(), nameOff = r.u32();
    uint32_t exportCount = r.u32(), exportOff = r.u32();
    uint32_t importCount = r.u32(), importOff = r.u32();
    (void)nameOff;

    Reader n(data, nameOff, data.size());
    names.reserve(nameCount);
    for (uint32_t i = 0; i < nameCount; ++i) {
        int32_t len = n.idx();
        if (len < 0) throw FormatError(file + ": bad name length");
        n.need(size_t(len));
        std::string s(reinterpret_cast<const char*>(data.data() + n.p), size_t(len));
        s = s.substr(0, s.find('\0'));
        names.push_back(std::move(s));
        n.p += size_t(len);
        n.u32();
    }
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == "None") { noneIndex = int(i); break; }

    Reader im(data, importOff, data.size());
    for (uint32_t i = 0; i < importCount; ++i) {
        Import x;
        int32_t cp = im.idx(), cn = im.idx();
        x.outer = im.i32();
        int32_t on = im.idx();
        x.clsPackage = names.at(size_t(cp));
        x.clsName = names.at(size_t(cn));
        x.name = names.at(size_t(on));
        imports.push_back(std::move(x));
    }

    Reader ex(data, exportOff, data.size());
    for (uint32_t i = 0; i < exportCount; ++i) {
        Export x;
        x.cls = ex.idx();
        x.super = ex.idx();
        x.outer = ex.i32();
        x.name = names.at(size_t(ex.idx()));
        x.flags = ex.u32();
        x.size = ex.idx();
        x.off = x.size > 0 ? ex.idx() : 0;
        if (x.size < 0 || size_t(x.off) + size_t(x.size) > data.size())
            throw FormatError(file + ": export " + x.name + " lies outside the file");
        exports.push_back(std::move(x));
    }
}

const std::string& Package::refName(int32_t ref) const {
    if (ref > 0 && size_t(ref) <= exports.size()) return exports[size_t(ref) - 1].name;
    if (ref < 0 && size_t(-ref) <= imports.size()) return imports[size_t(-ref) - 1].name;
    return ref == 0 ? kNone : kUnknown;
}

std::string Package::classOf(const Export& e) const {
    return e.cls ? refName(e.cls) : std::string("Class");
}

Name Package::name(int i) const {
    if (interned_.empty() && !names.empty()) {
        interned_.reserve(names.size());
        for (const auto& s : names) interned_.emplace_back(s);
    }
    if (i < 0 || size_t(i) >= interned_.size()) throw FormatError(file + ": name index out of range");
    return interned_[size_t(i)];
}

}  // namespace ffa
