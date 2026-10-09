#include "world/AI.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>
#include <unordered_map>

#include "world/Collision.h"
#include "world/Physics.h"

namespace ffa {

namespace {

// The engine's constants for these, none of them in the data.
constexpr float kMaxStep = 35;          // a step up a walk takes, as physics does
constexpr float kMaxDrop = 160;         // a drop a walk may take off a ledge
constexpr float kReachDistance = 1200;  // farther than this nothing is reachable at once
constexpr float kAnchorDistance = 1200; // path nodes a route may start or end at
constexpr int kTurnTolerance = 2000;    // FinishRotation: close enough, in yaw units

Vec3 vget(World& w, Object* a, const char* n) {
    Vec3 v;
    w.vm.unvector(w.var(a, n), v.x, v.y, v.z);
    return v;
}
void vset(World& w, Object* a, const char* n, Vec3 v) { w.var(a, n) = w.vm.vector(v.x, v.y, v.z); }
float fopt(Object* a, const char* n, float def) {
    Prop* p = a->cls->findProp(Name(n));
    return p ? a->props[size_t(p->slot)].f() : def;
}
Object* oopt(Object* a, const char* n) {
    Prop* p = a->cls->findProp(Name(n));
    return p ? a->props[size_t(p->slot)].o() : nullptr;
}
float length2d(Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y); }
bool live(Object* o) { return o && !o->deleted; }

Class* navClass(World& w) { return w.linker.findClass("NavigationPoint"); }

int32_t yawTo(Vec3 d) { return int32_t(std::lround(std::atan2(d.y, d.x) * 32768.0 / M_PI)) & 0xFFFF; }

// Whether a pawn is at p, or at a goal actor: within its radius, half the
// pawn's, of it across and within their heights and a step of it up and down.
bool atPlace(World& w, Object* pawn, Vec3 p, Object* goal) {
    Vec3 at = vget(w, pawn, "Location");
    float r = std::max(8.0f, w.var(pawn, "CollisionRadius").f() * 0.5f);
    float h = w.var(pawn, "CollisionHeight").f() + kMaxStep;
    if (goal) {
        p = vget(w, goal, "Location");
        r += fopt(goal, "CollisionRadius", 0);
        h += fopt(goal, "CollisionHeight", 0);
    }
    return length2d(p - at) <= r && std::fabs(p.z - at.z) <= h;
}

// The latent move every MoveTo and MoveToward is: accelerate toward the
// destination until there, or until the move's time runs out.
void startMove(NativeCall& c, Vec3 dest, Object* target, Object* focus, bool walk) {
    World& w = *World::of(c.vm);
    Object* ctl = c.self;
    Object* pawn = w.obj(ctl, "Pawn");
    w.var(ctl, "bMoveToSuccess") = Value::Bool(false);
    w.var(ctl, "MoveTarget") = Value::Obj(target);
    w.var(ctl, "Focus") = Value::Obj(focus);
    vset(w, ctl, "Destination", dest);
    if (!focus) vset(w, ctl, "FocalPoint", dest);
    // A move that cannot be made still waits a tick, as every latent call
    // does: the factory workers' MoveTowardHero goes straight back to its
    // MoveToward when one fails, and with a dead worker's controller, its pawn
    // gone, it ran a million statements a frame and the game stood still.
    if (!live(pawn)) {
        ctl->latent = [](float) { return true; };
        return;
    }
    w.var(pawn, "bIsWalking") = Value::Bool(walk);
    // As the engine sets it: time for the distance at the pawn's speed, a
    // third over, and a second.
    float speed = w.var(pawn, "GroundSpeed").f();
    if (walk) speed *= fopt(pawn, "WalkingPct", 1);
    float dist = length(dest - vget(w, pawn, "Location"));
    w.var(ctl, "MoveTimer") = Value::Float(1 + 1.3f * dist / std::max(speed, 1.0f));
    VM* vm = &c.vm;
    // Going round what it walks into, as the engine's AI adjusts: a pawn that
    // has hardly moved for a few frames while it means to steps aside, to the
    // nearer side its box is free at, and goes on from there.
    struct Round {
        Vec3 last;
        int stuck = 0;
        bool adjusting = false;
        Vec3 adjust;
        float adjustLeft = 0;
        float first = 1;            // the side tried first, turned about each time
    };
    auto round = std::make_shared<Round>();
    round->last = vget(w, pawn, "Location");
    ctl->latent = [vm, ctl, target, round](float dt) {
        World& w = *World::of(*vm);
        Object* pawn = w.obj(ctl, "Pawn");
        if (!live(pawn)) return true;
        if (target && !live(target)) {
            vset(w, pawn, "Acceleration", {});
            return true;
        }
        Vec3 dest = target ? vget(w, target, "Location") : vget(w, ctl, "Destination");
        if (target) vset(w, ctl, "Destination", dest);
        if (atPlace(w, pawn, dest, target)) {
            vset(w, pawn, "Acceleration", {});
            w.var(ctl, "bMoveToSuccess") = Value::Bool(true);
            return true;
        }
        float left = w.var(ctl, "MoveTimer").f() - dt;
        w.var(ctl, "MoveTimer") = Value::Float(left);
        if (left < 0) {
            vset(w, pawn, "Acceleration", {});
            return true;
        }
        Vec3 at = vget(w, pawn, "Location");
        int phys = w.var(pawn, "Physics").i();
        bool ground = phys != PHYS_Flying && phys != PHYS_Swimming;
        float radius = w.var(pawn, "CollisionRadius").f();
        float speed = w.var(pawn, "GroundSpeed").f();
        if (w.flag(pawn, "bIsWalking")) speed *= fopt(pawn, "WalkingPct", 1);
        // stuck: under a tenth of its speed's way this frame
        float moved = length2d(at - round->last);
        round->last = at;
        // stuck, on the way or on the way round, where another pawn may have
        // stepped in: a new way round, the other side first this time
        if (phys == PHYS_Walking && dt > 0 && moved < 0.1f * speed * dt) {
            if (++round->stuck >= 3 && w.collision) {
                round->stuck = 0;
                round->adjusting = false;
                round->first = -round->first;
                Vec3 to = dest - at;
                to.z = 0;
                float len = length(to);
                if (len > 0) {
                    to = to * (1 / len);
                    Vec3 side{-to.y, to.x, 0};
                    float h = w.var(pawn, "CollisionHeight").f();
                    Vec3 ext{radius, radius, h};
                    // the nearest free step to either side, then one ahead
                    for (float k : {1.5f, 3.0f, 5.0f}) {
                        bool found = false;
                        for (float sgn : {round->first, -round->first}) {
                            Vec3 p = at + side * (sgn * k * radius) + Vec3{0, 0, 2};
                            if (w.collision->boxCheck(at + Vec3{0, 0, 2}, p, ext, pawn)) continue;
                            if (w.collision->boxCheck(p, p + to * (2 * radius), ext, pawn)) continue;
                            round->adjust = p + to * radius;
                            round->adjusting = true;
                            round->adjustLeft = 1.0f;
                            found = true;
                            break;
                        }
                        if (found) break;
                    }
                }
            }
        } else {
            round->stuck = 0;
        }
        Vec3 goal = dest;
        if (round->adjusting) {
            round->adjustLeft -= dt;
            if (length2d(round->adjust - at) < std::max(8.0f, radius * 0.5f) || round->adjustLeft <= 0)
                round->adjusting = false;
            else
                goal = round->adjust;
        }
        Vec3 d = goal - at;
        if (ground) d.z = 0;
        float n = length(d);
        float rate = fopt(pawn, "AccelRate", 2048);
        vset(w, pawn, "Acceleration", n > 0 ? d * (rate / n) : Vec3{});
        return false;
    };
}

// The edges of the level's path network, read once a world: per
// NavigationPoint, the ReachSpecs leaving it.
struct Edge {
    Object* end;
    float distance, radius, height;
    int flags;
};
struct Paths {
    std::vector<Object*> nodes;
    std::unordered_map<Object*, std::vector<Edge>> out;
};
const Paths& pathsOf(World& w) {
    if (w.ai) return *static_cast<const Paths*>(w.ai.get());
    auto made = std::make_shared<Paths>();
    w.ai = made;
    Paths& p = *made;
    Class* nav = navClass(w);
    if (!nav) return p;
    for (Object* a : w.actors) {
        if (a->deleted || !a->isA(nav)) continue;
        p.nodes.push_back(a);
        const Value& list = w.var(a, "PathList");
        if (!list.isArr()) continue;
        for (const Value& v : list.arr()) {
            Object* s = v.o();
            if (!s) continue;
            Object* end = oopt(s, "End");
            if (!end) continue;
            p.out[a].push_back({end, std::max(1.0f, float(w.var(s, "Distance").i())),
                                float(w.var(s, "CollisionRadius").i()), float(w.var(s, "CollisionHeight").i()),
                                w.var(s, "reachFlags").i()});
        }
    }
    return p;
}

// reachFlags the engine's walkers do not take: R_PROSCRIBED, and the ones a
// pawn needs a special move for, ladders and specials, which are left to
// script.
constexpr int R_SPECIAL = 32, R_LADDER = 64, R_PROSCRIBED = 128;

// The cheapest route from where the pawn is to any of the goals, each with
// the cost of the last step to it: the nodes along it, the first one first.
std::vector<Object*> route(World& w, Object* pawn, const std::vector<std::pair<Object*, float>>& goals,
                           float& total) {
    const Paths& p = pathsOf(w);
    Vec3 at = vget(w, pawn, "Location");
    float radius = w.var(pawn, "CollisionRadius").f(), height = w.var(pawn, "CollisionHeight").f();
    std::unordered_map<Object*, float> cost, goalCost;
    std::unordered_map<Object*, Object*> from;
    for (auto& [g, c] : goals) goalCost[g] = c;
    using Item = std::pair<float, Object*>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    // Start from the nearest nodes the pawn can walk to.
    std::vector<std::pair<float, Object*>> near;
    for (Object* n : p.nodes) {
        float d = length(vget(w, n, "Location") - at);
        if (d <= kAnchorDistance) near.emplace_back(d, n);
    }
    std::sort(near.begin(), near.end(), [](auto& x, auto& y) { return x.first < y.first; });
    int tried = 0;
    for (auto& [d, n] : near) {
        if (++tried > 8) break;
        if (!reachable(w, pawn, vget(w, n, "Location"), n)) continue;
        cost[n] = d;
        open.push({d, n});
    }
    Object* best = nullptr;
    float bestCost = 1e30f;
    size_t steps = 0;
    while (!open.empty() && ++steps < 100000) {
        auto [c, n] = open.top();
        open.pop();
        if (c > cost[n] || c >= bestCost) continue;
        auto g = goalCost.find(n);
        if (g != goalCost.end() && c + g->second < bestCost) {
            bestCost = c + g->second;
            best = n;
        }
        auto e = p.out.find(n);
        if (e == p.out.end()) continue;
        for (const Edge& ed : e->second) {
            if (ed.flags & (R_SPECIAL | R_LADDER | R_PROSCRIBED)) continue;
            if (ed.radius < radius || ed.height < height) continue;
            float nc = c + ed.distance;
            auto it = cost.find(ed.end);
            if (it != cost.end() && it->second <= nc) continue;
            cost[ed.end] = nc;
            from[ed.end] = n;
            open.push({nc, ed.end});
        }
    }
    std::vector<Object*> out;
    if (!best) return out;
    for (Object* n = best; n; n = from.count(n) ? from[n] : nullptr) out.push_back(n);
    std::reverse(out.begin(), out.end());
    total = bestCost;
    return out;
}

// Fill the controller's RouteCache with a route and the goal after it, and
// give the first place to move to: past the first node when the pawn is
// already there.
Object* takeRoute(World& w, Object* ctl, Object* pawn, std::vector<Object*> nodes, Object* goal, float total) {
    if (goal && (nodes.empty() || nodes.back() != goal)) nodes.push_back(goal);
    if (nodes.size() > 1 && atPlace(w, pawn, {}, nodes.front())) nodes.erase(nodes.begin());
    if (Prop* rc = ctl->cls->findProp(Name("RouteCache")))
        for (int i = 0; i < rc->dim; ++i)
            ctl->props[size_t(rc->slot + i)] = Value::Obj(size_t(i) < nodes.size() ? nodes[size_t(i)] : nullptr);
    w.var(ctl, "RouteGoal") = Value::Obj(goal);
    w.var(ctl, "RouteDist") = Value::Float(total);
    return nodes.empty() ? nullptr : nodes.front();
}

// The goals a route can end at for an actor or a point: the node itself, or
// the nodes near it from which it can be walked to, by how far.
std::vector<std::pair<Object*, float>> goalsFor(World& w, Object* pawn, Vec3 p, Object* goal) {
    std::vector<std::pair<Object*, float>> goals;
    Class* nav = navClass(w);
    if (goal && nav && goal->isA(nav)) {
        goals.emplace_back(goal, 0.0f);
        return goals;
    }
    std::vector<std::pair<float, Object*>> near;
    for (Object* n : pathsOf(w).nodes) {
        float d = length(vget(w, n, "Location") - p);
        if (d <= kAnchorDistance) near.emplace_back(d, n);
    }
    std::sort(near.begin(), near.end(), [](auto& x, auto& y) { return x.first < y.first; });
    for (size_t i = 0; i < near.size() && i < 8; ++i) {
        Collision& col = *w.collision;
        Vec3 np = vget(w, near[i].second, "Location");
        // a clear line from the node, as a walk from it is the pawn's to make
        if (!col.lineCheck(np, p, pawn, false, true)) goals.emplace_back(near[i].second, near[i].first);
    }
    return goals;
}

Vec3 eyeOf(World& w, Object* pawn) {
    return vget(w, pawn, "Location") + Vec3{0, 0, fopt(pawn, "BaseEyeHeight", 0)};
}

bool lineOfSight(World& w, Object* pawn, Object* other) {
    Collision& col = *w.collision;
    Vec3 eye = eyeOf(w, pawn), at = vget(w, other, "Location");
    if (!col.lineCheck(eye, at, pawn, false, true)) return true;
    // its head, when its middle is hidden
    float h = fopt(other, "CollisionHeight", 0);
    return h > 0 && !col.lineCheck(eye, at + Vec3{0, 0, h * 0.8f}, pawn, false, true);
}

}  // namespace

bool aiSteering(World& w, Object* ctl) {
    Object* focus = w.obj(ctl, "Focus");
    if (live(focus)) return true;
    Vec3 fp = vget(w, ctl, "FocalPoint");
    return fp.x != 0 || fp.y != 0 || fp.z != 0;
}

void aiTick(World& w, Object* ctl) {
    Object* pawn = w.obj(ctl, "Pawn");
    if (!live(pawn)) return;
    Object* focus = w.obj(ctl, "Focus");
    if (focus && focus->deleted) {
        w.var(ctl, "Focus") = Value::Obj(nullptr);
        focus = nullptr;
    }
    if (focus) vset(w, ctl, "FocalPoint", vget(w, focus, "Location"));
    if (!aiSteering(w, ctl)) return;
    Vec3 fp = vget(w, ctl, "FocalPoint");
    Vec3 d = fp - vget(w, pawn, "Location");
    if (length2d(d) < 1) return;
    int32_t p, y, r;
    w.vm.unrotator(w.var(ctl, "DesiredRotation"), p, y, r);
    w.var(ctl, "DesiredRotation") = w.vm.rotator(p, yawTo(d), r);
}

bool reachable(World& w, Object* pawn, Vec3 to, Object* goal) {
    if (!w.collision || !live(pawn)) return false;
    Collision& col = *w.collision;
    Vec3 at = vget(w, pawn, "Location");
    if (goal) to = vget(w, goal, "Location");
    if (length2d(to - at) > kReachDistance) return false;
    float r = w.var(pawn, "CollisionRadius").f(), h = w.var(pawn, "CollisionHeight").f();
    Vec3 ext{r * 0.9f, r * 0.9f, h * 0.9f};
    int phys = w.var(pawn, "Physics").i();
    if (phys == PHYS_Flying || phys == PHYS_Swimming) {
        TraceHit hit = col.boxCheck(at, to, ext, pawn);
        return !hit || hit.actor == goal;
    }
    // A walk: steps of a radius across, each up a step, across, and down to
    // a floor, which must be there within a drop and walkable.
    Vec3 cur = at;
    float stepLen = std::clamp(r, 16.0f, 48.0f);
    for (int k = 0; k < 200; ++k) {
        Vec3 d = to - cur;
        d.z = 0;
        float left = length(d);
        float goalR = goal ? fopt(goal, "CollisionRadius", 0) : 0;
        if (left <= std::max(8.0f, r * 0.5f) + goalR) break;
        Vec3 next = cur + d * (std::min(stepLen, left) / left);
        Vec3 up = cur + Vec3{0, 0, kMaxStep};
        TraceHit hit = col.boxCheck(up, next + Vec3{0, 0, kMaxStep}, ext, pawn);
        if (hit && hit.actor != goal) return false;
        if (hit && hit.actor == goal) return true;
        Vec3 high = next + Vec3{0, 0, kMaxStep};
        TraceHit floor = col.boxCheck(high, high - Vec3{0, 0, kMaxStep + kMaxDrop}, ext, pawn);
        if (!floor || floor.normal.z < 0.7f) return false;
        cur = floor.location;
    }
    float goalH = goal ? fopt(goal, "CollisionHeight", 0) : 0;
    return std::fabs(to.z - cur.z) <= h + goalH + kMaxStep;
}

void registerAINatives(VM& vm) {
    auto& n = vm.natives;
    // MoveTo(NewDestination, optional ViewFocus, optional bShouldWalk)
    n["controller.moveto"] = [](NativeCall& c) {
        Vec3 d;
        c.vm.unvector(c.get(0), d.x, d.y, d.z);
        startMove(c, d, nullptr, c.o(1), c.b(2));
        return Value();
    };
    // MoveToward(NewTarget, optional ViewFocus, optional DestinationOffset,
    // optional bUseStrafing, optional bShouldWalk): the target is looked at
    // unless something else is.
    n["controller.movetoward"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* t = c.o(0);
        if (!live(t)) {
            w.var(c.self, "bMoveToSuccess") = Value::Bool(false);
            c.self->latent = [](float) { return true; };
            return Value();
        }
        startMove(c, vget(w, t, "Location"), t, c.o(1) ? c.o(1) : t, c.b(4));
        return Value();
    };
    // FinishRotation: until the pawn faces where its controller wants it to,
    // or five seconds, as a pawn that cannot turn would hold it for ever.
    n["controller.finishrotation"] = [](NativeCall& c) {
        VM* vm = &c.vm;
        Object* ctl = c.self;
        auto left = std::make_shared<float>(5.0f);
        ctl->latent = [vm, ctl, left](float dt) {
            World& w = *World::of(*vm);
            Object* pawn = w.obj(ctl, "Pawn");
            if (!live(pawn) || (*left -= dt) <= 0) return true;
            aiTick(w, ctl);
            int32_t p, y, r, pp, py, pr;
            w.vm.unrotator(w.var(ctl, "DesiredRotation"), p, y, r);
            w.vm.unrotator(w.var(pawn, "Rotation"), pp, py, pr);
            return std::abs(int16_t((y - py) & 0xFFFF)) < kTurnTolerance;
        };
        return Value();
    };
    // WaitForLanding: until the pawn is not falling.
    n["controller.waitforlanding"] = [](NativeCall& c) {
        VM* vm = &c.vm;
        Object* ctl = c.self;
        auto left = std::make_shared<float>(10.0f);
        ctl->latent = [vm, ctl, left](float dt) {
            World& w = *World::of(*vm);
            Object* pawn = w.obj(ctl, "Pawn");
            return !live(pawn) || w.var(pawn, "Physics").i() != PHYS_Falling || (*left -= dt) <= 0;
        };
        return Value();
    };
    n["controller.actorreachable"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* t = c.o(0);
        return Value::Bool(live(t) && reachable(w, w.obj(c.self, "Pawn"), {}, t));
    };
    n["controller.pointreachable"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Vec3 p;
        c.vm.unvector(c.get(0), p.x, p.y, p.z);
        return Value::Bool(reachable(w, w.obj(c.self, "Pawn"), p));
    };
    // FindPathToward(anActor, optional bWeightDetours): the actor itself when
    // it can be walked to, else the first node of the cheapest route to it.
    n["controller.findpathtoward"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* pawn = w.obj(c.self, "Pawn");
        Object* goal = c.o(0);
        if (!live(pawn) || !live(goal) || !w.collision) return Value::Obj(nullptr);
        if (reachable(w, pawn, {}, goal)) return Value::Obj(takeRoute(w, c.self, pawn, {}, goal, 0));
        float total = 0;
        std::vector<Object*> nodes = route(w, pawn, goalsFor(w, pawn, vget(w, goal, "Location"), goal), total);
        if (nodes.empty()) return Value::Obj(nullptr);
        return Value::Obj(takeRoute(w, c.self, pawn, nodes, goal, total));
    };
    n["controller.findpathto"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* pawn = w.obj(c.self, "Pawn");
        if (!live(pawn) || !w.collision) return Value::Obj(nullptr);
        Vec3 p;
        c.vm.unvector(c.get(0), p.x, p.y, p.z);
        float total = 0;
        std::vector<Object*> nodes = route(w, pawn, goalsFor(w, pawn, p, nullptr), total);
        if (nodes.empty()) return Value::Obj(nullptr);
        return Value::Obj(takeRoute(w, c.self, pawn, nodes, nullptr, total));
    };
    // FindPathTowardNearest(GoalClass): a route to the nearest node of a class.
    n["controller.findpathtowardnearest"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* pawn = w.obj(c.self, "Pawn");
        Object* cls = c.o(0);
        if (!live(pawn) || !cls || !cls->isClass() || !w.collision) return Value::Obj(nullptr);
        std::vector<std::pair<Object*, float>> goals;
        for (Object* nd : pathsOf(w).nodes)
            if (nd->isA(static_cast<Class*>(cls))) goals.emplace_back(nd, 0.0f);
        float total = 0;
        std::vector<Object*> nodes = route(w, pawn, goals, total);
        if (nodes.empty()) return Value::Obj(nullptr);
        return Value::Obj(takeRoute(w, c.self, pawn, nodes, nodes.back(), total));
    };
    n["controller.findrandomdest"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        const Paths& p = pathsOf(w);
        if (p.nodes.empty()) return Value::Obj(nullptr);
        return Value::Obj(p.nodes[std::uniform_int_distribution<size_t>(0, p.nodes.size() - 1)(c.vm.rng)]);
    };
    // LineOfSightTo(Other): from the pawn's eyes to it, through nothing of
    // the world's.
    n["controller.lineofsightto"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* pawn = w.obj(c.self, "Pawn");
        Object* other = c.o(0);
        return Value::Bool(live(pawn) && live(other) && w.collision && lineOfSight(w, pawn, other));
    };
    // CanSee(Other): in line of sight, within the pawn's SightRadius, and
    // within its PeripheralVision, the cosine of the half angle it sees.
    n["controller.cansee"] = [](NativeCall& c) {
        World& w = *World::of(c.vm);
        Object* pawn = w.obj(c.self, "Pawn");
        Object* other = c.o(0);
        if (!live(pawn) || !live(other) || !w.collision) return Value::Bool(false);
        Vec3 d = vget(w, other, "Location") - vget(w, pawn, "Location");
        float dist = length(d);
        if (dist > fopt(pawn, "SightRadius", 5000)) return Value::Bool(false);
        int32_t p, y, r;
        w.vm.unrotator(w.var(pawn, "Rotation"), p, y, r);
        float a = float(y) * float(M_PI) / 32768.0f;
        Vec3 facing{std::cos(a), std::sin(a), 0};
        if (dist > 0 && dot(d, facing) / dist < fopt(pawn, "PeripheralVision", -1)) return Value::Bool(false);
        return Value::Bool(lineOfSight(w, pawn, other));
    };
}

}  // namespace ffa
