// Actor physics, run each frame for an actor whose Physics is set.
//
// A moving actor is a box of its collision extent swept through the level's
// collision (Collision::boxCheck). What the modes do follows the engine
// family's published behaviour; the constants with it are the engine's own,
// none of them in the data: a floor is walkable when its normal's Z is at
// least 0.7, a step up is at most 35 units, and a walking pawn is kept between
// 1.9 and 2.4 units above its floor, which the data agrees with: path nodes,
// placed where a pawn of their size stands, are a median 2.5 above it.
#pragma once

#include "world/World.h"

namespace ffa {

enum Physics : int {
    PHYS_None = 0, PHYS_Walking = 1, PHYS_Falling = 2, PHYS_Swimming = 3, PHYS_Flying = 4,
    PHYS_Rotating = 5, PHYS_Projectile = 6, PHYS_Interpolating = 7, PHYS_MovingBrush = 8,
    PHYS_Spider = 9, PHYS_Trailer = 10, PHYS_Ladder = 11, PHYS_RootMotion = 12, PHYS_Karma = 13,
    PHYS_KarmaRagDoll = 14, PHYS_PushPulled = 15,
};

// One frame of an actor's physics. Modes not done yet are counted in the VM's
// missingCalls under their name.
void performPhysics(World& w, Object* a, float dt);

// Touching: begin and end touches for an actor that has moved, against the
// actors whose cylinders it overlaps and that do not both block.
void updateTouching(World& w, Object* a);

// TouchingActors and SetPhysics with its events.
void registerPhysicsNatives(VM& vm);

}  // namespace ffa
