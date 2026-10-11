// Particle effects: the Emitter actors and their ParticleEmitters, the
// engine's native part of them. An Emitter's Emitters are objects whose
// variables say how particles are spawned, move, grow, turn, fade and are
// drawn; script reads and sets them (KnowWonder's Emitter.Trigger and
// SetActive switch spawning on and off) and calls Trigger and SpawnParticle,
// and the engine runs the particles. SpriteEmitters are run, 560 of the 575
// the levels place and those the game's classes spawn; beams and meshes are
// not yet. What the renderer draws is gathered here each tick.
#pragma once

#include <vector>

#include "world/Geometry.h"
#include "world/World.h"

namespace ffa {

struct ParticleSprite {
    Vec3 at;
    float width = 0, height = 0;
    float spin = 0;             // radians
    float color[4] = {1, 1, 1, 1};
    int frame = 0;              // the subdivision of the texture
    Vec3 up;                    // PTDU_Up: along its velocity
};

// One ParticleEmitter's particles as they are to be drawn this frame.
struct ParticleDraw {
    Object* actor = nullptr;    // the Emitter
    Object* texture = nullptr;
    int style = 3;              // EParticleDrawStyle
    int columns = 1, rows = 1;  // TextureU/VSubdivisions
    int facing = 0;             // EParticleDirectionUsage
    Vec3 normal{0, 0, 1};       // ProjectionNormal, for PTDU_Normal
    bool zWrite = false, zTest = true;
    std::vector<ParticleSprite> sprites;
};

// A new Emitter's own copies of the ParticleEmitters its class's defaults
// name, before its script runs: each Emitter runs its own.
void giveEmitters(World& w, Object* actor);

// The particles a tick on, and what is to be drawn.
void particlesTick(World& w, float dt);
const std::vector<ParticleDraw>& particleDraws(World& w);

// ParticleEmitter's Trigger and SpawnParticle, Emitter's Kill.
void registerParticleNatives(VM& vm);

}  // namespace ffa
