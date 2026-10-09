// Skeletal animation as the engine plays it: MeshAnimation records read for
// their sequences, and each actor's channels running them.
//
// A MeshAnimation record (tools/uanim.py has the layout and its proof), after
// an empty property block:
//
//   u32     Version, 0 or 4
//   index   bone count, then name index, u32 flags, i32 parent each
//   index   motion chunk count, then per chunk: FVector RootSpeed3D, f32
//           TrackTime, i32 StartBone, u32 Flags, the bone indices, the
//           tracks, the root track, and for version 4 one more index
//   index   sequence count, then per sequence: f32 not understood, name,
//           groups, i32 StartFrame, i32 NumFrames, the notifies (f32 time,
//           name, object), f32 Rate in frames a second
//
// A track: u32 flags, then rotation keys (16 bytes), position keys (12) and
// key times (4).
//
// A SkeletalMesh names its default animation right after its reference
// skeleton, found by the skeleton's signature as tools/uskel.py finds it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Package.h"

namespace ffa {

struct AnimNotifyKey {
    float time;                     // 0 to 1 through the sequence
    std::string name;
    int32_t object;                 // an AnimNotify, or 0
};

struct AnimSequence {
    std::string name;
    std::vector<std::string> groups;
    int startFrame = 0, numFrames = 0;
    float rate = 0;                 // frames a second
    std::vector<AnimNotifyKey> notifies;
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

// A bone's motion through a sequence: rotation and position keys at times
// counted in frames. A zero quaternion is a key with no rotation.
struct AnimTrack {
    std::vector<Quat> rotations;
    std::vector<float> positions;   // x y z each
    std::vector<float> times;
};

// One sequence's motion: a track per animated bone, by bone index when the
// chunk lists them, else in bone order.
struct AnimChunk {
    float trackTime = 0;
    std::vector<int32_t> boneIndices;
    std::vector<AnimTrack> tracks;
};

class MeshAnimation {
public:
    // Throws FormatError when the record does not read to its exact end.
    MeshAnimation(const Package& p, int idx);

    const Package* package;
    int index;
    std::vector<std::string> bones;
    std::vector<AnimChunk> chunks;  // one a sequence, in order
    std::vector<AnimSequence> sequences;

    const AnimSequence* find(const std::string& name) const;
};

// The default animation reference of a SkeletalMesh export, or 0.
int32_t skeletalDefaultAnim(const Package& p, int idx);

}  // namespace ffa
