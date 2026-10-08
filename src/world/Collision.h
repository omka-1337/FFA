// What a trace in a level can hit: the level's BSP, its terrains, and the
// actors drawn as static meshes that block traces.
//
// A mesh is found by the path its actor refers to it by: in the map itself, or
// in a package of the game's directories, opened when first needed.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "world/Bsp.h"
#include "world/StaticMesh.h"
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
    // Every actor the segment passes through before it hits the BSP, nearest
    // first, and then the BSP's hit as the LevelInfo.
    std::vector<TraceHit> multiLineCheck(Vec3 a, Vec3 b, const Object* ignore = nullptr);

    // Move an actor to p, and update its Region; with `events`, the zones'
    // ActorLeaving and ActorEntered and the actor's ZoneChange when the zone
    // is another. Collision is not tested.
    void place(Object* a, Vec3 p, bool events = true);

    // The collision of a StaticMesh object, or null when it cannot be found.
    const StaticMeshCollision* mesh(Object* meshObject);

    size_t meshActors = 0, meshesMissing = 0;
    std::map<std::string, size_t> problems;

private:
    struct Placed {
        Object* actor;
        const StaticMeshCollision* mesh;
        bool fixed;                 // bStatic: its transform is kept
        bool world;                 // bWorldGeometry
        float m[3][3], inv[3][3];
        Vec3 origin, lo, hi;        // and its box in the world
        bool invertible = false;
    };
    void update(Placed& pl);
    void meshHits(Vec3 a, Vec3 b, const Object* ignore, bool worldOnly, std::vector<TraceHit>* all,
                  TraceHit& best);
    // An actor's mesh to world transform: world = origin + M v.
    struct Transform {
        float m[3][3];              // columns are the scaled axes
        Vec3 origin;
    };
    Transform transform(Object* a);

    const Package& map_;
    std::string gameDir_;
    std::vector<Placed> placed_;
    std::map<std::string, std::unique_ptr<Package>> packages_;
    std::map<std::string, std::unique_ptr<StaticMeshCollision>> meshes_;   // by path
};

// Trace, FastTrace, TraceActors and SetLocation, on the World's Collision.
void registerCollisionNatives(VM& vm);

}  // namespace ffa
