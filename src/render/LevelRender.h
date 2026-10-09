// Drawing a level with OpenGL ES 2: its BSP, textured and lit as the game
// baked it, from where the player's camera is.
//
// The light is the texture times the lightmap, doubled, the lightmap holding
// the zone's ambient already (docs/rendering.md, the ambient). Surfaces
// flagged unlit take the texture alone.
//
// No window here: the caller makes the GL context current first.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/Library.h"
#include "render/Font.h"
#include "render/Material.h"
#include "render/Texture.h"
#include "world/Session.h"

namespace ffa {

class LevelRender {
public:
    // Builds the GL resources; the context must be current.
    LevelRender(Session& s, Library& lib);
    ~LevelRender();

    // One frame of the view from location along rotation, into a viewport of
    // width by height, with a horizontal field of view in degrees.
    void draw(Vec3 location, const int32_t rotation[3], int width, int height, float fovDegrees);

    size_t triangles = 0, batches = 0, textures = 0, lightMaps = 0, missingTextures = 0, meshes = 0, characters = 0, relit = 0;

    // The HUD, over the frame: the player's HUD's PostRender on a Canvas, as
    // the engine calls it, and the tiles and text its natives draw
    // (render/Hud.cpp). Called by draw.
    void drawHud(int width, int height);
    size_t hudTiles = 0, hudFailures = 0;

    // What Canvas's natives draw into, one rectangle of a texture each, in
    // pixels from the top left, and the fonts they draw text with.
    struct HudTile {
        unsigned texture = 0;
        float x = 0, y = 0, w = 0, h = 0, u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        float color[4] = {1, 1, 1, 1};
        int style = 1;
    };
    void hudPush(const HudTile& t) { hud_.push_back(t); }
    // A material's texture for the HUD, and its size in texels; 0 for none.
    unsigned hudTexture(Object* material, int& width, int& height);
    const FontData* hudFont(Object* font);
    unsigned hudPage(const FontData& f, int page, int& width, int& height);
    // A texture object script reaches, given its USize, VSize and the rest.
    void fillTexture(Object* o);

private:
    struct Batch {
        unsigned texture = 0, lightMap = 0;
        SurfaceMaterial mat;
        bool unlit = false;
        unsigned buffer = 0;
        int count = 0;              // vertices
    };
    unsigned textureFor(const SurfaceMaterial& m, int& width, int& height);
    void buildBsp();
    void buildTerrains();

    // A terrain: bands of rows indexed with 16 bits, one program for its
    // number of layers, and per layer a texture and its u and v rows.
    struct TerrainDraw {
        unsigned program = 0;
        struct Band {
            unsigned vertices = 0, indices = 0;
            int count = 0;
        };
        std::vector<Band> bands;
        std::vector<unsigned> textures;
        std::vector<std::array<float, 8>> uv;   // u row then v row, each x y z w
    };
    std::vector<TerrainDraw> terrains_;

    // Static meshes: each mesh's positions, UVs and indices once, and per
    // actor its transform, its baked vertex colours, and a texture a section.
    void buildMeshes();
    ObjectRef refOf(const Object* o);
    struct MeshBuffers {
        unsigned vertices = 0, indices = 0;
    };
    struct MeshDraw {
        const MeshBuffers* mesh = nullptr;
        float model[16];
        unsigned colors = 0;        // R G B per vertex, or 0 for none baked
        Vec3 ambient;
        struct Part {
            int first, count;
            unsigned texture;
            SurfaceMaterial mat;
        };
        std::vector<Part> parts;
        float radius = 0;           // around its origin, in the world
        bool unlit = false;         // bUnlit: the texture as it is
    };
    // Skeletal meshes: per actor its wedges' positions, skinned each frame on
    // the CPU from the animator's pose, and its faces by material.
    struct SkelDraw {
        const SkeletalMesh* mesh = nullptr;
        unsigned vertices = 0, indices = 0;
        std::vector<MeshDraw::Part> parts;
        std::vector<float> scratch;
        std::vector<Vec3> world;    // the posed points this frame, for its shadow
        bool failed = false;
        // this frame's, kept from the opaque pass for the blended one
        bool drawn = false;
        float mvp[16];
        Vec3 ambient;
    };
    // Projectors: what they cover drawn again with their texture laid on.
    void drawProjectors(const float mvp[16], Vec3 eye);
    void drawReceivers(Vec3 lo, Vec3 hi);
    unsigned shadowFor(Object* projector, Object* actor, Vec3 apex, const Vec3 axes[3], float tanHalf);
    unsigned projProgram_ = 0, shadowProgram_ = 0, shadowFbo_ = 0, blob_ = 0;
    // The screen flash over the frame: the player controller's FlashScale and
    // FlashFog, which the game's fades set.
    void drawFlash();
    unsigned flashProgram_ = 0;
    std::vector<HudTile> hud_;
    Object* canvas_ = nullptr;
    unsigned hudProgram_ = 0;
    std::map<std::pair<const Package*, int>, FontData> fonts_;
    std::map<Object*, unsigned> shadowTex_;
    Class* shadowClass_ = nullptr;
    // Skeletal meshes: posed and their opaque parts drawn, or, after every
    // opaque thing, their blended parts.
    void drawSkeletal(const float mvp[16], Vec3 eye, bool blended);
    // The BSP's opaque batches, or its blended ones.
    void drawBsp(const float mvp[16], bool blended);
    std::vector<Vec3> characterLight(Object* a, const SkeletalMesh& mesh, const std::vector<Vec3>& pts,
                                     const float r[3][3], Vec3 loc);
    std::vector<Object*> lights_;
    bool lightsBuilt_ = false;
    SkelDraw& skelFor(Object* a);
    std::map<Object*, SkelDraw> skel_;
    std::map<const void*, MeshBuffers> meshBuffers_;
    std::vector<MeshDraw> meshDraws_;
    unsigned meshProgram_ = 0;

    Session& session_;
    Library& lib_;
    MaterialResolver materials_;
    std::map<std::pair<const Package*, int>, std::pair<unsigned, std::pair<int, int>>> textures_;
    std::vector<unsigned> lightMapTex_;
    std::vector<Batch> batches_;
    unsigned program_ = 0, white_ = 0;
    int aPos_ = -1, aUv_ = -1, aUv2_ = -1, aAmb_ = -1;
    int uMvp_ = -1, uTex_ = -1, uLm_ = -1, uLit_ = -1, uCut_ = -1;
};

// The pixels of the current GL framebuffer, as RGBA rows top to bottom.
Image readFramebuffer(int width, int height);
// An RGBA image as a PNG file, stored without compression.
void writePng(const std::string& path, const Image& img);

}  // namespace ffa
