#include "world/World.h"

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
    }
    if (actors.empty()) throw std::runtime_error("the level lists no actors");
    info = actors[0];
    outer_ = info->outer;
    for (Object* a : actors) names_.insert(a->name);
    vm.host = this;
}

Value& World::var(Object* a, const char* name) {
    Prop* p = a->cls ? a->cls->findProp(Name(name)) : nullptr;
    if (!p) throw vm.error(a->path() + " has no variable " + name);
    return *vm.slot(a, p);
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
    static Class* actorClass = vm.findClass("Actor");
    if (!c || !c->isChildOf(actorClass)) return nullptr;
    // Placed for good: such a class is only ever loaded with its level.
    Object* d = c->defaults();
    if (flag(d, "bStatic") || flag(d, "bNoDelete")) {
        vm.write("Warning", "Spawn of " + c->name.str() + " failed: the class is bStatic or bNoDelete");
        return nullptr;
    }
    Object* a = vm.spawn(c, uniqueName(c), outer_);
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
    if (owner) setOwner(a, owner);
    if (!begunPlay) return a;
    for (const char* ev : {"Spawned", "PreBeginPlay", "BeginPlay", "PostBeginPlay", "PostNetBeginPlay",
                           "SetInitialState"}) {
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
    var(info, "bStartup") = Value::Bool(false);
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

    n["levelinfo.issoftwarerendering"] = [](NativeCall&) { return Value::Bool(false); };
    n["playercontroller.setviewtarget"] = [](NativeCall& c) {
        world(c).var(c.self, "ViewTarget") = Value::Obj(c.o(0));
        return Value();
    };
}

}  // namespace ffa
