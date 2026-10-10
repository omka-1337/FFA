// KnowWonder's lip sync, the block after each sound's file (tools/usound.py
// is the specification, docs/package-format.md has how it was read).
//
// Version 2, nearly all the dialogue, is the engine's FLipGenLipSync: 30
// frames a second of 26 curves, from a frame before the sound starts, 9 or
// more, to one after it ends. Channels 0 to 14 are the weights of phonemes,
// 0 to 1, told apart by the letters of the lines they peak in; 17 and 18 the
// left and right blinks; 19 to 25 turn the head and the eyes, the eyes against
// it. The faces have poses for the engine's visemes, AI, E, O, U, CDGKNRSthYZ,
// L, WQ, MBP and FV, two frames each, the first the face at rest, so a weight
// is a place between them. Version 1 is FAmplitudeLipSync: the voice's
// loudness, 0 to 255, 50 times a second.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ffa {

struct LipSync {
    int version = 0;                // 1 or 2
    float rate = 30;                // frames a second
    int first = 0;                  // the first frame's number, sound start 0
    int channels = 26;              // 1 for version 1
    std::vector<float> values;      // frames by channels
    int frames() const { return channels ? int(values.size()) / channels : 0; }
    // Seconds from the sound's start to the last frame.
    float end() const { return float(first + frames() - 1) / rate; }
};

// The faces' poses, by the sequence names the meshes have.
enum Viseme { V_AI, V_E, V_O, V_U, V_CDG, V_L, V_WQ, V_MBP, V_FV, V_Count };
extern const char* const kVisemeNames[V_Count];

struct FaceWeights {
    float viseme[V_Count] = {};
    float blink[2] = {};            // left, right
};

// False when the block holds nothing to move a face with: version 0, or a
// version 1 or 2 that does not read to its end.
bool parseLipSync(const uint8_t* data, size_t size, LipSync& out);

// The weights t seconds after the sound started.
FaceWeights faceAt(const LipSync& lips, float t);

}  // namespace ffa
