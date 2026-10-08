// A terrain, read for collision from the engine's own copy of its grid: the
// native part of a TerrainInfo record, after its properties, which holds every
// vertex in world space and the normals of its triangles.
//
//   index    sector count, then index each
//   index    vertex count, then FVector each, the grid in world space
//   i32 x2   SectorsX, SectorsY
//   index    count, then 6 floats each: the normals of a quad's two triangles
//   FCoords  to world; FCoords to the heightmap, 48 bytes each
//   i32 x2   heightmap X and Y
//   index    count, then 4 bytes each: the baked light of every vertex
//
// A quad is cut from (x, y) to (x+1, y+1), or along the other diagonal where
// its bit in EdgeTurnBitmap is set; a quad whose QuadVisibilityBitmap bit is
// clear is a hole. docs/package-format.md has how this was established;
// tools/uterrain.py reads the same.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Package.h"
#include "world/Geometry.h"

namespace ffa {

class Terrain {
public:
    // The TerrainInfo export idx; the bitmaps are its properties. Throws
    // FormatError when the native part does not read to the record's end.
    Terrain(const Package& p, int idx, size_t propertiesEnd, std::vector<uint32_t> visible,
            std::vector<uint32_t> edgeTurn);

    int X = 0, Y = 0;
    std::vector<Vec3> vertices;
    std::vector<Vec3> normals;          // two per quad, as stored

    bool visible(int x, int y) const { return bit(visible_, x, y, true); }
    bool turned(int x, int y) const { return bit(edgeTurn_, x, y, false); }
    // The two triangles of quad (x, y), as vertex indices.
    void quad(int x, int y, int out[2][3]) const;

    Hit lineCheck(Vec3 a, Vec3 b) const;

    // The largest difference between a stored normal and the normal of the
    // triangle the split rule makes, over every quad.
    float normalCheck() const;

private:
    bool bit(const std::vector<uint32_t>& w, int x, int y, bool def) const;
    bool cell(int x, int y, Vec3 a, Vec3 b, Hit& hit) const;

    std::vector<uint32_t> visible_, edgeTurn_;
    Vec3 origin_;
    float stepX_ = 1, stepY_ = 1;
};

}  // namespace ffa
