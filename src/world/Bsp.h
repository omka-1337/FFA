// A level's BSP: the Model record, read for collision and zones.
//
// The record is read to its exact end, like tools/ubsp.py, so a wrong field
// anywhere shows as a record that does not end where it should. The arrays
// that only drawing needs, render sections, lightmaps and their textures, are
// stepped over. docs/package-format.md has the layout and how it was proven.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Package.h"
#include "world/Geometry.h"

namespace ffa {

struct BspNode {
    Plane plane;
    uint8_t flags = 0;
    int32_t vertPool = 0, surf = 0, back = 0, front = 0, coplanar = 0;
    int32_t collisionBound = 0, renderBound = 0;
    uint8_t zoneBack = 0, zone = 0, numVerts = 0;
    int32_t leafBack = -1, leafFront = -1;
    int32_t section = -1, firstVertex = 0, lightMap = -1;   // for drawing
};

struct BspSurf {
    int32_t material = 0;
    uint32_t flags = 0;
    int32_t base = 0, normal = 0, textureU = 0, textureV = 0;   // points and vectors
};

// What drawing needs: the render sections, a buffer of vertices each, and the
// lightmaps, rectangles in DXT1 textures.
struct BspSection {
    size_t at = 0;                  // the first vertex: position, texture u v,
    int32_t count = 0;              // lightmap u v, normal, 40 bytes
    int32_t material = 0;
    uint32_t flags = 0;
    int32_t lightMapTexture = -1;
};
struct BspLightMapTexture {
    size_t at = 0, size = 0;        // the first mip's DXT1 data
    int width = 0, height = 0;
    int format = 0;
};

struct BspLeaf {
    int32_t zone = 0, permeating = 0, volumetric = 0;
};

// The editor's polygons of a brush, from its Polys export, in the brush's own
// space; a brush's Model is an empty BSP and keeps its shape here. Layout in
// tools/upolys.py. Throws FormatError when the record does not read to its end.
struct BrushPolygon {
    Vec3 normal;
    std::vector<Vec3> vertices;
    uint32_t flags = 0;
};
std::vector<BrushPolygon> readPolys(const Package& p, int idx);

class BspModel {
public:
    // The Model export idx of p. Throws FormatError when the record does not
    // read to its end.
    BspModel(const Package& p, int idx);

    std::vector<Vec3> vectors, points;
    std::vector<BspNode> nodes;
    std::vector<BspSurf> surfs;
    std::vector<int32_t> vertPoints;    // a node's polygon: vertPool .. + numVerts
    std::vector<int32_t> zoneActors;    // per zone, its ZoneInfo export or 0
    std::vector<int32_t> leafHulls;
    std::vector<BspLeaf> leaves;
    int32_t polys = 0;                  // the Polys export, the editor's polygons
    std::vector<BspSection> sections;
    std::vector<int32_t> lightMaps;     // each lightmap's texture
    std::vector<BspLightMapTexture> lightMapTextures;

    // The leaf and zone a point is in, by the walk from the root: to the
    // front or back child by the side of each plane, until there is none on
    // that side. Leaf -1 is solid.
    struct Region {
        int32_t leaf = -1;
        int zone = 0;
    };
    Region regionAt(Vec3 p) const;
    bool solidAt(Vec3 p) const { return regionAt(p).leaf < 0; }

    // The first point where the segment from a to b enters solid. A segment
    // that starts in solid hits at time 0 with startSolid.
    Hit lineCheck(Vec3 a, Vec3 b) const;

    // Whether p lies on the polygon of node i, or of a node coplanar with it,
    // within tolerance units of its plane and edges.
    bool onPolygon(int node, Vec3 p, float tolerance) const;

private:
    bool child(int32_t c) const { return c > 0 && size_t(c) < nodes.size(); }
    bool segment(int i, Vec3 a, Vec3 b, float ta, float tb, Vec3 enter, int enterNode, Hit& hit) const;
    bool side(const BspNode& n, bool front, Vec3 a, Vec3 b, float ta, float tb, Vec3 enter,
              int enterNode, Hit& hit) const;
};

}  // namespace ffa
