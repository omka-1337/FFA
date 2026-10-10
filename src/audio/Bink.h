// Bink Audio, the codec of 3353 of the game's sounds and its movies' sound:
// its container and its DCT variant, mono or stereo, the only one the game
// uses. tools/ubink.py is the specification, with the layout and how it was
// found; its output agrees with FFmpeg's decoding of the game's files to float
// precision.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ffa {

struct DecodedSound {
    int rate = 0;
    int channels = 1;
    std::vector<float> samples;     // interleaved by channel, -1 to 1
};

// Throws FormatError for what is not a Bink file of the game's kind.
DecodedSound decodeBink(const uint8_t* data, size_t size);

// A RIFF WAV of 16 bit PCM, the game's other 416, made mono.
DecodedSound decodeWav(const uint8_t* data, size_t size);

}  // namespace ffa
