#include "world/Karma.h"

#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "world/Animator.h"
#include "world/Collision.h"
#include "world/Physics.h"

namespace ffa {

namespace {

// An impulse in the game's units over a body's KMass gives this much more
// than its speed in units a second. The scale is not in the data: tuned so
// that Shrek's punch, ForceFromHit 10000, swings a bag of KMass 2 on its
// rope some 30 degrees, and his walking into it, 5000, half as far.
const float kImpulseScale = 16;

struct Member {
    Object* actor;
    Vec3 offset;                    // from the anchor, hanging still
    float axes[3][3];               // its axes, hanging still
    float mass;
};

struct Hanger {
    Vec3 anchor;
    std::vector<Member> members;
    float rot[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};   // turned from still
    Vec3 spin;                      // angular velocity, radians a second
    float damping = 0;
    float gravScale = 1;
    bool awake = false;
};

struct KarmaWorld {
    bool built = false;
    std::vector<Hanger> hangers;
    std::map<const Object*, size_t> hangerOf;
};

Vec3 vget(World& w, Object* a, const char* n) {
    Vec3 v;
    w.vm.unvector(w.var(a, n), v.x, v.y, v.z);
    return v;
}

float param(World& w, Object* a, const char* n, float def) {
    Object* k = a->cls->findProp(Name("KParams")) ? w.obj(a, "KParams") : nullptr;
    if (!k || !k->cls->findProp(Name(n))) return def;
    return w.var(k, n).f();
}

Vec3 mul(const float m[3][3], Vec3 v) {
    // the columns are the turned axes: x along the first, and so on
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z, m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z};
}

KarmaWorld& karmaOf(World& w) {
    if (!w.karma) w.karma = std::make_shared<KarmaWorld>();
    KarmaWorld& k = *static_cast<KarmaWorld*>(w.karma.get());
    if (k.built) return k;
    k.built = true;
    // KBSJoints: one held to the world (no KConstraintActor2) is an anchor;
    // one between two actors hangs the second from the first
    Class* joint = w.vm.findClass("KConstraint");
    if (!joint) return k;
    std::map<Object*, Vec3> anchorOf;
    std::map<Object*, std::vector<Object*>> below;
    for (Object* j : w.actors) {
        if (j->deleted || !j->isA(joint)) continue;
        Object* a1 = w.obj(j, "KConstraintActor1");
        Object* a2 = w.obj(j, "KConstraintActor2");
        if (a1 && !a2) anchorOf[a1] = vget(w, j, "Location");
        if (a1 && a2) below[a1].push_back(a2);
    }
    for (auto& [top, anchor] : anchorOf) {
        Hanger h;
        h.anchor = anchor;
        std::vector<Object*> todo{top};
        while (!todo.empty()) {
            Object* a = todo.back();
            todo.pop_back();
            if (k.hangerOf.count(a)) continue;
            Member m;
            m.actor = a;
            m.offset = vget(w, a, "Location") - anchor;
            int32_t p, y, r;
            w.vm.unrotator(w.var(a, "Rotation"), p, y, r);
            rotationAxes(p, y, r, m.axes);
            m.mass = std::max(0.01f, param(w, a, "KMass", 1));
            h.damping = std::max(h.damping, param(w, a, "KAngularDamping", 0.2f));
            h.gravScale = param(w, a, "KActorGravScale", 1);
            k.hangerOf[a] = k.hangers.size();
            h.members.push_back(m);
            for (Object* b : below[a]) todo.push_back(b);
        }
        k.hangers.push_back(std::move(h));
    }
    return k;
}

// About the anchor: the members' moment, as points.
float inertia(const Hanger& h) {
    float i = 0;
    for (const Member& m : h.members) i += m.mass * dot(m.offset, m.offset);
    return std::max(i, 1e-3f);
}

void place(World& w, Hanger& h) {
    for (const Member& m : h.members) {
        if (m.actor->deleted) continue;
        Vec3 at = h.anchor + mul(h.rot, m.offset);
        Vec3 ax[3];
        for (int c = 0; c < 3; ++c) ax[c] = mul(h.rot, Vec3{m.axes[c][0], m.axes[c][1], m.axes[c][2]});
        int32_t r[3];
        rotatorOf(ax, r);
        Vec3 v = cross(h.spin, at - h.anchor);
        w.var(m.actor, "Velocity") = w.vm.vector(v.x, v.y, v.z);
        w.var(m.actor, "Rotation") = w.vm.rotator(r[0], r[1], r[2]);
        w.collision->place(m.actor, at);
    }
}

}  // namespace

void karmaTick(World& w, Object* a, float dt) {
    KarmaWorld& k = karmaOf(w);
    auto it = k.hangerOf.find(a);
    if (it == k.hangerOf.end()) return;            // a free body: not done
    Hanger& h = k.hangers[it->second];
    // the whole is moved once a frame, by its first member still in Karma
    for (const Member& m : h.members)
        if (!m.actor->deleted && w.var(m.actor, "Physics").i() == PHYS_Karma) {
            if (m.actor != a) return;
            break;
        }
    if (!h.awake) return;
    // gravity about the anchor, on the members' middle
    Vec3 g{0, 0, -950.0f * h.gravScale};
    Vec3 torque{};
    for (const Member& m : h.members) torque = torque + cross(mul(h.rot, m.offset), g * m.mass);
    h.spin = h.spin + torque * (dt / inertia(h));
    h.spin = h.spin * std::exp(-h.damping * dt);
    // turn by the spin: Rodrigues's rotation of each axis
    float angle = length(h.spin) * dt;
    if (angle > 1e-6f) {
        Vec3 n = h.spin * (1 / length(h.spin));
        float c = std::cos(angle), s = std::sin(angle);
        for (auto& col : h.rot) {
            Vec3 v{col[0], col[1], col[2]};
            Vec3 t = v * c + cross(n, v) * s + n * (dot(n, v) * (1 - c));
            col[0] = t.x;
            col[1] = t.y;
            col[2] = t.z;
        }
    }
    place(w, h);
    // still again: asleep, where it hangs
    Vec3 down = mul(h.rot, Vec3{0, 0, -1});
    if (length(h.spin) < 0.02f && down.z < -0.999f) h.awake = false;
}

void registerKarmaNatives(VM& vm) {
    auto& n = vm.natives;
    // KAddImpulse(Impulse, Position, optional BoneName)
    n["actor.kaddimpulse"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w || !w->collision) return Value();
        KarmaWorld& k = karmaOf(*w);
        auto it = k.hangerOf.find(c.self);
        if (it == k.hangerOf.end()) return Value();
        Hanger& h = k.hangers[it->second];
        Vec3 j, p;
        c.vm.unvector(c.get(0), j.x, j.y, j.z);
        c.vm.unvector(c.get(1), p.x, p.y, p.z);
        // turned about the anchor by the impulse's moment there
        h.spin = h.spin + cross(p - h.anchor, j * (1 / kImpulseScale)) * (1 / inertia(h));
        h.awake = true;
        return Value();
    };
    n["actor.kisawake"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value::Bool(false);
        KarmaWorld& k = karmaOf(*w);
        auto it = k.hangerOf.find(c.self);
        return Value::Bool(it != k.hangerOf.end() && k.hangers[it->second].awake);
    };
    n["actor.ksleep"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value();
        KarmaWorld& k = karmaOf(*w);
        auto it = k.hangerOf.find(c.self);
        if (it != k.hangerOf.end()) {
            k.hangers[it->second].awake = false;
            k.hangers[it->second].spin = {};
        }
        return Value();
    };
    n["actor.kwake"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value();
        KarmaWorld& k = karmaOf(*w);
        auto it = k.hangerOf.find(c.self);
        if (it != k.hangerOf.end()) k.hangers[it->second].awake = true;
        return Value();
    };
}

}  // namespace ffa
