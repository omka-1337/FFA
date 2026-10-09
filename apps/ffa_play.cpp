// ffa-play: a level of the game in a window.
//
//   ffa-play <System dir> <map.unr>                        play it
//   ffa-play <System dir> <map.unr> --shot <png> <seconds> run that long, save a frame, quit
//   ... --hold <key>                                       hold a key from the start, as W
//
// The level begins as the engine begins it (world/Session.h), the world ticks
// at a fixed thirty frames a second, the keys held are given to the player's
// controller through the game's own bindings in DefUser.ini, and the view is
// the one the controller's PlayerCalcView gives. Escape quits.
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "core/Library.h"
#include "render/LevelRender.h"
#include "world/Session.h"

using namespace ffa;

namespace {

// The engine's name for an SDL key, as DefUser.ini binds them: letters and
// digits as they are, the arrows Up, Down, Left, Right, and a few others.
std::string engineKey(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return "Up";
    case SDLK_DOWN: return "Down";
    case SDLK_LEFT: return "Left";
    case SDLK_RIGHT: return "Right";
    case SDLK_SPACE: return "Space";
    case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
    case SDLK_LCTRL: case SDLK_RCTRL: return "Ctrl";
    case SDLK_LALT: case SDLK_RALT: return "Alt";
    case SDLK_RETURN: return "Enter";
    case SDLK_TAB: return "Tab";
    }
    const char* n = SDL_GetKeyName(k);
    return n && std::strlen(n) == 1 ? std::string(n) : std::string();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ffa-play <System dir> <map.unr> [--shot <png> <seconds>]\n");
        return 2;
    }
    std::string sys = argv[1], map = argv[2];
    const char* shot = nullptr;
    float shotAt = 0;
    std::vector<std::string> holdKeys;
    for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "--shot" && i + 2 < argc) {
            shot = argv[i + 1];
            shotAt = std::stof(argv[i + 2]);
        }
        if (std::string(argv[i]) == "--hold" && i + 1 < argc) holdKeys.push_back(argv[i + 1]);
    }
    try {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        int width = 1280, height = 720;
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (shot ? SDL_WINDOW_HIDDEN : 0);
        SDL_Window* win = SDL_CreateWindow("Far Far Away", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width,
                                           height, flags);
        if (!win) throw std::runtime_error(SDL_GetError());
        SDL_GLContext gl = SDL_GL_CreateContext(win);
        if (!gl) throw std::runtime_error(SDL_GetError());
        SDL_GL_SetSwapInterval(1);
        std::printf("GL %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

        Session session(sys, map);
        Library lib(sys + "/..");
        lib.adopt(session.linker->packages[size_t(session.pkg)].get());
        session.begin();
        LevelRender render(session, lib);
        std::printf("drawing             %zu triangles: %zu BSP batches, %zu meshes; %zu textures (%zu not decoded), "
                    "%zu lightmaps\n",
                    render.triangles, render.batches, render.meshes, render.textures, render.missingTextures,
                    render.lightMaps);
        std::printf("characters          drawn as they come into view\n");

        World& w = *session.world;
        std::map<std::string, std::vector<std::pair<std::string, float>>> bindings;
        std::set<std::string> down(holdKeys.begin(), holdKeys.end());
        const float step = 1.0f / 30.0f;
        Uint64 last = SDL_GetPerformanceCounter();
        double behind = 0;
        bool running = true;
        int drawn = 0;
        Uint64 titled = SDL_GetPerformanceCounter();
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) running = false;
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
                if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                    std::string k = engineKey(e.key.keysym.sym);
                    if (k.empty()) continue;
                    if (e.type == SDL_KEYDOWN) down.insert(k);
                    else down.erase(k);
                }
                if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    width = e.window.data1;
                    height = e.window.data2;
                }
            }
            // the axes of the keys held, by their bindings
            w.held.clear();
            for (const std::string& k : down) {
                auto it = bindings.find(k);
                if (it == bindings.end()) it = bindings.emplace(k, session.axesOf(k)).first;
                w.held.insert(w.held.end(), it->second.begin(), it->second.end());
            }
            Uint64 now = SDL_GetPerformanceCounter();
            behind += double(now - last) / double(SDL_GetPerformanceFrequency());
            last = now;
            if (shot) behind = step;    // a shot runs as fast as it can, frame by frame
            for (int n = 0; behind >= step && n < 5; ++n) {
                w.tick(step);
                behind -= step;
            }
            Vec3 loc;
            int32_t rot[3] = {0, 0, 0};
            session.view(loc, rot);
            float fov = 85;
            if (session.controller) fov = w.var(session.controller, "FovAngle").f();
            if (fov < 10 || fov > 170) fov = 85;
            render.draw(loc, rot, width, height, fov);
            if (shot && w.time >= shotAt) {
                writePng(shot, readFramebuffer(width, height));
                std::printf("shot                %s at %.2f s, camera (%.0f, %.0f, %.0f) rotation (%d, %d, %d), fov %.0f\n",
                            shot, w.time, loc.x, loc.y, loc.z, rot[0], rot[1], rot[2], fov);
                running = false;
            }
            SDL_GL_SwapWindow(win);
            // frames a second in the title, once a second
            ++drawn;
            Uint64 t = SDL_GetPerformanceCounter();
            if (double(t - titled) / double(SDL_GetPerformanceFrequency()) >= 1.0) {
                char title[128];
                std::snprintf(title, sizeof title, "Far Far Away - %s - %d fps", session.level.map.c_str(), drawn);
                SDL_SetWindowTitle(win, title);
                drawn = 0;
                titled = t;
            }
        }
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
