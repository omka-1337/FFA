// What a trace in a level can hit: the level's BSP, its terrains, and the
// actors drawn as static meshes that block traces.
//
// A mesh is found by the path its actor refers to it by: in the map itself, or
// in a package of the game's directories, opened when first needed.
#pragma once

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <string>
#include <vector>

#include "core/Library.h"
#include "world/Bsp.h"
#include "world/StaticMesh.h"
#include "world/Sweep.h"
#include "world/Terrain.h"
#include "world/World.h"

namespace ffa {

struct TraceHit : Hit {
    Object* actor = nullptr;        // what was hit; none for the level's BSP
};

class Collision {
public:
    // gameDir: the game's directory, the one holding System and StaticMeshes.
    Collision(World& w, int mapPkg, int32_t model, const std::string& gameDir);

    World& world;
    BspModel bsp;
    std::vector<std::unique_ptr<Terrain>> terrains;
    std::vector<Object*> terrainActors;

    // The first thing the segment from a to b hits: the BSP, the terrains,
    // and every blocking static mesh actor but `ignore`. With `actors`, also
    // the collision cylinders of the actors that block traces. With
    // `worldOnly`, only what is world geometry.
    TraceHit lineCheck(Vec3 a, Vec3 b, const Object* ignore = nullptr, bool actors = false,
                       bool worldOnly = false);
    // The Material a trace's hit lies on, for Trace's out Material: the BSP
    // surface's, or the terrain's layer that shows most where it hit; null
    // for an actor, whose own MaterialType tells. Its MaterialType is the
    // one its package gives it.
    Object* materialOf(const TraceHit& h);
    Library* library = nullptr;     // for the terrains' alpha maps

    // Every actor the segment passes through before it hits the BSP, nearest
    // first, and then the BSP's hit as the LevelInfo.
    std::vector<TraceHit> multiLineCheck(Vec3 a, Vec3 b, const Object* ignore = nullptr);

    // The first thing a box of half size `extent` moving from a to b
    // touches: the level's solid faces, the terrains, the static meshes and
    // the cylinders of the actors that block, all but `ignore`.
    TraceHit boxCheck(Vec3 a, Vec3 b, Vec3 extent, const Object* ignore = nullptr);
    // Whether a box of half size `extent` at p touches none of that.
    bool fits(Vec3 p, Vec3 extent, const Object* ignore = nullptr);

    // The level's polygons that bound solid, used for boxes: those with solid
    // just behind them and open space just in front, by the same walk.
    size_t bspFaces = 0, bspFacesSkipped = 0, staticTriangles = 0;
    size_t brushActors = 0, brushTriangles = 0;

    // Move an actor to p, and update its Region; with `events`, the zones'
    // ActorLeaving and ActorEntered and the actor's ZoneChange when the zone
    // is another. Collision is not tested.
    void place(Object* a, Vec3 p, bool events = true);

    // The collision of a StaticMesh object, or null when it cannot be found.
    const StaticMeshCollision* mesh(Object* meshObject);
    // The polygons of a brush, by its Model, in the map; null when there are
    // none.
    const std::vector<BrushPolygon>* brushPolygons(Object* modelObject);
    // A brush actor's model to world transform, world = origin + M v:
    // Location + PostScale R MainScale (v - PrePivot).
    void brushTransform(Object* a, float m[3][3], Vec3& origin);

    bool everyTriangle = false;     // test meshes without their trees, as a check
    size_t meshActors = 0, meshesMissing = 0;
    std::map<std::string, size_t> problems;

private:
    // a terrain's layers, bottom first: the material and its weight at
    // every grid vertex, from its alpha map
    struct Layer {
        Object* material;
        std::vector<uint8_t> weight;
    };
    std::map<size_t, std::vector<Layer>> terrainLayers_;
    std::map<Object*, bool> materialTyped_;
    void refreshOff();
    std::unordered_set<const Object*> off_;
    size_t offChanges_ = ~size_t(0);
    int mapPkg_ = 0;
    ObjectRef refOf(const Object* o);
    struct Placed {
        Object* actor;
        const StaticMeshCollision* mesh;
        bool fixed;                 // bStatic: its transform is kept
        bool world;                 // bWorldGeometry
        bool line, box;             // blocks lines, boxes
        float m[3][3], inv[3][3];
        Vec3 origin, lo, hi;        // and its box in the world
        Vec3 meshLo, meshHi;        // its box in the mesh, found once
        bool invertible = false;
    };
    void update(Placed& pl);
    // The fixed meshes that block lines, by cells of the ground they cover,
    // and the others, asked one by one.
    static constexpr float kCell = 1024;
    std::unordered_map<int64_t, std::vector<uint32_t>> lineCells_;
    std::vector<uint32_t> lineMoving_, lineStamp_;
    uint32_t stamp_ = 0;
    void buildLineCells();
    // The actors whose collision is on, kept for a frame, until one is
    // spawned or SetCollision is called.
    const std::vector<Object*>& colliders();
    std::vector<Object*> colliders_;
    size_t collidersFrame_ = ~size_t(0), collidersCount_ = 0, collidersChanges_ = 0;
    void meshHits(Vec3 a, Vec3 b, const Object* ignore, bool worldOnly, std::vector<TraceHit>* all,
                  TraceHit& best);
    // An actor's mesh to world transform: world = origin + M v.
    struct Transform {
        float m[3][3];              // columns are the scaled axes
        Vec3 origin;
    };
    Transform transform(Object* a);

    void buildStatics();
    std::vector<Triangle> worldTriangles(Placed& pl);
    bool blocks(Object* o, const Object* ignore);
    TriangleTree statics_;
    // Volumes and other brushes that may block, whose flags script can switch:
    // asked at each query.
    TriangleTree brushTris_;
    void buildBrushes();
    bool brushBlocks(Object* b, const Object* mover, bool line, bool worldOnly);

    const Package& map_;
    std::string gameDir_;
    std::vector<Placed> placed_;
    std::map<std::string, std::unique_ptr<Package>> packages_;
    std::map<std::string, std::unique_ptr<StaticMeshCollision>> meshes_;   // by path
    std::map<const Object*, std::unique_ptr<std::vector<BrushPolygon>>> brushes_;
};

// An actor's collision shape, KnowWonder's CollideType: CT_Cylinder, the
// engine's cylinder of CollisionRadius and CollisionHeight about Location, or
// CT_Box, a box turned with the actor's yaw whose half sizes are
// CollisionRadius along its X, CollisionWidth along its Y and CollisionHeight
// up. A conveyor of the factory, conveyor_belt_base at DrawScale3D 1.25, is
// 170 by 49 by 40 from its middle, and its CT_Box is 176 by 52 by 48.
struct ActorShape {
    Vec3 center;
    float radius = 0, width = 0, height = 0;
    bool box = false;
    float c = 1, s = 0;                 // the yaw's cosine and sine, for a box
    // a point of the world in the box's frame, and a direction back out
    Vec3 local(Vec3 p) const { Vec3 d = p - center; return {c * d.x + s * d.y, -s * d.x + c * d.y, d.z}; }
    Vec3 world(Vec3 v) const { return {c * v.x - s * v.y, s * v.x + c * v.y, v.z}; }
};
ActorShape shapeOf(World& w, Object* a);
// The time a box of half size `extent` moving from a by d enters a box shape,
// or 1 or more for none; less than 0 when it starts inside. The normal is the
// face it enters by.
float enterBox(const ActorShape& sh, Vec3 a, Vec3 d, Vec3 extent, Vec3& normal);

// Trace, FastTrace, TraceActors and SetLocation, on the World's Collision.
void registerCollisionNatives(VM& vm);

}  // namespace ffa
