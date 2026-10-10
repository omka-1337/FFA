#include "world/Audio.h"

#include "audio/SoundBank.h"

namespace ffa {

namespace {

World* worldOf(NativeCall& c) { return World::of(c.vm); }

// PlaySound(Sound, optional Slot, optional Volume, optional bNoOverride,
// optional Radius, optional Pitch, optional Attenuate, optional No3D): what
// is left out the actor's Transient ones, as the engine does.
Value play(NativeCall& c) {
    World* w = worldOf(c);
    if (!w) return Value();
    SoundPlay s;
    s.actor = c.self;
    s.sound = c.o(0);
    if (!s.sound) return Value();
    s.slot = c.i(1);
    s.volume = c.has(2) ? c.f(2) : w->var(c.self, "TransientSoundVolume").f();
    s.noOverride = c.b(3);
    s.radius = c.has(4) ? c.f(4) : w->var(c.self, "TransientSoundRadius").f();
    s.pitch = c.has(5) ? c.f(5) : w->var(c.self, "TransientSoundPitch").f();
    if (s.pitch <= 0) s.pitch = 1;
    s.attenuate = c.has(6) ? c.b(6) : true;
    s.no3D = c.b(7);
    ++w->soundsPlayed;
    if (w->audio) w->audio->play(s);
    return Value();
}

}  // namespace

void registerAudioNatives(VM& vm) {
    auto& n = vm.natives;
    n["actor.playsound"] = play;
    n["actor.playownedsound"] = play;
    n["actor.demoplaysound"] = play;
    n["actor.stopsound"] = [](NativeCall& c) {
        World* w = worldOf(c);
        if (w && w->audio) w->audio->stop(c.self, c.o(0));
        return Value();
    };
    // GetSoundDuration(Sound): its length in seconds, which dialogue waits
    // on (KWPawn's DeliverLocalizedDialog, the cutscenes' WaitForSay)
    n["actor.getsoundduration"] = [](NativeCall& c) {
        World* w = worldOf(c);
        return Value::Float(w && w->sounds ? w->sounds->duration(c.o(0)) : 0.0f);
    };
    // PlayMusic(Song, FadeInTime, optional bLoop, optional bStab): a song of
    // Music/, by name, its handle
    n["actor.playmusic"] = [](NativeCall& c) {
        World* w = worldOf(c);
        if (!w || !w->audio) return Value::Int(0);
        return Value::Int(w->audio->playMusic(utf8(c.s(0)), c.f(1), c.has(2) ? c.b(2) : true));
    };
    n["actor.stopmusic"] = [](NativeCall& c) {
        World* w = worldOf(c);
        if (w && w->audio) w->audio->stopMusic(c.i(0), c.f(1));
        return Value();
    };
    n["actor.stopallmusic"] = [](NativeCall& c) {
        World* w = worldOf(c);
        if (w && w->audio) w->audio->stopMusic(-1, c.f(0));
        return Value();
    };
}

}  // namespace ffa
