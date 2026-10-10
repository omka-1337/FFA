#include "world/Session.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>

#include "world/AI.h"
#include "world/Gui.h"
#include "world/Karma.h"
#include "world/Movie.h"
#include "world/Audio.h"
#include "world/Physics.h"

namespace ffa {

namespace {

std::string lower(std::string x) {
    for (char& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return x;
}

}  // namespace

void Session::setIni(const std::string& section, const std::string& key, const std::string& value) {
    set_[lower(section) + "/" + lower(key)] = value;
}

std::string Session::ini(const std::string& file, const std::string& section, const std::string& key) const {
    if (auto it = set_.find(lower(section) + "/" + lower(key)); it != set_.end()) return it->second;
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

bool Session::localize(const std::string& section, const std::string& key, const std::string& package, String& out) {
    std::string pkg = lower(package);
    auto it = int_.find(pkg);
    if (it == int_.end()) {
        auto& table = int_[pkg];
        // the file, each part of its path matched without minding case
        namespace fs = std::filesystem;
        fs::path at = systemDir;
        std::stringstream parts(pkg + ".int");
        bool found = true;
        for (std::string part; found && std::getline(parts, part, '\\');) {
            found = false;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(at, ec))
                if (lower(e.path().filename().string()) == part) {
                    at = e.path();
                    found = true;
                    break;
                }
        }
        std::string bytes;
        if (found) {
            std::ifstream in(at, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), {});
        }
        if (bytes.size() > (8u << 20)) bytes.clear();       // no localisation file is near this
        // UTF-16 with its byte order mark, else Latin-1
        String text;
        if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFF && uint8_t(bytes[1]) == 0xFE)
            for (size_t i = 2; i + 1 < bytes.size(); i += 2) text += char16_t(uint8_t(bytes[i]) | uint8_t(bytes[i + 1]) << 8);
        else
            for (char ch : bytes) text += char16_t(uint8_t(ch));
        std::string at2;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(u'\n', pos);
            if (end == String::npos) end = text.size();
            String line = text.substr(pos, end - pos);
            pos = end + 1;
            if (!line.empty() && line.back() == u'\r') line.pop_back();
            if (line.size() > 1 && line[0] == u'[') {
                at2 = lower(utf8(line.substr(1, line.find(u']') - 1)));
                continue;
            }
            size_t eq = line.find(u'=');
            if (eq == String::npos) continue;
            String value = line.substr(eq + 1);
            if (value.size() >= 2 && value.front() == u'"' && value.back() == u'"') value = value.substr(1, value.size() - 2);
            table.emplace(at2 + "/" + lower(utf8(line.substr(0, eq))), value);
        }
        it = int_.find(pkg);
    }
    auto v = it->second.find(lower(section) + "/" + lower(key));
    if (v == it->second.end()) return false;
    out = v->second;
    return true;
}

Session::Session(const std::string& sys, const std::string& map, const std::string& travel)
    : systemDir(sys), gameDir(sys + "/..") {
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
    registerAINatives(*vm);
    registerKarmaNatives(*vm);
    registerAudioNatives(*vm);
    registerMovieNatives(*vm);
    registerGuiNatives(*vm);
    world = std::make_unique<World>(*vm, pkg, level);
    world->gameDir = gameDir;
    world->mapFile = map.substr(map.find_last_of("/\\") + 1);
    world->config = [this](const std::string& section, const std::string& key) {
        std::string v = ini("Default.ini", section, key);
        return v.empty() ? ini("DefUser.ini", section, key) : v;
    };
    world->configSet = [this](const std::string& section, const std::string& key, const std::string& value) {
        setIni(section, key, value);
    };
    collision = std::make_unique<Collision>(*world, pkg, level.model, gameDir);
    world->collision = collision.get();
    library = std::make_unique<Library>(gameDir);
    collision->library = library.get();
    sounds = std::make_unique<SoundBank>(*library);
    world->sounds = sounds.get();
    library->adopt(linker->packages[size_t(pkg)].get());
    animator = std::make_unique<Animator>(*world, *library);
    registerAnimationNatives(*vm);
    vm->loadObject = [this](const std::string& path, const Class* want) { return loadObject(path, want); };
    vm->localize = [this](const std::string& s, const std::string& k, const std::string& p, String& out) {
        return localize(s, k, p, out);
    };
    // The game: a Game= option of the URL, else the engine's default.
    gameName = ini("Default.ini", "Engine.Engine", "DefaultGame");
    // the travel's options over the level's, by name
    std::vector<std::string> opts = level.options, given;
    std::stringstream ts(travel);
    for (std::string o; std::getline(ts, o, '?');)
        if (!o.empty()) given.push_back(o);
    auto key = [](const std::string& o) { return lower(o.substr(0, o.find('='))); };
    for (const std::string& g : given) {
        opts.erase(std::remove_if(opts.begin(), opts.end(), [&](const std::string& o) { return key(o) == key(g); }),
                   opts.end());
        opts.push_back(g);
    }
    for (const std::string& o : opts) {
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
    // the player's GUIController, as the engine makes it with the viewport
    setupGui(*world, ini("Default.ini", "Engine.Engine", "GUIController"));
}

Object* Session::loadObject(const std::string& path, const Class* want) {
    std::string key = lower(path) + (want ? "/" + lower(want->name.str()) : std::string());
    auto it = loaded_.find(key);
    if (it != loaded_.end()) return it->second;
    Object*& slot = loaded_[key];
    std::vector<std::string> parts;
    std::stringstream ss(path);
    for (std::string part; std::getline(ss, part, '.');) parts.push_back(part);
    if (parts.size() < 2) return nullptr;
    const Package* p = library->package(parts[0]);
    if (!p) return nullptr;
    // The same path can name objects of different classes, a skeletal mesh
    // and its animation: the one of the class asked for, when one is.
    std::vector<std::string> rest(parts.begin() + 1, parts.end());
    int idx = 0;
    for (int i = 1; i <= int(p->exports.size()) && !idx; ++i) {
        int k = i;
        size_t j = rest.size();
        while (j > 0 && k > 0 && lower(p->exp(k).name) == lower(rest[j - 1])) {
            k = p->exp(k).outer;
            --j;
        }
        if (j != 0 || k != 0) continue;
        Class* c = linker->findClass(p->classOf(i));
        if (!want || (c && c->isChildOf(want))) idx = i;
    }
    if (!idx) return nullptr;
    Object* outer = nullptr;
    for (size_t i = 0; i < parts.size(); ++i) {
        auto o = std::make_unique<Object>();
        o->name = Name(i == 0 ? p->stem : parts[i]);
        o->outer = outer;
        // a class with script, or none for a native one such as MeshAnimation
        if (i + 1 == parts.size()) {
            o->cls = linker->findClass(p->classOf(idx));
            if (o->cls) o->props = o->cls->defaults()->props;
        }
        outer = o.get();
        owned_.push_back(std::move(o));
    }
    slot = outer;
    if (linker->onStub && outer->cls) linker->onStub(outer);
    return slot;
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
    // An alias's own name inside it is the command of that name, not the
    // alias again: Jump is "Jump | Axis aUp Speed=+1200.0", and expanded
    // again it held aUp four times over.
    std::function<void(const std::string&, int, std::vector<std::string>&)> add =
        [&](const std::string& text, int depth, std::vector<std::string>& path) {
            std::stringstream parts(text);
            std::string part;
            while (std::getline(parts, part, '|')) {
                part = trim(part);
                if (part.empty()) continue;
                bool inside = std::find(path.begin(), path.end(), lower(part)) != path.end();
                std::string expanded = depth < 4 && !inside ? aliasOf(part) : std::string();
                if (!expanded.empty() && lower(expanded) != lower(part)) {
                    path.push_back(lower(part));
                    add(expanded, depth + 1, path);
                    path.pop_back();
                } else {
                    out.push_back(part);
                }
            }
        };
    std::vector<std::string> path;
    add(binding, 0, path);
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
    // pawn, its HUD, its PlayerInput or its CheatManager, in the engine's
    // order: Space's BypassCutscene is the HUD's. The rest are its arguments,
    // not passed yet.
    std::istringstream cmd(command);
    std::string name;
    cmd >> name;
    if (name.empty() || lower(name) == "axis" || lower(name) == "button" || lower(name) == "count") return;
    auto member = [this](const char* n) {
        return controller && controller->cls->findProp(Name(n)) ? world->obj(controller, n) : nullptr;
    };
    for (Object* o : {controller, pawn(), member("myHUD"), member("PlayerInput"), member("CheatManager")}) {
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
