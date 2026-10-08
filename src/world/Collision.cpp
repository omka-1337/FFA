#include "world/Collision.h"

#include <cctype>
#include <filesystem>

namespace ffa {

namespace {

const int DT_StaticMesh = 8;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The export of p at a dotted path below the package, or 0.
int findByPath(const Package& p, const std::vector<std::string>& parts) {
    for (int i = 1; i <= int(p.exports.size()); ++i) {
        int k = i;
        size_t j = parts.size();
        while (j > 0 && k > 0 && lower(p.exp(k).name) == parts[j - 1]) {
            k = p.exp(k).outer;
            --j;
        }
        if (j == 0 && k == 0) return i;
    }
    return 0;
}

bool invert(const float m[3][3], float out[3][3]) {
    float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (std::fabs(det) < 1e-12f) return false;
    float d = 1 / det;
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * d;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * d;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * d;
    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * d;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * d;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * d;
    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * d;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * d;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * d;
    return true;
}

Vec3 mul(const float m[3][3], Vec3 v) {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Vec3 mulT(const float m[3][3], Vec3 v) {
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
}

}  // namespace

Collision::Collision(World& w, int mapPkg, int32_t model, const std::string& gameDir)
    : world(w), bsp(*w.linker.packages[size_t(mapPkg)], model), map_(*w.linker.packages[size_t(mapPkg)]),
      gameDir_(gameDir) {
    const Package& p = map_;
    for (Object* a : w.actors) {
        if (a->deleted || a->cls->name != Name("TerrainInfo")) continue;
        auto bits = [&](const char* name) {
            std::vector<uint32_t> out;
            const Value& v = w.var(a, name);
            if (v.isArr())
                for (const Value& x : v.arr()) out.push_back(uint32_t(x.i()));
            return out;
        };
        try {
            int idx = w.exportOf.at(a);
            std::vector<TagEntry> entries;
            size_t end = 0;
            const Export& e = p.exp(idx);
            if (!parseTagged(p, Linker::propertiesStart(p, e), size_t(e.off + e.size), entries, end))
                throw FormatError("the TerrainInfo has no property block");
            terrains.push_back(std::make_unique<Terrain>(p, idx, end, bits("QuadVisibilityBitmap"),
                                                         bits("EdgeTurnBitmap")));
            terrainActors.push_back(a);
        } catch (const std::exception& ex) {
            problems[std::string("TerrainInfo: ") + ex.what()]++;
        }
    }
    for (Object* a : w.actors) {
        if (a->deleted || w.var(a, "DrawType").i() != DT_StaticMesh) continue;
        if (!w.flag(a, "bCollideActors") || !w.flag(a, "bBlockZeroExtentTraces")) continue;
        Object* m = w.obj(a, "StaticMesh");
        if (!m) continue;
        ++meshActors;
        const StaticMeshCollision* c = mesh(m);
        if (!c) {
            ++meshesMissing;
            continue;
        }
        placed_.push_back({a, c});
    }
}

const StaticMeshCollision* Collision::mesh(Object* o) {
    std::vector<std::string> parts;
    for (const Object* k = o; k; k = k->outer) parts.insert(parts.begin(), lower(k->name.str()));
    std::string key;
    for (const std::string& s : parts) key += (key.empty() ? "" : ".") + s;
    auto it = meshes_.find(key);
    if (it != meshes_.end()) return it->second.get();
    std::unique_ptr<StaticMeshCollision>& slot = meshes_[key];
    if (parts.size() < 2) return nullptr;
    // The top of the path is the package: the map, or a .usx.
    const Package* p = nullptr;
    if (parts[0] == lower(map_.stem)) {
        p = &map_;
    } else {
        auto pk = packages_.find(parts[0]);
        if (pk == packages_.end()) {
            // A package is named by its file's stem, whatever the extension:
            // the beanstalk bonus maps take meshes from Textures/Beanstalk.utx.
            static const char* dirs[] = {"StaticMeshes", "Textures", "System", "Animations", "Sounds", "Music",
                                         "Maps"};
            static const char* exts[] = {".usx", ".utx", ".u", ".ukx", ".uax", ".umx", ".unr"};
            std::unique_ptr<Package> opened;
            for (const char* d : dirs) {
                std::filesystem::path base = std::filesystem::path(gameDir_) / d;
                if (opened || !std::filesystem::is_directory(base)) continue;
                for (const auto& f : std::filesystem::directory_iterator(base)) {
                    std::string ext = lower(f.path().extension().string());
                    bool known = false;
                    for (const char* e : exts) known |= ext == e;
                    if (known && lower(f.path().stem().string()) == parts[0]) {
                        opened = std::make_unique<Package>(f.path().string());
                        break;
                    }
                }
            }
            pk = packages_.emplace(parts[0], std::move(opened)).first;
        }
        p = pk->second.get();
    }
    if (!p) {
        problems["mesh package not found: " + parts[0]]++;
        return nullptr;
    }
    int idx = findByPath(*p, std::vector<std::string>(parts.begin() + 1, parts.end()));
    if (!idx || p->classOf(idx) != "StaticMesh") {
        problems["mesh not found in its package"]++;
        return nullptr;
    }
    try {
        slot = std::make_unique<StaticMeshCollision>(*p, idx);
    } catch (const FormatError& ex) {
        problems[std::string("StaticMesh: ") + ex.what()]++;
    }
    return slot.get();
}

Collision::Transform Collision::transform(Object* a) {
    // Location + R S (v - PrePivot), as measured for static meshes
    // (docs/package-format.md).
    Transform t;
    VM& vm = world.vm;
    int32_t pitch, yaw, roll;
    vm.unrotator(world.var(a, "Rotation"), pitch, yaw, roll);
    float axes[3][3];
    rotationAxes(pitch, yaw, roll, axes);
    float s = world.var(a, "DrawScale").f();
    Vec3 s3, loc, pp;
    vm.unvector(world.var(a, "DrawScale3D"), s3.x, s3.y, s3.z);
    vm.unvector(world.var(a, "Location"), loc.x, loc.y, loc.z);
    vm.unvector(world.var(a, "PrePivot"), pp.x, pp.y, pp.z);
    const float sc[3] = {s * s3.x, s * s3.y, s * s3.z};
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) t.m[r][k] = axes[k][r] * sc[k];
    t.origin = loc - mul(t.m, pp);
    return t;
}

TraceHit Collision::lineCheck(Vec3 a, Vec3 b, const Object* ignore) {
    TraceHit best;
    static_cast<Hit&>(best) = bsp.lineCheck(a, b);
    for (size_t i = 0; i < terrains.size(); ++i) {
        Hit h = terrains[i]->lineCheck(a, b);
        if (!h || h.time >= best.time) continue;
        static_cast<Hit&>(best) = h;
        best.actor = terrainActors[i];
    }
    for (const Placed& pl : placed_) {
        if (pl.actor == ignore || pl.actor->deleted) continue;
        Transform t = transform(pl.actor);
        float inv[3][3];
        if (!invert(t.m, inv)) continue;
        Hit h = pl.mesh->lineCheck(mul(inv, a - t.origin), mul(inv, b - t.origin));
        if (!h || h.time >= best.time) continue;
        best.time = h.time;
        best.location = lerp(a, b, h.time);
        Vec3 n = mulT(inv, h.normal);
        float len = length(n);
        best.normal = len > 0 ? n * (1 / len) : n;
        best.node = -1;
        best.startSolid = false;
        best.actor = pl.actor;
    }
    return best;
}

}  // namespace ffa
