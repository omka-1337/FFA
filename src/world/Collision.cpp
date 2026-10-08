#include "world/Collision.h"

#include <cctype>
#include <algorithm>
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
        Placed pl{};
        pl.actor = a;
        pl.mesh = c;
        pl.fixed = w.flag(a, "bStatic");
        pl.world = w.flag(a, "bWorldGeometry");
        update(pl);
        placed_.push_back(pl);
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

void Collision::update(Placed& pl) {
    Transform t = transform(pl.actor);
    std::copy(&t.m[0][0], &t.m[0][0] + 9, &pl.m[0][0]);
    pl.origin = t.origin;
    pl.invertible = invert(pl.m, pl.inv);
    // the mesh's box, carried into the world by its eight corners
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const Vec3& v : pl.mesh->positions) {
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    pl.lo = {1e30f, 1e30f, 1e30f};
    pl.hi = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < 8; ++k) {
        Vec3 c{k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
        Vec3 w = pl.origin + mul(pl.m, c);
        pl.lo = {std::min(pl.lo.x, w.x), std::min(pl.lo.y, w.y), std::min(pl.lo.z, w.z)};
        pl.hi = {std::max(pl.hi.x, w.x), std::max(pl.hi.y, w.y), std::max(pl.hi.z, w.z)};
    }
}

namespace {

bool segmentMeetsBox(Vec3 a, Vec3 b, Vec3 lo, Vec3 hi) {
    float t0 = 0, t1 = 1;
    const float pa[3] = {a.x, a.y, a.z}, pb[3] = {b.x, b.y, b.z};
    const float mn[3] = {lo.x - 1, lo.y - 1, lo.z - 1}, mx[3] = {hi.x + 1, hi.y + 1, hi.z + 1};
    for (int k = 0; k < 3; ++k) {
        float d = pb[k] - pa[k];
        if (std::fabs(d) < 1e-12f) {
            if (pa[k] < mn[k] || pa[k] > mx[k]) return false;
            continue;
        }
        float u = (mn[k] - pa[k]) / d, v = (mx[k] - pa[k]) / d;
        if (u > v) std::swap(u, v);
        t0 = std::max(t0, u);
        t1 = std::min(t1, v);
        if (t0 > t1) return false;
    }
    return true;
}

// Where the segment a..b first meets an upright cylinder, or 1.
float cylinder(Vec3 a, Vec3 b, Vec3 c, float r, float h, Vec3& normal) {
    Vec3 d = b - a, o = a - c;
    float best = 1;
    // the side
    float A = d.x * d.x + d.y * d.y, B = 2 * (o.x * d.x + o.y * d.y), C = o.x * o.x + o.y * o.y - r * r;
    if (A > 1e-12f) {
        float disc = B * B - 4 * A * C;
        if (disc >= 0) {
            float t = (-B - std::sqrt(disc)) / (2 * A);
            float z = o.z + d.z * t;
            if (t >= 0 && t < best && std::fabs(z) <= h) {
                best = t;
                Vec3 p = o + d * t;
                normal = Vec3{p.x, p.y, 0} * (1 / r);
            }
        }
    }
    // the caps
    for (float cap : {h, -h}) {
        if (std::fabs(d.z) < 1e-12f) break;
        float t = (cap - o.z) / d.z;
        if (t < 0 || t >= best) continue;
        Vec3 p = o + d * t;
        if (p.x * p.x + p.y * p.y > r * r) continue;
        if ((cap > 0) != (d.z < 0)) continue;   // a cap is met from outside only
        best = t;
        normal = {0, 0, cap > 0 ? 1.0f : -1.0f};
    }
    return best;
}

}  // namespace

void Collision::meshHits(Vec3 a, Vec3 b, const Object* ignore, bool worldOnly, std::vector<TraceHit>* all,
                         TraceHit& best) {
    for (Placed& pl : placed_) {
        if (pl.actor == ignore || pl.actor->deleted || (worldOnly && !pl.world)) continue;
        if (!pl.fixed) update(pl);
        if (!pl.invertible || !segmentMeetsBox(a, b, pl.lo, pl.hi)) continue;
        Vec3 ma = mul(pl.inv, a - pl.origin), mb = mul(pl.inv, b - pl.origin);
        Hit h = everyTriangle ? pl.mesh->lineCheckAll(ma, mb) : pl.mesh->lineCheck(ma, mb);
        if (!h || (!all && h.time >= best.time)) continue;
        TraceHit t;
        t.time = h.time;
        t.location = lerp(a, b, h.time);
        Vec3 n = mulT(pl.inv, h.normal);
        float len = length(n);
        t.normal = len > 0 ? n * (1 / len) : n;
        t.actor = pl.actor;
        if (all) all->push_back(t);
        if (t.time < best.time) best = t;
    }
}

TraceHit Collision::lineCheck(Vec3 a, Vec3 b, const Object* ignore, bool actors, bool worldOnly) {
    TraceHit best;
    static_cast<Hit&>(best) = bsp.lineCheck(a, b);
    for (size_t i = 0; i < terrains.size(); ++i) {
        Hit h = terrains[i]->lineCheck(a, b);
        if (!h || h.time >= best.time) continue;
        static_cast<Hit&>(best) = h;
        best.actor = terrainActors[i];
    }
    meshHits(a, b, ignore, worldOnly, nullptr, best);
    if (actors) {
        for (Object* o : world.actors) {
            if (o == ignore || o->deleted || o == world.info) continue;
            if (!world.flag(o, "bCollideActors") || !world.flag(o, "bBlockZeroExtentTraces")) continue;
            if (world.var(o, "DrawType").i() == DT_StaticMesh && !world.flag(o, "bUseCylinderCollision")) continue;
            Vec3 c, n;
            world.vm.unvector(world.var(o, "Location"), c.x, c.y, c.z);
            float t = cylinder(a, b, c, world.var(o, "CollisionRadius").f(), world.var(o, "CollisionHeight").f(), n);
            if (t >= best.time) continue;
            best.time = t;
            best.location = lerp(a, b, t);
            best.normal = n;
            best.node = -1;
            best.startSolid = false;
            best.actor = o;
        }
    }
    return best;
}

std::vector<TraceHit> Collision::multiLineCheck(Vec3 a, Vec3 b, const Object* ignore) {
    Hit level = bsp.lineCheck(a, b);
    std::vector<TraceHit> all;
    TraceHit best;
    best.time = level.time;
    meshHits(a, b, ignore, false, &all, best);
    for (size_t i = 0; i < terrains.size(); ++i) {
        Hit h = terrains[i]->lineCheck(a, b);
        if (!h) continue;
        TraceHit t;
        static_cast<Hit&>(t) = h;
        t.actor = terrainActors[i];
        all.push_back(t);
    }
    for (Object* o : world.actors) {
        if (o == ignore || o->deleted || o == world.info) continue;
        if (!world.flag(o, "bCollideActors")) continue;
        if (world.var(o, "DrawType").i() == DT_StaticMesh && !world.flag(o, "bUseCylinderCollision")) continue;
        Vec3 c, n;
        world.vm.unvector(world.var(o, "Location"), c.x, c.y, c.z);
        float t = cylinder(a, b, c, world.var(o, "CollisionRadius").f(), world.var(o, "CollisionHeight").f(), n);
        if (t >= 1) continue;
        TraceHit h;
        h.time = t;
        h.location = lerp(a, b, t);
        h.normal = n;
        h.actor = o;
        all.push_back(h);
    }
    std::vector<TraceHit> out;
    for (const TraceHit& h : all)
        if (h.time <= level.time) out.push_back(h);
    std::sort(out.begin(), out.end(), [](const TraceHit& x, const TraceHit& y) { return x.time < y.time; });
    // The level's own geometry ends the list, as the LevelInfo: the camera's
    // script asks a hit whether it IsA('LevelInfo').
    if (level) {
        TraceHit h;
        static_cast<Hit&>(h) = level;
        h.actor = world.info;
        out.push_back(h);
    }
    return out;
}

void Collision::place(Object* a, Vec3 p, bool events) {
    VM& vm = world.vm;
    world.var(a, "Location") = vm.vector(p.x, p.y, p.z);
    BspModel::Region r = bsp.regionAt(p);
    Object* zone = world.info;
    if (r.zone >= 0 && size_t(r.zone) < bsp.zoneActors.size()) {
        auto it = world.actorAt.find(bsp.zoneActors[size_t(r.zone)]);
        if (it != world.actorAt.end()) zone = it->second;
    }
    Value& region = world.var(a, "Region");
    StructVal& sv = region.st();
    Prop* zf = sv.type->field(Name("Zone"));
    Prop* lf = sv.type->field(Name("iLeaf"));
    Prop* nf = sv.type->field(Name("ZoneNumber"));
    Object* old = sv.f[size_t(zf->slot)].o();
    sv.f[size_t(lf->slot)] = Value::Int(r.leaf);
    sv.f[size_t(nf->slot)] = Value::Int(r.zone);
    if (old == zone) return;
    sv.f[size_t(zf->slot)] = Value::Obj(zone);
    if (!events) return;
    if (old) vm.event(old, "ActorLeaving", {Value::Obj(a)});
    vm.event(a, "ZoneChange", {Value::Obj(zone)});
    vm.event(zone, "ActorEntered", {Value::Obj(a)});
}

// ================================================================ natives
namespace {

Collision& collisionOf(NativeCall& c) {
    World* w = World::of(c.vm);
    if (!w || !w->collision) throw c.vm.error("native " + c.fn->qualname() + " needs a level's collision");
    return *w->collision;
}

Vec3 vecArg(NativeCall& c, size_t k) {
    Vec3 v;
    c.vm.unvector(c.get(k), v.x, v.y, v.z);
    return v;
}

Vec3 locationOf(World& w, Object* a) {
    Vec3 v;
    w.vm.unvector(w.var(a, "Location"), v.x, v.y, v.z);
    return v;
}

bool hasExtent(NativeCall& c, size_t k) {
    if (!c.has(k)) return false;
    Vec3 e = vecArg(c, k);
    return e.x != 0 || e.y != 0 || e.z != 0;
}

}  // namespace

void registerCollisionNatives(VM& vm) {
    auto& n = vm.natives;
    // Trace(out HitLocation, out HitNormal, End, optional Start, optional
    // bTraceActors, optional Extent, out optional Material, optional flags).
    // What it returns for the level's own geometry is the LevelInfo.
    n["actor.trace"] = [](NativeCall& c) {
        Collision& col = collisionOf(c);
        World& w = col.world;
        Vec3 end = vecArg(c, 2);
        Vec3 start = c.has(3) ? vecArg(c, 3) : locationOf(w, c.self);
        // A box is traced as a line until extents are done.
        if (hasExtent(c, 5)) c.vm.missingCalls["Actor.Trace with an extent, traced as a line"]++;
        TraceHit h = col.lineCheck(start, end, c.self, c.b(4));
        if (!h) {
            c.out(0, c.vm.vector(0, 0, 0));
            c.out(1, c.vm.vector(0, 0, 0));
            return Value::Obj(nullptr);
        }
        c.out(0, c.vm.vector(h.location.x, h.location.y, h.location.z));
        c.out(1, c.vm.vector(h.normal.x, h.normal.y, h.normal.z));
        return Value::Obj(h.actor ? h.actor : w.info);
    };
    // FastTrace(End, optional Start): whether nothing of the world's geometry
    // is in the way.
    n["actor.fasttrace"] = [](NativeCall& c) {
        Collision& col = collisionOf(c);
        Vec3 start = c.has(1) ? vecArg(c, 1) : locationOf(col.world, c.self);
        return Value::Bool(!col.lineCheck(start, vecArg(c, 0), c.self, false, true));
    };
    // TraceActors(BaseClass, out Actor, out HitLoc, out HitNorm, End, optional
    // Start, optional Extent, optional flags): every actor of the class along
    // the line up to the level's geometry.
    n["actor.traceactors"] = [](NativeCall& c) {
        Collision& col = collisionOf(c);
        Object* base = c.o(0);
        if (!base || !base->isClass()) return Value();
        Vec3 start = c.has(5) ? vecArg(c, 5) : locationOf(col.world, c.self);
        if (hasExtent(c, 6)) c.vm.missingCalls["Actor.TraceActors with an extent, traced as a line"]++;
        for (const TraceHit& h : col.multiLineCheck(start, vecArg(c, 4), c.self)) {
            if (h.actor && h.actor->isA(static_cast<Class*>(base)))
                c.yield({Value::Obj(h.actor), c.vm.vector(h.location.x, h.location.y, h.location.z),
                         c.vm.vector(h.normal.x, h.normal.y, h.normal.z)});
        }
        return Value();
    };
    // SetLocation moves the actor and updates its zone. For an actor that
    // collides the engine also refuses a place it would not fit, which needs
    // box checks; until then it is moved regardless, and counted.
    n["actor.setlocation"] = [](NativeCall& c) {
        Collision& col = collisionOf(c);
        World& w = col.world;
        if (w.flag(c.self, "bCollideActors") || w.flag(c.self, "bCollideWorld"))
            c.vm.missingCalls["Actor.SetLocation of a colliding actor, not tested for room"]++;
        col.place(c.self, vecArg(c, 0));
        return Value::Bool(true);
    };
}

}  // namespace ffa
