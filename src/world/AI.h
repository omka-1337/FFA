// The engine's half of what controllers do: turning a pawn toward what its
// controller looks at, the latent moves state code waits on, whether a place
// can be walked to, routes over the level's paths, and sight.
//
// The paths are the level's own: every NavigationPoint holds its PathList of
// ReachSpecs, each with its Start and End, Distance, the largest
// CollisionRadius and CollisionHeight that fit along it, and its reachFlags,
// all as the editor built them, loaded as script objects. What is computed is
// the engine's C++, of which the published UE2 behaviour is followed and the
// constants are this engine's choices, said where they are made
// (docs/script-vm.md, AI).
#pragma once

#include "world/Geometry.h"
#include "world/World.h"

namespace ffa {

// Once a frame for a controller that is not the player's: a Focus sets the
// FocalPoint to where it is, and a FocalPoint turns the controller's
// DesiredRotation toward it, which the pawn's physics turns to.
void aiTick(World& w, Object* controller);
// Whether a controller turns its pawn: it has a Focus or a FocalPoint.
bool aiSteering(World& w, Object* controller);

// Whether a pawn could walk from where it is to p, or to an actor; with
// `goal`, reaching its cylinder is enough.
bool reachable(World& w, Object* pawn, Vec3 p, Object* goal = nullptr);

// MoveTo, MoveToward, FinishRotation, WaitForLanding, actorReachable,
// pointReachable, FindPathToward, FindPathTo, FindPathTowardNearest,
// FindRandomDest, LineOfSightTo and CanSee.
void registerAINatives(VM& vm);

}  // namespace ffa
