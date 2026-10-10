// Sound for script: PlaySound and its kin, StopSound, GetSoundDuration and the
// music natives. What is asked for is handed to whoever plays it, the World's
// audio sink (ffa-play's mixer); without one, sounds are only timed, which is
// what WaitForSay and the like need.
#pragma once

#include <functional>
#include <string>

#include "world/World.h"

namespace ffa {

struct SoundPlay {
    Object* actor = nullptr;        // what it comes from
    Object* sound = nullptr;
    int slot = 0;                   // ESoundSlot: None, Misc, Pain, Interact, Ambient, Talk, Interface
    float volume = 1, radius = 0, pitch = 1;
    bool noOverride = false, attenuate = true, no3D = false;
};

struct AudioSink {
    virtual ~AudioSink() = default;
    virtual void play(const SoundPlay& s) = 0;
    virtual void stop(Object* actor, Object* sound) = 0;      // sound null for all of it
    virtual int playMusic(const std::string& song, float fadeIn, bool loop) = 0;
    virtual void stopMusic(int handle, float fadeOut) = 0;     // -1 for all
};

void registerAudioNatives(VM& vm);

}  // namespace ffa
