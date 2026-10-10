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

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "script/VM.h"
#include "world/Geometry.h"
#include "world/Level.h"

namespace ffa {

class Collision;
struct LipSync;

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
    Class* volumeClass = nullptr;
    Class* playerControllerClass = nullptr;
    Class* controllerClass = nullptr;
    std::unordered_map<const Object*, int32_t> exportOf;   // the loaded actors' exports
    std::unordered_map<int32_t, Object*> actorAt;         // and back
    Collision* collision = nullptr;  // what traces hit, once attached
    class Animator* animator = nullptr;   // the actors' animation, once attached
    // Projectors attached, by AttachProjector, until DetachProjector: what the
    // renderer projects this frame.
    std::unordered_set<Object*> projectors;
    // The AI's view of the level's paths, built when first asked (world/AI.cpp).
    std::shared_ptr<void> ai;
    // Sound: the game's sounds, and who plays them, ffa-play's mixer, or
    // none, when they are only timed (world/Audio.h).
    class SoundBank* sounds = nullptr;
    struct AudioSink* audio = nullptr;
    size_t soundsPlayed = 0;
    // Who speaks a sound with lip sync, and when it started: the face it
    // moves until it ends or is stopped (world/Animator.cpp).
    struct Speech {
        std::shared_ptr<const LipSync> lips;
        Object* sound = nullptr;
        float start = 0;
    };
    std::map<Object*, Speech> speaking;
    // The movies the HUD plays, by their Movie objects (world/Movie.cpp).
    struct MoviePlay {
        std::string file;
        float start = 0, length = 0, pausedAt = -1;
        int width = 0, height = 0;
        bool playing = false, loop = false;
    };
    std::map<Object*, MoviePlay> movies;
    std::string gameDir;            // the directory above System
    std::string mapFile;            // the level's file, 1_Shreks_Swamp.unr
    // The level the game asked to go to, its URL: Level.NextURL once its
    // countdown is out, as ServerTravel leaves it, or ClientTravel's, or an
    // open command's. Whoever plays the world goes there.
    std::string travel;
    bool quit = false;              // the game asked to end: exit or quit
    // The GUI's own state: its controller, the controls made, the mouse,
    // timers (world/Gui.cpp).
    std::shared_ptr<void> gui;
    // Karma's hanging bodies, found when first asked (world/Karma.cpp).
    std::shared_ptr<void> karma;
    // Where a walking pawn last found its floor, standing on the world: a pawn
    // that has not moved since need not look for it again (world/Physics.cpp).
    std::unordered_map<const Object*, Vec3> restingAt;
    // When a pawn whose Mount turned a ledge down may be asked again.
    std::unordered_map<const Object*, float> mountTried;
    std::unordered_set<const Object*> jumpPressed;
    // What each pawn last climbed or hung from, until it walks again.
    std::unordered_map<const Object*, const Object*> mountedFrom;
    // Counted up by SetCollision, so that what keeps a list of the actors
    // that collide knows it is out of date.
    size_t collisionChanges = 0;

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
    bool paused = false;            // Level.Pauser is set, this frame
    // What still ticks while paused: the player's controller, and what is
    // bAlwaysTick.
    bool ticksWhilePaused(Object* a);

    // The local player's input, as the engine's input system gives it each
    // frame: every variable of the controller declared `input` set to zero,
    // then each held axis given its speed. The axes are the controller's
    // variables, aBaseY and the rest, and the speeds the bindings' (DefUser.ini).
    std::vector<std::pair<std::string, float>> held;
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
    // The actor the tick is at, and the part of it, for a watchdog to say
    // where a frame that does not end is.
    Object* ticking = nullptr;
    const char* tickPart = "";
    bool savingSeen = false;        // LEVACT_Saving has had its frame

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
