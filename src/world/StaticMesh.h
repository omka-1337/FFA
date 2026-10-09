// A static mesh, read for collision: its vertex positions and its triangle
// tree, and the collision model it may point at.
//
// The record is read to its exact end, like tools/umesh.py; the streams only
// drawing needs are stepped over. docs/package-format.md has the layout.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Package.h"
#include "world/Geometry.h"

namespace ffa {

struct MeshTriangle {
    uint16_t v[3];
    int32_t section;
};

// A node of the triangle tree: one triangle, the next triangle in its plane,
// the subtrees in front of and behind it, and a box around the whole subtree.
struct MeshNode {
    uint16_t triangle, coplanar, front, back;   // 65535 for none
    Vec3 min, max;
};

class StaticMeshCollision {
public:
    StaticMeshCollision(const Package& p, int idx);

    std::vector<Vec3> positions;
    std::vector<MeshTriangle> triangles;
    std::vector<MeshNode> nodes;
    int32_t collisionModel = 0;         // a Model export, or 0

    // What drawing needs: the first UV stream, the index buffer, and the
    // sections, a material's run of it each, FirstIndex and NumFaces.
    std::vector<Vec3> normals;          // one per vertex
    std::vector<float> uv;              // two per vertex
    std::vector<uint16_t> indices;
    struct Section {
        int firstIndex, faces;
    };
    std::vector<Section> sections;

    // The first triangle the segment a..b crosses, in the mesh's own space,
    // from either side.
    Hit lineCheck(Vec3 a, Vec3 b) const;
    // The same by testing every triangle, without the tree: a check on it.
    Hit lineCheckAll(Vec3 a, Vec3 b) const;

private:
    void node(size_t i, Vec3 a, Vec3 b, Hit& hit) const;
    bool triangle(const MeshTriangle& t, Vec3 a, Vec3 b, Hit& hit) const;
};

}  // namespace ffa
