#include "world/Physics.h"

#include <algorithm>
#include <cmath>

#include "world/AI.h"
#include "world/Collision.h"

namespace ffa {

namespace {

const float MinFloorZ = 0.7f;
const float MaxStepHeight = 35.0f;
const float MinFloorDist = 1.9f, MaxFloorDist = 2.4f;

Vec3 vget(World& w, Object* a, const char* name) {
    Vec3 v;
    w.vm.unvector(w.var(a, name), v.x, v.y, v.z);
    return v;
}

void vset(World& w, Object* a, const char* name, Vec3 v) { w.var(a, name) = w.vm.vector(v.x, v.y, v.z); }

Vec3 extentOf(World& w, Object* a) {
    float r = w.var(a, "CollisionRadius").f();
    return {r, r, w.var(a, "CollisionHeight").f()};
}

// A variable only some actors have, Pawn's movement settings, or a default.
float fopt(World&, Object* a, const char* name, float def) {
    Prop* p = a->cls->findProp(Name(name));
    return p ? a->props[size_t(p->slot)].f() : def;
}
bool bopt(World& w, Object* a, const char* name) {
    (void)w;
    Prop* p = a->cls->findProp(Name(name));
    return p && a->props[size_t(p->slot)].b();
}
Object* controllerOf(World&, Object* a) {
    Prop* p = a->cls->findProp(Name("Controller"));
    return p ? a->props[size_t(p->slot)].o() : nullptr;
}

bool collides(World& w, Object* a) { return w.flag(a, "bCollideWorld") || w.flag(a, "bCollideActors"); }

// The physics volume an actor is in, for gravity and friction: its own
// PhysicsVolume, else the level's.
Object* volumeOf(World& w, Object* a) {
    Object* v = w.obj(a, "PhysicsVolume");
    return v ? v : nullptr;
}

Vec3 gravityOf(World& w, Object* a) {
    Object* v = volumeOf(w, a);
    return v ? vget(w, v, "Gravity") : Vec3{0, 0, -1000};
}

void setBase(World& w, Object* a, Object* base) {
    Object* old = w.obj(a, "Base");
    if (old == base) return;
    if (old) w.vm.event(old, "Detach", {Value::Obj(a)});
    w.var(a, "Base") = Value::Obj(base);
    if (base) w.vm.event(base, "Attach", {Value::Obj(a)});
    w.vm.event(a, "BaseChange");
}

void setPhysics(World& w, Object* a, int mode) {
    if (w.var(a, "Physics").i() == mode) return;
    w.var(a, "Physics") = Value::Int(mode);
    if (mode != PHYS_Walking) setBase(w, a, nullptr);
}

// Move a by delta, stopping at the first thing in the way, a tenth of a unit
// short of it. Returns the hit, its time along delta.
TraceHit move(World& w, Object* a, Vec3 delta) {
    Collision& c = *w.collision;
    Vec3 from = vget(w, a, "Location");
    if (!collides(w, a)) {
        c.place(a, from + delta);
        return TraceHit{};
    }
    TraceHit h = c.boxCheck(from, from + delta, extentOf(w, a), a);
    Vec3 to = from + delta;
    if (h) {
        float len = length(delta);
        float t = len > 0 ? std::max(0.0f, h.time - 0.1f / len) : 0.0f;
        to = from + delta * t;
    }
    c.place(a, to);
    updateTouching(w, a);
    return h;
}

void hitWall(World& w, Object* a, const TraceHit& h) {
    Object* wall = h.actor ? h.actor : w.info;
    Value n = w.vm.vector(h.normal.x, h.normal.y, h.normal.z);
    Object* ctl = controllerOf(w, a);
    if (ctl && w.vm.event(ctl, "NotifyHitWall", {n, Value::Obj(wall)}).b()) return;
    w.vm.event(a, "HitWall", {n, Value::Obj(wall)});
}

void landed(World& w, Object* a, const TraceHit& h) {
    Value n = w.vm.vector(h.normal.x, h.normal.y, h.normal.z);
    Object* ctl = controllerOf(w, a);
    if (!(ctl && w.vm.event(ctl, "NotifyLanded", {n}).b())) w.vm.event(a, "Landed", {n});
    if (w.var(a, "Physics").i() == PHYS_Falling) setPhysics(w, a, PHYS_Walking);
    if (h.actor && h.actor->cls->name != Name("TerrainInfo")) setBase(w, a, h.actor);
    else setBase(w, a, w.info);
}

// Turning: toward DesiredRotation at RotationRate, or at RotationRate for ever.
// The player's pawn turns toward its controller's DesiredRotation instead, in
// yaw: the game's scripts set that one to turn it (KWHeroController's
// UpdateRotation sets it to the camera's rotation each frame, BaseCam's
// ApplyMouseXToDestYaw adds the mouse to it), so Shrek comes round to face
// where the camera looks, at his RotationRate. So does a pawn whose other
// controller looks at something, its Focus or FocalPoint (world/AI.cpp); one
// that looks at nothing leaves the pawn to its own.
void physicsRotation(World& w, Object* a, float dt) {
    int32_t r[3], rate[3], want[3];
    w.vm.unrotator(w.var(a, "Rotation"), r[0], r[1], r[2]);
    w.vm.unrotator(w.var(a, "RotationRate"), rate[0], rate[1], rate[2]);
    bool toDesired = w.flag(a, "bRotateToDesired"), fixed = w.flag(a, "bFixedRotationDir");
    Object* ctl = w.pawnClass && a->isA(w.pawnClass) ? controllerOf(w, a) : nullptr;
    bool player = ctl && !ctl->deleted &&
                  ((w.playerControllerClass && ctl->isA(w.playerControllerClass)) || aiSteering(w, ctl));
    if (!toDesired && !fixed && !player) return;
    w.vm.unrotator(w.var(a, "DesiredRotation"), want[0], want[1], want[2]);
    if (player) {
        int32_t cw[3];
        w.vm.unrotator(w.var(ctl, "DesiredRotation"), cw[0], cw[1], cw[2]);
        want[1] = cw[1];
        if (!toDesired) want[0] = r[0], want[2] = r[2];
        toDesired = true;
        fixed = false;
    }
    for (int k = 0; k < 3; ++k) {
        int32_t step = int32_t(float(rate[k]) * dt);
        if (toDesired) {
            int32_t d = int32_t(int16_t((want[k] - r[k]) & 0xFFFF));   // the short way round
            int32_t mag = std::abs(step);
            r[k] += std::clamp(d, -mag, mag);
        } else {
            r[k] += step;
        }
        r[k] &= 0xFFFF;
    }
    w.var(a, "Rotation") = w.vm.rotator(r[0], r[1], r[2]);
}

// A ledge to climb, for KnowWonder's pawns, which climb by their script's Mount
// once the engine has found one: in the air, or on the ground when its player
// pressed jump, as Shrek wading cannot jump. Something in the way where the
// pawn is going or, still, faces; on it, towards its middle for an actor, else
// into its face, a floor higher than a step and no higher than MaxMountHeight
// above the feet, where the pawn fits. Mount takes the move to there and what
// it stands on: none for the level, which its MountVolumes make climbable,
// else an actor that lets itself be climbed by bIsMountable. The swamp's lily
// pads are such, 40 over Shrek's feet where he wades, and he steps up onto
// them. One that Mount turns down is not asked again for a quarter of a
// second. How the game finds its ledges is not known; this is a model.
void tryMount(World& w, Object* a) {
    float maxH = fopt(w, a, "MaxMountHeight", 0);
    if (maxH <= 0 || !a->cls->findFunction(Name("Mount"))) return;
    auto& next = w.mountTried[a];
    if (w.time < next) return;
    Collision& c = *w.collision;
    Vec3 at = vget(w, a, "Location"), ext = extentOf(w, a);
    Vec3 acc = vget(w, a, "Acceleration");
    acc.z = 0;
    Vec3 dir;
    if (length(acc) > 1) {
        dir = acc * (1 / length(acc));
    } else {
        int32_t pitch, yaw, roll;
        w.vm.unrotator(w.var(a, "Rotation"), pitch, yaw, roll);
        float ax[3][3];
        rotationAxes(0, yaw, 0, ax);
        dir = {ax[0][0], ax[0][1], 0};
    }
    float feet = at.z - ext.z;
    // what is in the way, and the way into it, against its face: a pawn
    // sliding along a lily pad meets it to one side
    TraceHit wall = c.boxCheck(at, at + dir * (ext.x + 8), ext, a);
    if (!wall || wall.normal.z >= MinFloorZ) return;
    Vec3 into{-wall.normal.x, -wall.normal.y, 0};
    if (wall.actor && wall.actor != w.info && !w.flag(wall.actor, "bWorldGeometry")) {
        Vec3 o = vget(w, wall.actor, "Location");
        into = Vec3{o.x - at.x, o.y - at.y, 0};
    }
    if (length(into) < 0.1f) return;
    into = into * (1 / length(into));
    for (float d = ext.x + 8; d <= 2 * ext.x + 40; d += 16) {
        Vec3 p = at + into * d;
        TraceHit top = c.lineCheck(Vec3{p.x, p.y, feet + maxH}, Vec3{p.x, p.y, feet + MaxStepHeight}, a, true);
        if (!top || top.startSolid || top.normal.z < MinFloorZ) continue;
        Vec3 dest{p.x, p.y, top.location.z + ext.z + MaxFloorDist};
        if (!c.fits(dest, ext, a)) continue;
        Object* on = top.actor;
        if (on == w.info || (on && (on->cls->name == Name("TerrainInfo") || w.flag(on, "bWorldGeometry")))) on = nullptr;
        Vec3 delta = dest - at;
        if (!w.vm.event(a, "Mount", {w.vm.vector(delta.x, delta.y, delta.z), Value::Obj(on)}).b()) next = w.time + 0.25f;
        return;
    }
}

void physFalling(World& w, Object* a, float dt) {
    tryMount(w, a);
    if (a->deleted || w.var(a, "Physics").i() != PHYS_Falling) return;
    Vec3 v = vget(w, a, "Velocity");
    Vec3 acc = vget(w, a, "Acceleration");
    float air = fopt(w, a, "AirControl", 0);
    v = v + Vec3{acc.x * air, acc.y * air, 0} * dt + gravityOf(w, a) * dt;
    Object* vol = volumeOf(w, a);
    float terminal = vol ? w.var(vol, "TerminalVelocity").f() : 2500.0f;
    if (terminal > 0 && length(v) > terminal) v = v * (terminal / length(v));
    vset(w, a, "Velocity", v);
    Vec3 delta = v * dt;
    for (int pass = 0; pass < 3 && length(delta) > 0.01f; ++pass) {
        TraceHit h = move(w, a, delta);
        if (!h || a->deleted) return;
        if (h.normal.z >= MinFloorZ) {
            Vec3 vv = vget(w, a, "Velocity");
            vv.z = 0;
            vset(w, a, "Velocity", vv);
            landed(w, a, h);
            return;
        }
        hitWall(w, a, h);
        if (a->deleted || w.var(a, "Physics").i() != PHYS_Falling) return;
        // slide along what was hit with what is left of the move
        Vec3 rest = delta * (1 - h.time);
        delta = rest - h.normal * dot(rest, h.normal);
        Vec3 vv = vget(w, a, "Velocity");
        vset(w, a, "Velocity", vv - h.normal * dot(vv, h.normal));
    }
}

void physWalking(World& w, Object* a, float dt) {
    if (w.jumpPressed.count(a)) {
        tryMount(w, a);
        if (a->deleted || w.var(a, "Physics").i() != PHYS_Walking) return;
    }
    Vec3 v = vget(w, a, "Velocity");
    Vec3 acc = vget(w, a, "Acceleration");
    acc.z = 0;
    v.z = 0;
    // script sets Acceleration to the input as it is; the pawn's AccelRate
    // bounds it
    float rate = fopt(w, a, "AccelRate", 0);
    if (rate > 0 && length(acc) > rate) acc = acc * (rate / length(acc));
    Object* vol = volumeOf(w, a);
    float friction = vol ? w.var(vol, "GroundFriction").f() : 8.0f;
    float speed = fopt(w, a, "GroundSpeed", 0);
    if (bopt(w, a, "bIsWalking")) speed *= fopt(w, a, "WalkingPct", 1);
    // Friction slows a pawn left to itself; acceleration turns and speeds it.
    float vs = length(v);
    if (length(acc) < 1e-3f) {
        Vec3 nv = v - v * (2 * friction * dt);
        v = dot(nv, v) <= 0 ? Vec3{} : nv;
    } else {
        Vec3 dir = acc * (1 / length(acc));
        v = v - (v - dir * vs) * std::min(1.0f, dt * friction);
        v = v + acc * dt;
    }
    if (length(v) > speed && speed > 0) v = v * (speed / length(v));
    vset(w, a, "Velocity", v);
    Vec3 delta = v * dt;
    Vec3 start = vget(w, a, "Location");
    if (length(delta) > 0.01f) {
        TraceHit h = move(w, a, delta);
        if (a->deleted) return;
        if (h && h.normal.z < MinFloorZ) {
            // A step up: lift, move on, and come down on what is there. If
            // that does not land on a floor, back to before it, and slide
            // along the wall instead.
            Vec3 before = vget(w, a, "Location");
            Vec3 rest = delta * (1 - h.time);
            TraceHit up = move(w, a, {0, 0, MaxStepHeight});
            float lifted = up ? up.time * MaxStepHeight : MaxStepHeight;
            // a wall still there a step up is a wall, not a step: Donkey, walking
            // out of the factory's elevator into its wall, stepped up, met it
            // again, came down where he was, and never slid along it
            TraceHit ahead = move(w, a, rest);
            TraceHit down = move(w, a, {0, 0, -(lifted + MaxFloorDist)});
            if (a->deleted) return;
            if (!(down && down.normal.z >= MinFloorZ) || (ahead && ahead.normal.z < MinFloorZ)) {
                w.collision->place(a, before);
                hitWall(w, a, h);
                if (a->deleted || w.var(a, "Physics").i() != PHYS_Walking) return;
                // along the wall's face as it stands, its slope left out:
                // along a steep bank the slope's own normal pressed Shrek
                // into it and up onto it, where he fell, and landed, and
                // walked into it again, four frames a round
                Vec3 n{h.normal.x, h.normal.y, 0};
                if (length(n) > 1e-3f) {
                    n = n * (1 / length(n));
                    Vec3 slide = rest - n * dot(rest, n);
                    slide.z = 0;
                    move(w, a, slide);
                }
            }
        }
    }
    // What the pawn did, not what it meant to: a wall leaves it slower.
    if (dt > 0 && !a->deleted) {
        Vec3 moved = vget(w, a, "Location") - start;
        Vec3 vv = vget(w, a, "Velocity");
        vset(w, a, "Velocity", Vec3{moved.x / dt, moved.y / dt, vv.z});
    }
    // The floor: keep to it, or fall when it is gone. A pawn standing where
    // it found it last frame, on the level or on an actor that is not a
    // mover and is still there, is on it still: the factory's 25 idle workers
    // took 10 ms a frame looking for it again.
    Collision& c = *w.collision;
    Vec3 at = vget(w, a, "Location");
    {
        auto rest = w.restingAt.find(a);
        Object* base = w.obj(a, "Base");
        bool still = rest != w.restingAt.end() && length(rest->second - at) < 0.01f;
        bool solid = base == w.info || (base && !base->deleted && !base->isA(w.linker.findClass("Mover")));
        if (still && solid) return;
        w.restingAt.erase(a);
    }
    float probe = MaxStepHeight + MaxFloorDist;
    TraceHit floor = c.boxCheck(at, at - Vec3{0, 0, probe}, extentOf(w, a), a);
    // The box's edge on a steep bank where the pawn's middle stands on the
    // ground is still standing: it stays where it is, on what is under its
    // middle.
    if (floor && floor.normal.z < MinFloorZ) {
        float h = extentOf(w, a).z;
        TraceHit mid = c.lineCheck(at, at - Vec3{0, 0, h + probe}, a);
        if (mid && mid.normal.z >= MinFloorZ) {
            if (mid.actor && mid.actor->cls->name != Name("TerrainInfo")) setBase(w, a, mid.actor);
            else setBase(w, a, w.info);
            w.restingAt[a] = at;
            return;
        }
    }
    if (!floor || floor.normal.z < MinFloorZ) {
        setPhysics(w, a, PHYS_Falling);
        w.vm.event(a, "Falling");
        return;
    }
    float dist = floor.time * probe;
    if (dist < MinFloorDist || dist > MaxFloorDist) {
        float target = 0.5f * (MinFloorDist + MaxFloorDist);
        c.place(a, at - Vec3{0, 0, dist - target});
        updateTouching(w, a);
    }
    if (floor.actor && floor.actor->cls->name != Name("TerrainInfo")) setBase(w, a, floor.actor);
    else setBase(w, a, w.info);
    w.restingAt[a] = vget(w, a, "Location");
}

// Flying: acceleration up to AirSpeed, no gravity, sliding along what it meets.
void physFlying(World& w, Object* a, float dt) {
    Vec3 v = vget(w, a, "Velocity") + vget(w, a, "Acceleration") * dt;
    float speed = fopt(w, a, "AirSpeed", 0);
    if (speed > 0 && length(v) > speed) v = v * (speed / length(v));
    vset(w, a, "Velocity", v);
    Vec3 delta = v * dt;
    for (int pass = 0; pass < 2 && length(delta) > 0.01f; ++pass) {
        TraceHit h = move(w, a, delta);
        if (!h || a->deleted) return;
        hitWall(w, a, h);
        if (a->deleted || w.var(a, "Physics").i() != PHYS_Flying) return;
        Vec3 rest = delta * (1 - h.time);
        delta = rest - h.normal * dot(rest, h.normal);
    }
}

void physProjectile(World& w, Object* a, float dt) {
    Vec3 v = vget(w, a, "Velocity") + vget(w, a, "Acceleration") * dt;
    vset(w, a, "Velocity", v);
    TraceHit h = move(w, a, v * dt);
    if (h && !a->deleted) hitWall(w, a, h);
}

void physTrailer(World& w, Object* a) {
    Object* owner = w.obj(a, "Owner");
    if (!owner) return;
    Vec3 at = vget(w, owner, "Location");
    if (w.flag(a, "bTrailerPrePivot")) at = at + vget(w, a, "PrePivot");
    w.collision->place(a, at);
    if (w.flag(a, "bTrailerSameRotation")) w.var(a, "Rotation") = w.var(owner, "Rotation");
}

// An element of a static array variable.
Value& element(World& w, Object* a, const char* name, int k) {
    Prop* p = a->cls->findProp(Name(name));
    if (!p || k < 0 || k >= p->dim) throw w.vm.error(a->path() + std::string(": no element of ") + name);
    return a->props[size_t(p->slot + k)];
}

// Moving a mover: what stands on it goes with it.
void moveMover(World& w, Object* m, Vec3 to, int32_t rot[3]) {
    Vec3 from = vget(w, m, "Location");
    Vec3 delta = to - from;
    w.collision->place(m, to);
    w.var(m, "Rotation") = w.vm.rotator(rot[0], rot[1], rot[2]);
    if (length(delta) == 0) return;
    for (Object* o : w.actors)
        if (!o->deleted && o != m && w.obj(o, "Base") == m) w.collision->place(o, vget(w, o, "Location") + delta);
}

// MovingBrush: a mover on its way from OldPos and OldRot to its key, at
// PhysRate, eased in and out for MV_GlideByTime. At the end, bInterpolating
// goes and FinishedInterpolation is sent.
void physMovingBrush(World& w, Object* a, float dt) {
    if (!w.flag(a, "bInterpolating")) return;
    float alpha = w.var(a, "PhysAlpha").f() + w.var(a, "PhysRate").f() * dt;
    bool done = alpha >= 1;
    alpha = std::min(alpha, 1.0f);
    w.var(a, "PhysAlpha") = Value::Float(alpha);
    float t = alpha;
    if (w.var(a, "MoverGlideType").i() == 1) t = alpha * alpha * (3 - 2 * alpha);
    int key = w.var(a, "KeyNum").i();
    Vec3 keyPos, basePos = vget(w, a, "BasePos"), oldPos = vget(w, a, "OldPos");
    w.vm.unvector(element(w, a, "KeyPos", key), keyPos.x, keyPos.y, keyPos.z);
    int32_t kr[3], br[3], orr[3], r[3];
    w.vm.unrotator(element(w, a, "KeyRot", key), kr[0], kr[1], kr[2]);
    w.vm.unrotator(w.var(a, "BaseRot"), br[0], br[1], br[2]);
    w.vm.unrotator(w.var(a, "OldRot"), orr[0], orr[1], orr[2]);
    bool shortest = w.flag(a, "bUseShortestRotation");
    for (int k = 0; k < 3; ++k) {
        int32_t d = br[k] + kr[k] - orr[k];
        if (shortest) d = int32_t(int16_t(d & 0xFFFF));
        r[k] = orr[k] + int32_t(float(d) * t);
    }
    moveMover(w, a, oldPos + (basePos + keyPos - oldPos) * t, r);
    if (done) {
        w.var(a, "bInterpolating") = Value::Bool(false);
        w.vm.event(a, "FinishedInterpolation");
    }
}

}  // namespace

void performPhysics(World& w, Object* a, float dt) {
    if (!w.collision) return;
    int mode = w.var(a, "Physics").i();
    switch (mode) {
    case PHYS_None:
        return;
    case PHYS_Walking:
        physicsRotation(w, a, dt);
        physWalking(w, a, dt);
        return;
    case PHYS_Falling:
        physicsRotation(w, a, dt);
        physFalling(w, a, dt);
        return;
    case PHYS_Flying:
        physicsRotation(w, a, dt);
        physFlying(w, a, dt);
        return;
    case PHYS_Rotating:
        physicsRotation(w, a, dt);
        return;
    case PHYS_Projectile:
        physicsRotation(w, a, dt);
        physProjectile(w, a, dt);
        return;
    case PHYS_Trailer:
        physTrailer(w, a);
        return;
    case PHYS_MovingBrush:
        physMovingBrush(w, a, dt);
        return;
    default: {
        static const char* names[] = {"", "", "", "Swimming", "", "", "", "Interpolating", "",
                                      "Spider", "", "Ladder", "RootMotion", "Karma", "KarmaRagDoll", "PushPulled"};
        w.vm.missingCalls[std::string("physics PHYS_") + (mode >= 0 && mode < 16 ? names[mode] : "?")]++;
    }
    }
}

void updateTouching(World& w, Object* a) {
    if (!w.flag(a, "bCollideActors")) return;
    Vec3 p = vget(w, a, "Location");
    float r = w.var(a, "CollisionRadius").f(), h = w.var(a, "CollisionHeight").f();
    bool blocksA = w.flag(a, "bBlockActors");
    Value& mine = w.var(a, "Touching");
    if (!mine.isArr()) mine = Value::Arr(Array());
    auto touching = [&](Object* x, Object* y) {
        const Value& t = w.var(x, "Touching");
        if (!t.isArr()) return false;
        for (const Value& e : t.arr())
            if (e.o() == y) return true;
        return false;
    };
    auto add = [&](Object* x, Object* y) {
        Value& t = w.var(x, "Touching");
        if (!t.isArr()) t = Value::Arr(Array());
        t.arr().push_back(Value::Obj(y));
    };
    auto remove = [&](Object* x, Object* y) {
        Value& t = w.var(x, "Touching");
        if (!t.isArr()) return;
        auto& arr = t.arr();
        arr.erase(std::remove_if(arr.begin(), arr.end(), [&](const Value& e) { return e.o() == y; }), arr.end());
    };
    for (Object* o : w.actors) {
        if (o == a || o->deleted || o == w.info || !w.flag(o, "bCollideActors")) continue;
        if (w.var(o, "DrawType").i() == 8 && !w.flag(o, "bUseCylinderCollision") && w.obj(o, "StaticMesh") &&
            w.flag(o, "bStatic"))
            continue;   // level decoration touches through its triangles, not done
        if (blocksA && w.flag(o, "bBlockActors")) continue;
        ActorShape sh = shapeOf(w, o);
        Vec3 q = sh.center;
        float dr = r + sh.radius, dh = h + sh.height;
        bool over;
        if (sh.box) {
            // a cylinder against a box, as a box of its radius
            Vec3 l = sh.local(p);
            over = std::fabs(l.x) <= sh.radius + r && std::fabs(l.y) <= sh.width + r && std::fabs(l.z) <= dh;
        } else {
            over = std::fabs(q.z - p.z) <= dh && (q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) <= dr * dr;
        }
        bool was = touching(a, o);
        if (over && !was) {
            add(a, o);
            add(o, a);
            w.vm.event(o, "Touch", {Value::Obj(a)});
            if (!a->deleted && !o->deleted) w.vm.event(a, "Touch", {Value::Obj(o)});
        } else if (!over && was) {
            remove(a, o);
            remove(o, a);
            w.vm.event(o, "UnTouch", {Value::Obj(a)});
            if (!a->deleted && !o->deleted) w.vm.event(a, "UnTouch", {Value::Obj(o)});
        }
        if (a->deleted) return;
    }
}

void registerPhysicsNatives(VM& vm) {
    auto& n = vm.natives;
    n["actor.touchingactors"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        Object* base = c.o(0);
        if (!w || !base || !base->isClass()) return Value();
        const Value& t = w->var(c.self, "Touching");
        if (t.isArr())
            for (const Value& e : Array(t.arr()))
                if (e.o() && !e.o()->deleted && e.o()->isA(static_cast<Class*>(base))) c.yield({e});
        return Value();
    };
    // FinishInterpolation: state code waits until the interpolation is over.
    n["actor.finishinterpolation"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        Object* self = c.self;
        if (!w || !w->flag(self, "bInterpolating")) return Value();
        self->latent = [w, self](float) { return self->deleted || !w->flag(self, "bInterpolating"); };
        return Value();
    };
    // Move(Delta): an actor that collides with the world stops at what is in
    // the way; one that does not goes all the way. True when it moved at all.
    n["actor.move"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w || !w->collision) return Value::Bool(false);
        Vec3 d;
        c.vm.unvector(c.get(0), d.x, d.y, d.z);
        Vec3 before = vget(*w, c.self, "Location");
        if (w->flag(c.self, "bCollideWorld")) {
            move(*w, c.self, d);
        } else {
            w->collision->place(c.self, before + d);
            updateTouching(*w, c.self);
        }
        return Value::Bool(length(vget(*w, c.self, "Location") - before) > 0 || length(d) == 0);
    };
    // SetBase(NewBase, optional NewFloor): what the actor stands on, which
    // it then moves with; KWPawn's climb stands on the ledge it climbs.
    n["actor.setbase"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (w) setBase(*w, c.self, c.o(0));
        return Value();
    };
    n["actor.setphysics"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value();
        setPhysics(*w, c.self, c.i(0));
        return Value();
    };
}

}  // namespace ffa
