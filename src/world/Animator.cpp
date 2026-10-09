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
    return world.vm.loadObject(m->package->stem + "." + path);
}

const MeshAnimation* Animator::animation(const Object* o) { return animation(refOf(o, "MeshAnimation")); }

void Animator::resolve(Object* a, AnimState& s) {
    s.resolved = true;
    // the mesh's default animation, named after its reference skeleton
    ObjectRef mesh = refOf(world.obj(a, "Mesh"), "SkeletalMesh");
    if (mesh && mesh.cls() == "SkeletalMesh") {
        int32_t ref = skeletalDefaultAnim(*mesh.pkg, mesh.idx);
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
        if (was == 0) s.channels[0].notify = true;  // channel 0 notifies unless told not to
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
        const AnimSequence* seq = find(a, name);
        if (!seq || (ch.seq == seq && ch.looping && ch.animating)) return;
        ch.seq = seq;
        ch.name = seq->name;
        ch.rate = seq->numFrames > 0 ? seq->rate / float(seq->numFrames) : 0;
        ch.frame = 0;
        ch.tween = blend;
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
    const AnimSequence* seq = an.find(c.self, name);
    if (!seq) {
        ++an.notFound;
        an.missing[c.self->cls->name.str() + "." + name]++;
        return;
    }
    // looping a sequence that already loops only changes its rate
    bool same = ch.seq == seq && ch.looping && ch.animating && loop;
    ch.seq = seq;
    ch.name = seq->name;
    ch.rate = seq->numFrames > 0 ? rate * seq->rate / float(seq->numFrames) : 0;
    if (!same) {
        ch.frame = 0;
        ch.tween = tween > 0 ? tween : 0;
    }
    ch.looping = loop;
    ch.stopAtEnd = false;
    ch.animating = true;
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
        const AnimSequence* seq = an.find(c.self, c.n(0).str());
        if (!seq) {
            ++an.notFound;
            return Value();
        }
        ch.seq = seq;
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
