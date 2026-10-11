// Ogg Vorbis, the game's music: written from the Vorbis I specification, not
// from a decoder. tools/uogg.py is the specification of this code, with how
// each part goes; its output and this agree with FFmpeg's decoding of the
// game's files to float precision.
//
// The game's 69 files are all one kind, libVorbis I 20020717 at 44100 Hz
// stereo: floor 1, residue 2, blocks of 256 and 2048. What the specification
// allows besides that the game has not (floor 0) is refused.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ffa {

class VorbisDecoder {
public:
    // The whole file; throws FormatError for what is not Ogg Vorbis.
    explicit VorbisDecoder(std::vector<uint8_t> file);
    ~VorbisDecoder();

    int channels() const;
    int rate() const;

    // Up to `frames` frames, interleaved, -1 to 1; fewer at the end, 0 past it.
    size_t read(float* out, size_t frames);
    // Back to the first sample, for a song that loops.
    void rewind();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace ffa
