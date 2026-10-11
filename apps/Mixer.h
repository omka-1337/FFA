// ffa-play's sound: the world's audio sink, mixed by SDL at 44100 Hz stereo.
//
// A sound plays as a voice from its actor: its volume falls off with distance
// to nothing at its radius, as the engine's linear fall off, and it is panned
// by where it is against the camera's right. A sound in a slot other than
// SLOT_None stops the one its actor had playing there, or with bNoOverride
// does not play while that one does; Talk is a slot, so a line cuts the last.
// Each actor's AmbientSound loops while it is in reach, SoundRadius times 25
// as the engine family measures it, at SoundVolume / 255 and SoundPitch / 64.
// A stereo clip keeps its two channels. A movie the HUD plays has its sound
// from its own file, at where its time is, while it plays and is not paused.
// Music is the songs of Music/, Ogg Vorbis (audio/Vorbis.h), decoded as they
// play, faded in and out as PlayMusic and StopMusic ask, looping unless told
// not, at the configuration's MusicVolume, 0.3 in Default.ini.
#pragma once

#include <SDL2/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "audio/Bink.h"
#include "audio/Vorbis.h"
#include "audio/SoundBank.h"
#include "world/Audio.h"
#include "world/Geometry.h"
#include "world/Movie.h"

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
        if (w_.config) {
            std::string v = w_.config("ALAudio.ALAudioSubsystem", "MusicVolume");
            if (!v.empty()) musicVolume_ = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
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
            if (v.actor == actor && (!sound || v.sound == sound) && !v.ambient && !v.movie) v.done = true;
    }
    // Every sound playing ends, as skipping a cutscene asks; the ambient
    // ones are taken up again where they are in reach, and a movie's sound
    // is the movie's.
    void stopAll() override {
        std::lock_guard<std::mutex> lock(m_);
        for (Voice& v : voices_)
            if (!v.movie) v.done = true;
    }
    // A song of Music/ by its name, its case not minded, faded in over
    // fadeIn seconds; its handle for StopMusic.
    int playMusic(const std::string& song, float fadeIn, bool loop) override {
        int handle = ++music_;
        if (!device_) return handle;
        std::string path = songFile(song);
        if (path.empty()) return handle;
        Song s;
        try {
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            s.decoder = std::make_shared<VorbisDecoder>(std::move(data));
        } catch (const std::exception&) {
            return handle;
        }
        s.handle = handle;
        s.loop = loop;
        s.gain = fadeIn > 0 ? 0.0f : 1.0f;
        s.target = 1;
        s.fade = fadeIn > 0 ? 1.0f / fadeIn : 0.0f;
        s.step = double(s.decoder->rate()) / double(rate_);
        std::lock_guard<std::mutex> lock(m_);
        songs_.push_back(std::move(s));
        return handle;
    }
    // A song faded out over fadeOut seconds and ended; -1 for every song.
    void stopMusic(int handle, float fadeOut) override {
        std::lock_guard<std::mutex> lock(m_);
        for (Song& s : songs_)
            if (handle < 0 || s.handle == handle) {
                s.target = 0;
                if (fadeOut > 0) {
                    s.fade = 1.0f / fadeOut;
                } else {
                    s.gain = 0;
                    s.done = true;
                }
            }
    }

    // Once a frame, from the game's side: where the listener is, the voices'
    // gains for it, and the ambient sounds in reach.
    void update(Vec3 at, const int32_t rot[3]) {
        if (!device_) return;
        float ax[3][3];
        rotationAxes(rot[0], rot[1], rot[2], ax);
        movieSound();
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
        bool positional = true, ambient = false, movie = false, done = false;
        Vec3 at;
        float left = 1, right = 1;
    };

    // The playing movie's sound, begun where its time is; ended when the
    // movie ends, pauses or begins again. Decoded outside the lock, so the
    // voices playing go on meanwhile.
    void movieSound() {
        Object* want = nullptr;
        const World::MoviePlay* play = nullptr;
        for (auto& [o, m] : w_.movies)
            if (m.playing && m.pausedAt < 0) {
                want = o;
                play = &m;
                break;
            }
        if (movie_ && (want != movie_ || play->start != movieStart_)) {
            std::lock_guard<std::mutex> lock(m_);
            for (Voice& v : voices_)
                if (v.movie) v.done = true;
            movie_ = nullptr;
        }
        if (!want || want == movie_) return;
        movie_ = want;
        movieStart_ = play->start;
        std::string path = movieFile(w_, play->file);
        if (path.empty()) return;
        auto it = movieClips_.find(path);
        if (it == movieClips_.end()) {
            std::shared_ptr<const DecodedSound> clip;
            try {
                std::ifstream in(path, std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                clip = std::make_shared<DecodedSound>(decodeBink(data.data(), data.size()));
            } catch (const std::exception&) {
                // black.bik has no sound
            }
            it = movieClips_.emplace(path, clip).first;
        }
        if (!it->second || it->second->samples.empty()) return;
        Voice v;
        v.clip = it->second;
        v.movie = true;
        v.positional = false;
        v.step = double(v.clip->rate) / double(rate_);
        v.pos = std::max(0.0, double(w_.time - play->start) * double(v.clip->rate));
        std::lock_guard<std::mutex> lock(m_);
        voices_.push_back(std::move(v));
    }

    struct Song {
        std::shared_ptr<VorbisDecoder> decoder;
        int handle = 0;
        bool loop = true, done = false;
        float gain = 1, target = 1, fade = 0;   // fade: gain a second
        double step = 1, pos = 0;               // in the song's frames
        std::vector<float> buf;                 // decoded, interleaved
        size_t frames = 0;                      // in buf
    };

    std::string songFile(const std::string& song) {
        namespace fs = std::filesystem;
        std::string want = song;
        if (want.size() < 4 || want.substr(want.size() - 4) != ".ogg") want += ".ogg";
        for (char& c : want) c = char(std::tolower(static_cast<unsigned char>(c)));
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(w_.gameDir) / "Music", ec)) {
            std::string f = e.path().filename().string();
            for (char& c : f) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (f == want) return e.path().string();
        }
        return {};
    }

    // A frame of the song at its position, between the two decoded frames
    // around it; false at its end.
    bool songFrame(Song& s, float out[2]) {
        int ch = s.decoder->channels();
        while (s.pos + 1 >= double(s.frames)) {
            // keep the last frame, for the step across into the next lot
            size_t keep = s.frames ? 1 : 0;
            if (keep) std::copy(s.buf.end() - ch, s.buf.end(), s.buf.begin());
            s.pos -= double(s.frames - keep);
            s.buf.resize(size_t(ch) * (keep + 1024));
            size_t n = s.decoder->read(s.buf.data() + keep * size_t(ch), 1024);
            if (n == 0 && s.loop) {
                s.decoder->rewind();
                n = s.decoder->read(s.buf.data() + keep * size_t(ch), 1024);
            }
            s.frames = keep + n;
            if (n == 0) return false;
        }
        size_t k = size_t(s.pos);
        float t = float(s.pos - double(k));
        for (int c = 0; c < 2; ++c) {
            int cc = std::min(c, ch - 1);
            float a = s.buf[k * size_t(ch) + size_t(cc)], b = s.buf[(k + 1) * size_t(ch) + size_t(cc)];
            out[c] = a + (b - a) * t;
        }
        s.pos += s.step;
        return true;
    }

    void mixSongs(float* out, int frames) {
        float volume = musicVolume_;
        const float dt = 1.0f / float(rate_);
        for (Song& s : songs_) {
            if (s.done) continue;
            for (int i = 0; i < frames; ++i) {
                if (s.gain != s.target) {
                    float d = s.fade > 0 ? s.fade * dt : 1.0f;
                    s.gain = s.gain < s.target ? std::min(s.target, s.gain + d) : std::max(s.target, s.gain - d);
                }
                if (s.target == 0 && s.gain <= 0) {
                    s.done = true;
                    break;
                }
                float f[2];
                if (!songFrame(s, f)) {
                    s.done = true;
                    break;
                }
                out[2 * i] += f[0] * s.gain * volume;
                out[2 * i + 1] += f[1] * s.gain * volume;
            }
        }
        songs_.erase(std::remove_if(songs_.begin(), songs_.end(), [](const Song& s) { return s.done; }), songs_.end());
    }

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
            const size_t ch = v.clip->channels == 2 ? 2 : 1;
            const double n = double(s.size() / ch);
            for (int i = 0; i < frames; ++i) {
                if (v.pos >= n - 1) {
                    if (!v.ambient) {
                        v.done = true;
                        break;
                    }
                    v.pos = std::fmod(v.pos, n - 1);
                }
                size_t k = size_t(v.pos) * ch;
                float t = float(v.pos - std::floor(v.pos));
                float l = s[k] + (s[k + ch] - s[k]) * t;
                if (ch == 1) {
                    out[2 * i] += l * v.left;
                    out[2 * i + 1] += l * v.right;
                } else {
                    // each side its own, at the gain a centred mono one has
                    float r = s[k + 1] + (s[k + 3] - s[k + 1]) * t;
                    out[2 * i] += l * v.left * float(M_SQRT2);
                    out[2 * i + 1] += r * v.right * float(M_SQRT2);
                }
                v.pos += v.step;
            }
        }
        mixSongs(out, frames);
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
    std::vector<Song> songs_;
    float musicVolume_ = 0.3f;
    Object* movie_ = nullptr;
    float movieStart_ = 0;
    std::map<std::string, std::shared_ptr<const DecodedSound>> movieClips_;
};

}  // namespace ffa
