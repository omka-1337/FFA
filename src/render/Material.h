// From a material reference to the texture that gives a surface its colour.
//
// A surface rarely names a Texture. Between them stand shaders, blends and
// modifiers, and each class keeps what it wraps under its own property name;
// tools/umaterial.py surveyed which:
//
//   Shader                 Diffuse, else FallbackMaterial
//   Combiner               Material1, else FallbackMaterial
//   FinalBlend, TexPanner, TexOscillator, TexRotator, TexScaler, TexEnvMap,
//   ColorModifier, OpacityModifier, MaterialSwitch
//                          Material, else FallbackMaterial
//   MaterialSequence       the first Material of SequenceItems
//
// A cubemap is a reflection, not a colour, and ends the walk.
#pragma once

#include <map>

#include "core/Library.h"

namespace ffa {

// How a surface goes onto the frame, by FinalBlend's FrameBufferBlending, a
// Shader's OutputBlending, or the texture's own bMasked and bAlphaTexture.
enum class Blend : uint8_t { Opaque, Alpha, Modulate, Translucent, Brighten, Darken, Invisible };

struct SurfaceMaterial {
    ObjectRef texture;              // the base texture, or none
    Blend blend = Blend::Opaque;
    float alphaRef = -1;            // alpha below it is cut, 0 to 1; -1 none
    bool zwrite = true;
    bool twoSided = false;
    bool blended() const { return blend != Blend::Opaque; }
};

class MaterialResolver {
public:
    explicit MaterialResolver(Library& lib) : lib_(lib) {}
    SurfaceMaterial resolve(const Package& p, int32_t ref);

private:
    SurfaceMaterial walk(const ObjectRef& o, int depth);
    Library& lib_;
    std::map<std::pair<const Package*, int>, SurfaceMaterial> cache_;
};

// The elements of an array of structs, each its own tagged list: a compact
// count, then the lists. Returns false when they do not parse.
bool tagStructArray(const Package& p, const struct TagEntry& t, std::vector<std::vector<struct TagEntry>>& out);

}  // namespace ffa
