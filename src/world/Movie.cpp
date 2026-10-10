#include "world/Movie.h"

#include <cctype>
#include <cstdio>
#include <filesystem>

namespace ffa {

namespace {

World* worldOf(NativeCall& c) { return World::of(c.vm); }

// A file of Movies/, its name's case not minded: the game asks for
// KWlogo.bik, and the disc has KWLogo.bik.
std::string movieFile(const World& w, const std::string& name) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(w.gameDir) / "Movies";
    std::error_code ec;
    std::string want = name;
    for (char& ch : want) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string f = e.path().filename().string();
        for (char& ch : f) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (f == want) return e.path().string();
    }
    return {};
}

uint32_t le32(const unsigned char* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Frames at 8, width and height at 20 and 24, the frame rate as a fraction at
// 28 and 32.
bool header(const std::string& path, World::MoviePlay& m) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[36];
    bool ok = std::fread(h, 1, sizeof h, f) == sizeof h && h[0] == 'B' && h[1] == 'I' && h[2] == 'K';
    std::fclose(f);
    if (!ok) return false;
    uint32_t frames = le32(h + 8), num = le32(h + 28), den = le32(h + 32);
    m.width = int(le32(h + 20));
    m.height = int(le32(h + 24));
    m.length = num ? float(double(frames) * double(den) / double(num)) : 0;
    return true;
}

World::MoviePlay* playOf(NativeCall& c) {
    World* w = worldOf(c);
    if (!w) return nullptr;
    auto it = w->movies.find(c.self);
    return it == w->movies.end() ? nullptr : &it->second;
}

void end(World& w, Object* movie) {
    auto it = w.movies.find(movie);
    if (it == w.movies.end() || !it->second.playing) return;
    it->second.playing = false;
    w.vm.event(movie, "MovieEnded");
}

}  // namespace

void giveMovie(World& w, Object* hud) {
    Class* hudClass = w.vm.findClass("HUD");
    if (!hudClass || !hud->isA(hudClass) || w.obj(hud, "Movie")) return;
    if (Class* mc = w.vm.findClass("Movie")) w.var(hud, "Movie") = Value::Obj(w.vm.spawn(mc, Name(), hud->outer));
}

void movieTick(World& w) {
    std::vector<Object*> ended;
    for (auto& [o, m] : w.movies)
        if (m.playing && m.pausedAt < 0 && !m.loop && w.time - m.start >= m.length) ended.push_back(o);
    for (Object* o : ended) end(w, o);
}

void registerMovieNatives(VM& vm) {
    auto& n = vm.natives;
    // Play(MovieFilename, UseSound, LoopMovie)
    n["movie.play"] = [](NativeCall& c) {
        World* w = worldOf(c);
        if (!w) return Value();
        World::MoviePlay m;
        m.file = utf8(c.s(0));
        m.loop = c.b(2);
        m.start = w->time;
        std::string path = movieFile(*w, m.file);
        // one that is not there ends at once, as a movie of no length
        if (path.empty() || !header(path, m)) m.length = 0;
        m.playing = true;
        w->movies[c.self] = m;
        return Value();
    };
    n["movie.stopnow"] = [](NativeCall& c) {
        if (World* w = worldOf(c)) end(*w, c.self);
        return Value();
    };
    n["movie.stopatend"] = [](NativeCall& c) {
        if (World::MoviePlay* m = playOf(c)) m->loop = false;
        return Value();
    };
    n["movie.pause"] = [](NativeCall& c) {
        World* w = worldOf(c);
        World::MoviePlay* m = playOf(c);
        if (!m) return Value();
        if (c.b(0) && m->pausedAt < 0) {
            m->pausedAt = w->time;
        } else if (!c.b(0) && m->pausedAt >= 0) {
            m->start += w->time - m->pausedAt;
            m->pausedAt = -1;
        }
        return Value();
    };
    n["movie.isplaying"] = [](NativeCall& c) {
        World::MoviePlay* m = playOf(c);
        return Value::Bool(m && m->playing);
    };
    n["movie.ispaused"] = [](NativeCall& c) {
        World::MoviePlay* m = playOf(c);
        return Value::Bool(m && m->pausedAt >= 0);
    };
    n["movie.getwidth"] = [](NativeCall& c) {
        World::MoviePlay* m = playOf(c);
        return Value::Int(m ? m->width : 0);
    };
    n["movie.getheight"] = [](NativeCall& c) {
        World::MoviePlay* m = playOf(c);
        return Value::Int(m ? m->height : 0);
    };
}

}  // namespace ffa
