// Skeletal meshes, and the pose of one at a frame of a sequence.
//
// A SkeletalMesh record (tools/uskel.py has the layout and how it was found):
// an empty property block, the bounding box and sphere, then u32 Version,
// u32 VertexCount, the packed vertices, the materials, FVector MeshScale,
// FVector MeshOrigin, Rotator RotOrigin; further on the reference skeleton,
// found by its signature, then the default animation; and LOD models, each
// four lazy arrays in order: influences (f32 weight, u16 point, u16 bone),
// wedges (u16 point, f32 U, f32 V), faces (u16 wedge x3, u16 material) and
// points (FVector), each proven by its own skip offset.
//
// Posing follows tools/uanim.py, which measured it: keys are interpolated
// linearly, a zero rotation key standing for the reference; composing the
// hierarchy takes the root's rotation as it is and every other bone's
// conjugated, the only reading that puts the joints on the skin; and a point
// moves by the weighted sum of its bones' change from the reference pose.
// Into the actor's space a point goes as RotOrigin ((p - MeshOrigin) x
// MeshScale), the sign of MeshOrigin settled by characters' feet on the bottom
// of their collision cylinders.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/Package.h"
#include "world/Animation.h"
#include "world/Geometry.h"

namespace ffa {

struct SkelBone {
    std::string name;
    Quat rotation;
    Vec3 position;
    int parent = 0;
};

// A bone's transform: rotate, then move.
struct BoneTransform {
    Quat q;
    Vec3 p;
};

class SkeletalMesh {
public:
    // Throws FormatError when the header or the first LOD model is not found.
    SkeletalMesh(const Package& p, int idx);

    const Package* package;
    int index;
    std::vector<int32_t> materials;     // references, by face material index
    Vec3 scale, origin;
    int32_t rotOrigin[3] = {0, 0, 0};
    std::vector<SkelBone> bones;
    int32_t defaultAnim = 0;

    // the first LOD model
    std::vector<Vec3> points;
    struct Wedge {
        uint16_t point;
        float u, v;
    };
    std::vector<Wedge> wedges;
    struct Face {
        uint16_t wedge[3];
        uint16_t material;
    };
    std::vector<Face> faces;
    struct Influence {
        float weight;
        uint16_t point, bone;
    };
    std::vector<Influence> influences;

    // A bone by name, or -1.
    int bone(const std::string& name) const;
    // The reference pose, each bone's own transform.
    std::vector<BoneTransform> referenceLocals() const;
    // Bone to mesh transforms from each bone's own.
    std::vector<BoneTransform> compose(const std::vector<BoneTransform>& locals) const;
    // Each bone's own transform at a frame, counted in frames, of a sequence:
    // bones are matched to tracks by name, and one not animated keeps its
    // reference.
    std::vector<BoneTransform> locals(const MeshAnimation& anim, size_t sequence, float frame) const;
    // The points in mesh space, posed by bone to mesh transforms.
    std::vector<Vec3> skin(const std::vector<BoneTransform>& global) const;
    // A point of mesh space into the actor's: RotOrigin ((p - MeshOrigin) x MeshScale).
    Vec3 toActor(Vec3 p) const;

private:
    std::vector<BoneTransform> reference_;
    // per animation, each bone's track index in each chunk, or -1: names
    // matched once, not every frame
    mutable std::map<const MeshAnimation*, std::vector<std::vector<int>>> tracks_;
    const std::vector<int>& trackMap(const MeshAnimation& anim, size_t chunk) const;
};

Quat qmul(Quat a, Quat b);
Quat qconj(Quat q);
Vec3 qrot(Quat q, Vec3 v);
Quat qnlerp(Quat a, Quat b, float t);

}  // namespace ffa
