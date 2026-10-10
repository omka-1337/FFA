// Movies, as the HUD plays them: its Movie, an object the engine gives every
// HUD (HUD's PostBeginPlay sets its HudParent), and the Movie natives. A
// movie is a Bink file of Movies/, its length its frames over its frame rate,
// from its header. When it ends, or StopNow ends it, the Movie's MovieEnded
// runs, which relays it to the HUD and on to the level's MovieManager:
// SH2_Preamble's plays the logos one after another and then goes to the
// menu. Here a movie is only timed; ffa-play shows its pictures
// (render/Hud.cpp) and plays its sound (apps/Mixer.h) as its time goes.
#pragma once

#include "world/World.h"

namespace ffa {

// A file of Movies/, its name's case not minded: the game asks for
// KWlogo.bik, and the disc has KWLogo.bik. Empty for none.
std::string movieFile(const World& w, const std::string& name);

// A new HUD's Movie, before its PostBeginPlay.
void giveMovie(World& w, Object* hud);

// The movies playing, a frame on: those at their end end.
void movieTick(World& w);

// Play, StopNow, StopAtEnd, Pause, IsPlaying, IsPaused, GetWidth, GetHeight.
void registerMovieNatives(VM& vm);

}  // namespace ffa
