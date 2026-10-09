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
        if (!w.flag(a, "bCollideActors")) continue;
        // a line and a box ask different flags
        bool line = w.flag(a, "bBlockZeroExtentTraces");
        bool box = w.flag(a, "bBlockNonZeroExtentTraces") && (w.flag(a, "bBlockActors") || w.flag(a, "bBlockPlayers"));
        if (!line && !box) continue;
        Object* m = w.obj(a, "StaticMesh");
        if (!m) continue;
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
        pl.line = line;
        pl.box = box;
        if (line) ++meshActors;
        pl.meshLo = {1e30f, 1e30f, 1e30f};
        pl.meshHi = {-1e30f, -1e30f, -1e30f};
        for (const Vec3& v : c->positions) {
            pl.meshLo = {std::min(pl.meshLo.x, v.x), std::min(pl.meshLo.y, v.y), std::min(pl.meshLo.z, v.z)};
            pl.meshHi = {std::max(pl.meshHi.x, v.x), std::max(pl.meshHi.y, v.y), std::max(pl.meshHi.z, v.z)};
        }
        update(pl);
        placed_.push_back(pl);
    }
    buildLineCells();
    buildStatics();
    buildBrushes();
}

void Collision::buildStatics() {
    std::vector<Triangle> tris;
    // The BSP's faces. A node's polygon bounds solid when the point a unit
    // behind its middle is solid and the point a unit in front is not; sheets
    // and the faces of non solid brushes have open space on both sides.
    for (size_t i = 0; i < bsp.nodes.size(); ++i) {
        const BspNode& n = bsp.nodes[i];
        if (n.numVerts < 3) continue;
        Vec3 mid{};
        for (int k = 0; k < n.numVerts; ++k) mid = mid + bsp.points[size_t(bsp.vertPoints[size_t(n.vertPool + k)])];
        mid = mid * (1.0f / float(n.numVerts));
        bool behind = bsp.solidAt(mid - n.plane.n), before = bsp.solidAt(mid + n.plane.n);
        if (behind == before) {
            ++bspFacesSkipped;
            continue;
        }
        ++bspFaces;
        Vec3 out = behind ? n.plane.n : -n.plane.n;
        Vec3 v0 = bsp.points[size_t(bsp.vertPoints[size_t(n.vertPool)])];
        for (int k = 1; k + 1 < n.numVerts; ++k) {
            Triangle t;
            t.v[0] = v0;
            t.v[1] = bsp.points[size_t(bsp.vertPoints[size_t(n.vertPool + k)])];
            t.v[2] = bsp.points[size_t(bsp.vertPoints[size_t(n.vertPool + k + 1)])];
            if (dot(cross(t.v[1] - t.v[0], t.v[2] - t.v[0]), out) < 0) std::swap(t.v[1], t.v[2]);
            t.oneSided = true;
            tris.push_back(t);
        }
    }
    for (size_t i = 0; i < terrains.size(); ++i) {
        const Terrain& tr = *terrains[i];
        for (int y = 0; y + 1 < tr.Y; ++y)
            for (int x = 0; x + 1 < tr.X; ++x) {
                if (!tr.visible(x, y)) continue;
                int q[2][3];
                tr.quad(x, y, q);
                for (auto& k : q) {
                    Triangle t;
                    for (int j = 0; j < 3; ++j) t.v[j] = tr.vertices[size_t(k[j])];
                    t.actor = terrainActors[i];
                    tris.push_back(t);
                }
            }
    }
    for (Placed& pl : placed_)
        if (pl.fixed && pl.box)
            for (const Triangle& t : worldTriangles(pl)) tris.push_back(t);
    staticTriangles = tris.size();
    statics_.build(std::move(tris));
}

void Collision::buildBrushes() {
    // Every brush the game loads that collides, its polygons carried into the
    // world: Location + PostScale R MainScale (v - PrePivot), as the editor's
    // brushes prove against the level's BSP (docs/package-format.md). Facing
    // out as their normals do, so that what is inside can leave.
    std::vector<Triangle> tris;
    Class* brushClass = world.brushClass;
    for (Object* a : world.actors) {
        if (a->deleted || !a->isA(brushClass) || !world.flag(a, "bCollideActors")) continue;
        Object* mo = world.obj(a, "Brush");
        const std::vector<BrushPolygon>* polys = mo ? brushPolygons(mo) : nullptr;
        if (!polys || polys->empty()) continue;
        float m[3][3], inv[3][3];
        Vec3 o;
        brushTransform(a, m, o);
        if (!invert(m, inv)) continue;
        ++brushActors;
        for (const BrushPolygon& q : *polys) {
            Vec3 n = mulT(inv, q.normal);
            for (size_t k = 1; k + 1 < q.vertices.size(); ++k) {
                Triangle t;
                t.v[0] = o + mul(m, q.vertices[0]);
                t.v[1] = o + mul(m, q.vertices[k]);
                t.v[2] = o + mul(m, q.vertices[k + 1]);
                if (dot(cross(t.v[1] - t.v[0], t.v[2] - t.v[0]), n) < 0) std::swap(t.v[1], t.v[2]);
                t.actor = a;
                t.oneSided = true;
                tris.push_back(t);
            }
        }
    }
    brushTriangles = tris.size();
    brushTris_.build(std::move(tris));
}

bool Collision::brushBlocks(Object* b, const Object* mover, bool line, bool worldOnly) {
    if (b == mover || b->deleted || !world.flag(b, "bCollideActors")) return false;
    if (worldOnly && !world.flag(b, "bWorldGeometry")) return false;
    if (line) return world.flag(b, "bBlockZeroExtentTraces") && world.flag(b, "bWorldGeometry");
    if (!world.flag(b, "bBlockNonZeroExtentTraces")) return false;
    // a player is stopped by bBlockPlayers, anything else by bBlockActors
    bool player = false;
    if (mover) {
        if (mover->isA(world.pawnClass)) {
            Object* c = world.obj(const_cast<Object*>(mover), "Controller");
            player = c && c->isA(world.playerControllerClass);
        }
    }
    return world.flag(b, player ? "bBlockPlayers" : "bBlockActors");
}

std::vector<Triangle> Collision::worldTriangles(Placed& pl) {
    std::vector<Triangle> out;
    for (const MeshTriangle& m : pl.mesh->triangles) {
        Triangle t;
        for (int j = 0; j < 3; ++j) t.v[j] = pl.origin + mul(pl.m, pl.mesh->positions[m.v[j]]);
        t.actor = pl.actor;
        out.push_back(t);
    }
    return out;
}

bool Collision::blocks(Object* o, const Object* ignore) {
    if (o == ignore || o->deleted || o == world.info) return false;
    if (!world.flag(o, "bCollideActors") || !(world.flag(o, "bBlockActors") || world.flag(o, "bBlockPlayers")))
        return false;
    // a static mesh blocks with its triangles, unless it asks for its cylinder
    if (world.var(o, "DrawType").i() == DT_StaticMesh && world.obj(o, "StaticMesh") &&
        !world.flag(o, "bUseCylinderCollision"))
        return false;
    if (o->cls->name == Name("TerrainInfo")) return false;
    return true;
}

TraceHit Collision::boxCheck(Vec3 a, Vec3 b, Vec3 extent, const Object* ignore) {
    TraceHit best;
    best.location = b;
    Vec3 d = b - a;
    Vec3 lo{std::min(a.x, b.x) - extent.x, std::min(a.y, b.y) - extent.y, std::min(a.z, b.z) - extent.z};
    Vec3 hi{std::max(a.x, b.x) + extent.x, std::max(a.y, b.y) + extent.y, std::max(a.z, b.z) + extent.z};
    auto consider = [&](const Triangle& t) {
        if (t.actor && t.actor == ignore) return;
        float time;
        Vec3 n;
        if (!sweepBox(t, a, d, extent, best.time, time, n)) return;
        best.time = time;
        best.normal = n;
        best.actor = t.actor;
        best.startSolid = time == 0;
    };
    statics_.query(lo, hi, consider);
    std::map<Object*, bool> asked;
    brushTris_.query(lo, hi, [&](const Triangle& t) {
        auto it = asked.find(t.actor);
        if (it == asked.end()) it = asked.emplace(t.actor, brushBlocks(t.actor, ignore, false, false)).first;
        if (it->second) consider(t);
    });
    for (Placed& pl : placed_) {
        if (pl.fixed || !pl.box || pl.actor == ignore || pl.actor->deleted) continue;
        update(pl);
        if (pl.hi.x < lo.x || pl.lo.x > hi.x || pl.hi.y < lo.y || pl.lo.y > hi.y || pl.hi.z < lo.z || pl.lo.z > hi.z)
            continue;
        for (const Triangle& t : worldTriangles(pl)) consider(t);
    }
    // The cylinders of the actors that block, as boxes: summed with the
    // moving box they are a larger box the segment enters.
    for (Object* o : colliders()) {
        if (!blocks(o, ignore)) continue;
        ActorShape sh = shapeOf(world, o);
        if (sh.box) {
            Vec3 n;
            float t = enterBox(sh, a, d, extent, n);
            // already overlapping: let it move apart
            if (t < 0 || t > 1 || t >= best.time) continue;
            best.time = t;
            best.normal = n;
            best.actor = o;
            best.startSolid = false;
            continue;
        }
        Vec3 c = sh.center;
        float r = sh.radius, h = sh.height;
        Vec3 bl = c - Vec3{r + extent.x, r + extent.y, h + extent.z}, bh = c + Vec3{r + extent.x, r + extent.y, h + extent.z};
        float enter = -1e30f, exit = 1e30f;
        Vec3 axis{};
        bool miss = false;
        const float pa[3] = {a.x, a.y, a.z}, pd[3] = {d.x, d.y, d.z};
        const float mn[3] = {bl.x, bl.y, bl.z}, mx[3] = {bh.x, bh.y, bh.z};
        for (int k = 0; k < 3 && !miss; ++k) {
            if (std::fabs(pd[k]) < 1e-9f) {
                miss = pa[k] < mn[k] || pa[k] > mx[k];
                continue;
            }
            float t0 = (mn[k] - pa[k]) / pd[k], t1 = (mx[k] - pa[k]) / pd[k];
            if (t0 > t1) std::swap(t0, t1);
            if (t0 > enter) {
                enter = t0;
                axis = {};
                (&axis.x)[k] = pd[k] > 0 ? -1.0f : 1.0f;
            }
            exit = std::min(exit, t1);
            miss = enter > exit;
        }
        if (miss || exit < 0 || enter > 1 || enter >= best.time) continue;
        if (enter < 0) continue;    // already overlapping: let it move apart
        best.time = enter;
        best.normal = axis;
        best.actor = o;
        best.startSolid = false;
    }
    if (best) best.location = a + d * best.time;
    return best;
}

bool Collision::fits(Vec3 p, Vec3 extent, const Object* ignore) {
    bool free = true;
    statics_.query(p - extent, p + extent, [&](const Triangle& t) {
        if (free && !(t.actor && t.actor == ignore) && overlapsBox(t, p, extent)) free = false;
    });
    brushTris_.query(p - extent, p + extent, [&](const Triangle& t) {
        if (free && brushBlocks(t.actor, ignore, false, false) && overlapsBox(t, p, extent)) free = false;
    });
    return free;
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

const std::vector<BrushPolygon>* Collision::brushPolygons(Object* o) {
    auto it = brushes_.find(o);
    if (it != brushes_.end()) return it->second.get();
    auto& slot = brushes_[o];
    std::vector<std::string> parts;
    for (const Object* k = o; k; k = k->outer) parts.insert(parts.begin(), lower(k->name.str()));
    if (parts.size() < 2 || parts[0] != lower(map_.stem)) return nullptr;
    int idx = findByPath(map_, std::vector<std::string>(parts.begin() + 1, parts.end()));
    if (!idx || map_.classOf(idx) != "Model") return nullptr;
    try {
        BspModel m(map_, idx);
        if (m.polys <= 0 || map_.classOf(m.polys) != "Polys") return nullptr;
        slot = std::make_unique<std::vector<BrushPolygon>>(readPolys(map_, m.polys));
    } catch (const FormatError& ex) {
        problems[std::string("brush: ") + ex.what()]++;
    }
    return slot.get();
}

void Collision::brushTransform(Object* a, float m[3][3], Vec3& origin) {
    VM& vm = world.vm;
    int32_t pitch, yaw, roll;
    vm.unrotator(world.var(a, "Rotation"), pitch, yaw, roll);
    float axes[3][3];
    rotationAxes(pitch, yaw, roll, axes);
    auto scale = [&](const char* name) {
        const StructVal& sv = world.var(a, name).st();
        Vec3 v;
        vm.unvector(sv.f[size_t(sv.type->field(Name("Scale"))->slot)], v.x, v.y, v.z);
        return v;
    };
    Vec3 main = scale("MainScale"), post = scale("PostScale"), loc, pp;
    vm.unvector(world.var(a, "Location"), loc.x, loc.y, loc.z);
    vm.unvector(world.var(a, "PrePivot"), pp.x, pp.y, pp.z);
    const float ms[3] = {main.x, main.y, main.z}, ps[3] = {post.x, post.y, post.z};
    // column k: the rotated, scaled axis k
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) m[r][k] = ps[r] * axes[k][r] * ms[k];
    origin = loc - mul(m, pp);
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
    const Vec3 lo = pl.meshLo, hi = pl.meshHi;
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
}  // namespace

ActorShape shapeOf(World& w, Object* a) {
    ActorShape sh;
    w.vm.unvector(w.var(a, "Location"), sh.center.x, sh.center.y, sh.center.z);
    sh.radius = w.var(a, "CollisionRadius").f();
    sh.height = w.var(a, "CollisionHeight").f();
    if (Prop* p = a->cls->findProp(Name("CollideType")); p && a->props[size_t(p->slot)].i() == 1) {
        sh.box = true;
        sh.width = w.var(a, "CollisionWidth").f();
        int32_t pitch, yaw, roll;
        w.vm.unrotator(w.var(a, "Rotation"), pitch, yaw, roll);
        float ang = float(yaw) * 3.14159265f / 32768.0f;
        sh.c = std::cos(ang);
        sh.s = std::sin(ang);
    }
    return sh;
}

float enterBox(const ActorShape& sh, Vec3 a, Vec3 d, Vec3 extent, Vec3& normal) {
    // in the box's frame, the moving box's extent turned with it, made
    // square to its axes
    Vec3 la = sh.local(a), ld{sh.c * d.x + sh.s * d.y, -sh.s * d.x + sh.c * d.y, d.z};
    float ac = std::fabs(sh.c), as = std::fabs(sh.s);
    Vec3 e{ac * extent.x + as * extent.y, as * extent.x + ac * extent.y, extent.z};
    const float half[3] = {sh.radius + e.x, sh.width + e.y, sh.height + e.z};
    const float pa[3] = {la.x, la.y, la.z}, pd[3] = {ld.x, ld.y, ld.z};
    float enter = -1e30f, exit = 1e30f;
    Vec3 axis{};
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(pd[k]) < 1e-9f) {
            if (pa[k] < -half[k] || pa[k] > half[k]) return 2;
            continue;
        }
        float t0 = (-half[k] - pa[k]) / pd[k], t1 = (half[k] - pa[k]) / pd[k];
        if (t0 > t1) std::swap(t0, t1);
        if (t0 > enter) {
            enter = t0;
            axis = {};
            (&axis.x)[k] = pd[k] > 0 ? -1.0f : 1.0f;
        }
        exit = std::min(exit, t1);
        if (enter > exit) return 2;
    }
    if (exit < 0) return 2;
    normal = sh.world(axis);
    return enter;
}

namespace {

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

void Collision::buildLineCells() {
    lineStamp_.assign(placed_.size(), 0);
    for (uint32_t i = 0; i < placed_.size(); ++i) {
        const Placed& pl = placed_[i];
        if (!pl.line) continue;
        if (!pl.fixed) {
            lineMoving_.push_back(i);
            continue;
        }
        int x0 = int(std::floor(pl.lo.x / kCell)), x1 = int(std::floor(pl.hi.x / kCell));
        int y0 = int(std::floor(pl.lo.y / kCell)), y1 = int(std::floor(pl.hi.y / kCell));
        if ((int64_t(x1) - x0 + 1) * (int64_t(y1) - y0 + 1) > 4096) {
            lineMoving_.push_back(i);        // too big for cells: asked always
            continue;
        }
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) lineCells_[int64_t(x) * 1000003 + y].push_back(i);
    }
}

const std::vector<Object*>& Collision::colliders() {
    if (collidersFrame_ != world.frames || collidersCount_ != world.actors.size() ||
        collidersChanges_ != world.collisionChanges) {
        collidersFrame_ = world.frames;
        collidersCount_ = world.actors.size();
        collidersChanges_ = world.collisionChanges;
        colliders_.clear();
        for (Object* o : world.actors)
            if (!o->deleted && o != world.info && world.flag(o, "bCollideActors")) colliders_.push_back(o);
    }
    return colliders_;
}

void Collision::meshHits(Vec3 a, Vec3 b, const Object* ignore, bool worldOnly, std::vector<TraceHit>* all,
                         TraceHit& best) {
    // the meshes whose cells the segment's box covers, each once, and the
    // moving ones; a long segment asks them all
    std::vector<uint32_t> ask;
    int x0 = int(std::floor(std::min(a.x, b.x) / kCell)), x1 = int(std::floor(std::max(a.x, b.x) / kCell));
    int y0 = int(std::floor(std::min(a.y, b.y) / kCell)), y1 = int(std::floor(std::max(a.y, b.y) / kCell));
    if ((int64_t(x1) - x0 + 1) * (int64_t(y1) - y0 + 1) > 64) {
        for (uint32_t i = 0; i < placed_.size(); ++i) ask.push_back(i);
    } else {
        if (++stamp_ == 0) {
            std::fill(lineStamp_.begin(), lineStamp_.end(), 0);
            stamp_ = 1;
        }
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y) {
                auto it = lineCells_.find(int64_t(x) * 1000003 + y);
                if (it == lineCells_.end()) continue;
                for (uint32_t i : it->second)
                    if (lineStamp_[i] != stamp_) {
                        lineStamp_[i] = stamp_;
                        ask.push_back(i);
                    }
            }
        ask.insert(ask.end(), lineMoving_.begin(), lineMoving_.end());
    }
    for (uint32_t i : ask) {
        Placed& pl = placed_[i];
        if (!pl.line || pl.actor == ignore || pl.actor->deleted || (worldOnly && !pl.world)) continue;
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
    {
        Vec3 lo{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
        Vec3 hi{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
        Vec3 d = b - a;
        brushTris_.query(lo, hi, [&](const Triangle& t) {
            float time;
            Vec3 n;
            if (!sweepBox(t, a, d, Vec3{}, best.time, time, n) || time == 0) return;
            if (!brushBlocks(t.actor, ignore, !actors, worldOnly) && !(actors && brushBlocks(t.actor, ignore, false, worldOnly)))
                return;
            best.time = time;
            best.location = lerp(a, b, time);
            best.normal = n;
            best.node = -1;
            best.startSolid = false;
            best.actor = t.actor;
        });
    }
    if (actors) {
        for (Object* o : colliders()) {
            if (o == ignore || o->deleted || o == world.info) continue;
            if (!world.flag(o, "bCollideActors") || !world.flag(o, "bBlockZeroExtentTraces")) continue;
            if (world.var(o, "DrawType").i() == DT_StaticMesh && !world.flag(o, "bUseCylinderCollision")) continue;
            ActorShape sh = shapeOf(world, o);
            Vec3 n;
            float t = sh.box ? enterBox(sh, a, b - a, {}, n) : cylinder(a, b, sh.center, sh.radius, sh.height, n);
            if (t < 0 || t >= best.time) continue;
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
    for (Object* o : colliders()) {
        if (o == ignore || o->deleted || o == world.info) continue;
        if (!world.flag(o, "bCollideActors")) continue;
        if (world.var(o, "DrawType").i() == DT_StaticMesh && !world.flag(o, "bUseCylinderCollision")) continue;
        ActorShape sh = shapeOf(world, o);
        Vec3 n;
        float t = sh.box ? enterBox(sh, a, b - a, {}, n) : cylinder(a, b, sh.center, sh.radius, sh.height, n);
        if (t < 0 || t >= 1) continue;
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
        TraceHit h = hasExtent(c, 5) ? col.boxCheck(start, end, vecArg(c, 5), c.self)
                                     : col.lineCheck(start, end, c.self, c.b(4));
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
    // VisibleCollidingActors(BaseClass, out Actor, Radius, optional Loc,
    // optional bIgnoreHidden): every actor of the class that collides with
    // actors, within the radius of the place, and seen from it through the
    // world's geometry.
    n["actor.visiblecollidingactors"] = [](NativeCall& c) {
        Collision& col = collisionOf(c);
        World& w = col.world;
        Object* base = c.o(0);
        if (!base || !base->isClass()) return Value();
        float radius = c.f(2);
        Vec3 at = c.has(3) ? vecArg(c, 3) : locationOf(w, c.self);
        bool skipHidden = c.b(4);
        std::vector<Object*> found;
        for (Object* a : w.actors) {
            if (a->deleted || !a->isA(static_cast<Class*>(base)) || !w.flag(a, "bCollideActors")) continue;
            if (skipHidden && w.flag(a, "bHidden")) continue;
            Vec3 p = locationOf(w, a);
            if (length(p - at) > radius) continue;
            if (col.lineCheck(at, p, c.self, false, true)) continue;
            found.push_back(a);
        }
        for (Object* a : found) c.yield({Value::Obj(a)});
        return Value();
    };
    // SetLocation moves the actor and updates its zone, refusing a place that
    // an actor colliding with the world would not fit.
    n["actor.setlocation"] = [](NativeCall& c) {
        Collision& col = collisionOf(c);
        World& w = col.world;
        Vec3 to = vecArg(c, 0);
        // An actor that collides with the world goes only where it fits.
        if (w.flag(c.self, "bCollideWorld")) {
            float r = w.var(c.self, "CollisionRadius").f();
            if (!col.fits(to, Vec3{r, r, w.var(c.self, "CollisionHeight").f()}, c.self)) return Value::Bool(false);
        }
        col.place(c.self, to);
        return Value::Bool(true);
    };
}

}  // namespace ffa
