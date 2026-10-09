#include "render/Material.h"

#include <cstdint>
#include <cstring>

#include "render/Texture.h"
#include "script/Tagged.h"

namespace ffa {

bool tagStructArray(const Package& p, const TagEntry& t, std::vector<std::vector<TagEntry>>& out) {
    out.clear();
    if (t.type != T_Array) return false;
    try {
        Reader r(p.data, t.at, t.at + t.size);
        int32_t n = r.idx();
        if (n < 0 || n > 4096) return false;
        size_t pos = r.p;
        for (int32_t i = 0; i < n; ++i) {
            std::vector<TagEntry> item;
            size_t next = 0;
            if (!parseTagged(p, pos, t.at + t.size, item, next)) return false;
            out.push_back(std::move(item));
            pos = next;
        }
        return pos == t.at + t.size;
    } catch (const FormatError&) {
        return false;
    }
}

SurfaceMaterial MaterialResolver::resolve(const Package& p, int32_t ref) {
    ObjectRef o = lib_.resolve(p, ref);
    if (!o) return {};
    return walk(o, 0);
}

SurfaceMaterial MaterialResolver::walk(const ObjectRef& o, int depth) {
    if (depth > 12) return {};
    auto key = std::make_pair(o.pkg, o.idx);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    SurfaceMaterial out;
    std::string cls = o.cls();
    if (cls == "Texture") {
        out.texture = o;
        try {
            TextureInfo t = readTexture(o);
            if (t.masked) out.alphaRef = 0.5f;
            // An alpha texture blends and writes depth, its nearly clear
            // texels left out so that they cut no holes in what is behind:
            // the textures so used are nearly all opaque, the factory's
            // workers' suits, its arches, furnaces and signs, and drawn
            // without depth their far faces and what lay behind came over
            // them, where the game's frames show them solid.
            if (t.alphaTexture && !t.masked) {
                out.blend = Blend::Alpha;
                out.alphaRef = 0.02f;
            }
            out.twoSided = t.twoSided;
        } catch (const FormatError&) {
            out.texture = {};
        }
        cache_[key] = out;
        return out;
    }
    std::vector<TagEntry> tags;
    size_t end = 0;
    if (!Library::properties(o, tags, end)) return cache_[key] = out;
    const Package& p = *o.pkg;
    std::vector<int32_t> refs;
    if (cls == "Shader") {
        refs = {tagObject(p, tags, "Diffuse"), tagObject(p, tags, "FallbackMaterial")};
    } else if (cls == "Combiner") {
        refs = {tagObject(p, tags, "Material1"), tagObject(p, tags, "FallbackMaterial")};
    } else if (cls == "MaterialSequence") {
        if (const TagEntry* t = findTag(p, tags, "SequenceItems")) {
            std::vector<std::vector<TagEntry>> items;
            if (tagStructArray(p, *t, items))
                for (auto& item : items) refs.push_back(tagObject(p, item, "Material"));
        }
    } else {
        static const char* wraps[] = {"FinalBlend", "TexPanner", "TexOscillator", "TexRotator", "TexScaler",
                                      "TexEnvMap", "ColorModifier", "OpacityModifier", "MaterialSwitch"};
        for (const char* w : wraps)
            if (cls == w) refs = {tagObject(p, tags, "Material"), tagObject(p, tags, "FallbackMaterial")};
    }
    for (int32_t r : refs) {
        if (!r) continue;
        ObjectRef next = lib_.resolve(p, r);
        if (!next) continue;
        out = walk(next, depth + 1);
        if (out.texture) break;
    }
    // What wraps decides over what it wraps. FrameBufferBlending: FB_Overwrite,
    // FB_Modulate, FB_AlphaBlend, FB_AlphaModulate_MightNotFogCorrectly,
    // FB_Translucent, FB_Darken, FB_Brighten, FB_Invisible, FB_ShadowBlend.
    if (cls == "FinalBlend") {
        static const Blend fb[] = {Blend::Opaque, Blend::Modulate, Blend::Alpha, Blend::Alpha, Blend::Translucent,
                                   Blend::Darken, Blend::Brighten, Blend::Invisible, Blend::Modulate};
        int b = tagInt(p, tags, "FrameBufferBlending");
        out.blend = b >= 0 && b < 9 ? fb[b] : Blend::Opaque;
        out.zwrite = tagInt(p, tags, "ZWrite", 1) != 0;    // True by default
        out.alphaRef = tagInt(p, tags, "AlphaTest") ? float(tagInt(p, tags, "AlphaRef")) / 255.0f : -1.0f;
        if (tagInt(p, tags, "TwoSided")) out.twoSided = true;
    }
    // OutputBlending: OB_Normal, OB_Masked, OB_Modulate, OB_Translucent,
    // OB_Invisible, OB_Brighten, OB_Darken.
    if (cls == "Shader") {
        int b = tagInt(p, tags, "OutputBlending");
        static const Blend ob[] = {Blend::Opaque, Blend::Opaque, Blend::Modulate, Blend::Translucent,
                                   Blend::Invisible, Blend::Brighten, Blend::Darken};
        if (b > 0 && b < 7) {
            out.blend = ob[b];
            out.zwrite = b == 1;
            if (b == 1) out.alphaRef = 0.5f;
        }
        if (tagInt(p, tags, "TwoSided")) out.twoSided = true;
    }
    return cache_[key] = out;
}

}  // namespace ffa
