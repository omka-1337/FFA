// The game's sounds by their script objects: a Sound record is an empty
// property block, its file type's name (bik or WAV), the file whole as a lazy
// array, and KnowWonder's lip sync after it (docs/package-format.md, Sounds).
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "audio/Bink.h"
#include "audio/LipSync.h"
#include "core/Library.h"

namespace ffa {

struct Object;

class SoundBank {
public:
    explicit SoundBank(Library& lib) : lib_(lib) {}

    // The sound decoded, kept once decoded; null for what is not a sound or
    // does not decode.
    std::shared_ptr<const DecodedSound> clip(const Object* sound);
    // Its length in seconds, from its file's own count of what it decodes to,
    // without decoding it; 0 for none.
    float duration(const Object* sound);
    // Its lip sync, kept once read; null for a sound without one to move a
    // face with.
    std::shared_ptr<const LipSync> lipSync(const Object* sound);

    size_t decoded = 0, failed = 0;

private:
    struct File {
        ObjectRef ref;
        std::string type;
        size_t at = 0, size = 0;
        size_t end = 0;             // of the record, where the lip sync ends
    };
    bool file(const Object* sound, File& out);
    Library& lib_;
    std::mutex mutex_;
    std::map<const Object*, std::shared_ptr<const DecodedSound>> clips_;
    std::map<const Object*, float> durations_;
    std::map<const Object*, std::shared_ptr<const LipSync>> lips_;
};

}  // namespace ffa
