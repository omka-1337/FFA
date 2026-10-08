// A running level: its actors, the game that rules it, and the natives that
// act on them.
//
// The actor list starts as the Level record's, LevelInfo first, less the
// editor's own actors, and grows as script spawns. A destroyed actor stays in the list marked bDeleteMe, which
// is what script tests, and the iterators and the start up pass skip it.
//
// What the engine does when it spawns, destroys and starts a level is its
// published behaviour, not something the data records. Where that behaviour
// depends on collision, which the engine does not have yet, it is left out and
// said so at the place.
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "script/VM.h"
#include "world/Level.h"

namespace ffa {

class Collision;

class World {
public:
    World(VM& vm, int pkg, const LevelRecord& level);

    VM& vm;
    Linker& linker;
    std::vector<Object*> actors;
    Object* info = nullptr;         // the LevelInfo
    Object* game = nullptr;         // the GameInfo, once begun
    bool begunPlay = false;
    size_t editorOnly = 0;          // listed actors the game does not load
    // Classes the engine's own code asks about, of this VM's linker. Kept
    // here, not in statics, as a VM and its classes live and die together.
    Class* actorClass = nullptr;
    Class* pawnClass = nullptr;
    Class* brushClass = nullptr;
    Class* playerControllerClass = nullptr;
    std::unordered_map<const Object*, int32_t> exportOf;   // the loaded actors' exports
    std::unordered_map<int32_t, Object*> actorAt;         // and back
    Collision* collision = nullptr;  // what traces hit, once attached

    // The world a VM's natives act on; null when none is attached.
    static World* of(VM& vm) { return static_cast<World*>(vm.host); }

    // A new actor of class c. With play begun it also takes its start up
    // events. Null when the class cannot be spawned, or when the actor
    // destroyed itself on the way.
    Object* spawn(Class* c, Object* spawner, Object* owner = nullptr, Name tag = Name(),
                  const Value* location = nullptr, const Value* rotation = nullptr);
    // False for an actor that is bStatic or bNoDelete.
    bool destroy(Object* a);
    void setOwner(Object* a, Object* owner);

    // Spawn the game, InitGame it with the URL's options, and send every
    // actor the start up events: PreBeginPlay, BeginPlay, PostBeginPlay with
    // PostNetBeginPlay, SetInitialState.
    void beginPlay(Class* gameClass, const String& options);

    // The local player joins, as the engine has it once play has begun: the
    // game's Login gives a PlayerController, which gets a Player, and then
    // PostLogin. Null when Login gives none.
    Object* login(const String& portal, const String& options);
    Object* player = nullptr;       // the local Player

    // One frame of dt seconds of real time. The level's TimeDilation scales
    // it; then every actor that is not bStatic, in list order, has the
    // player's PlayerTick if it is the local controller, Tick, its state code
    // run on to its next wait, its timer, its physics, and its LifeSpan. Actors spawned
    // during the frame first tick in the next.
    void tick(float dt);
    float time = 0;                 // Level.TimeSeconds, as kept here
    size_t frames = 0;

    // An actor variable by name, as the engine's own code reaches it. The
    // name must be a string literal: lookups are kept by its address.
    Value& var(Object* a, const char* name);
    bool flag(Object* a, const char* name) { return var(a, name).b(); }
    Object* obj(Object* a, const char* name) { return var(a, name).o(); }

    // Every event the start up pass sent, by name, and how many failed.
    std::unordered_map<std::string, size_t> sent, failed;
    std::unordered_map<std::string, size_t> failures;   // by message

private:
    void send(Object* a, const char* event);
    Name uniqueName(Class* c);

    Object* outer_ = nullptr;
    std::unordered_set<Name> names_;
    std::unordered_map<Class*, int> counters_;
    struct VarKey {
        const Class* cls;
        const char* name;
        bool operator==(const VarKey& o) const { return cls == o.cls && name == o.name; }
    };
    struct VarHash {
        size_t operator()(const VarKey& k) const {
            return std::hash<const void*>()(k.cls) * 31 + std::hash<const void*>()(k.name);
        }
    };
    std::unordered_map<VarKey, Prop*, VarHash> varCache_;
};

// Actor, LevelInfo, Pawn and Controller natives that act on a World.
void registerWorldNatives(VM& vm);

}  // namespace ffa
