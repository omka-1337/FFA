// Textures: the mip chain of a Texture export and the pixel formats Shrek 2
// uses, decoded to RGBA.
//
// After the tagged properties (Format, USize, VSize, Palette and the rest):
//
//   index   mip count
//   per mip:
//     u32     the absolute file offset just past the data
//     index   data length
//     bytes   pixel data
//     u32     USize, u32 VSize, u8 UBits, u8 VBits
//
// Formats by ETextureFormat: 0 P8 into a Palette, 3 DXT1, 5 RGBA8 stored
// B G R A, 7 DXT3, 8 DXT5, 9 L8, 10 G16. Palette entries are R G B A. All of
// it is tools/utexture.py's, which has how the two colour orders were settled.
#pragma once

#include <cstdint>
#include <vector>

#include "core/Library.h"

namespace ffa {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

struct TextureInfo {
    int format = 0;
    struct Mip {
        int width, height;
        size_t at, size;
    };
    std::vector<Mip> mips;
    int32_t palette = 0;            // the Palette reference, for P8
    bool masked = false;            // bMasked: alpha is a cut, not a blend
    bool alphaTexture = false;      // bAlphaTexture: alpha blends
    bool twoSided = false;
};

// The texture's properties and mip chain; throws FormatError when the chain
// does not end on the record.
TextureInfo readTexture(const ObjectRef& t);

// The largest mip no side of which is more than maxSide (0: the first),
// decoded to RGBA. Throws FormatError for a format it does not know.
Image decodeTexture(Library& lib, const ObjectRef& t, int maxSide = 0);

// S3TC, as published: fmt 3, 7 or 8.
void decodeDxt(const uint8_t* data, size_t size, int w, int h, int fmt, uint8_t* out);

}  // namespace ffa
