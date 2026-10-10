#include "world/World.h"

#include "world/AI.h"
#include "world/Animator.h"
#include "world/Collision.h"
#include "world/Gui.h"
#include "world/Movie.h"
#include "world/Physics.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <stdexcept>

namespace ffa {

World::World(VM& vm, int pkg, const LevelRecord& level) : vm(vm), linker(vm.linker) {
    // The game loads only what is flagged for it: the level also lists the
    // editor's builder brushes and viewport cameras, flagged NotForClient and
    // NotForServer in place of the load bits (docs/package-format.md).
    const uint32_t loadForGame = 0x00010000 | 0x00020000;
    const Package& p = *linker.packages[size_t(pkg)];
    for (int32_t idx : level.actors) {
        if (idx <= 0) continue;
        if ((p.exp(idx).flags & loadForGame) != loadForGame) {
            ++editorOnly;
            continue;
        }
        actors.push_back(linker.instanceAt(pkg, idx));
        exportOf[actors.back()] = idx;
        actorAt[idx] = actors.back();
    }
    if (actors.empty()) throw std::runtime_error("the level lists no actors");
    info = actors[0];
    outer_ = info->outer;
    for (Object* a : actors) names_.insert(a->name);
    vm.host = this;
    actorClass = vm.findClass("Actor");
    pawnClass = vm.findClass("Pawn");
    brushClass = vm.findClass("Brush");
    volumeClass = vm.findClass("Volume");
    playerControllerClass = vm.findClass("PlayerController");
    controllerClass = vm.findClass("Controller");
}

Value& World::var(Object* a, const char* name) {
    // Looked up by name once per class and kept: physics asks for the same
    // few variables thousands of times a frame.
    Prop*& p = varCache_[{a->cls, name}];
    if (!p) p = a->cls ? a->cls->findProp(Name(name)) : nullptr;
    if (!p) throw vm.error(a->path() + " has no variable " + name);
    if (size_t(p->slot) >= a->props.size()) return *vm.slot(a, p);
    return a->props[size_t(p->slot)];
}

Name World::uniqueName(Class* c) {
    // The engine names a new object after its class with the next free
    // number, as the editor named the placed actors.
    int& k = counters_[c];
    while (true) {
        Name n(c->name.str() + std::to_string(k++));
        if (names_.insert(n).second) return n;
    }
}

void World::setOwner(Object* a, Object* owner) {
    Object* old = obj(a, "Owner");
    if (old == owner) return;
    if (old) vm.event(old, "LostChild", {Value::Obj(a)});
    var(a, "Owner") = Value::Obj(owner);
    if (owner) vm.event(owner, "GainedChild", {Value::Obj(a)});
}

Object* World::spawn(Class* c, Object* spawner, Object* owner, Name tag, const Value* location,
                     const Value* rotation) {
    if (!c || !c->isChildOf(actorClass)) return nullptr;
    // Placed for good: such a class is only ever loaded with its level.
    Object* d = c->defaults();
    if (flag(d, "bStatic") || flag(d, "bNoDelete")) {
        vm.write("Warning", "Spawn of " + c->name.str() + " failed: the class is bStatic or bNoDelete");
        return nullptr;
    }
    Object* a = vm.spawn(c, uniqueName(c), outer_);
    giveMovie(*this, a);
    // An actor's Tag is its class's name until it is given one: 1359 of the
    // swamp's 1650 placed actors still carry theirs.
    var(a, "Tag") = Value::Nm(tag.isNone() ? c->name : tag);
    var(a, "Level") = Value::Obj(info);
    var(a, "XLevel") = var(info, "XLevel");
    // Where the spawner stands, unless told otherwise. The engine also moves
    // the new actor out of anything it would sit inside, or refuses it when
    // there is no room: that is collision, not done yet.
    if (spawner) {
        var(a, "Location") = location ? *location : var(spawner, "Location");
        var(a, "Rotation") = rotation ? *rotation : var(spawner, "Rotation");
        var(a, "Instigator") = var(spawner, "Instigator");
    } else {
        if (location) var(a, "Location") = *location;
        if (rotation) var(a, "Rotation") = *rotation;
    }
    actors.push_back(a);
    // its zone, from where it stands
    if (collision) {
        Vec3 at;
        vm.unvector(var(a, "Location"), at.x, at.y, at.z);
        collision->place(a, at, false);
    }
    if (owner) setOwner(a, owner);
    if (!begunPlay) return a;
    for (const char* ev : {"Spawned", "PreBeginPlay", "BeginPlay", "PostBeginPlay", "PostNetBeginPlay",
                           "SetInitialState", "FilterForCurrentGameState"}) {
        vm.event(a, ev);
        if (a->deleted) return nullptr;
    }
    return a;
}

bool World::destroy(Object* a) {
    if (a->deleted) return true;
    if (flag(a, "bStatic") || flag(a, "bNoDelete")) return false;
    vm.event(a, "Destroyed");
    if (a->deleted) return true;
    if (Object* o = obj(a, "Owner")) {
        vm.event(o, "LostChild", {Value::Obj(a)});
        var(a, "Owner") = Value::Obj(nullptr);
    }
    // What it owned is owned by nothing now.
    for (Object* b : actors)
        if (b != a && !b->deleted && obj(b, "Owner") == a) setOwner(b, nullptr);
    var(a, "bDeleteMe") = Value::Bool(true);
    a->deleted = true;
    ++collisionChanges;     // what collides is another set now
    return true;
}

void World::send(Object* a, const char* event) {
    try {
        vm.event(a, event);
        sent[event]++;
    } catch (const std::exception& ex) {
        failed[event]++;
        failures[std::string(event) + ": " + ex.what()]++;
    }
}

void World::beginPlay(Class* gameClass, const String& options) {
    if (gameClass) {
        game = spawn(gameClass, nullptr);
        var(info, "Game") = Value::Obj(game);
    }
    begunPlay = true;
    var(info, "bBegunPlay") = Value::Bool(true);
    var(info, "bStartup") = Value::Bool(true);
    if (game) {
        try {
            vm.event(game, "InitGame", {Value::Str(options), Value::Str(String())});
            sent["InitGame"]++;
        } catch (const std::exception& ex) {
            failed["InitGame"]++;
            failures[std::string("InitGame: ") + ex.what()]++;
        }
    }
    // Each pass reaches the actors spawned during the passes before it. An
    // actor spawned once play has begun took all its events then, and
    // SetInitialState marked it bScriptInitialized, so it is passed over.
    auto pass = [&](std::initializer_list<const char*> events) {
        for (size_t i = 0; i < actors.size(); ++i) {
            Object* a = actors[i];
            if (a->deleted || flag(a, "bScriptInitialized")) continue;
            for (const char* ev : events)
                if (!a->deleted) send(a, ev);
        }
    };
    pass({"PreBeginPlay"});
    pass({"BeginPlay"});
    pass({"PostBeginPlay", "PostNetBeginPlay"});
    pass({"SetInitialState"});
    // KnowWonder's engine then puts the level in its game state and restores
    // what was saved of it: nothing in script calls these but supers, yet the
    // game relies on them. FilterForCurrentGameState hides and stops the
    // actors of other game states, CutSceneTrigger turns itself off out of its
    // own, and KWPawn's PostPersistentDataRestored sets up its blend channels.
    // Nothing is saved yet, so the restore itself is empty.
    for (const char* ev : {"FilterForCurrentGameState", "PrePersistentDataRestored", "PostPersistentDataRestored"})
        for (size_t i = 0; i < actors.size(); ++i)
            if (!actors[i]->deleted) send(actors[i], ev);
    var(info, "bStartup") = Value::Bool(false);
}

Object* World::login(const String& portal, const String& options) {
    if (!game) return nullptr;
    Object* pc = nullptr;
    try {
        pc = vm.event(game, "Login", {Value::Str(portal), Value::Str(options), Value::Str(String())}).o();
        sent["Login"]++;
    } catch (const std::exception& ex) {
        failed["Login"]++;
        failures[std::string("Login: ") + ex.what()]++;
    }
    if (!pc) return nullptr;
    // The engine's local player is a Viewport, a Player of its own whose class
    // has no script; a Player stands for it.
    player = vm.spawn(vm.findClass("Player"), Name("Player"), outer_);
    var(player, "Actor") = Value::Obj(pc);
    var(pc, "Player") = Value::Obj(player);
    // A local player's controller makes its input object now: the engine
    // calls InitInputSystem when it gives the controller its Player.
    try {
        vm.event(pc, "InitInputSystem");
        sent["InitInputSystem"]++;
    } catch (const std::exception& ex) {
        failed["InitInputSystem"]++;
        failures[std::string("InitInputSystem: ") + ex.what()]++;
    }
    try {
        vm.event(game, "PostLogin", {Value::Obj(pc)});
        sent["PostLogin"]++;
    } catch (const std::exception& ex) {
        failed["PostLogin"]++;
        failures[std::string("PostLogin: ") + ex.what()]++;
    }
    return pc;
}

bool World::ticksWhilePaused(Object* a) {
    return flag(a, "bAlwaysTick") || (playerControllerClass && a->isA(playerControllerClass));
}

void World::tick(float dt) {
    dt *= var(info, "TimeDilation").f();
    // Paused, Level.Pauser set by GameInfo's SetPause, as the in-game menu
    // does: the level's time stands, and only the player's controller and
    // what is bAlwaysTick tick, the menu's book among them.
    paused = info->cls->findProp(Name("Pauser")) && obj(info, "Pauser");
    if (!paused) time += dt;
    var(info, "TimeSeconds") = Value::Float(time);
    // the clock, as the engine keeps it in the LevelInfo: the menus' fades
    // time themselves by its Millisecond
    {
        using namespace std::chrono;
        auto now = system_clock::now();
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&t, &tm);
        int ms = int(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
        const std::pair<const char*, int> parts[] = {{"Year", tm.tm_year + 1900}, {"Month", tm.tm_mon + 1},
                                                     {"Day", tm.tm_mday},         {"DayOfWeek", tm.tm_wday},
                                                     {"Hour", tm.tm_hour},        {"Minute", tm.tm_min},
                                                     {"Second", tm.tm_sec},       {"Millisecond", ms}};
        for (auto& [k, v] : parts)
            if (info->cls->findProp(Name(k))) var(info, k) = Value::Int(v);
    }
    ++frames;
    Value delta = Value::Float(dt);
    size_t n = actors.size();
    // bTicked: the engine flips it on every actor it ticks, and script
    // compares it to see whether a frame has passed (KWHeroController's
    // PlayerCalcView does).
    bool parity = frames & 1;
    // Animation first, so that a sequence ending this frame ends FinishAnim
    // before the state code that waits on it runs; then what hangs on bones.
    // A save the game asked for, Level.LevelAction = LEVACT_Saving, as
    // KWHeroController's SaveSlottedGame sets it: the engine saves and puts
    // it back to LEVACT_None, and the HUD's SAVING is gone. Saving itself is
    // not done yet; the action ends after the frame that showed it.
    if (info && info->cls->findProp(Name("LevelAction"))) {
        Value& act = var(info, "LevelAction");
        if (act.i() == 2) {
            if (savingSeen) {
                act = Value::Int(0);
                savingSeen = false;
            } else {
                savingSeen = true;
            }
        }
    }
    // The players who pressed jump since the last frame: a pawn that cannot
    // jump where it stands, as Shrek wading, climbs what it stands against
    // from the ground (Physics.cpp, tryMount); the controller's PlayerMove
    // has let go of bPressedJump before the physics runs.
    jumpPressed.clear();
    if (playerControllerClass)
        for (size_t i = 0; i < n; ++i) {
            Object* c = actors[i];
            if (c->deleted || !c->isA(playerControllerClass) || !flag(c, "bPressedJump")) continue;
            if (Object* p = obj(c, "Pawn"); p && !p->deleted) jumpPressed.insert(p);
        }
    ticking = nullptr;
    tickPart = "animation";
    if (animator) {
        try {
            animator->tick(dt, paused);
            animator->attachments();
        } catch (const std::exception& ex) {
            failed["animation"]++;
            failures[std::string("animation: ") + ex.what()]++;
        }
    }
    tickPart = "movies";
    movieTick(*this);
    tickPart = "GUI";
    guiTick(*this, dt);
    for (size_t i = 0; i < n; ++i) {
        Object* a = actors[i];
        if (a->deleted || flag(a, "bStatic")) continue;
        if (paused && !ticksWhilePaused(a)) continue;
        ticking = a;
        tickPart = "Tick";
        var(a, "bTicked") = Value::Bool(parity);
        try {
            if (player && a == obj(player, "Actor")) {
                // CPF_Input, 0x4: the axes and buttons, reset each frame
                for (Prop* p : a->cls->layout())
                    if (p->flags & 0x4) a->props[size_t(p->slot)] = p->kind == Kind::Float ? Value::Float(0)
                                                                    : p->kind == Kind::Bool  ? Value::Bool(false)
                                                                                             : Value::Int(0);
                for (auto& [axis, speed] : held) {
                    Prop* p = a->cls->findProp(Name(axis));
                    if (!p) continue;
                    Value& v = a->props[size_t(p->slot)];
                    if (p->kind == Kind::Float) v = Value::Float(v.f() + speed);
                    else if (p->kind == Kind::Bool) v = Value::Bool(speed != 0);
                    else v = Value::Int(std::min(255, v.i() + int(speed)));
                }
                vm.event(a, "PlayerTick", {delta});
                sent["PlayerTick"]++;
            }
            vm.event(a, "Tick", {delta});
            sent["Tick"]++;
            if (a->deleted) continue;
            tickPart = "state code";
            vm.processState(a, dt);
            if (a->deleted) continue;
            if (controllerClass && a->isA(controllerClass) && !(playerControllerClass && a->isA(playerControllerClass)))
                aiTick(*this, a);
            // The timer counts while it is set. On reaching its rate it fires
            // once however many periods the frame covered, keeping the
            // remainder when it loops and stopping when it does not.
            float rate = var(a, "TimerRate").f();
            if (rate > 0) {
                float counter = var(a, "TimerCounter").f() + dt;
                if (counter >= rate) {
                    float passed = std::floor(counter / rate);
                    counter -= rate * passed;
                    if (!flag(a, "bTimerLoop")) var(a, "TimerRate") = Value::Float(0);
                    var(a, "TimerCounter") = Value::Float(counter);
                    tickPart = "Timer";
                    vm.event(a, "Timer");
                    sent["Timer"]++;
                } else {
                    var(a, "TimerCounter") = Value::Float(counter);
                }
            }
            if (a->deleted) continue;
            tickPart = "physics";
            performPhysics(*this, a, dt);
            if (a->deleted) continue;
            float life = var(a, "LifeSpan").f();
            if (life != 0) {
                life -= dt;
                var(a, "LifeSpan") = Value::Float(life);
                if (life <= 0.0001f) destroy(a);
            }
        } catch (const std::exception& ex) {
            failed["tick"]++;
            failures[std::string("tick: ") + ex.what()]++;
        }
    }
    ticking = nullptr;
    tickPart = "";
    // A level change ServerTravel asked for: Level.NextURL, once
    // NextSwitchCountdown, which the game sets, has run out.
    if (info && travel.empty() && info->cls->findProp(Name("NextURL"))) {
        std::string url = utf8(var(info, "NextURL").s());
        if (!url.empty()) {
            Value& left = var(info, "NextSwitchCountdown");
            left = Value::Float(left.f() - dt);
            if (left.f() <= 0) travel = url;
        }
    }
}

// ================================================================ natives
namespace {

World& world(NativeCall& c) {
    World* w = World::of(c.vm);
    if (!w) throw c.vm.error("native " + c.fn->qualname() + " needs a level");
    return *w;
}

Class* classArg(NativeCall& c, size_t k) {
    Object* o = c.o(k);
    return o && o->isClass() ? static_cast<Class*>(o) : nullptr;
}

// AllActors and DynamicActors: every live actor of a class, optionally with
// a Tag. DynamicActors leaves out the bStatic ones, which never change.
void actorsOf(NativeCall& c, bool dynamic) {
    World& w = world(c);
    Class* base = classArg(c, 0);
    if (!base) return;
    Name tag = c.n(2);
    for (Object* a : w.actors) {
        if (a->deleted || !a->isA(base)) continue;
        if (dynamic && w.flag(a, "bStatic")) continue;
        if (!tag.isNone() && w.var(a, "Tag").n() != tag) continue;
        c.yield({Value::Obj(a)});
    }
}

void unlink(NativeCall& c, const char* head, const char* next) {
    World& w = world(c);
    Value* at = &w.var(w.info, head);
    for (size_t guard = 0; at->o() && guard <= w.actors.size(); ++guard) {
        if (at->o() == c.self) {
            *at = w.var(c.self, next);
            return;
        }
        at = &w.var(at->o(), next);
    }
}

}  // namespace

void registerWorldNatives(VM& vm) {
    auto& n = vm.natives;

    n["actor.spawn"] = [](NativeCall& c) {
        World& w = world(c);
        Value loc, rot;
        if (c.has(3)) loc = c.get(3);
        if (c.has(4)) rot = c.get(4);
        Object* a = w.spawn(classArg(c, 0), c.self, c.o(1), c.n(2), c.has(3) ? &loc : nullptr,
                            c.has(4) ? &rot : nullptr);
        return Value::Obj(a);
    };
    // GetURLMap: the level's file, as the in-game menu asks to place its book
    // by (the swamp's and the hunt's sit further back).
    n["actor.geturlmap"] = [](NativeCall& c) { return Value::Str(widen(world(c).mapFile)); };
    // SaveGameExists(iSlot): no game is saved yet, so none is.
    n["actor.savegameexists"] = [](NativeCall&) { return Value::Bool(false); };
    // ClientTravel(URL, TravelType, bItems): the level to go to.
    n["playercontroller.clienttravel"] = [](NativeCall& c) {
        World& w = world(c);
        if (w.travel.empty()) w.travel = utf8(c.s(0));
        return Value();
    };
    // ConsoleCommand(Command): of the engine's commands, open, start and
    // travel go to a level, exit and quit end the game, getcurrentres tells
    // the screen; what else script asks of the console, such as get ini:,
    // answers nothing, which the game takes as the default.
    auto console = [](NativeCall& c) {
        World& w = world(c);
        std::string cmd = utf8(c.s(0));
        size_t sp = cmd.find(' ');
        std::string verb = cmd.substr(0, sp);
        for (char& ch : verb) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if ((verb == "open" || verb == "start" || verb == "travel") && sp != std::string::npos && w.travel.empty())
            w.travel = cmd.substr(cmd.find_first_not_of(' ', sp));
        if (verb == "getcurrentres") return Value::Str(widen(guiResolution(w)));
        if (verb == "exit" || verb == "quit") w.quit = true;
        return Value::Str(String());
    };
    n["actor.consolecommand"] = console;
    n["interaction.consolecommand"] = [](NativeCall& c) {
        c.vm.natives["actor.consolecommand"](c);
        return Value::Bool(true);
    };
    n["playercontroller.consolecommand"] = console;
    n["actor.sleep"] = [](NativeCall& c) {
        float left = c.f(0);
        c.self->latent = [left](float dt) mutable { return (left -= dt) <= 0.0f; };
        return Value();
    };
    n["actor.destroy"] = [](NativeCall& c) { return Value::Bool(world(c).destroy(c.self)); };
    n["actor.setowner"] = [](NativeCall& c) {
        world(c).setOwner(c.self, c.o(0));
        return Value();
    };
    n["actor.allactors"] = [](NativeCall& c) {
        actorsOf(c, false);
        return Value();
    };
    n["actor.dynamicactors"] = [](NativeCall& c) {
        actorsOf(c, true);
        return Value();
    };

    // Setters whose engine side also updates collision and physics state the
    // engine does not keep yet: for now each sets its variables.
    n["actor.setphysics"] = [](NativeCall& c) {
        world(c).var(c.self, "Physics") = Value::Int(c.i(0));
        return Value();
    };
    n["actor.setrotation"] = [](NativeCall& c) {
        world(c).var(c.self, "Rotation") = c.get(0);
        return Value::Bool(true);
    };
    n["actor.setcollision"] = [](NativeCall& c) {
        World& w = world(c);
        // a left out argument keeps the flag as it is
        ++w.collisionChanges;
        if (c.has(0)) w.var(c.self, "bCollideActors") = Value::Bool(c.b(0));
        if (c.has(1)) w.var(c.self, "bBlockActors") = Value::Bool(c.b(1));
        if (c.has(2)) w.var(c.self, "bBlockPlayers") = Value::Bool(c.b(2));
        return Value();
    };
    n["actor.setcollisionsize"] = [](NativeCall& c) {
        World& w = world(c);
        w.var(c.self, "CollisionRadius") = Value::Float(c.f(0));
        w.var(c.self, "CollisionHeight") = Value::Float(c.f(1));
        return Value::Bool(true);
    };
    n["actor.setdrawscale"] = [](NativeCall& c) {
        world(c).var(c.self, "DrawScale") = Value::Float(c.f(0));
        return Value();
    };
    // SetDrawScale3D: the swamp's shimmy vines are stretched by it from the
    // ShimmyStatVine that spawns them to the stump they reach
    n["actor.setdrawscale3d"] = [](NativeCall& c) {
        world(c).var(c.self, "DrawScale3D") = c.get(0);
        return Value();
    };
    n["actor.setdrawtype"] = [](NativeCall& c) {
        world(c).var(c.self, "DrawType") = Value::Int(c.i(0));
        return Value();
    };
    n["actor.settimer"] = [](NativeCall& c) {
        World& w = world(c);
        w.var(c.self, "TimerRate") = Value::Float(c.f(0));
        w.var(c.self, "TimerCounter") = Value::Float(0);
        w.var(c.self, "bTimerLoop") = Value::Bool(c.b(1));
        return Value();
    };

    // The level's lists of pawns and controllers, linked through each one.
    n["pawn.addpawn"] = [](NativeCall& c) {
        World& w = world(c);
        w.var(c.self, "nextPawn") = w.var(w.info, "PawnList");
        w.var(w.info, "PawnList") = Value::Obj(c.self);
        return Value();
    };
    n["controller.addcontroller"] = [](NativeCall& c) {
        World& w = world(c);
        w.var(c.self, "nextController") = w.var(w.info, "ControllerList");
        w.var(w.info, "ControllerList") = Value::Obj(c.self);
        return Value();
    };
    n["pawn.removepawn"] = [](NativeCall& c) {
        unlink(c, "PawnList", "nextPawn");
        return Value();
    };
    n["controller.removecontroller"] = [](NativeCall& c) {
        unlink(c, "ControllerList", "nextController");
        return Value();
    };

    n["projector.attachprojector"] = [](NativeCall& c) {
        world(c).projectors.insert(c.self);
        return Value();
    };
    n["projector.detachprojector"] = [](NativeCall& c) {
        world(c).projectors.erase(c.self);
        return Value();
    };
    // AbandonProjector leaves the projection where it is, for good: here it
    // stays attached.
    n["projector.abandonprojector"] = [](NativeCall&) { return Value(); };
    n["levelinfo.issoftwarerendering"] = [](NativeCall&) { return Value::Bool(false); };
    n["playercontroller.setviewtarget"] = [](NativeCall& c) {
        world(c).var(c.self, "ViewTarget") = Value::Obj(c.o(0));
        return Value();
    };
}

}  // namespace ffa
