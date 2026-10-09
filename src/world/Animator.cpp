#include "world/Animator.h"

#include <algorithm>
#include <cmath>
#include <strings.h>

namespace ffa {

Animator::Animator(World& w, Library& lib) : world(w), library(lib) { w.animator = this; }

Animator* Animator::of(VM& vm) {
    World* w = World::of(vm);
    return w ? w->animator : nullptr;
}

ObjectRef Animator::refOf(const Object* o, const char* cls) {
    if (!o) return {};
    std::vector<std::string> parts;
    for (const Object* k = o; k; k = k->outer) parts.insert(parts.begin(), k->name.str());
    if (parts.size() < 2) return {};
    const Package* p = library.package(parts[0]);
    if (!p) return {};
    int idx = Library::findByPath(*p, std::vector<std::string>(parts.begin() + 1, parts.end()), cls);
    return idx ? ObjectRef{p, idx} : ObjectRef{};
}

const MeshAnimation* Animator::animation(const ObjectRef& r) {
    if (!r || r.cls() != "MeshAnimation") return nullptr;
    auto key = std::make_pair(r.pkg, r.idx);
    auto it = anims_.find(key);
    if (it != anims_.end()) return it->second.get();
    std::unique_ptr<MeshAnimation>& slot = anims_[key];
    try {
        slot = std::make_unique<MeshAnimation>(*r.pkg, r.idx);
    } catch (const FormatError&) {
    }
    return slot.get();
}

Object* Animator::objectFor(const MeshAnimation* m) {
    if (!m || !world.vm.loadObject) return nullptr;
    std::string path;
    for (int k = m->index; k > 0; k = m->package->exp(k).outer)
        path = m->package->exp(k).name + (path.empty() ? "" : "." + path);
    return world.vm.loadObject(m->package->stem + "." + path, world.vm.linker.findClass("MeshAnimation"));
}

const SkeletalMesh* Animator::skeletal(const ObjectRef& r) {
    if (!r) return nullptr;
    auto key = std::make_pair(r.pkg, r.idx);
    auto it = meshes_.find(key);
    if (it != meshes_.end()) return it->second.get();
    std::unique_ptr<SkeletalMesh>& slot = meshes_[key];
    try {
        slot = std::make_unique<SkeletalMesh>(*r.pkg, r.idx);
    } catch (const FormatError&) {
    }
    return slot.get();
}

std::vector<BoneTransform> Animator::pose(Object* a) {
    AnimState& s = state(a);
    if (!s.mesh) return {};
    const SkeletalMesh& m = *s.mesh;
    auto at = [&](const AnimChannel& c) {
        std::vector<BoneTransform> now = channelLocals(s, c);
        if (c.blendLeft > 0 && c.blendTime > 0 && c.from.size() == now.size()) {
            float t = 1 - c.blendLeft / c.blendTime;
            for (size_t i = 0; i < now.size(); ++i) {
                now[i].q = qnlerp(c.from[i].q, now[i].q, t);
                now[i].p = lerp(c.from[i].p, now[i].p, t);
            }
        }
        return now;
    };
    std::vector<BoneTransform> locals = m.referenceLocals();
    for (size_t k = 0; k < s.channels.size(); ++k) {
        const AnimChannel& c = s.channels[k];
        if (!c.seq || !c.set) continue;
        float alpha = k == 0 ? 1.0f : c.alpha;
        if (alpha <= 0) continue;
        std::vector<BoneTransform> ch = at(c);
        // from the blend bone down, or the whole skeleton
        int root = c.bone.empty() || c.bone == "None" ? -1 : m.bone(c.bone);
        for (size_t i = 0; i < locals.size(); ++i) {
            bool under = root < 0;
            for (int j = int(i); !under && j >= 0; j = j == 0 ? -1 : m.bones[size_t(j)].parent)
                if (j == root) under = true;
            if (!under) continue;
            locals[i].q = qnlerp(locals[i].q, ch[i].q, alpha);
            locals[i].p = lerp(locals[i].p, ch[i].p, alpha);
        }
    }
    return m.compose(locals);
}

std::vector<BoneTransform> Animator::channelLocals(const AnimState& s, const AnimChannel& c) const {
    if (!s.mesh || !c.seq || !c.set) return s.mesh ? s.mesh->referenceLocals() : std::vector<BoneTransform>{};
    size_t si = size_t(c.seq - c.set->sequences.data());
    float frames = float(c.seq->numFrames);
    return s.mesh->locals(*c.set, si, c.frame * frames, c.looping, frames);
}

void Animator::startBlend(Object* a, int k, float time) {
    AnimState& s = state(a);
    AnimChannel& c = channel(a, k);
    if (time <= 0 || !c.seq || !s.mesh) {
        c.blendLeft = c.blendTime = 0;
        c.from.clear();
        return;
    }
    // from where the channel is, blend included, so that a blend cut short
    // does not jump either
    std::vector<BoneTransform> now = channelLocals(s, c);
    if (c.blendLeft > 0 && c.blendTime > 0 && c.from.size() == now.size()) {
        float t = 1 - c.blendLeft / c.blendTime;
        for (size_t i = 0; i < now.size(); ++i) {
            now[i].q = qnlerp(c.from[i].q, now[i].q, t);
            now[i].p = lerp(c.from[i].p, now[i].p, t);
        }
    }
    c.from = std::move(now);
    c.blendTime = c.blendLeft = time;
}

void Animator::meshToWorld(Object* a, float out[3][3], Vec3& origin) {
    // Location + R S, as the viewer places skeletal meshes; PrePivot is not
    // settled for them and is left out.
    int32_t pitch, yaw, roll;
    world.vm.unrotator(world.var(a, "Rotation"), pitch, yaw, roll);
    float ax[3][3];
    rotationAxes(pitch, yaw, roll, ax);
    float sc = world.var(a, "DrawScale").f();
    Vec3 s3;
    world.vm.unvector(world.var(a, "DrawScale3D"), s3.x, s3.y, s3.z);
    const float k[3] = {sc * s3.x, sc * s3.y, sc * s3.z};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r][c] = ax[c][r] * k[c];
    world.vm.unvector(world.var(a, "Location"), origin.x, origin.y, origin.z);
}

bool Animator::boneWorld(Object* a, const std::string& bone, Vec3& origin, Vec3 axes[3]) {
    AnimState& s = state(a);
    if (!s.mesh) return false;
    int b = s.mesh->bone(bone);
    if (b < 0) return false;
    std::vector<BoneTransform> g = pose(a);
    float m[3][3];
    Vec3 loc;
    meshToWorld(a, m, loc);
    auto toWorld = [&](Vec3 v) {
        return Vec3{m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                    m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    };
    const SkeletalMesh& mesh = *s.mesh;
    origin = loc + toWorld(mesh.toActor(g[size_t(b)].p));
    // the axes go through the mesh's rotation and the actor's, not its offset
    const Vec3 unit[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int k = 0; k < 3; ++k) {
        Vec3 d = toWorld(mesh.toActor(mesh.origin + qrot(g[size_t(b)].q, unit[k])) - mesh.toActor(mesh.origin));
        float len = length(d);
        axes[k] = len > 0 ? d * (1 / len) : d;
    }
    return true;
}

// A rotator whose axes are X, Y and Z, the inverse of rotationAxes.
void rotatorOf(const Vec3 axes[3], int32_t out[3]) {
    const Vec3& X = axes[0];
    const Vec3& Y = axes[1];
    const Vec3& Z = axes[2];
    float pitch = std::atan2(X.z, std::sqrt(X.x * X.x + X.y * X.y));
    float yaw = std::atan2(X.y, X.x);
    // the Y axis of pitch and yaw alone, then the roll that turns it to Y
    float sy = std::sin(yaw), cy = std::cos(yaw);
    Vec3 y0{-sy, cy, 0};
    Vec3 z0 = cross(X, y0);
    float roll = std::atan2(-dot(Y, z0), dot(Y, y0));
    (void)Z;
    const float k = 32768.0f / 3.14159265f;
    out[0] = int32_t(std::lround(pitch * k)) & 0xFFFF;
    out[1] = int32_t(std::lround(yaw * k)) & 0xFFFF;
    out[2] = int32_t(std::lround(roll * k)) & 0xFFFF;
}

void Animator::attachments() {
    for (Object* a : world.actors) {
        if (a->deleted) continue;
        Name bone = world.var(a, "AttachmentBone").n();
        Object* base = world.obj(a, "Base");
        if (bone.isNone() || !base || base->deleted) continue;
        Vec3 o, ax[3];
        if (!boneWorld(base, bone.str(), o, ax)) continue;
        Vec3 rel;
        world.vm.unvector(world.var(a, "RelativeLocation"), rel.x, rel.y, rel.z);
        int32_t rr[3];
        world.vm.unrotator(world.var(a, "RelativeRotation"), rr[0], rr[1], rr[2]);
        Vec3 at = o + ax[0] * rel.x + ax[1] * rel.y + ax[2] * rel.z;
        // the attachment's axes: its relative rotation within the bone's
        float r[3][3];
        rotationAxes(rr[0], rr[1], rr[2], r);
        Vec3 axes[3];
        for (int k = 0; k < 3; ++k) axes[k] = ax[0] * r[k][0] + ax[1] * r[k][1] + ax[2] * r[k][2];
        int32_t rot[3];
        rotatorOf(axes, rot);
        world.var(a, "Location") = world.vm.vector(at.x, at.y, at.z);
        world.var(a, "Rotation") = world.vm.rotator(rot[0], rot[1], rot[2]);
    }
}

const MeshAnimation* Animator::animation(const Object* o) { return animation(refOf(o, "MeshAnimation")); }

void Animator::resolve(Object* a, AnimState& s) {
    s.resolved = true;
    // the mesh's default animation, named after its reference skeleton
    ObjectRef mesh = refOf(world.obj(a, "Mesh"), "SkeletalMesh");
    if (mesh && mesh.cls() == "SkeletalMesh") {
        s.mesh = skeletal(mesh);
        int32_t ref = s.mesh ? s.mesh->defaultAnim : skeletalDefaultAnim(*mesh.pkg, mesh.idx);
        if (ref) s.defaults = animation(library.resolve(*mesh.pkg, ref));
    }
}

AnimState& Animator::state(Object* a) {
    AnimState& s = states_[a];
    if (!s.resolved) {
        resolve(a, s);
        // Nothing in script calls Actor.AddAnimNotifys but its overrides'
        // supers, yet Shrek's footsteps are added there: the engine sends it,
        // and here that is when an actor's animation is first set up.
        if (s.defaults) world.vm.event(a, "AddAnimNotifys");
    }
    return states_[a];
}

AnimChannel& Animator::channel(Object* a, int k) {
    AnimState& s = state(a);
    k = std::clamp(k, 0, 63);
    if (s.channels.size() <= size_t(k)) {
        size_t was = s.channels.size();
        s.channels.resize(size_t(k) + 1);
        // channel 0 notifies unless told not to, and is the base pose; a
        // channel above it adds nothing until AnimBlendParams gives it an
        // alpha. KWPawn plays its blinks on channels 34 to 39 with their lid
        // and brow bones set that way; with an alpha of 1 they took the whole
        // skeleton to the reference pose the blink leaves it in.
        for (size_t i = was; i < s.channels.size(); ++i) s.channels[i].alpha = s.channels[i].alphaTarget = i == 0;
        if (was == 0) s.channels[0].notify = true;
    }
    return s.channels[size_t(k)];
}

const AnimSequence* Animator::find(Object* a, const std::string& name, const MeshAnimation** set) {
    AnimState& s = state(a);
    for (const MeshAnimation* m : s.linked)
        if (const AnimSequence* q = m->find(name)) {
            if (set) *set = m;
            return q;
        }
    if (s.defaults)
        if (const AnimSequence* q = s.defaults->find(name)) {
            if (set) *set = s.defaults;
            return q;
        }
    return nullptr;
}

void Animator::tick(float dt) {
    // Script called from here may start sequences, add channels and notifies,
    // and give other actors states, so every element is found again by its
    // index after each call out.
    for (auto it = states_.begin(); it != states_.end(); ++it) {
        Object* a = it->first;
        AnimState& s = it->second;
        if (!a->deleted && world.pawnClass && a->isA(world.pawnClass)) movement(a);
        for (size_t k = 0; k < s.channels.size() && !a->deleted; ++k) {
            {
                AnimChannel& c = s.channels[k];
                if (c.blendLeft > 0) {
                    c.blendLeft -= dt;
                    if (c.blendLeft <= 0) {
                        c.blendLeft = 0;
                        c.from.clear();
                    }
                }
                if (c.alphaRate != 0) {
                    c.alpha += c.alphaRate * dt;
                    if ((c.alphaRate > 0 && c.alpha >= c.alphaTarget) || (c.alphaRate < 0 && c.alpha <= c.alphaTarget)) {
                        c.alpha = c.alphaTarget;
                        c.alphaRate = 0;
                    }
                }
                if (!s.channels[k].animating) continue;
            }
            bool ended = false;
            if (s.channels[k].tween > 0) {
                AnimChannel& c = s.channels[k];
                c.tween -= dt;
                if (c.tween > 0) continue;
                c.tween = 0;
                if (c.rate == 0) {
                    c.animating = false;
                    ended = true;
                }
            } else {
                float old = s.channels[k].frame;
                float now = old + s.channels[k].rate * dt;
                s.channels[k].frame = now;
                const AnimSequence* seq = s.channels[k].seq;
                // the notifies the frame passes on this channel's sequence
                for (size_t i = 0; i < s.added.size() && !a->deleted; ++i)
                    if (s.added[i].seq == seq && s.added[i].frame > old && s.added[i].frame <= now) {
                        std::string event = s.added[i].event;
                        s.notifyChannel = int(k);
                        world.vm.event(a, event);
                        ++notifiesSent;
                    }
                if (a->deleted || k >= s.channels.size()) break;
                AnimChannel& c = s.channels[k];
                if (c.seq == seq && c.frame >= 1) {
                    ended = true;
                    if (c.looping && !c.stopAtEnd) {
                        c.frame -= std::floor(c.frame);
                    } else {
                        c.frame = 1;
                        c.animating = false;
                    }
                }
            }
            if (ended && s.channels[k].notify) {
                s.notifyChannel = int(k);
                world.vm.event(a, "AnimEnd", {Value::Int(int32_t(k))});
                ++animEnds;
            }
        }
    }
}

void Animator::movement(Object* a) {
    World& w = world;
    if (!w.flag(a, "bPhysicsAnimUpdate") || w.var(a, "Physics").i() != 1) return;
    Vec3 v;
    w.vm.unvector(w.var(a, "Velocity"), v.x, v.y, v.z);
    float speed = std::sqrt(v.x * v.x + v.y * v.y);
    Prop* mp = a->cls->findProp(Name("MovementAnims"));
    if (!mp) return;
    AnimChannel& ch = channel(a, 0);
    auto isMovement = [&](const std::string& name) {
        for (int k = 0; k < mp->dim && k < 4; ++k)
            if (strcasecmp(a->props[size_t(mp->slot + k)].n().str().c_str(), name.c_str()) == 0) return true;
        return false;
    };
    float blend = w.var(a, "BlendChangeTime").f();
    auto loop = [&](const std::string& name) {
        const MeshAnimation* set = nullptr;
        const AnimSequence* seq = find(a, name, &set);
        if (!seq || (ch.seq == seq && ch.looping && ch.animating)) return;
        startBlend(a, 0, blend);
        AnimChannel& ch = channel(a, 0);
        ch.seq = seq;
        ch.set = set;
        ch.name = seq->name;
        ch.rate = seq->numFrames > 0 ? seq->rate / float(seq->numFrames) : 0;
        ch.frame = 0;
        ch.tween = 0;
        ch.looping = true;
        ch.stopAtEnd = false;
        ch.animating = true;
        ++sequencesPlayed;
    };
    if (speed > 10) {
        int32_t pitch, yaw, roll;
        w.vm.unrotator(w.var(a, "Rotation"), pitch, yaw, roll);
        float ax[3][3];
        rotationAxes(0, yaw, 0, ax);
        float fwd = v.x * ax[0][0] + v.y * ax[0][1], right = v.x * ax[1][0] + v.y * ax[1][1];
        int dir = std::fabs(fwd) >= std::fabs(right) ? (fwd >= 0 ? 0 : 1) : (right < 0 ? 2 : 3);
        std::string want = a->props[size_t(mp->slot + std::min(dir, mp->dim - 1))].n().str();
        if (!want.empty() && want != "None") loop(want);
    } else if (ch.seq && isMovement(ch.name)) {
        Prop* ip = a->cls->findProp(Name("IdleAnimName"));
        if (ip) loop(a->props[size_t(ip->slot)].n().str());
    }
}

// ================================================================ natives
namespace {

Animator& animator(NativeCall& c) {
    Animator* an = Animator::of(c.vm);
    if (!an) throw c.vm.error("native " + c.fn->qualname() + " needs a level's animator");
    return *an;
}

// Start a sequence on a channel: from its first frame, at rate times its
// frames a second over its frame count.
void play(NativeCall& c, bool loop) {
    Animator& an = animator(c);
    std::string name = c.n(0).str();
    float rate = c.has(1) ? c.f(1) : 1.0f, tween = c.f(2);
    int k = c.i(3);
    AnimChannel& ch = an.channel(c.self, k);
    const MeshAnimation* set = nullptr;
    const AnimSequence* seq = an.find(c.self, name, &set);
    if (!seq) {
        ++an.notFound;
        an.missing[c.self->cls->name.str() + "." + name]++;
        return;
    }
    // looping a sequence that already loops only changes its rate
    bool same = ch.seq == seq && ch.looping && ch.animating && loop;
    if (!same) an.startBlend(c.self, k, tween);
    AnimChannel& chn = an.channel(c.self, k);
    (void)ch;
    AnimChannel& ch2 = chn;
    ch2.seq = seq;
    ch2.set = set;
    ch2.name = seq->name;
    ch2.rate = seq->numFrames > 0 ? rate * seq->rate / float(seq->numFrames) : 0;
    if (!same) {
        ch2.frame = 0;
        ch2.tween = 0;
    }
    ch2.looping = loop;
    ch2.stopAtEnd = false;
    ch2.animating = true;
    ++an.sequencesPlayed;
}

}  // namespace

void registerAnimationNatives(VM& vm) {
    auto& n = vm.natives;
    n["actor.playanim"] = [](NativeCall& c) {
        play(c, false);
        return Value();
    };
    n["actor.loopanim"] = [](NativeCall& c) {
        play(c, true);
        return Value();
    };
    n["actor.tweenanim"] = [](NativeCall& c) {
        Animator& an = animator(c);
        AnimChannel& ch = an.channel(c.self, c.i(2));
        const MeshAnimation* set = nullptr;
        const AnimSequence* seq = an.find(c.self, c.n(0).str(), &set);
        if (!seq) {
            ++an.notFound;
            return Value();
        }
        ch.seq = seq;
        ch.set = set;
        ch.name = seq->name;
        ch.frame = 0;
        ch.rate = 0;
        ch.looping = false;
        ch.tween = c.f(1);
        ch.animating = ch.tween > 0;
        return Value();
    };
    // A controller has no animation of its own: it waits on its pawn's.
    // KnowWonder's BounceController plays its pawn's idle and finishes it in
    // a loop, Pawn.PlayAnim(IdleAnim); FinishAnim(); goto 'Begin', which could
    // only ever wait that way.
    n["actor.finishanim"] = [](NativeCall& c) {
        Animator& an = animator(c);
        int k = c.i(0);
        Object* self = c.self;
        Object* target = self;
        if (an.world.controllerClass && self->isA(an.world.controllerClass))
            if (Object* pawn = an.world.obj(self, "Pawn")) target = pawn;
        AnimChannel& ch = an.channel(target, k);
        if (!ch.animating) return Value();
        ch.stopAtEnd = true;
        Animator* ap = &an;
        self->latent = [ap, target, k](float) { return target->deleted || !ap->channel(target, k).animating; };
        return Value();
    };
    n["actor.isanimating"] = [](NativeCall& c) {
        return Value::Bool(animator(c).channel(c.self, c.i(0)).animating);
    };
    n["actor.hasanim"] = [](NativeCall& c) { return Value::Bool(animator(c).find(c.self, c.n(0).str()) != nullptr); };
    n["actor.getanimsequence"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(0));
        return Value::Nm(ch.seq ? Name(ch.name) : Name());
    };
    n["actor.getanimparams"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(0));
        c.out(1, Value::Nm(ch.seq ? Name(ch.name) : Name()));
        c.out(2, Value::Float(ch.frame));
        c.out(3, Value::Float(ch.rate));
        return Value();
    };
    n["actor.getanimframe"] = [](NativeCall& c) { return Value::Float(animator(c).channel(c.self, c.i(0)).frame); };
    n["actor.animisingroup"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(0));
        std::string g = c.n(1).str();
        if (ch.seq)
            for (const std::string& x : ch.seq->groups)
                if (strcasecmp(x.c_str(), g.c_str()) == 0) return Value::Bool(true);
        return Value::Bool(false);
    };
    n["actor.stopanimating"] = [](NativeCall& c) {
        AnimState& s = animator(c).state(c.self);
        for (size_t k = c.b(0) ? 1 : 0; k < s.channels.size(); ++k) s.channels[k].animating = false;
        return Value();
    };
    n["actor.freezeanimat"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(1));
        ch.frame = std::clamp(c.f(0), 0.0f, 1.0f);
        ch.animating = false;
        return Value();
    };
    n["actor.setanimframe"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(1));
        float f = c.f(0);
        // UnitFlag 1 counts in frames
        if (c.i(2) == 1 && ch.seq && ch.seq->numFrames > 0) f /= float(ch.seq->numFrames);
        ch.frame = std::clamp(f, 0.0f, 1.0f);
        return Value();
    };
    n["actor.animblendparams"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(0));
        ch.alpha = ch.alphaTarget = c.f(1);
        ch.alphaRate = 0;
        ch.bone = c.n(4).str();
        return Value();
    };
    n["actor.animblendtoalpha"] = [](NativeCall& c) {
        AnimChannel& ch = animator(c).channel(c.self, c.i(0));
        float target = c.f(1), time = c.f(2);
        ch.alphaTarget = target;
        if (time <= 0) {
            ch.alpha = target;
            ch.alphaRate = 0;
        } else {
            ch.alphaRate = (target - ch.alpha) / time;
        }
        return Value();
    };
    n["actor.enablechannelnotify"] = [](NativeCall& c) {
        animator(c).channel(c.self, c.i(0)).notify = c.i(1) != 0;
        return Value();
    };
    n["actor.getnotifychannel"] = [](NativeCall& c) { return Value::Int(animator(c).state(c.self).notifyChannel); };
    n["actor.linkskelanim"] = [](NativeCall& c) {
        Animator& an = animator(c);
        AnimState& s = an.state(c.self);
        if (const MeshAnimation* m = an.animation(c.o(0))) {
            s.linked.erase(std::remove(s.linked.begin(), s.linked.end(), m), s.linked.end());
            s.linked.insert(s.linked.begin(), m);
        }
        return Value();
    };
    // MakeSkins: the actor's Skins filled from its mesh's materials where they
    // are none, so that script can read them, as Knight keeps Skins[1] to put
    // back after its freeze.
    n["actor.makeskins"] = [](NativeCall& c) {
        Animator& an = animator(c);
        const SkeletalMesh* m = an.state(c.self).mesh;
        if (!m || !an.world.vm.loadObject) return Value();
        Value& skins = an.world.var(c.self, "Skins");
        if (!skins.isArr()) skins = Value::Arr({});
        Array& arr = skins.arr();
        if (arr.size() < m->materials.size()) arr.resize(m->materials.size(), Value::Obj(nullptr));
        for (size_t i = 0; i < m->materials.size(); ++i) {
            if (arr[i].o()) continue;
            ObjectRef r = an.library.resolve(*m->package, m->materials[i]);
            if (!r) continue;
            std::string path;
            for (int k = r.idx; k > 0; k = r.pkg->exp(k).outer)
                path = r.pkg->exp(k).name + (path.empty() ? "" : "." + path);
            arr[i] = Value::Obj(an.world.vm.loadObject(r.pkg->stem + "." + path, nullptr));
        }
        return Value();
    };
    n["actor.linkmesh"] = [](NativeCall& c) {
        Animator& an = animator(c);
        an.world.var(c.self, "Mesh") = Value::Obj(c.o(0));
        AnimState& s = an.state(c.self);
        bool keep = c.b(1);
        s.resolved = false;
        if (!keep) s.linked.clear();
        an.state(c.self);
        return Value();
    };
    // AddNotify(AnimSet, Sequence, Frame, EventName): Frame counts in frames.
    // GetBoneCoords: Coords of Origin and X, Y, Z axes, in the world.
    n["actor.getbonecoords"] = [](NativeCall& c) {
        Animator& an = animator(c);
        Vec3 o, ax[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        if (!an.boneWorld(c.self, c.n(0).str(), o, ax)) c.vm.unvector(an.world.var(c.self, "Location"), o.x, o.y, o.z);
        StructType* st = c.vm.linker.findStruct("Coords");
        if (!st) return Value();
        Value v = st->make();
        const char* names[4] = {"Origin", "XAxis", "YAxis", "ZAxis"};
        const Vec3 vals[4] = {o, ax[0], ax[1], ax[2]};
        for (int k = 0; k < 4; ++k)
            if (Prop* f = st->field(Name(names[k]))) v.st().f[size_t(f->slot)] = c.vm.vector(vals[k].x, vals[k].y, vals[k].z);
        return v;
    };
    n["actor.getbonerotation"] = [](NativeCall& c) {
        Animator& an = animator(c);
        Vec3 o, ax[3];
        if (!an.boneWorld(c.self, c.n(0).str(), o, ax)) return an.world.var(c.self, "Rotation");
        int32_t r[3];
        rotatorOf(ax, r);
        return c.vm.rotator(r[0], r[1], r[2]);
    };
    // AttachToBone(Attachment, BoneName): it follows the bone from now on, at
    // its RelativeLocation and RelativeRotation, with this actor its Base.
    n["actor.attachtobone"] = [](NativeCall& c) {
        Animator& an = animator(c);
        Object* att = c.o(0);
        if (!att || an.state(c.self).mesh == nullptr || an.state(c.self).mesh->bone(c.n(1).str()) < 0)
            return Value::Bool(false);
        an.world.var(att, "AttachmentBone") = Value::Nm(c.n(1));
        an.world.var(att, "Base") = Value::Obj(c.self);
        an.attachments();
        return Value::Bool(true);
    };
    n["actor.detachfrombone"] = [](NativeCall& c) {
        Animator& an = animator(c);
        Object* att = c.o(0);
        if (!att || an.world.obj(att, "Base") != c.self) return Value::Bool(false);
        an.world.var(att, "AttachmentBone") = Value::Nm(Name());
        an.world.var(att, "Base") = Value::Obj(nullptr);
        return Value::Bool(true);
    };
    n["actor.setrelativelocation"] = [](NativeCall& c) {
        animator(c).world.var(c.self, "RelativeLocation") = c.get(0);
        return Value::Bool(true);
    };
    n["actor.setrelativerotation"] = [](NativeCall& c) {
        animator(c).world.var(c.self, "RelativeRotation") = c.get(0);
        return Value::Bool(true);
    };
    // GetRenderBoundingSphere: a Plane of the centre and the radius, in the
    // world; for a skeletal mesh around its posed points, else its collision
    // cylinder's.
    n["actor.getrenderboundingsphere"] = [](NativeCall& c) {
        Animator& an = animator(c);
        World& w = an.world;
        Vec3 centre;
        w.vm.unvector(w.var(c.self, "Location"), centre.x, centre.y, centre.z);
        float radius = std::max(w.var(c.self, "CollisionRadius").f(), w.var(c.self, "CollisionHeight").f());
        if (const SkeletalMesh* m = an.state(c.self).mesh) {
            float r[3][3];
            Vec3 loc;
            an.meshToWorld(c.self, r, loc);
            Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const Vec3& p0 : m->points) {
                Vec3 p = m->toActor(p0);
                Vec3 q{loc.x + r[0][0] * p.x + r[0][1] * p.y + r[0][2] * p.z, loc.y + r[1][0] * p.x + r[1][1] * p.y + r[1][2] * p.z,
                       loc.z + r[2][0] * p.x + r[2][1] * p.y + r[2][2] * p.z};
                lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
                hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
            }
            if (!m->points.empty()) {
                centre = (lo + hi) * 0.5f;
                radius = length(hi - lo) * 0.5f;
            }
        }
        StructType* st = c.vm.linker.findStruct("Plane");
        if (!st) return Value();
        Value v = st->make();
        const char* names[4] = {"X", "Y", "Z", "W"};
        const float vals[4] = {centre.x, centre.y, centre.z, radius};
        for (int k = 0; k < 4; ++k)
            if (Prop* f = st->field(Name(names[k]))) v.st().f[size_t(f->slot)] = Value::Float(vals[k]);
        return v;
    };
    // The MeshAnimation of the actor's that holds a sequence.
    n["actor.getanimobjectbyname"] = [](NativeCall& c) {
        Animator& an = animator(c);
        const MeshAnimation* set = nullptr;
        if (!an.find(c.self, c.n(0).str(), &set) || !set) return Value::Obj(nullptr);
        return Value::Obj(an.objectFor(set));
    };
    n["actor.addnotify"] = [](NativeCall& c) {
        Animator& an = animator(c);
        const MeshAnimation* m = an.animation(c.o(0));
        const AnimSequence* seq = m ? m->find(c.n(1).str()) : an.find(c.self, c.n(1).str());
        if (!seq || seq->numFrames <= 0) return Value();
        AnimState& s = an.state(c.self);
        s.added.push_back({seq, c.f(2) / float(seq->numFrames), c.n(3).str()});
        return Value();
    };
}

}  // namespace ffa
