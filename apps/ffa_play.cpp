// ffa-play: a level of the game in a window.
//
//   ffa-play <System dir> <map.unr>                        play it
//   ffa-play <System dir> <map.unr> --shot <png> <seconds> run that long, save a frame, quit
//   ... --hold <key>                                       hold a key from the start, as W
//   ... --exec <seconds>=<command>                         run a command once, as BypassCutscene
//   ... --log <file>                                       the script's log, keys, frames a second,
//                                                          and where a frame that does not end is
//
// The level begins as the engine begins it (world/Session.h), the world ticks
// once a frame drawn by the time it took, the keys held are given to the player's
// controller through the game's own bindings in DefUser.ini, and the view is
// the one the controller's PlayerCalcView gives. The mouse goes through its
// bindings too, MouseX and MouseY, and the mouse buttons; Tab lets the mouse
// go. Escape is the game's: its in-game menu, or a movie skipped. A game
// controller plays as the keys and the mouse do (the game's own Joy bindings
// are all empty): the left stick moves as WASD, by how
// far it is pushed, the right stick turns the camera as the mouse does, A
// jumps (RightMouse), X punches (LeftMouse), B grabs (G), Y uses (Enter), LB
// ducks (C), RB turns to the nearest (O), Start skips a cutscene (Space), the
// pad is the arrow keys and the right stick's button zooms (MiddleMouse).
// While a menu is open the mouse is its pointer, and the pad's A, B and arrows
// are Enter, Escape and the arrows. Without a map the game begins as its own
// does, at Default.ini's LocalMap, and goes on to the levels it travels to.
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "Mixer.h"
#include "core/Library.h"
#include "render/LevelRender.h"
#include "world/Gui.h"
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
    case SDLK_ESCAPE: return "Escape";
    }
    const char* n = SDL_GetKeyName(k);
    return n && std::strlen(n) == 1 ? std::string(n) : std::string();
}

// The engine's number for a key a menu minds (EInputKey), or 0.
int guiKeyOf(SDL_Keycode k) {
    switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 13;
    case SDLK_ESCAPE: return 27;
    case SDLK_LEFT: return 37;
    case SDLK_UP: return 38;
    case SDLK_RIGHT: return 39;
    case SDLK_DOWN: return 40;
    }
    return 0;
}

// A key of a section of an .ini file, or empty.
std::string iniValue(const std::string& file, const std::string& section, const std::string& key) {
    std::ifstream in(file);
    std::string line, at;
    auto lower = [](std::string x) {
        for (char& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
        return x;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && line[0] == '[' && line.back() == ']') {
            at = lower(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (at == lower(section) && eq != std::string::npos && lower(line.substr(0, eq)) == lower(key))
            return line.substr(eq + 1);
    }
    return {};
}

// A travel URL's level, Book_FrontEnd.unr?GameState=GSTATE000: its file in
// Maps/, the extension added when it has none, and its options. A path that
// is a file is taken as it is.
std::string levelPath(const std::string& sys, const std::string& url, std::string& options) {
    namespace fs = std::filesystem;
    size_t q = url.find('?');
    std::string name = url.substr(0, q);
    options = q == std::string::npos ? std::string() : url.substr(q);
    if (fs::is_regular_file(name)) return name;
    if (fs::path(name).extension().empty()) name += ".unr";
    return (fs::path(sys) / ".." / "Maps" / name).string();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ffa-play <game dir> [map.unr] [--shot <png> <seconds>] ...\n");
        return 2;
    }
    // the game's directory, or its System directory as before
    namespace fs = std::filesystem;
    std::string sys = argv[1];
    if (fs::is_directory(fs::path(sys) / "System")) sys = (fs::path(sys) / "System").string();
    int first = 2;
    std::string map;
    if (argc > 2 && argv[2][0] != '-') {
        map = argv[2];
        first = 3;
    }
    const char* shot = nullptr;
    float shotAt = 0;
    std::vector<std::string> holdKeys;
    std::vector<std::pair<float, std::string>> execs;
    const char* logPath = nullptr;
    for (int i = first; i < argc; ++i) {
        if (std::string(argv[i]) == "--shot" && i + 2 < argc) {
            shot = argv[i + 1];
            shotAt = std::stof(argv[i + 2]);
        }
        if (std::string(argv[i]) == "--hold" && i + 1 < argc) holdKeys.push_back(argv[i + 1]);
        if (std::string(argv[i]) == "--log" && i + 1 < argc) logPath = argv[i + 1];
        if (std::string(argv[i]) == "--exec" && i + 1 < argc) {
            std::string e = argv[i + 1];
            size_t eq = e.find('=');
            if (eq != std::string::npos) execs.emplace_back(std::stof(e.substr(0, eq)), e.substr(eq + 1));
        }
    }
    try {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) throw std::runtime_error(SDL_GetError());
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

        // The level to begin with: the one asked for, else the game's own
        // first, Default.ini's LocalMap, SH2_Preamble.unr, whose logos go on
        // to the menu. A level the game travels to replaces the one playing.
        std::string url = map.empty() ? iniValue(sys + "/Default.ini", "URL", "LocalMap") : map;
        if (url.empty()) throw std::runtime_error("no map given, and Default.ini names no LocalMap");
        bool running = true;
        FILE* logFile = logPath ? std::fopen(logPath, "w") : nullptr;
        std::mutex logLock;
        auto logLine = [&](const std::string& line) {
            if (!logFile) return;
            std::lock_guard<std::mutex> g(logLock);
            std::fprintf(logFile, "%s\n", line.c_str());
        };
        SDL_GameController* pad = nullptr;
        auto openPad = [&] {
            for (int i = 0; !pad && i < SDL_NumJoysticks(); ++i)
                if (SDL_IsGameController(i) && (pad = SDL_GameControllerOpen(i)))
                    std::printf("controller          %s\n", SDL_GameControllerName(pad));
        };
        openPad();
        while (running && !url.empty()) {
            std::string options;
            std::string mapPath = levelPath(sys, url, options);
            url.clear();
            Session session(sys, mapPath, options);
            // --log: what script writes, the keys, once a second the frames and the
            // player, and from a watchdog where a frame that does not end is
            World* worldPtr = session.world.get();
            auto stamp = [&]() {
                char t[32];
                std::snprintf(t, sizeof t, "%8.2f  ", worldPtr->time);
                return std::string(t);
            };
            if (logFile) session.vm->sink = [&](const std::string& tag, const std::string& text) { logLine(stamp() + tag + "  " + text); };
            Library lib(sys + "/..");
            lib.adopt(session.linker->packages[size_t(session.pkg)].get());
            // the sound, before play begins, so that the level's first sounds play
            Mixer mixer(*session.world);
            if (mixer.open()) session.world->audio = &mixer;
            else std::printf("sound               none: %s\n", SDL_GetError());
            session.begin();
            LevelRender render(session, lib);
            std::printf("drawing             %zu triangles: %zu BSP batches, %zu meshes; %zu textures (%zu not decoded), "
                        "%zu lightmaps\n",
                        render.triangles, render.batches, render.meshes, render.textures, render.missingTextures,
                        render.lightMaps);
            std::printf("meshes relit        %zu whose baked light was black, from their lights and masks\n", render.relit);

            World& w = *session.world;
            std::atomic<uint64_t> framesDone{0};
            std::atomic<bool> quitting{false};
            std::thread watchdog;
            if (logFile)
                watchdog = std::thread([&] {
                    uint64_t seen = 0;
                    auto since = std::chrono::steady_clock::now();
                    int reported = 0;
                    while (!quitting) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                        uint64_t now = framesDone;
                        if (now != seen) {
                            seen = now;
                            since = std::chrono::steady_clock::now();
                            reported = 0;
                            continue;
                        }
                        double stuck = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
                        // after 3 seconds, and every 10 after: where the frame is,
                        // read while it runs, which a frame stuck in place allows
                        if (stuck < 3 + 10 * reported) continue;
                        ++reported;
                        Object* a = w.ticking;
                        std::string where = a ? a->path() + " (" + a->cls->name.str() + "), state " +
                                                    (a->state ? a->state->name.str() : std::string("none"))
                                              : std::string("no actor");
                        logLine(stamp() + "HANG  frame not done for " + std::to_string(int(stuck)) + " s, at " + where +
                                ", in " + w.tickPart);
                        for (const std::string& f : session.vm->trace()) logLine("          script  " + f);
                        std::lock_guard<std::mutex> g(logLock);
                        std::fflush(logFile);
                    }
                });
            std::map<std::string, size_t> failuresSeen;
            std::map<std::string, std::vector<std::pair<std::string, float>>> bindings;
            std::set<std::string> down(holdKeys.begin(), holdKeys.end());
            const float step = 1.0f / 30.0f;
            Uint64 last = SDL_GetPerformanceCounter();
            double behind = 0;
            float mouseX = 0, mouseY = 0;
            bool menuWasOpen = false;
            float played = 0;
            Uint64 lastMouse = SDL_GetPerformanceCounter();
            if (!shot) SDL_SetRelativeMouseMode(SDL_TRUE);
            int drawn = 0;
            Uint64 titled = SDL_GetPerformanceCounter();
            while (running && w.travel.empty() && !w.quit) {
                SDL_Event e;
                while (SDL_PollEvent(&e)) {
                    if (e.type == SDL_QUIT) running = false;
                    if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) openPad();
                    if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad &&
                        e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
                        SDL_GameControllerClose(pad);
                        pad = nullptr;
                        openPad();
                    }
                    // An open menu takes the mouse, the keys it minds and the pad
                    // (world/Gui.cpp): the pad's A is Enter, B and Start Escape,
                    // its arrows the arrows, which move between the buttons.
                    if (guiActive(w)) {
                        if (e.type == SDL_MOUSEMOTION) guiMouse(w, float(e.motion.x), float(e.motion.y));
                        if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.button == SDL_BUTTON_LEFT)
                            guiKey(w, 1, e.type == SDL_MOUSEBUTTONDOWN ? 1 : 3);
                        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat)
                            if (int k = guiKeyOf(e.key.keysym.sym)) guiKey(w, k, e.type == SDL_KEYDOWN ? 1 : 3);
                        if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
                            int k = 0;
                            switch (e.cbutton.button) {
                            case SDL_CONTROLLER_BUTTON_A: k = 13; break;
                            case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_START: k = 27; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k = 37; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_UP: k = 38; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k = 39; break;
                            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k = 40; break;
                            default: break;
                            }
                            if (k) guiKey(w, k, e.type == SDL_CONTROLLERBUTTONDOWN ? 1 : 3);
                        }
                        continue;
                    }
                    if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_TAB) {
                        // Tab lets the mouse go and takes it again
                        SDL_SetRelativeMouseMode(SDL_GetRelativeMouseMode() ? SDL_FALSE : SDL_TRUE);
                        continue;
                    }
                    std::string pressed, released;
                    if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
                        std::string k = engineKey(e.key.keysym.sym);
                        (e.type == SDL_KEYDOWN ? pressed : released) = k;
                    }
                    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                        std::string k = e.button.button == SDL_BUTTON_LEFT ? "LeftMouse"
                                        : e.button.button == SDL_BUTTON_RIGHT ? "RightMouse" : "MiddleMouse";
                        (e.type == SDL_MOUSEBUTTONDOWN ? pressed : released) = k;
                    }
                    if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP) {
                        const char* k = nullptr;
                        switch (e.cbutton.button) {
                        case SDL_CONTROLLER_BUTTON_A: k = "RightMouse"; break;
                        case SDL_CONTROLLER_BUTTON_X: k = "LeftMouse"; break;
                        case SDL_CONTROLLER_BUTTON_B: k = "G"; break;
                        case SDL_CONTROLLER_BUTTON_Y: k = "Enter"; break;
                        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: k = "C"; break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: k = "O"; break;
                        case SDL_CONTROLLER_BUTTON_START: k = "Space"; break;
                        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: k = "MiddleMouse"; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_UP: k = "Up"; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: k = "Down"; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: k = "Left"; break;
                        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: k = "Right"; break;
                        default: break;
                        }
                        if (k) (e.type == SDL_CONTROLLERBUTTONDOWN ? pressed : released) = k;
                    }
                    if (e.type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode()) {
                        mouseX += float(e.motion.xrel);
                        mouseY -= float(e.motion.yrel);     // the engine's up is positive
                    }
                    if (!pressed.empty()) {
                        if (logFile) {
                            std::string cs;
                            for (const std::string& c : session.commandsOf(pressed)) cs += " " + c;
                            logLine(stamp() + "key  " + pressed + " ->" + cs);
                        }
                        down.insert(pressed);
                        // what is not an axis runs once, on the press
                        for (const std::string& c : session.commandsOf(pressed)) session.exec(c);
                    }
                    if (!released.empty()) down.erase(released);
                    if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                        width = e.window.data1;
                        height = e.window.data2;
                    }
                }
                // the mouse is the GUI's pointer while a menu is open, and the
                // camera's otherwise
                if (!shot) {
                    bool menu = guiActive(w);
                    if (menu != menuWasOpen) {
                        SDL_SetRelativeMouseMode(menu ? SDL_FALSE : SDL_TRUE);
                        if (menu) {
                            int mx = 0, my = 0;
                            SDL_GetMouseState(&mx, &my);
                            guiMouse(w, float(mx), float(my));
                        }
                        down.clear();
                        menuWasOpen = menu;
                    }
                }
                // the axes of the keys held, by their bindings, and the mouse's
                // movement this frame through its own, MouseX and MouseY
                w.held.clear();
                // The mouse's movement as a rate: a frame's counts over the frame's
                // time, scaled so that 600 counts a second turn Shrek's camera
                // some 90 degrees a second. The scale is tuned, not the engine's:
                // how many counts its input system gives a pixel is in its native
                // code.
                Uint64 nowMouse = SDL_GetPerformanceCounter();
                float frameTime = std::max(0.001f, float(double(nowMouse - lastMouse) / double(SDL_GetPerformanceFrequency())));
                lastMouse = nowMouse;
                // The sticks, past a dead zone of a fifth: the left one as the
                // movement keys, by how far it is pushed; the right one as the
                // mouse, 1200 counts a second at its full, which turns Shrek's
                // camera some 180 degrees a second.
                if (pad && !menuWasOpen) {
                    auto stick = [&](SDL_GameControllerAxis ax) {
                        float v = float(SDL_GameControllerGetAxis(pad, ax)) / 32767.0f;
                        if (std::fabs(v) < 0.2f) return 0.0f;
                        return std::clamp((v - std::copysign(0.2f, v)) / 0.8f, -1.0f, 1.0f);
                    };
                    float lx = stick(SDL_CONTROLLER_AXIS_LEFTX), ly = stick(SDL_CONTROLLER_AXIS_LEFTY);
                    float rx = stick(SDL_CONTROLLER_AXIS_RIGHTX), ry = stick(SDL_CONTROLLER_AXIS_RIGHTY);
                    if (ly != 0) w.held.emplace_back("aBaseY", -ly * 1200.0f);
                    if (lx != 0) w.held.emplace_back("aStrafe", lx * 1200.0f);
                    mouseX += rx * 1200.0f * frameTime;
                    mouseY -= ry * 1200.0f * frameTime;
                }
                mouseX *= 0.2f / frameTime;
                mouseY *= 0.2f / frameTime;
                if (mouseX != 0 || mouseY != 0) {
                    static std::vector<std::pair<std::string, float>> mx = session.axesOf("MouseX"), my = session.axesOf("MouseY");
                    // an axis scaled by the movement, a count once
                    for (auto& [axis, speed] : mx)
                        if (mouseX != 0) w.held.emplace_back(axis, axis[0] == 'b' ? speed : speed * mouseX);
                    for (auto& [axis, speed] : my)
                        if (mouseY != 0) w.held.emplace_back(axis, axis[0] == 'b' ? speed : speed * mouseY);
                    mouseX = mouseY = 0;
                }
                for (const std::string& k : down) {
                    auto it = bindings.find(k);
                    if (it == bindings.end()) it = bindings.emplace(k, session.axesOf(k)).first;
                    w.held.insert(w.held.end(), it->second.begin(), it->second.end());
                }
                Uint64 now = SDL_GetPerformanceCounter();
                behind += double(now - last) / double(SDL_GetPerformanceFrequency());
                last = now;
                // The engine ticks once a frame drawn, by the time the frame took,
                // as long as it is not too long; a shot runs at thirty frames a
                // second exactly, as fast as it can.
                float dt = shot ? step : float(std::min(behind, 0.1));
                behind = 0;
                // the time played, which runs on while the game is paused, for
                // --exec and --shot
                played += dt;
                for (auto& [t, command] : execs)
                    if (!command.empty() && played >= t) {
                        session.exec(command);
                        command.clear();
                    }
                w.tick(dt);
                ++framesDone;
                Vec3 loc;
                int32_t rot[3] = {0, 0, 0};
                session.view(loc, rot);
                mixer.update(loc, rot);
                float fov = 85;
                if (session.controller) fov = w.var(session.controller, "FovAngle").f();
                if (fov < 10 || fov > 170) fov = 85;
                render.draw(loc, rot, width, height, fov);
                if (shot && played >= shotAt) {
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
                    if (logFile) {
                        Object* pawn = session.pawn();
                        std::string who = "no pawn";
                        if (pawn) {
                            float x, y, z;
                            w.vm.unvector(w.var(pawn, "Location"), x, y, z);
                            char at[160];
                            std::snprintf(at, sizeof at, "%s at (%.0f, %.0f, %.0f), state %s", pawn->name.str().c_str(), x, y, z,
                                          pawn->state ? pawn->state->name.str().c_str() : "none");
                            who = at;
                            if (pawn->cls->findProp(Name("Health"))) who += ", health " + std::to_string(int(w.var(pawn, "Health").f()));
                        }
                        logLine(stamp() + "frame  " + std::to_string(drawn) + " fps, " + who);
                        for (auto& [msg, n] : w.failures)
                            if (failuresSeen[msg] != n) {
                                logLine(stamp() + "ERROR  " + msg + " (" + std::to_string(n) + " times)");
                                failuresSeen[msg] = n;
                            }
                        std::lock_guard<std::mutex> g(logLock);
                        std::fflush(logFile);
                    }
                    drawn = 0;
                    titled = t;
                }
            }
            quitting = true;
            if (watchdog.joinable()) watchdog.join();
            if (w.quit) running = false;
            if (!w.travel.empty() && !w.quit) {
                url = w.travel;
                std::printf("travel              %s\n", url.c_str());
                if (logFile) logLine(stamp() + "travel  " + url);
            }
        }
        if (logFile) std::fclose(logFile);
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(win);
        SDL_Quit();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
    return 0;
}
