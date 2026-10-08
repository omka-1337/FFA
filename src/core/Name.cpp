#include "core/Name.h"

#include <unordered_map>
#include <vector>

namespace ffa {

namespace {

struct Table {
    std::vector<std::string> spelling{"None"};
    std::unordered_map<std::string, uint32_t> index{{"none", 0}};
};

Table& table() {
    static Table t;
    return t;
}

}  // namespace

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

Name::Name(std::string_view s) {
    Table& t = table();
    std::string key = lower(s);
    auto it = t.index.find(key);
    if (it != t.index.end()) {
        id_ = it->second;
        return;
    }
    id_ = uint32_t(t.spelling.size());
    t.spelling.emplace_back(s);
    t.index.emplace(std::move(key), id_);
}

const std::string& Name::str() const { return table().spelling[id_]; }

}  // namespace ffa
