// A level played: its packages linked, the VM with the engine's natives, the
// world and its collision, the game begun and the local player logged in,
// the way the engine loads a map. ffa-script's start and run and the game's
// window share it.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/Library.h"
#include "script/Linker.h"
#include "script/VM.h"
#include "world/Animator.h"
#include "world/Collision.h"
#include "world/Level.h"
#include "world/World.h"

namespace ffa {

class Session {
public:
    // systemDir: the game's System directory; map: a .unr file.
    Session(const std::string& systemDir, const std::string& map);

    // Spawn the game, InitGame, the start up events, and the player's login.
    void begin();

    std::string systemDir, gameDir;
    std::unique_ptr<Linker> linker;
    int pkg = -1;
    LevelRecord level;
    std::unique_ptr<VM> vm;
    std::unique_ptr<World> world;
    std::unique_ptr<Collision> collision;
    std::unique_ptr<Library> library;
    std::unique_ptr<Animator> animator;

    std::string gameName;           // Game= of the URL, else Default.ini's DefaultGame
    Class* gameClass = nullptr;
    String options;                 // the URL's options, ?a=b?c=d
    Object* controller = nullptr;   // the local player's, once logged in
    Object* pawn() const;

    // The axes a key or an alias of DefUser.ini moves, with their speeds:
    // W=MoveForward and Aliases[n]=(Command="Axis aBaseY Speed=+1200.0",
    // Alias=MoveForward), or Up=Axis aArrowUp SpeedBase=0.7 directly.
    std::vector<std::pair<std::string, float>> axesOf(const std::string& keyOrAlias) const;
    // Every command a key or alias stands for, aliases within expanded.
    std::vector<std::string> commandsOf(const std::string& keyOrAlias) const;
    // Run a console command as a key pressed would: an exec function.
    void exec(const std::string& command);

    // Where the player looks from, by the controller's own PlayerCalcView.
    bool view(Vec3& location, int32_t rotation[3]);

    // An object of any package of the game, by its path and, when given, the
    // class it must be of, made for script: its outers as plain objects, its
    // class the linker's of that name.
    Object* loadObject(const std::string& path, const Class* want = nullptr);
    void eachLoaded(const std::function<void(Object*)>& f) const {
        for (auto& [k, o] : loaded_)
            if (o) f(o);
    }

    // Localize: a key of a section of System/<package>.int, the package a path
    // under System such as Cutscenes\SwampIntro, its case not minded.
    bool localize(const std::string& section, const std::string& key, const std::string& package, String& out);

    // A key of an .ini file in the System directory, or empty.
    std::string ini(const std::string& file, const std::string& section, const std::string& key) const;

private:
    std::map<std::string, Object*> loaded_;
    // localisation files read, by lowercase package: section/key to value
    std::map<std::string, std::map<std::string, String>> int_;
    std::vector<std::unique_ptr<Object>> owned_;
};

}  // namespace ffa
