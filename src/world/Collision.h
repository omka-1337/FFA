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
    // and every blocking static mesh actor but `ignore`.
    TraceHit lineCheck(Vec3 a, Vec3 b, const Object* ignore = nullptr);

    // The collision of a StaticMesh object, or null when it cannot be found.
    const StaticMeshCollision* mesh(Object* meshObject);

    size_t meshActors = 0, meshesMissing = 0;
    std::map<std::string, size_t> problems;

private:
    struct Placed {
        Object* actor;
        const StaticMeshCollision* mesh;
    };
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

}  // namespace ffa
