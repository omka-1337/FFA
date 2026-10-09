#include "world/Session.h"

#include <cctype>
#include <fstream>
#include <functional>
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
    library = std::make_unique<Library>(gameDir);
    library->adopt(linker->packages[size_t(pkg)].get());
    animator = std::make_unique<Animator>(*world, *library);
    registerAnimationNatives(*vm);
    vm->loadObject = [this](const std::string& path) { return loadObject(path); };
    // The game: a Game= option of the URL, else the engine's default.
    gameName = ini("Default.ini", "Engine.Engine", "DefaultGame");
    for (const std::string& o : level.options) {
        options += u"?" + widen(o);
        if (o.size() > 5 && lower(o.substr(0, 5)) == "game=") gameName = o.substr(5);
    }
    gameClass = vm->findClass(gameName.substr(gameName.find('.') + 1));
}

void Session::begin() {
    // The engine makes the level's WorldInfo before play, of the class
    // Default.ini names, [Engine.Engine] WorldInfo=ShGame.ShWorldInfo: the
    // game states and their master list are in its defaults.
    std::string wiName = ini("Default.ini", "Engine.Engine", "WorldInfo");
    if (Class* wi = wiName.empty() ? nullptr : linker->findClass(wiName.substr(wiName.find('.') + 1)))
        world->var(world->info, "WorldInfo") = Value::Obj(vm->spawn(wi, Name("WorldInfo"), world->info->outer));
    world->beginPlay(gameClass, options);
    controller = world->login(widen(level.portal), options);
}

Object* Session::loadObject(const std::string& path) {
    std::string key = lower(path);
    auto it = loaded_.find(key);
    if (it != loaded_.end()) return it->second;
    Object*& slot = loaded_[key];
    std::vector<std::string> parts;
    std::stringstream ss(path);
    for (std::string part; std::getline(ss, part, '.');) parts.push_back(part);
    if (parts.size() < 2) return nullptr;
    const Package* p = library->package(parts[0]);
    if (!p) return nullptr;
    int idx = Library::findByPath(*p, std::vector<std::string>(parts.begin() + 1, parts.end()));
    if (!idx) return nullptr;
    Object* outer = nullptr;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto o = std::make_unique<Object>();
        o->name = Name(i == 0 ? p->stem : parts[i]);
        o->outer = outer;
        // a class with script, or none for a native one such as MeshAnimation
        if (i + 1 == parts.size()) o->cls = linker->findClass(p->classOf(idx));
        outer = o.get();
        owned_.push_back(std::move(o));
    }
    return slot = outer;
}

Object* Session::pawn() const { return controller ? world->obj(controller, "Pawn") : nullptr; }

std::vector<std::string> Session::commandsOf(const std::string& keyOrAlias) const {
    std::ifstream in(systemDir + "/DefUser.ini");
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    auto trim = [](std::string x) {
        size_t a = x.find_first_not_of(" \t"), b = x.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
    };
    // a key's binding, if it is a key
    std::string binding = keyOrAlias;
    for (const std::string& l : lines) {
        size_t eq = l.find('=');
        if (eq != std::string::npos && lower(l.substr(0, eq)) == lower(keyOrAlias) && l.rfind("Aliases", 0) != 0) {
            binding = l.substr(eq + 1);
            break;
        }
    }
    // commands joined with |, each an alias's own when it is one
    auto aliasOf = [&](const std::string& name) {
        for (const std::string& l : lines) {
            size_t c = l.find("Command=\""), al = l.find("Alias=");
            if (c == std::string::npos || al == std::string::npos) continue;
            std::string alias = l.substr(al + 6);
            alias = alias.substr(0, alias.find(')'));
            if (lower(alias) == lower(name)) return l.substr(c + 9, l.find('"', c + 9) - c - 9);
        }
        return std::string();
    };
    std::vector<std::string> out;
    std::function<void(const std::string&, int)> add = [&](const std::string& text, int depth) {
        std::stringstream parts(text);
        std::string part;
        while (std::getline(parts, part, '|')) {
            part = trim(part);
            if (part.empty()) continue;
            std::string expanded = depth < 4 ? aliasOf(part) : std::string();
            if (!expanded.empty() && lower(expanded) != lower(part)) add(expanded, depth + 1);
            else out.push_back(part);
        }
    };
    add(binding, 0);
    return out;
}

std::vector<std::pair<std::string, float>> Session::axesOf(const std::string& keyOrAlias) const {
    std::vector<std::pair<std::string, float>> out;
    for (const std::string& part : commandsOf(keyOrAlias)) {
        std::istringstream cmd(part);
        std::string word, axis;
        cmd >> word;
        // Count counts the input's events into a byte, Button holds a bool
        // while the key is down: SmoothMouse divides by the count
        if (lower(word) == "count" || lower(word) == "button") {
            cmd >> axis;
            if (!axis.empty()) out.emplace_back(axis, 1.0f);
            continue;
        }
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

void Session::exec(const std::string& command) {
    // the first word names an exec function of the controller, else of its
    // pawn; the rest are its arguments, not passed yet
    std::istringstream cmd(command);
    std::string name;
    cmd >> name;
    if (name.empty() || lower(name) == "axis" || lower(name) == "button" || lower(name) == "count") return;
    for (Object* o : {controller, pawn()}) {
        if (!o) continue;
        Function* fn = vm->findVirtual(o, Name(name));
        if (!fn || !(fn->flags & FUNC_Exec)) continue;
        try {
            vm->callFunction(fn, o, {});
        } catch (const std::exception&) {
        }
        return;
    }
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
