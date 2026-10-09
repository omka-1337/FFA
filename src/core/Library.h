// The game's packages by name, opened when first asked for, and the objects
// that references between them point at.
//
// A package is named by its file's stem, whatever the extension: Unreal ties
// no object type to a file type, and Shrek 2 keeps some static meshes in
// texture packages (the beanstalk bonus maps take theirs from
// Textures/Beanstalk.utx). So every package file of the game's directories is
// a candidate, the first directory to hold a stem winning.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Package.h"

namespace ffa {

struct TagEntry;

// An object in a package: the package and its export index, or none.
struct ObjectRef {
    const Package* pkg = nullptr;
    int idx = 0;
    explicit operator bool() const { return pkg && idx > 0; }
    const Export& exp() const { return pkg->exp(idx); }
    std::string cls() const { return pkg->classOf(idx); }
};

class Library {
public:
    // gameDir: the directory holding System, Maps, Textures and the rest.
    explicit Library(const std::string& gameDir);

    // A package by its stem, case blind; null when there is none.
    const Package* package(const std::string& stem);
    // Every package file the game's directories hold.
    std::vector<std::string> files() const {
        std::vector<std::string> out;
        for (auto& [stem, path] : files_) out.push_back(path);
        return out;
    }
    // Make a package already open the one for its stem, such as a map.
    void adopt(const Package* p);

    // What reference ref, read in package p, points at: one of p's exports,
    // or through p's imports an export of another package, matched by its
    // path and class. None when it cannot be found.
    ObjectRef resolve(const Package& p, int32_t ref);
    // An export by its dotted path below its package, and by its class when
    // one is given, as one path can name several objects: in
    // ShrekCharacters.ukx, Shrek is a SkeletalMesh and a MeshAnimation. 0 for
    // none.
    static int findByPath(const Package& p, const std::vector<std::string>& parts, const char* cls = nullptr);

    // An object's tagged properties, after its state frame if it has one, and
    // where they end. False when the record has no valid property block.
    static bool properties(const ObjectRef& o, std::vector<TagEntry>& out, size_t& end);

private:
    std::map<std::string, std::string> files_;                  // stem -> path
    std::map<std::string, std::unique_ptr<Package>> owned_;
    std::map<std::string, const Package*> open_;
};

// Reading tagged values by property name, for the few engine classes whose
// properties the engine itself reads.
const TagEntry* findTag(const Package& p, const std::vector<TagEntry>& tags, const char* name, int index = 0);
int32_t tagInt(const Package& p, const std::vector<TagEntry>& tags, const char* name, int32_t def = 0);
float tagFloat(const Package& p, const std::vector<TagEntry>& tags, const char* name, float def = 0);
int32_t tagObject(const Package& p, const std::vector<TagEntry>& tags, const char* name);

}  // namespace ffa
