// Bink video, the pictures of the game's movies: Bink 1, revision i, the only
// one the game has, decoded as its own System/binkw32.dll decodes it, with the
// constant tables (the code tables, scan orders and quantisers) read from that
// DLL when a movie is opened. tools/ubinkv.py is the specification, with how
// the format was found; its frames and these are FFmpeg's to the byte.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ffa {

// The DLL's tables; throws FormatError for a file that is not the game's DLL.
struct BinkTables;
std::shared_ptr<const BinkTables> readBinkTables(const std::string& dllPath);

class BinkVideo {
public:
    // The whole file; throws FormatError for what is not a Bink 1 movie of
    // the game's kind.
    BinkVideo(std::vector<uint8_t> file, std::shared_ptr<const BinkTables> tables);
    ~BinkVideo();

    int width() const { return width_; }
    int height() const { return height_; }
    int frames() const { return frames_; }
    double fps() const { return fps_; }
    int frame() const { return frame_; }      // the last decoded, -1 for none

    // Frames decode in order, each from the one before; rewind starts again
    // at the first.
    void next();
    void rewind();

    // The frame as width x height RGBA, BT.601 at studio range.
    void rgba(uint8_t* out) const;
    // The frame as planar 4:2:0, Y then U then V, as ffmpeg -pix_fmt yuv420p.
    void yuv(std::vector<uint8_t>& out) const;

    struct Plane {
        int w = 0, h = 0;
        std::vector<uint8_t> p;
    };

private:
    std::vector<uint8_t> d_;
    std::shared_ptr<const BinkTables> t_;
    int width_ = 0, height_ = 0, frames_ = 0, tracks_ = 0, frame_ = -1;
    double fps_ = 0;
    std::vector<uint32_t> offsets_;
    // Y and the two chroma planes as they come, Cr then Cb
    Plane cur_[3], prev_[3];
};

}  // namespace ffa
