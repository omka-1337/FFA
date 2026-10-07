// Names, as the engine keeps them: one entry per string regardless of case,
// so 'Begin' and 'begin' are the same name and compare as one integer.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ffa {

class Name {
public:
    Name() = default;                       // None
    explicit Name(std::string_view s);

    const std::string& str() const;
    bool isNone() const { return id_ == 0; }
    uint32_t id() const { return id_; }

    bool operator==(Name o) const { return id_ == o.id_; }
    bool operator!=(Name o) const { return id_ != o.id_; }

private:
    uint32_t id_ = 0;
};

// Lowercase for case blind keys. Names and paths are ASCII or Latin-1, and
// only ASCII letters fold, as with the engine's own comparisons.
std::string lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);

}  // namespace ffa

template <>
struct std::hash<ffa::Name> {
    size_t operator()(ffa::Name n) const noexcept { return n.id(); }
};
