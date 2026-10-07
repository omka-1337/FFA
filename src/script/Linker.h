// Loads packages and turns their records into runtime types and objects.
//
// A reference is resolved by path: an import names its package and the chain
// of outers down to the object, and the export with the same path in that
// package is the object meant. Everything is built on first use and kept, so a
// whole game's script can be opened without building what is never touched.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Package.h"
#include "script/Bytecode.h"
#include "script/Tagged.h"
#include "script/Types.h"

namespace ffa {

// What FFA-Tools' uclass.py Reader.field returns: the links of a field record.
struct FieldInfo {
    std::string cls, name;
    int32_t super = 0, next = 0, children = 0;
    uint32_t flags = 0;
    int dim = 1;
    std::vector<int32_t> refs;
};

class Linker {
public:
    explicit Linker(const std::vector<std::string>& paths);
    static std::vector<std::string> packageFiles(const std::string& dir,
                                                 const std::vector<std::string>& exts = {".u"});

    std::vector<std::unique_ptr<Package>> packages;
    int packageIndex(std::string_view stem) const;

    // records
    FieldInfo field(int pkg, int idx);
    std::vector<int> members(int pkg, int idx);
    std::vector<std::string> enumValues(int pkg, int idx);
    std::string exportPath(int pkg, int idx) const;
    int findExport(int pkg, std::string_view path);
    std::optional<std::pair<int, int>> resolve(int pkg, int32_t ref);

    // types by export
    Class* classAt(int pkg, int idx);
    Prop* propAt(int pkg, int idx);
    Function* functionAt(int pkg, int idx);
    State* stateAt(int pkg, int idx);
    StructType* structAt(int pkg, int idx);
    EnumType* enumAt(int pkg, int idx);
    Object* instanceAt(int pkg, int idx);

    // types and objects by reference from inside a package
    Class* classRef(int pkg, int32_t ref);
    Prop* propRef(int pkg, int32_t ref);
    Function* functionRef(int pkg, int32_t ref);
    State* stateRef(int pkg, int32_t ref);
    StructType* structRef(int pkg, int32_t ref);
    EnumType* enumRef(int pkg, int32_t ref);
    Object* objectRef(int pkg, int32_t ref);

    Class* findClass(std::string_view name);
    StructType* findStruct(std::string_view name);
    Object* findObject(std::string_view path);
    Function* native(int index);
    const std::map<int, std::pair<int, int>>& nativeTable() const { return natives_; }

    ParsedCode bytecode(int pkg, int idx);

    // class default objects and tagged values
    Object* buildDefault(Class* c);
    void initClassObject(Class* c);     // a class's variables as an object
    bool locateDefaults(Class* c, std::vector<TagEntry>& out);
    void applyTagged(int pkg, Class* c, std::vector<Value>& props,
                     const std::vector<TagEntry>& entries, const std::string& where);
    Value decode(int pkg, const Prop* p, const TagEntry& e);

    // Every problem met while building defaults or instances: where, and what.
    std::vector<std::pair<std::string, std::string>> problems;

    // Objects the linker creates and owns: classes, defaults, instances, stubs.
    Object* adopt(std::unique_ptr<Object> o);
    // Name, Class and Outer, which Object declares and script reads.
    void setIntrinsics(Object* o);

private:
    std::string kindOf(int pkg, int idx) const;
    Object* stub(int pkg, int32_t ref);
    std::vector<std::string> importParts(int pkg, int32_t ref, std::string* cls = nullptr) const;
    Value structValue(int pkg, StructType* st, size_t at, size_t size);
    Value taggedStruct(int pkg, StructType* st, const std::vector<TagEntry>& entries);
    Value binaryStruct(int pkg, StructType* st, Reader& r);
    Value arrayValue(int pkg, const Prop* p, size_t at, size_t size);
    String readString(int pkg, Reader& r);

    std::unordered_map<std::string, int> byStem_;
    std::unordered_map<std::string, std::pair<int, int>> classes_;
    std::map<int, std::pair<int, int>> natives_;
    std::vector<std::unordered_map<int, std::vector<int>>> owned_;
    std::vector<std::unordered_map<std::string, int>> paths_;
    std::map<std::pair<int, int>, FieldInfo> fields_;

    std::map<std::pair<int, int>, std::unique_ptr<Prop>> props_;
    std::map<std::pair<int, int>, std::unique_ptr<Function>> functions_;
    std::map<std::pair<int, int>, std::unique_ptr<State>> states_;
    std::map<std::pair<int, int>, std::unique_ptr<StructType>> structs_;
    std::map<std::pair<int, int>, Object*> objects_;     // classes, enums, instances
    std::map<std::pair<int, int32_t>, Object*> stubs_;
    std::vector<std::unique_ptr<Object>> owned_objects_;
    std::vector<std::unique_ptr<Prop>> extra_props_;
};

}  // namespace ffa
