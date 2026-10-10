// Karma, as far as KnowWonder's hanging things need it: an actor in
// PHYS_Karma that hangs from the world through KBSJoints, the swamp's punching
// bags on their ropes, swings about the joint that holds it to the world as one
// rigid body with what hangs with it. An impulse (KAddImpulse) turns it about
// that point, gravity brings it back, and KAngularDamping slows it. The
// physics engine itself, its other constraints and its free bodies, is not
// done: a free Karma actor stays where it is.
#pragma once

#include "world/World.h"

namespace ffa {

// A frame of a PHYS_Karma actor.
void karmaTick(World& w, Object* a, float dt);

// KAddImpulse, KIsAwake, KSleep, KWake.
void registerKarmaNatives(VM& vm);

}  // namespace ffa
