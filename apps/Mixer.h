// ffa-play's sound: the world's audio sink, mixed by SDL at 44100 Hz stereo.
//
// A sound plays as a voice from its actor: its volume falls off with distance
// to nothing at its radius, as the engine's linear fall off, and it is panned
// by where it is against the camera's right. A sound in a slot other than
// SLOT_None stops the one its actor had playing there, or with bNoOverride
// does not play while that one does; Talk is a slot, so a line cuts the last.
// Each actor's AmbientSound loops while it is in reach, SoundRadius times 25
// as the engine family measures it, at SoundVolume / 255 and SoundPitch / 64.
// Music is not played yet: its Ogg Vorbis has no decoder here.
#pragma once

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/SoundBank.h"
#include "world/Audio.h"
#include "world/Geometry.h"

namespace ffa {

class Mixer : public AudioSink {
public:
    explicit Mixer(World& w) : w_(w) {
        SDL_AudioSpec want{}, have{};
        want.freq = 44100;
        want.format = AUDIO_F32SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = [](void* self, Uint8* out, int len) { static_cast<Mixer*>(self)->mix(reinterpret_cast<float*>(out), len / 8); };
        want.userdata = this;
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (device_) {
            rate_ = have.freq;
            SDL_PauseAudioDevice(device_, 0);
        }
    }
    ~Mixer() override {
        if (w_.audio == this) w_.audio = nullptr;
        if (device_) SDL_CloseAudioDevice(device_);
    }
    bool open() const { return device_ != 0; }

    void play(const SoundPlay& s) override {
        if (!device_ || !w_.sounds) return;
        auto clip = w_.sounds->clip(s.sound);
        if (!clip || clip->samples.empty()) return;
        std::lock_guard<std::mutex> lock(m_);
        if (s.slot != 0)
            for (Voice& v : voices_)
                if (!v.done && !v.ambient && v.actor == s.actor && v.slot == s.slot) {
                    if (s.noOverride) return;
                    v.done = true;
                }
        Voice v;
        v.clip = clip;
        v.actor = s.actor;
        v.sound = s.sound;
        v.slot = s.slot;
        v.volume = s.volume;
        v.radius = s.radius;
        v.step = double(clip->rate) / double(rate_) * double(s.pitch);
        v.positional = s.attenuate && !s.no3D;
        place(v);
        voices_.push_back(std::move(v));
    }
    void stop(Object* actor, Object* sound) override {
        std::lock_guard<std::mutex> lock(m_);
        for (Voice& v : voices_)
            if (v.actor == actor && (!sound || v.sound == sound) && !v.ambient) v.done = true;
    }
    int playMusic(const std::string&, float, bool) override { return ++music_; }
    void stopMusic(int, float) override {}

    // Once a frame, from the game's side: where the listener is, the voices'
    // gains for it, and the ambient sounds in reach.
    void update(Vec3 at, const int32_t rot[3]) {
        if (!device_) return;
        float ax[3][3];
        rotationAxes(rot[0], rot[1], rot[2], ax);
        std::lock_guard<std::mutex> lock(m_);
        ear_ = at;
        right_ = {ax[1][0], ax[1][1], ax[1][2]};
        // the ambient sounds: one looping voice an actor, while in reach
        std::map<Object*, Voice*> looping;
        for (Voice& v : voices_)
            if (v.ambient && !v.done) looping[v.actor] = &v;
        for (Object* a : w_.actors) {
            Object* s = (!a->deleted && a->cls->findProp(Name("AmbientSound"))) ? w_.obj(a, "AmbientSound") : nullptr;
            auto it = looping.find(a);
            Vec3 p;
            if (s) w_.vm.unvector(w_.var(a, "Location"), p.x, p.y, p.z);
            float radius = s ? w_.var(a, "SoundRadius").f() * 25.0f : 0;
            bool near = s && length(p - at) < radius;
            if (it != looping.end()) {
                Voice* was = it->second;
                looping.erase(it);
                if (near && was->sound == s) continue;
                was->done = true;
            }
            if (!near || !w_.sounds) continue;
            auto clip = w_.sounds->clip(s);
            if (!clip || clip->samples.empty()) continue;
            Voice v;
            v.clip = clip;
            v.actor = a;
            v.sound = s;
            v.ambient = true;
            v.volume = float(w_.var(a, "SoundVolume").i()) / 255.0f;
            v.radius = radius;
            v.step = double(clip->rate) / double(rate_) * double(std::max(1, w_.var(a, "SoundPitch").i())) / 64.0;
            v.positional = true;
            voices_.push_back(std::move(v));
        }
        for (Voice& v : voices_) place(v);
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.done; }), voices_.end());
    }

private:
    struct Voice {
        std::shared_ptr<const DecodedSound> clip;
        Object* actor = nullptr;
        Object* sound = nullptr;
        int slot = 0;
        float volume = 1, radius = 0;
        double pos = 0, step = 1;
        bool positional = true, ambient = false, done = false;
        Vec3 at;
        float left = 1, right = 1;
    };

    void place(Voice& v) {
        if (v.actor && !v.actor->deleted) w_.vm.unvector(w_.var(v.actor, "Location"), v.at.x, v.at.y, v.at.z);
        float g = v.volume;
        float pan = 0;
        if (v.positional) {
            Vec3 d = v.at - ear_;
            float dist = length(d);
            if (v.radius > 0) g *= std::clamp(1.0f - dist / v.radius, 0.0f, 1.0f);
            if (dist > 1) pan = std::clamp(dot(d * (1 / dist), right_), -1.0f, 1.0f) * 0.8f;
        }
        v.left = g * std::sqrt(0.5f * (1 - pan));
        v.right = g * std::sqrt(0.5f * (1 + pan));
    }

    void mix(float* out, int frames) {
        std::fill(out, out + frames * 2, 0.0f);
        std::lock_guard<std::mutex> lock(m_);
        for (Voice& v : voices_) {
            if (v.done) continue;
            const std::vector<float>& s = v.clip->samples;
            const double n = double(s.size());
            for (int i = 0; i < frames; ++i) {
                if (v.pos >= n - 1) {
                    if (!v.ambient) {
                        v.done = true;
                        break;
                    }
                    v.pos = std::fmod(v.pos, n - 1);
                }
                size_t k = size_t(v.pos);
                float t = float(v.pos - double(k));
                float x = s[k] + (s[k + 1] - s[k]) * t;
                out[2 * i] += x * v.left;
                out[2 * i + 1] += x * v.right;
                v.pos += v.step;
            }
        }
        for (int i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i] * gain_, -1.0f, 1.0f);
    }

    World& w_;
    SDL_AudioDeviceID device_ = 0;
    int rate_ = 44100;
    std::mutex m_;
    std::vector<Voice> voices_;
    Vec3 ear_, right_{0, 1, 0};
    float gain_ = 0.9f;
    int music_ = 0;
};

}  // namespace ffa
