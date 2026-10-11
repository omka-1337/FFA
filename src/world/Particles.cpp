#include "world/Particles.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "world/Collision.h"

namespace ffa {

namespace {

// --- reading an emitter's variables

const Value& prop(Object* o, const char* name) {
    static const Value none;
    Prop* p = o->cls->findProp(Name(name));
    return p ? o->props[size_t(p->slot)] : none;
}
float pf(Object* o, const char* n) { return prop(o, n).f(); }
int pi(Object* o, const char* n) { return prop(o, n).i(); }
bool pb(Object* o, const char* n) { return prop(o, n).b(); }

const Value* field(const Value& v, const char* name) {
    if (!v.isStruct()) return nullptr;
    const Prop* f = v.st().type ? v.st().type->field(Name(name)) : nullptr;
    return f ? &v.st().f[size_t(f->slot)] : nullptr;
}
float ff(const Value& v, const char* name, float def = 0) {
    const Value* f = field(v, name);
    return f ? f->f() : def;
}
Vec3 vec(const Value& v) { return {ff(v, "X"), ff(v, "Y"), ff(v, "Z")}; }

struct Range {
    float lo = 0, hi = 0;
};
Range range(const Value& v) { return {ff(v, "Min"), ff(v, "Max")}; }
struct RangeVec {
    Range x, y, z;
};
RangeVec rangeVec(const Value& v) {
    RangeVec r;
    if (const Value* f = field(v, "X")) r.x = range(*f);
    if (const Value* f = field(v, "Y")) r.y = range(*f);
    if (const Value* f = field(v, "Z")) r.z = range(*f);
    return r;
}

struct Key {
    float t;
    float v[4];
};

// SizeScale (RelativeTime, RelativeSize), ColorScale (RelativeTime, Color),
// VelocityScale (RelativeTime, RelativeVelocity), each as times and values.
std::vector<Key> keys(Object* o, const char* name, int kind) {
    std::vector<Key> out;
    const Value& a = prop(o, name);
    if (!a.isArr()) return out;
    for (const Value& e : a.arr()) {
        Key k{ff(e, "RelativeTime"), {1, 1, 1, 1}};
        if (kind == 0) {
            k.v[0] = ff(e, "RelativeSize", 1);
        } else if (kind == 1) {
            if (const Value* c = field(e, "Color")) {
                k.v[0] = ff(*c, "R") / 255.0f;
                k.v[1] = ff(*c, "G") / 255.0f;
                k.v[2] = ff(*c, "B") / 255.0f;
                k.v[3] = ff(*c, "A") / 255.0f;
            }
        } else if (const Value* r = field(e, "RelativeVelocity")) {
            Vec3 v = vec(*r);
            k.v[0] = v.x;
            k.v[1] = v.y;
            k.v[2] = v.z;
        }
        out.push_back(k);
    }
    std::stable_sort(out.begin(), out.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    // a size scale starts from the particle's own size: the torches' fire
    // has one key, none at 1.5 of its life, and shrinks to a third by its
    // end; the smoke's 3 at its end grows it threefold
    if (kind == 0 && !out.empty() && out.front().t > 0) out.insert(out.begin(), Key{0, {1, 1, 1, 1}});
    return out;
}

// A scale at t, 0 to 1 of the particle's life, between the keys around it;
// before the first its value, after the last the last's.
void at(const std::vector<Key>& ks, float t, int n, float* out) {
    if (ks.empty()) return;
    if (t <= ks.front().t) {
        std::copy(ks.front().v, ks.front().v + n, out);
        return;
    }
    for (size_t i = 1; i < ks.size(); ++i)
        if (t <= ks[i].t) {
            const Key &a = ks[i - 1], &b = ks[i];
            float u = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1;
            for (int k = 0; k < n; ++k) out[k] = a.v[k] + (b.v[k] - a.v[k]) * u;
            return;
        }
    std::copy(ks.back().v, ks.back().v + n, out);
}

// What a tick needs of an emitter's variables, read once a tick.
struct Settings {
    int maxParticles = 10;
    bool disabled = false, respawn = true, autoInitial = true;
    float pps = 0, initialPps = 0;
    Range lifetime{4, 4}, initialDelay;
    Vec3 acceleration, startOffset;
    RangeVec startLocation, velocity, velocityLoss, size, spinsPerSecond, startSpin, colorMultiplier, damping;
    Range sphereRadius, radialVelocity, maxCollisions;
    RangeVec polar;
    int shape = 0, coordinates = 0, rotationFrom = 0, velocityFrom = 0, facing = 0, style = 3;
    bool uniformSize = true, spin = false, useSize = false, useColor = false, useVelocityScale = false;
    std::vector<Key> sizeScale, colorScale, velocityScale;
    float sizeRepeats = 0, colorRepeats = 0, velocityRepeats = 0;
    bool fadeIn = false, fadeOut = false;
    float fadeInEnd = 0, fadeOutStart = 0, opacity = 1;
    float fadeInFactor[4] = {1, 1, 1, 1}, fadeOutFactor[4] = {1, 1, 1, 1};
    Vec3 spinDirection{0.5f, 0.5f, 0.5f}, maxVelocity, normal{0, 0, 1};
    Object* texture = nullptr;
    int columns = 1, rows = 1, subStart = 0, subEnd = 0;
    bool randomSub = false, collide = false, useMaxCollisions = false, addOwnerVelocity = false;
    bool zWrite = false, zTest = true;
};

void readFactor(Object* o, const char* name, float* out) {
    const Value& v = prop(o, name);
    if (!v.isStruct()) return;
    out[0] = ff(v, "X", 1);
    out[1] = ff(v, "Y", 1);
    out[2] = ff(v, "Z", 1);
    out[3] = ff(v, "W", 1);
}

Settings settingsOf(Object* e) {
    Settings s;
    s.maxParticles = std::clamp(pi(e, "MaxParticles"), 0, 2000);
    s.disabled = pb(e, "Disabled");
    s.respawn = pb(e, "RespawnDeadParticles");
    s.autoInitial = pb(e, "AutomaticInitialSpawning");
    s.pps = pf(e, "ParticlesPerSecond");
    s.initialPps = pf(e, "InitialParticlesPerSecond");
    s.lifetime = range(prop(e, "LifetimeRange"));
    s.initialDelay = range(prop(e, "InitialDelayRange"));
    s.acceleration = vec(prop(e, "Acceleration"));
    s.startOffset = vec(prop(e, "StartLocationOffset"));
    s.startLocation = rangeVec(prop(e, "StartLocationRange"));
    s.velocity = rangeVec(prop(e, "StartVelocityRange"));
    s.velocityLoss = rangeVec(prop(e, "VelocityLossRange"));
    s.size = rangeVec(prop(e, "StartSizeRange"));
    s.spinsPerSecond = rangeVec(prop(e, "SpinsPerSecondRange"));
    s.startSpin = rangeVec(prop(e, "StartSpinRange"));
    s.colorMultiplier = rangeVec(prop(e, "ColorMultiplierRange"));
    s.damping = rangeVec(prop(e, "DampingFactorRange"));
    s.sphereRadius = range(prop(e, "SphereRadiusRange"));
    s.radialVelocity = range(prop(e, "StartVelocityRadialRange"));
    s.maxCollisions = range(prop(e, "MaxCollisions"));
    s.polar = rangeVec(prop(e, "StartLocationPolarRange"));
    s.shape = pi(e, "StartLocationShape");
    s.coordinates = pi(e, "CoordinateSystem");
    s.rotationFrom = pi(e, "UseRotationFrom");
    s.velocityFrom = pi(e, "GetVelocityDirectionFrom");
    s.facing = pi(e, "UseDirectionAs");
    s.style = pi(e, "DrawStyle");
    s.uniformSize = pb(e, "UniformSize");
    s.spin = pb(e, "SpinParticles");
    s.useSize = pb(e, "UseSizeScale");
    s.useColor = pb(e, "UseColorScale");
    s.useVelocityScale = pb(e, "UseVelocityScale");
    if (s.useSize) s.sizeScale = keys(e, "SizeScale", 0);
    if (s.useColor) s.colorScale = keys(e, "ColorScale", 1);
    if (s.useVelocityScale) s.velocityScale = keys(e, "VelocityScale", 2);
    s.sizeRepeats = pf(e, "SizeScaleRepeats");
    s.colorRepeats = pf(e, "ColorScaleRepeats");
    s.velocityRepeats = pf(e, "VelocityScaleRepeats");
    s.fadeIn = pb(e, "FadeIn");
    s.fadeOut = pb(e, "FadeOut");
    s.fadeInEnd = pf(e, "FadeInEndTime");
    s.fadeOutStart = pf(e, "FadeOutStartTime");
    readFactor(e, "FadeInFactor", s.fadeInFactor);
    readFactor(e, "FadeOutFactor", s.fadeOutFactor);
    s.opacity = pf(e, "Opacity");
    s.spinDirection = vec(prop(e, "SpinCCWorCW"));
    s.maxVelocity = vec(prop(e, "MaxAbsVelocity"));
    if (prop(e, "ProjectionNormal").isStruct()) s.normal = vec(prop(e, "ProjectionNormal"));
    s.texture = prop(e, "Texture").o();
    s.columns = std::max(1, pi(e, "TextureUSubdivisions"));
    s.rows = std::max(1, pi(e, "TextureVSubdivisions"));
    s.subStart = pi(e, "SubdivisionStart");
    s.subEnd = pi(e, "SubdivisionEnd");
    s.randomSub = pb(e, "UseRandomSubdivision");
    s.collide = pb(e, "UseCollision");
    s.useMaxCollisions = pb(e, "UseMaxCollisions");
    s.addOwnerVelocity = pb(e, "bAddOwnerVelocity");
    s.zWrite = pb(e, "ZWrite");
    s.zTest = prop(e, "ZTest").isBool() ? pb(e, "ZTest") : true;
    return s;
}

// --- the particles

struct Particle {
    bool alive = false;
    Vec3 loc, vel;              // in the world, or the owner's frame for PTCS_Relative
    Vec3 startSize, colorMul{1, 1, 1}, loss, damping;
    float time = 0, life = 1, spin0 = 0, spinRate = 0;
    int frame = 0, hits = 0, maxHits = 0;
};

struct EmitterRun {
    Object* actor = nullptr;
    std::vector<Particle> ps;
    bool started = false, killed = false;
    int spawned = 0;            // since the last reset
    float fraction = 0, delay = 0;
    int deferred = 0;           // SpawnOnTrigger's particles to come
    float deferredRate = 0;
};

struct Particles {
    std::map<Object*, EmitterRun> runs;   // by ParticleEmitter
    std::vector<ParticleDraw> draws;
    uint32_t seed = 0x2545f491u;
    float rand() {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return float(seed & 0xffffff) / float(0x1000000);
    }
    float in(Range r) { return r.lo + (r.hi - r.lo) * rand(); }
};

Particles& particles(World& w) {
    if (!w.particles) w.particles = std::make_shared<Particles>();
    return *static_cast<Particles*>(w.particles.get());
}

Class* emitterClass(World& w) { return w.vm.findClass("Emitter"); }

Vec3 rotate(const float ax[3][3], Vec3 v) {
    return {ax[0][0] * v.x + ax[1][0] * v.y + ax[2][0] * v.z, ax[0][1] * v.x + ax[1][1] * v.y + ax[2][1] * v.z,
            ax[0][2] * v.x + ax[1][2] * v.y + ax[2][2] * v.z};
}

struct Basis {
    Vec3 origin, velocity;
    float ax[3][3];
};

Basis frameOf(World& w, Object* actor) {
    Basis f;
    w.vm.unvector(w.var(actor, "Location"), f.origin.x, f.origin.y, f.origin.z);
    w.vm.unvector(w.var(actor, "Velocity"), f.velocity.x, f.velocity.y, f.velocity.z);
    int32_t p, y, r;
    w.vm.unrotator(w.var(actor, "Rotation"), p, y, r);
    rotationAxes(p, y, r, f.ax);
    return f;
}

Vec3 inRange(Particles& P, const RangeVec& r) { return {P.in(r.x), P.in(r.y), P.in(r.z)}; }

void spawnOne(Particles& P, EmitterRun& run, const Settings& s, const Basis& f) {
    Particle* slot = nullptr;
    for (Particle& p : run.ps)
        if (!p.alive) {
            slot = &p;
            break;
        }
    if (!slot) return;
    Particle& p = *slot;
    p = Particle{};
    p.alive = true;
    p.life = std::max(0.01f, P.in(s.lifetime));
    // where in the emitter's shape, PTLS_Box, PTLS_Sphere or PTLS_Polar
    Vec3 off = inRange(P, s.startLocation);
    if (s.shape == 1) {
        Vec3 d;
        do d = Vec3{P.rand() * 2 - 1, P.rand() * 2 - 1, P.rand() * 2 - 1};
        while (dot(d, d) > 1 || dot(d, d) < 1e-6f);
        off = off + d * (P.in(s.sphereRadius) / length(d));
    } else if (s.shape == 2) {
        // angles in degrees about Z and up from it, then a radius
        float a = P.in(s.polar.x) * float(M_PI) / 180, b = P.in(s.polar.y) * float(M_PI) / 180, rr = P.in(s.polar.z);
        off = Vec3{rr * std::sin(b) * std::cos(a), rr * std::sin(b) * std::sin(a), rr * std::cos(b)};
    }
    off = off + s.startOffset;
    Vec3 vel = inRange(P, s.velocity);
    // the emitter's turn applies to them when it rotates its particles, or
    // when they live in its frame
    bool turned = s.rotationFrom == 1 || s.coordinates == 1;
    if (turned) {
        off = rotate(f.ax, off);
        vel = rotate(f.ax, vel);
    }
    // a radial velocity, away from the emitter or towards it
    if (s.velocityFrom != 0 && length(off) > 1e-4f) {
        Vec3 dir = off * (1 / length(off));
        if (s.velocityFrom == 2) dir = -dir;
        Vec3 radial = dir * P.in(s.radialVelocity);
        vel = s.velocityFrom == 3 ? vel + radial : radial;
    }
    if (s.addOwnerVelocity) vel = vel + f.velocity;
    if (s.coordinates == 1) {
        // the owner's frame: kept unturned, turned when drawn
        float inv[3][3];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) inv[r][c] = f.ax[c][r];
        p.loc = rotate(inv, off);
        p.vel = rotate(inv, vel);
    } else {
        p.loc = (s.coordinates == 2 ? Vec3{} : f.origin) + off;
        p.vel = vel;
    }
    Vec3 sz = inRange(P, s.size);
    if (s.uniformSize) sz.y = sz.z = sz.x;
    p.startSize = sz;
    p.colorMul = inRange(P, s.colorMultiplier);
    p.loss = inRange(P, s.velocityLoss);
    p.damping = inRange(P, s.damping);
    if (s.spin) {
        p.spin0 = P.in(s.startSpin.x);
        p.spinRate = P.in(s.spinsPerSecond.x) * (P.rand() < s.spinDirection.x ? 1.0f : -1.0f);
    }
    int frames = s.columns * s.rows;
    int first = std::clamp(s.subStart, 0, frames - 1);
    int last = s.subEnd > 0 ? std::clamp(s.subEnd, first, frames - 1) : frames - 1;
    p.frame = s.randomSub ? first + int(P.rand() * float(last - first + 1)) % (last - first + 1) : first;
    p.maxHits = int(P.in(s.maxCollisions) + 0.5f);
    ++run.spawned;
}

// How many a tick spawns. With AutomaticInitialSpawning as many a second as
// keep MaxParticles alive for their longest life, ParticlesPerSecond and
// InitialParticlesPerSecond not minded: the torches' fire has 0.01 a second
// and is alight. Without it, the first MaxParticles at
// InitialParticlesPerSecond, then ParticlesPerSecond, or the dead respawned
// at once when that is 0. Without RespawnDeadParticles only the first
// MaxParticles come.
int toSpawn(Particles&, EmitterRun& run, const Settings& s, float dt) {
    if (s.disabled || run.killed || s.maxParticles == 0) return 0;
    bool first = run.spawned < s.maxParticles;
    if (!first && !s.respawn) return 0;
    float rate;
    if (s.autoInitial)
        rate = s.lifetime.hi > 0 ? float(s.maxParticles) / s.lifetime.hi : 0;
    else if (first)
        rate = s.initialPps > 0 ? s.initialPps : s.pps;
    else
        rate = s.pps > 0 ? s.pps : -1;
    if (rate < 0) {
        int dead = 0;
        for (const Particle& p : run.ps) dead += !p.alive;
        return dead;
    }
    run.fraction += rate * dt;
    int n = int(run.fraction);
    run.fraction -= float(n);
    if (first && !s.respawn) n = std::min(n, s.maxParticles - run.spawned);
    return n;
}

void step(World& w, Particles& P, EmitterRun& run, const Settings& s, const Basis& f, float dt) {
    if (int(run.ps.size()) != s.maxParticles) run.ps.resize(size_t(s.maxParticles));
    if (run.delay > 0) {
        run.delay -= dt;
        return;
    }
    for (Particle& p : run.ps) {
        if (!p.alive) continue;
        p.time += dt;
        if (p.time >= p.life) {
            p.alive = false;
            continue;
        }
        p.vel = p.vel + s.acceleration * dt;
        p.vel = p.vel - Vec3{p.vel.x * p.loss.x, p.vel.y * p.loss.y, p.vel.z * p.loss.z} * dt;
        if (s.maxVelocity.x > 0) p.vel.x = std::clamp(p.vel.x, -s.maxVelocity.x, s.maxVelocity.x);
        if (s.maxVelocity.y > 0) p.vel.y = std::clamp(p.vel.y, -s.maxVelocity.y, s.maxVelocity.y);
        if (s.maxVelocity.z > 0) p.vel.z = std::clamp(p.vel.z, -s.maxVelocity.z, s.maxVelocity.z);
        Vec3 v = p.vel;
        if (s.useVelocityScale && !s.velocityScale.empty()) {
            float t = p.time / p.life;
            if (s.velocityRepeats > 0) t = std::fmod(t * s.velocityRepeats, 1.0f);
            float k[3] = {1, 1, 1};
            at(s.velocityScale, t, 3, k);
            v = Vec3{v.x * k[0], v.y * k[1], v.z * k[2]};
        }
        Vec3 next = p.loc + v * dt;
        // the world in the way: off it by DampingFactorRange, and gone after
        // MaxCollisions
        if (s.collide && s.coordinates != 1 && w.collision) {
            TraceHit h = w.collision->lineCheck(p.loc, next, run.actor, false, true);
            if (h) {
                Vec3 n = h.normal;
                Vec3 r = p.vel - n * (2 * dot(p.vel, n));
                p.vel = Vec3{r.x * p.damping.x, r.y * p.damping.y, r.z * p.damping.z};
                next = h.location + n * 0.5f;
                if (s.useMaxCollisions && ++p.hits >= std::max(1, p.maxHits)) p.alive = false;
            }
        }
        p.loc = next;
    }
    for (int n = toSpawn(P, run, s, dt); n > 0; --n) spawnOne(P, run, s, f);
    if (run.deferred > 0) {
        float rate = run.deferredRate > 0 ? run.deferredRate : 1e9f;
        int n = std::min(run.deferred, std::max(1, int(rate * dt)));
        run.deferred -= n;
        for (; n > 0; --n) spawnOne(P, run, s, f);
    }
}

void start(World& w, Particles& P, EmitterRun& run, Object* e, const Settings& s, const Basis& f) {
    run.started = true;
    run.delay = P.in(s.initialDelay);
    // RelativeWarmupTime of the longest life, run before it is seen, at
    // WarmupTicksPerSecond
    float warm = pf(e, "RelativeWarmupTime") * s.lifetime.hi;
    if (warm > 0) {
        float tps = pf(e, "WarmupTicksPerSecond");
        float dt = tps > 0 ? 1 / tps : warm;
        for (float t = 0; t < warm; t += dt) step(w, P, run, s, f, dt);
    }
}

// Sprites of one emitter for the renderer.
void gather(World&, EmitterRun& run, const Settings& s, const Basis& f, ParticleDraw& d) {
    d.texture = s.texture;
    d.style = s.style;
    d.columns = s.columns;
    d.rows = s.rows;
    d.facing = s.facing;
    d.normal = s.normal;
    d.zWrite = s.zWrite;
    d.zTest = s.zTest;
    for (const Particle& p : run.ps) {
        if (!p.alive) continue;
        float t = p.time / p.life;
        ParticleSprite sp;
        sp.at = s.coordinates == 1 ? f.origin + rotate(f.ax, p.loc) : p.loc;
        float k = 1;
        if (s.useSize && !s.sizeScale.empty()) {
            float ts = s.sizeRepeats > 0 ? std::fmod(t * s.sizeRepeats, 1.0f) : t;
            at(s.sizeScale, ts, 1, &k);
        }
        sp.width = p.startSize.x * k;
        sp.height = p.startSize.y * k;
        sp.spin = s.spin ? (p.spin0 + p.spinRate * p.time) * 2 * float(M_PI) : 0;
        float c[4] = {1, 1, 1, 1};
        if (s.useColor && !s.colorScale.empty()) {
            float tc = s.colorRepeats > 0 ? std::fmod(t * s.colorRepeats, 1.0f) : t;
            at(s.colorScale, tc, 4, c);
        }
        c[0] *= p.colorMul.x;
        c[1] *= p.colorMul.y;
        c[2] *= p.colorMul.z;
        // fading in to FadeInEndTime and out from FadeOutStartTime, each
        // channel by its factor
        if (s.fadeIn && s.fadeInEnd > 0 && p.time < s.fadeInEnd) {
            float u = p.time / s.fadeInEnd;
            for (int i = 0; i < 4; ++i) c[i] *= 1 - (1 - u) * s.fadeInFactor[i];
        }
        if (s.fadeOut && p.time > s.fadeOutStart && p.life > s.fadeOutStart) {
            float u = (p.time - s.fadeOutStart) / (p.life - s.fadeOutStart);
            for (int i = 0; i < 4; ++i) c[i] *= 1 - u * s.fadeOutFactor[i];
        }
        c[3] *= s.opacity;
        for (int i = 0; i < 4; ++i) sp.color[i] = std::clamp(c[i], 0.0f, 1.0f);
        int frames = s.columns * s.rows;
        if (s.randomSub || frames <= 1) {
            sp.frame = p.frame;
        } else {
            int first = std::clamp(s.subStart, 0, frames - 1);
            int last = s.subEnd > 0 ? std::clamp(s.subEnd, first, frames - 1) : frames - 1;
            sp.frame = std::min(last, first + int(t * float(last - first + 1)));
        }
        Vec3 v = s.coordinates == 1 ? rotate(f.ax, p.vel) : p.vel;
        float len = length(v);
        sp.up = len > 1e-4f ? v * (1 / len) : Vec3{0, 0, 1};
        d.sprites.push_back(sp);
    }
}

bool done(const EmitterRun& run, const Settings& s) {
    for (const Particle& p : run.ps)
        if (p.alive) return false;
    if (run.deferred > 0) return false;
    if (run.killed || s.disabled || s.maxParticles == 0) return true;
    return !s.respawn && run.spawned >= s.maxParticles;
}

std::vector<Object*> emittersOf(World& w, Object* actor) {
    std::vector<Object*> out;
    const Value& v = prop(actor, "Emitters");
    if (v.isArr())
        for (const Value& e : v.arr())
            if (Object* o = e.o(); o && !o->deleted) out.push_back(o);
    (void)w;
    return out;
}

void reset(EmitterRun& run) {
    for (Particle& p : run.ps) p.alive = false;
    run.spawned = 0;
    run.fraction = 0;
}

}  // namespace

void giveEmitters(World& w, Object* actor) {
    Class* ec = emitterClass(w);
    if (!ec || !actor->isA(ec)) return;
    Prop* p = actor->cls->findProp(Name("Emitters"));
    if (!p) return;
    Value& v = actor->props[size_t(p->slot)];
    if (!v.isArr()) return;
    for (Value& e : v.arr()) {
        Object* t = e.o();
        if (!t || t->outer == actor) continue;
        Object* c = w.vm.spawn(t->cls, Name(), actor->outer);
        c->props = t->props;
        e = Value::Obj(c);
    }
}

void particlesTick(World& w, float dt) {
    Particles& P = particles(w);
    P.draws.clear();
    Class* ec = emitterClass(w);
    if (!ec) return;
    std::vector<Object*> finished;
    for (size_t i = 0; i < w.actors.size(); ++i) {
        Object* a = w.actors[i];
        if (a->deleted || !a->isA(ec)) continue;
        Basis f = frameOf(w, a);
        bool all = true, any = false;
        for (Object* e : emittersOf(w, a)) {
            if (!e->cls->findProp(Name("MaxParticles"))) continue;
            any = true;
            Settings s = settingsOf(e);
            EmitterRun& run = P.runs[e];
            run.actor = a;
            if (!run.started) start(w, P, run, e, s, f);
            step(w, P, run, s, f, dt);
            all = all && done(run, s);
            // only SpriteEmitters are drawn yet
            if (!w.flag(a, "bHidden") && e->cls->findProp(Name("UseDirectionAs"))) {
                ParticleDraw d;
                d.actor = a;
                gather(w, run, s, f, d);
                if (!d.sprites.empty()) P.draws.push_back(std::move(d));
            }
        }
        if (any && all && w.flag(a, "AutoDestroy")) finished.push_back(a);
    }
    for (Object* a : finished) w.destroy(a);
    // the runs of emitters gone with their actors
    for (auto it = P.runs.begin(); it != P.runs.end();)
        it = it->second.actor && it->second.actor->deleted ? P.runs.erase(it) : std::next(it);
}

const std::vector<ParticleDraw>& particleDraws(World& w) { return particles(w).draws; }

void registerParticleNatives(VM& vm) {
    auto& n = vm.natives;
    // ParticleEmitter.Trigger: Disabled toggled with TriggerDisabled, the
    // particles begun again with ResetOnTrigger, and SpawnOnTriggerRange's
    // spawned at SpawnOnTriggerPPS.
    n["particleemitter.trigger"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value();
        Particles& P = particles(*w);
        Object* e = c.self;
        EmitterRun& run = P.runs[e];
        if (pb(e, "TriggerDisabled")) {
            Prop* d = e->cls->findProp(Name("Disabled"));
            if (d) e->props[size_t(d->slot)] = Value::Bool(!pb(e, "Disabled"));
        }
        if (pb(e, "ResetOnTrigger")) reset(run);
        Range r = range(prop(e, "SpawnOnTriggerRange"));
        if (r.hi > 0) {
            run.deferred += int(P.in(r) + 0.5f);
            run.deferredRate = pf(e, "SpawnOnTriggerPPS");
        }
        return Value();
    };
    // SpawnParticle(Amount): that many at once
    n["particleemitter.spawnparticle"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value();
        Particles& P = particles(*w);
        EmitterRun& run = P.runs[c.self];
        run.deferred += std::max(0, c.i(0));
        run.deferredRate = 0;
        return Value();
    };
    // Kill: no more spawned, and the Emitter gone once its particles are
    n["emitter.kill"] = [](NativeCall& c) {
        World* w = World::of(c.vm);
        if (!w) return Value();
        Particles& P = particles(*w);
        for (Object* e : emittersOf(*w, c.self)) P.runs[e].killed = true;
        if (Prop* p = c.self->cls->findProp(Name("AutoDestroy"))) c.self->props[size_t(p->slot)] = Value::Bool(true);
        return Value();
    };
}

}  // namespace ffa
