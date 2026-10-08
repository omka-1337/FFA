#include "world/Session.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "world/Physics.h"

namespace ffa {

namespace {

std::string lower(std::string x) {
    for (char& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return x;
}

}  // namespace

std::string Session::ini(const std::string& file, const std::string& section, const std::string& key) const {
    std::ifstream in(systemDir + "/" + file);
    std::string line, at;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 1 && line[0] == '[') {
            at = lower(line.substr(1, line.find(']') - 1));
            continue;
        }
        size_t eq = line.find('=');
        if (at == lower(section) && eq != std::string::npos && lower(line.substr(0, eq)) == lower(key))
            return line.substr(eq + 1);
    }
    return std::string();
}

Session::Session(const std::string& sys, const std::string& map) : systemDir(sys), gameDir(sys + "/..") {
    std::vector<std::string> paths = Linker::packageFiles(sys);
    paths.push_back(map);
    linker = std::make_unique<Linker>(paths);
    std::string stem = map.substr(map.find_last_of("/\\") + 1);
    stem = stem.substr(0, stem.find_last_of('.'));
    pkg = linker->packageIndex(stem);
    if (pkg < 0) throw std::runtime_error("the level " + stem + " did not load");
    level = readLevel(*linker->packages[size_t(pkg)]);
    vm = std::make_unique<VM>(*linker);
    vm->sink = [](const std::string&, const std::string&) {};
    registerWorldNatives(*vm);
    registerCollisionNatives(*vm);
    registerPhysicsNatives(*vm);
    world = std::make_unique<World>(*vm, pkg, level);
    collision = std::make_unique<Collision>(*world, pkg, level.model, gameDir);
    world->collision = collision.get();
    // The game: a Game= option of the URL, else the engine's default.
    gameName = ini("Default.ini", "Engine.Engine", "DefaultGame");
    for (const std::string& o : level.options) {
        options += u"?" + widen(o);
        if (o.size() > 5 && lower(o.substr(0, 5)) == "game=") gameName = o.substr(5);
    }
    gameClass = vm->findClass(gameName.substr(gameName.find('.') + 1));
}

void Session::begin() {
    world->beginPlay(gameClass, options);
    controller = world->login(widen(level.portal), options);
}

Object* Session::pawn() const { return controller ? world->obj(controller, "Pawn") : nullptr; }

std::vector<std::pair<std::string, float>> Session::axesOf(const std::string& keyOrAlias) const {
    std::vector<std::pair<std::string, float>> out;
    std::ifstream in(systemDir + "/DefUser.ini");
    std::string line, binding = keyOrAlias;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    // a key's binding, if it is a key
    for (const std::string& l : lines) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && lower(l.substr(0, eq)) == lower(keyOrAlias) && l.rfind("Aliases", 0) != 0) {
            binding = l.substr(eq + 1);
            break;
        }
    }
    // an alias's command, if it is an alias
    std::string command = binding;
    for (const std::string& l : lines) {
        size_t c = l.find("Command=\""), al = l.find("Alias=");
        if (c == std::string::npos || al == std::string::npos) continue;
        std::string alias = l.substr(al + 6);
        alias = alias.substr(0, alias.find(')'));
        if (lower(alias) == lower(binding)) {
            command = l.substr(c + 9, l.find('"', c + 9) - c - 9);
            break;
        }
    }
    // commands joined with |; the axes among them
    std::stringstream parts(command);
    std::string part;
    while (std::getline(parts, part, '|')) {
        std::istringstream cmd(part);
        std::string word, axis;
        cmd >> word;
        if (lower(word) != "axis") continue;
        cmd >> axis;
        float speed = 1, base = 0, invert = 1;
        while (cmd >> word) {
            std::string w = lower(word);
            if (w.rfind("speed=", 0) == 0) speed = std::stof(word.substr(6));
            else if (w.rfind("speedbase=", 0) == 0) base = std::stof(word.substr(10));
            else if (w.rfind("invert=", 0) == 0) invert = std::stof(word.substr(7));
        }
        out.emplace_back(axis, (base != 0 ? base : speed) * invert);
    }
    return out;
}

bool Session::view(Vec3& location, int32_t rotation[3]) {
    if (!controller) return false;
    std::vector<Value> args = {Value::Obj(nullptr), vm->vector(0, 0, 0), vm->rotator(0, 0, 0)};
    vm->eventOut(controller, "PlayerCalcView", args);
    vm->unvector(args[1], location.x, location.y, location.z);
    vm->unrotator(args[2], rotation[0], rotation[1], rotation[2]);
    return true;
}

}  // namespace ffa
