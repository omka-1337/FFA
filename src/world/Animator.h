// Each actor's animation channels, as the engine runs them, and the natives
// script drives them with.
//
// A channel plays one sequence of the actor's animations: those LinkSkelAnim
// added, newest first, then its mesh's default animation. Its frame runs from
// 0 to 1 through the sequence at Rate x the sequence's frames a second over
// its frame count. Playing once, it stops at 1; looping, it goes round. At the
// end of a play or of a round, AnimEnd(channel) goes to the actor when the
// channel's notify is on, channel 0's by default. A tween holds the first frame
// for its time; PlayAnim's and LoopAnim's tween time blends the new sequence
// in from the pose before, as it plays. FinishAnim waits for the channel to stop, ending a loop at the
// end of its round. Notifies added by AddNotify call the actor's function of
// that name as the frame passes them; the game's animations carry none of
// their own.
//
// What the engine family does is the model; the frames and lengths are the
// data's. How a frame becomes a pose is not done yet.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Library.h"
#include "world/Animation.h"
#include "world/SkeletalMesh.h"
#include "world/Geometry.h"
#include "world/World.h"

namespace ffa {

struct AnimChannel {
    const AnimSequence* seq = nullptr;
    const MeshAnimation* set = nullptr;     // the animation the sequence is in
    std::string name;               // the sequence asked for
    float frame = 0;                // 0 to 1
    float rate = 0;                 // of the sequence a second
    float tween = 0;                // seconds of tween left
    bool looping = false, animating = false, notify = false, stopAtEnd = false;
    float alpha = 1, alphaTarget = 1, alphaRate = 0;   // blending, AnimBlendParams
    std::string bone;
    // A sequence started with a tween time blends in from the pose the
    // channel had, over that time, while it plays.
    std::vector<BoneTransform> from;
    float blendTime = 0, blendLeft = 0;
};

struct AnimState {
    bool resolved = false;
    const SkeletalMesh* mesh = nullptr;
    const MeshAnimation* defaults = nullptr;
    std::vector<const MeshAnimation*> linked;
    std::vector<AnimChannel> channels;
    struct Added {
        const AnimSequence* seq;
        float frame;                // 0 to 1
        std::string event;
    };
    std::vector<Added> added;
    int notifyChannel = 0;
    // The pose, kept until a channel changes: GetBoneCoords asks for it many
    // times a frame, the factory's lasers each frame the hero stands still.
    uint64_t version = 0, poseVersion = ~0ull;
    std::vector<BoneTransform> poseCache;
};

class Animator {
public:
    Animator(World& w, Library& lib);

    // A frame of every animating actor: frames, notifies and AnimEnd.
    void tick(float dt);

    // The engine's own animation of a pawn that moves, for pawns with
    // bPhysicsAnimUpdate: walking, its MovementAnims for the way it goes
    // against the way it faces, forward, back, left, right, looped on channel
    // 0 and blended in over BlendChangeTime; stopped again, KnowWonder's
    // IdleAnimName, which their ChangeAnimation keeps up to date for this. The
    // variables are the data's; when the engine switches is the model's.
    void movement(Object* pawn);

    AnimState& state(Object* a);
    AnimChannel& channel(Object* a, int k);
    // Start a channel's blend from its pose now, over time seconds.
    void startBlend(Object* a, int k, float time);
    // A channel's own pose, bone by bone, without the blend from before.
    std::vector<BoneTransform> channelLocals(const AnimState& s, const AnimChannel& c) const;
    // A sequence of the actor's animations by name, and the set it is in.
    const AnimSequence* find(Object* a, const std::string& name, const MeshAnimation** set = nullptr);
    // A MeshAnimation by an object script holds, or by reference.
    const MeshAnimation* animation(const Object* o);
    const MeshAnimation* animation(const ObjectRef& r);
    // The actor's bones now, bone to mesh: channel 0's sequence, and over it
    // each channel above with an alpha, from its blend bone down, or all of it.
    std::vector<BoneTransform> pose(Object* a);
    // A bone in the world: its origin and its X, Y and Z axes. False when the
    // actor has no such bone.
    bool boneWorld(Object* a, const std::string& bone, Vec3& origin, Vec3 axes[3]);
    // The actor's transform out of its mesh's space into the world.
    void meshToWorld(Object* a, float m[3][3], Vec3& origin);
    // Put the actors attached to bones where their bones are.
    void attachments();
    const SkeletalMesh* skeletal(const ObjectRef& r);

    // The object script knows a MeshAnimation by, made through the VM's
    // object loading when it has none.
    Object* objectFor(const MeshAnimation* m);

    World& world;
    Library& library;
    size_t sequencesPlayed = 0, animEnds = 0, notifiesSent = 0, notFound = 0;
    std::map<std::string, size_t> missing;  // sequences asked for and not found, by class

    static Animator* of(VM& vm);

private:
    void resolve(Object* a, AnimState& s);
    ObjectRef refOf(const Object* o, const char* cls);
    std::map<Object*, AnimState> states_;
    std::map<std::pair<const Package*, int>, std::unique_ptr<MeshAnimation>> anims_;
    std::map<std::pair<const Package*, int>, std::unique_ptr<SkeletalMesh>> meshes_;
};

void registerAnimationNatives(VM& vm);

// A rotator whose axes, as rotationAxes gives them, are X, Y and Z.
void rotatorOf(const Vec3 axes[3], int32_t out[3]);

}  // namespace ffa
