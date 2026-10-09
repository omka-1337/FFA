#include "render/LevelRender.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <tuple>

#include "script/Tagged.h"
#include "world/Animator.h"

namespace ffa {

namespace {

const uint32_t PF_Invisible = 0x00000001, PF_Portal = 0x04000000, PF_Unlit = 0x00400000;

const char* kVertex = R"(
attribute vec3 aPos;
attribute vec2 aUv;
attribute vec2 aUv2;
attribute vec3 aAmb;
uniform mat4 uMvp;
varying vec2 vUv;
varying vec2 vUv2;
varying vec3 vAmb;
void main() {
    vUv = aUv;
    vUv2 = aUv2;
    vAmb = aAmb;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char* kFragment = R"(
precision mediump float;
uniform sampler2D uTex;
uniform sampler2D uLm;
uniform float uLit;
uniform float uCut;
varying vec2 vUv;
varying vec2 vUv2;
varying vec3 vAmb;
void main() {
    vec4 c = texture2D(uTex, vUv);
    if (c.a < uCut) discard;
    vec3 light = uLit > 0.5 ? texture2D(uLm, vUv2).rgb * 2.0 + vAmb : vec3(1.0);
    gl_FragColor = vec4(c.rgb * light, c.a);
}
)";

unsigned compile(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        throw std::runtime_error(std::string("shader: ") + log);
    }
    return s;
}

unsigned upload(const Image& img, bool mipmaps) {
    unsigned t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.width, img.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    bool pot = !(img.width & (img.width - 1)) && !(img.height & (img.height - 1));
    if (mipmaps && pot) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLint wrap = pot ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    return t;
}

// A zone's ambient light, 0 to 1: AmbientHue, AmbientSaturation and
// AmbientBrightness as plain HSV, saturation 255 being white, as the viewer
// reads them (tools/uview.py).
Vec3 ambientOf(World& w, Object* zone) {
    float h = float(w.var(zone, "AmbientHue").i()) / 255.0f;
    float s = 1.0f - float(w.var(zone, "AmbientSaturation").i()) / 255.0f;
    float v = float(w.var(zone, "AmbientBrightness").i()) / 255.0f;
    float i = std::floor(h * 6), f = h * 6 - i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (int(i) % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

}  // namespace

LevelRender::LevelRender(Session& s, Library& lib) : session_(s), lib_(lib), materials_(lib) {
    program_ = glCreateProgram();
    glAttachShader(program_, compile(GL_VERTEX_SHADER, kVertex));
    glAttachShader(program_, compile(GL_FRAGMENT_SHADER, kFragment));
    glLinkProgram(program_);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) throw std::runtime_error("the shader program does not link");
    aPos_ = glGetAttribLocation(program_, "aPos");
    aUv_ = glGetAttribLocation(program_, "aUv");
    aUv2_ = glGetAttribLocation(program_, "aUv2");
    aAmb_ = glGetAttribLocation(program_, "aAmb");
    uMvp_ = glGetUniformLocation(program_, "uMvp");
    uTex_ = glGetUniformLocation(program_, "uTex");
    uLm_ = glGetUniformLocation(program_, "uLm");
    uLit_ = glGetUniformLocation(program_, "uLit");
    uCut_ = glGetUniformLocation(program_, "uCut");
    Image white;
    white.width = white.height = 1;
    white.rgba = {255, 255, 255, 255};
    white_ = upload(white, false);
    buildBsp();
    buildTerrains();
    buildMeshes();
}

LevelRender::~LevelRender() {
    for (Batch& b : batches_) glDeleteBuffers(1, &b.buffer);
    for (auto& [k, t] : textures_) glDeleteTextures(1, &t.first);
    for (unsigned t : lightMapTex_) glDeleteTextures(1, &t);
    glDeleteTextures(1, &white_);
    glDeleteProgram(program_);
}

unsigned LevelRender::textureFor(const SurfaceMaterial& m, int& width, int& height) {
    if (!m.texture) {
        width = height = 64;
        return white_;
    }
    auto key = std::make_pair(m.texture.pkg, m.texture.idx);
    auto it = textures_.find(key);
    if (it == textures_.end()) {
        unsigned t = white_;
        std::pair<int, int> size{64, 64};
        try {
            TextureInfo info = readTexture(m.texture);
            size = {info.mips[0].width, info.mips[0].height};
            // the full size for the first mip's UV scale; at most 1024 drawn
            t = upload(decodeTexture(lib_, m.texture, 1024), true);
            ++textures;
        } catch (const std::exception&) {
            ++missingTextures;
        }
        it = textures_.emplace(key, std::make_pair(t, size)).first;
    }
    width = it->second.second.first;
    height = it->second.second.second;
    return it->second.first;
}

void LevelRender::buildBsp() {
    World& w = *session_.world;
    const BspModel& m = session_.collision->bsp;
    const Package& p = *session_.linker->packages[size_t(session_.pkg)];
    // the lightmap textures, DXT1, decoded once
    for (const BspLightMapTexture& t : m.lightMapTextures) {
        if (t.size == 0 || t.width <= 0 || t.height <= 0) {
            lightMapTex_.push_back(0);      // never baked
            continue;
        }
        Image img;
        img.width = t.width;
        img.height = t.height;
        img.rgba.resize(size_t(t.width) * size_t(t.height) * 4);
        decodeDxt(p.data.data() + t.at, t.size, t.width, t.height, 3, img.rgba.data());
        lightMapTex_.push_back(upload(img, false));
        ++lightMaps;
    }
    // each zone's ambient
    std::vector<Vec3> ambient;
    for (int32_t actor : m.zoneActors) {
        auto it = w.actorAt.find(actor);
        ambient.push_back(ambientOf(w, it != w.actorAt.end() ? it->second : w.info));
    }
    // Triangles grouped by texture, lightmap and how they are drawn.
    struct Key {
        unsigned tex, lm;
        int blend;
        float cut;
        bool zwrite, unlit;
        bool operator<(const Key& o) const {
            return std::tie(tex, lm, blend, cut, zwrite, unlit) < std::tie(o.tex, o.lm, o.blend, o.cut, o.zwrite, o.unlit);
        }
    };
    std::map<Key, std::vector<float>> groups;
    for (const BspNode& n : m.nodes) {
        if (n.numVerts < 3 || n.surf < 0 || size_t(n.surf) >= m.surfs.size()) continue;
        const BspSurf& s = m.surfs[size_t(n.surf)];
        if (s.flags & (PF_Invisible | PF_Portal)) continue;
        SurfaceMaterial mat = materials_.resolve(p, s.material);
        int tw = 64, th = 64;
        unsigned tex = textureFor(mat, tw, th);
        unsigned lm = 0;
        if (n.lightMap >= 0 && size_t(n.lightMap) < m.lightMaps.size()) {
            int32_t t = m.lightMaps[size_t(n.lightMap)];
            if (t >= 0 && size_t(t) < lightMapTex_.size()) lm = lightMapTex_[size_t(t)];
        }
        bool unlit = (s.flags & PF_Unlit) || !lm;
        if (mat.blend == Blend::Invisible) continue;
        Key key{tex, lm ? lm : white_, int(mat.blend), mat.alphaRef, mat.zwrite, unlit};
        std::vector<float>& out = groups[key];
        Vec3 base = m.points[size_t(s.base)], tu = m.vectors[size_t(s.textureU)], tv = m.vectors[size_t(s.textureV)];
        Vec3 amb = n.zone < ambient.size() ? ambient[n.zone] : Vec3{};
        std::vector<std::array<float, 10>> corners;
        for (int k = 0; k < n.numVerts; ++k) {
            Vec3 q = m.points[size_t(m.vertPoints[size_t(n.vertPool + k)])];
            Vec3 d = q - base;
            float lu = 0, lv = 0;
            if (n.section >= 0 && size_t(n.section) < m.sections.size()) {
                const BspSection& sec = m.sections[size_t(n.section)];
                if (n.firstVertex + k < sec.count) {
                    float f[2];
                    std::memcpy(f, p.data.data() + sec.at + size_t(40 * (n.firstVertex + k)) + 20, 8);
                    lu = f[0];
                    lv = f[1];
                }
            }
            corners.push_back({q.x, q.y, q.z, dot(d, tu) / float(tw), dot(d, tv) / float(th), lu, lv, amb.x, amb.y, amb.z});
        }
        for (size_t k = 1; k + 1 < corners.size(); ++k)
            for (size_t c : {size_t(0), k, k + 1}) out.insert(out.end(), corners[c].begin(), corners[c].end());
    }
    for (auto& [key, verts] : groups) {
        Batch b;
        b.texture = key.tex;
        b.lightMap = key.lm;
        b.mat.blend = Blend(key.blend);
        b.mat.alphaRef = key.cut;
        b.mat.zwrite = key.zwrite;
        b.unlit = key.unlit;
        b.count = int(verts.size() / 10);
        glGenBuffers(1, &b.buffer);
        glBindBuffer(GL_ARRAY_BUFFER, b.buffer);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
        triangles += size_t(b.count / 3);
        batches_.push_back(b);
    }
    batches = batches_.size();
}

namespace {

// The program for a terrain of n layers: each layer's texture coordinates are
// the world position against its matrix's u and v rows, and the layers are
// laid over each other in order, each by its weight, as the viewer does.
unsigned terrainProgram(int n) {
    std::string vs = "attribute vec3 aPos; attribute vec3 aLit; attribute vec3 aAmb; attribute vec4 aW0; attribute vec4 aW1;\n"
                     "uniform mat4 uMvp; varying vec3 vLight; varying vec4 vW0; varying vec4 vW1;\n";
    std::string fs = "precision mediump float; varying vec3 vLight; varying vec4 vW0; varying vec4 vW1;\n";
    for (int i = 0; i < n; ++i) {
        std::string k = std::to_string(i);
        vs += "uniform vec4 uU" + k + "; uniform vec4 uV" + k + "; varying vec2 vT" + k + ";\n";
        fs += "uniform sampler2D uT" + k + "; varying vec2 vT" + k + ";\n";
    }
    vs += "void main() { vec4 p = vec4(aPos, 1.0);\n";
    for (int i = 0; i < n; ++i) {
        std::string k = std::to_string(i);
        vs += " vT" + k + " = vec2(dot(p, uU" + k + "), dot(p, uV" + k + "));\n";
    }
    vs += " vLight = aLit * 2.0 + aAmb; vW0 = aW0; vW1 = aW1; gl_Position = uMvp * p; }\n";
    static const char* W[] = {"vW0.x", "vW0.y", "vW0.z", "vW0.w", "vW1.x", "vW1.y", "vW1.z", "vW1.w"};
    fs += "void main() { vec3 c = vec3(0.0);\n";
    for (int i = 0; i < n; ++i) {
        std::string k = std::to_string(i);
        fs += " c = mix(c, texture2D(uT" + k + ", vT" + k + ").rgb, " + W[i] + ");\n";
    }
    fs += " gl_FragColor = vec4(c * vLight, 1.0); }\n";
    unsigned prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs.c_str()));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs.c_str()));
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aLit");
    glBindAttribLocation(prog, 2, "aAmb");
    glBindAttribLocation(prog, 3, "aW0");
    glBindAttribLocation(prog, 4, "aW1");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) throw std::runtime_error("a terrain program does not link");
    return prog;
}

float tagFloatIn(const Package& p, const std::vector<TagEntry>& tags, const char* name) {
    return tagFloat(p, tags, name, 0);
}

}  // namespace

void LevelRender::buildTerrains() {
    World& w = *session_.world;
    Collision& col = *session_.collision;
    const Package& p = *session_.linker->packages[size_t(session_.pkg)];
    const BspModel& bsp = col.bsp;
    for (size_t ti = 0; ti < col.terrains.size(); ++ti) {
        const Terrain& t = *col.terrains[ti];
        Object* actor = col.terrainActors[ti];
        auto ex = w.exportOf.find(actor);
        if (ex == w.exportOf.end()) continue;
        std::vector<TagEntry> tags;
        size_t end = 0;
        if (!Library::properties(ObjectRef{&p, ex->second}, tags, end)) continue;
        TerrainDraw td;
        std::vector<std::vector<uint8_t>> weights;
        // Layers: a fixed array of TerrainLayer structs, each its own tagged
        // list; TerrainMatrix in it a Matrix of four Planes, read as a row
        // vector: u = [x y z 1] against its first column, v its second
        // (tools/uterrain.py, read_layers).
        std::vector<const TagEntry*> layers;
        for (const TagEntry& e : tags)
            if (e.type == T_Struct && p.validName(e.name) && p.names[size_t(e.name)] == "Layers") layers.push_back(&e);
        std::sort(layers.begin(), layers.end(), [](const TagEntry* a, const TagEntry* b) { return a->index < b->index; });
        for (const TagEntry* e : layers) {
            if (td.textures.size() >= 8) break;
            std::vector<TagEntry> lay;
            size_t le = 0;
            if (!parseTagged(p, e->at, e->at + e->size, lay, le)) continue;
            int32_t texRef = tagObject(p, lay, "Texture"), alphaRef = tagObject(p, lay, "AlphaMap");
            const TagEntry* tm = findTag(p, lay, "TerrainMatrix");
            if (!texRef || !tm) continue;
            std::vector<TagEntry> planes;
            size_t pe = 0;
            if (!parseTagged(p, tm->at, tm->at + tm->size, planes, pe)) continue;
            std::array<float, 8> uv{};
            const char* rows[] = {"XPlane", "YPlane", "ZPlane", "WPlane"};
            for (int r = 0; r < 4; ++r) {
                const TagEntry* pl = findTag(p, planes, rows[r]);
                if (!pl) continue;
                std::vector<TagEntry> f;
                size_t fe = 0;
                if (!parseTagged(p, pl->at, pl->at + pl->size, f, fe)) continue;
                uv[size_t(r)] = tagFloatIn(p, f, "X");
                uv[size_t(4 + r)] = tagFloatIn(p, f, "Y");
            }
            if (!uv[0] && !uv[1] && !uv[2] && !uv[4] && !uv[5] && !uv[6]) continue;
            SurfaceMaterial mat = materials_.resolve(p, texRef);
            int tw, th;
            unsigned tex = textureFor(mat, tw, th);
            // the weight at every vertex: the alpha of its alpha map, a map
            // smaller than the grid spread over it
            std::vector<uint8_t> wt(size_t(t.X) * size_t(t.Y), 255);
            if (alphaRef) {
                ObjectRef ar = lib_.resolve(p, alphaRef);
                if (ar && ar.cls() == "Texture") {
                    try {
                        Image a = decodeTexture(lib_, ar, 0);
                        for (int y = 0; y < t.Y; ++y) {
                            int ay = y * a.height / t.Y;
                            for (int x = 0; x < t.X; ++x)
                                wt[size_t(y * t.X + x)] = a.rgba[(size_t(ay) * size_t(a.width) + size_t(x * a.width / t.X)) * 4 + 3];
                        }
                    } catch (const std::exception&) {
                    }
                }
            }
            td.textures.push_back(tex);
            td.uv.push_back(uv);
            weights.push_back(std::move(wt));
        }
        if (td.textures.empty()) continue;
        td.program = terrainProgram(int(td.textures.size()));
        // the vertices: position, light, the zone's ambient, eight weights
        std::vector<float> verts;
        verts.reserve(size_t(t.X) * size_t(t.Y) * 17);
        for (int y = 0; y < t.Y; ++y)
            for (int x = 0; x < t.X; ++x) {
                size_t v = size_t(y * t.X + x);
                Vec3 q = t.vertices[v];
                float lit[3] = {0.5f, 0.5f, 0.5f};
                if (t.light.size() >= (v + 1) * 4)
                    for (int k = 0; k < 3; ++k) lit[k] = t.light[v * 4 + size_t(k)] / 255.0f;
                BspModel::Region rg = bsp.regionAt(q + Vec3{0, 0, 8});
                Vec3 amb{};
                if (rg.zone >= 0 && size_t(rg.zone) < bsp.zoneActors.size()) {
                    auto it = w.actorAt.find(bsp.zoneActors[size_t(rg.zone)]);
                    amb = ambientOf(w, it != w.actorAt.end() ? it->second : w.info);
                }
                verts.insert(verts.end(), {q.x, q.y, q.z, lit[0], lit[1], lit[2], amb.x, amb.y, amb.z});
                for (size_t k = 0; k < 8; ++k) verts.push_back(k < weights.size() ? weights[k][v] / 255.0f : 0.0f);
            }
        // bands of rows, each under 65536 vertices, for 16 bit indices
        int rowsPer = std::max(2, 65535 / t.X);
        for (int y0 = 0; y0 + 1 < t.Y; y0 += rowsPer - 1) {
            int y1 = std::min(t.Y - 1, y0 + rowsPer - 1);
            std::vector<uint16_t> idx;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x + 1 < t.X; ++x) {
                    if (!t.visible(x, y)) continue;
                    int q[2][3];
                    t.quad(x, y, q);
                    for (auto& tri : q)
                        for (int c : tri) idx.push_back(uint16_t(c - y0 * t.X));
                }
            TerrainDraw::Band b;
            glGenBuffers(1, &b.vertices);
            glBindBuffer(GL_ARRAY_BUFFER, b.vertices);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(size_t(y1 - y0 + 1) * size_t(t.X) * 17 * sizeof(float)),
                         verts.data() + size_t(y0) * size_t(t.X) * 17, GL_STATIC_DRAW);
            glGenBuffers(1, &b.indices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.indices);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 2), idx.data(), GL_STATIC_DRAW);
            b.count = int(idx.size());
            triangles += idx.size() / 3;
            td.bands.push_back(b);
        }
        terrains_.push_back(std::move(td));
    }
}

namespace {

const char* kMeshVertex = R"(
attribute vec3 aPos;
attribute vec2 aUv;
attribute vec3 aColor;
uniform mat4 uMvp;
uniform vec3 uAmb;
uniform float uBaked;
varying vec2 vUv;
varying vec3 vLight;
void main() {
    vUv = aUv;
    vLight = uBaked > 0.5 ? aColor * 2.0 + uAmb : vec3(1.0);
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char* kMeshFragment = R"(
precision mediump float;
uniform sampler2D uTex;
uniform float uCut;
varying vec2 vUv;
varying vec3 vLight;
void main() {
    vec4 c = texture2D(uTex, vUv);
    if (c.a < uCut) discard;
    gl_FragColor = vec4(c.rgb * vLight, c.a);
}
)";

}  // namespace

namespace {

// A placed mesh's vertex light from its lights, as the engine computes the
// cache a StaticMeshInstance holds; fitted to the 6992 instances whose cache
// is there, it gives their colours to a median of 0 to 1 of 255 (docs). A
// light reaches a vertex where its mask's bit is set; a Light falls off as
// (1 - (d / 25 LightRadius)^2)^2, a Sunlight is a direction without falloff,
// and either lights by N.L; the colour is LightHue and LightSaturation as HSV,
// at 0.65 LightBrightness.
std::vector<uint8_t> bakeVertexLight(World& w, const StaticMeshCollision& m, const float model[16],
                                     const std::vector<std::pair<Object*, std::vector<uint8_t>>>& lights) {
    std::vector<float> acc(m.positions.size() * 3, 0.0f);
    for (const auto& [light, mask] : lights) {
        float bright = w.var(light, "LightBrightness").f(), radius = w.var(light, "LightRadius").f() * 25.0f;
        float h = float(w.var(light, "LightHue").i()) / 255.0f, sat = 1.0f - float(w.var(light, "LightSaturation").i()) / 255.0f;
        float hi = std::floor(h * 6), f = h * 6 - hi, pv = 1 - sat, qv = 1 - f * sat, tv = 1 - (1 - f) * sat;
        Vec3 col;
        switch (int(hi) % 6) {
        case 0: col = {1, tv, pv}; break;
        case 1: col = {qv, 1, pv}; break;
        case 2: col = {pv, 1, tv}; break;
        case 3: col = {pv, qv, 1}; break;
        case 4: col = {tv, pv, 1}; break;
        default: col = {1, pv, qv};
        }
        bool sun = light->cls->name == Name("Sunlight");
        Vec3 lp, dir;
        w.vm.unvector(w.var(light, "Location"), lp.x, lp.y, lp.z);
        int32_t pr, yr, rr;
        w.vm.unrotator(w.var(light, "Rotation"), pr, yr, rr);
        float ax[3][3];
        rotationAxes(pr, yr, rr, ax);
        dir = {ax[0][0], ax[0][1], ax[0][2]};
        for (size_t i = 0; i < m.positions.size() && i < m.normals.size(); ++i) {
            if (!(i / 8 < mask.size() && (mask[i / 8] >> (i % 8)) & 1)) continue;
            const Vec3 p = m.positions[i], nl = m.normals[i];
            Vec3 wp{model[0] * p.x + model[4] * p.y + model[8] * p.z + model[12],
                    model[1] * p.x + model[5] * p.y + model[9] * p.z + model[13],
                    model[2] * p.x + model[6] * p.y + model[10] * p.z + model[14]};
            Vec3 wn{model[0] * nl.x + model[4] * nl.y + model[8] * nl.z, model[1] * nl.x + model[5] * nl.y + model[9] * nl.z,
                    model[2] * nl.x + model[6] * nl.y + model[10] * nl.z};
            float ln = length(wn);
            if (ln > 0) wn = wn * (1 / ln);
            float k;
            if (sun) {
                k = std::max(0.0f, -dot(wn, dir));
            } else {
                Vec3 d = lp - wp;
                float dist = length(d);
                if (dist <= 0 || dist >= radius) continue;
                float x = dist / radius;
                k = (1 - x * x) * (1 - x * x) * std::max(0.0f, dot(wn, d) / dist);
            }
            acc[i * 3] += 0.65f * bright * k * col.x;
            acc[i * 3 + 1] += 0.65f * bright * k * col.y;
            acc[i * 3 + 2] += 0.65f * bright * k * col.z;
        }
    }
    std::vector<uint8_t> out(acc.size());
    for (size_t i = 0; i < acc.size(); ++i) out[i] = uint8_t(std::min(255.0f, acc[i]));
    return out;
}

}  // namespace

ObjectRef LevelRender::refOf(const Object* o) {
    if (!o) return {};
    std::vector<std::string> parts;
    for (const Object* k = o; k; k = k->outer) parts.insert(parts.begin(), k->name.str());
    if (parts.size() < 2) return {};
    const Package* p = lib_.package(parts[0]);
    if (!p) return {};
    int idx = Library::findByPath(*p, std::vector<std::string>(parts.begin() + 1, parts.end()));
    return idx ? ObjectRef{p, idx} : ObjectRef{};
}

void LevelRender::buildMeshes() {
    World& w = *session_.world;
    Collision& col = *session_.collision;
    const BspModel& bsp = col.bsp;
    meshProgram_ = glCreateProgram();
    glAttachShader(meshProgram_, compile(GL_VERTEX_SHADER, kMeshVertex));
    glAttachShader(meshProgram_, compile(GL_FRAGMENT_SHADER, kMeshFragment));
    glBindAttribLocation(meshProgram_, 0, "aPos");
    glBindAttribLocation(meshProgram_, 1, "aUv");
    glBindAttribLocation(meshProgram_, 2, "aColor");
    glLinkProgram(meshProgram_);
    for (Object* a : w.actors) {
        if (a->deleted || w.var(a, "DrawType").i() != 8 || w.flag(a, "bHidden")) continue;
        Object* mo = w.obj(a, "StaticMesh");
        const StaticMeshCollision* m = mo ? col.mesh(mo) : nullptr;
        if (!m || m->indices.empty()) continue;
        MeshBuffers& mb = meshBuffers_[m];
        if (!mb.vertices) {
            std::vector<float> v;
            for (size_t i = 0; i < m->positions.size(); ++i) {
                v.insert(v.end(), {m->positions[i].x, m->positions[i].y, m->positions[i].z});
                bool has = m->uv.size() >= (i + 1) * 2;
                v.push_back(has ? m->uv[i * 2] : 0);
                v.push_back(has ? m->uv[i * 2 + 1] : 0);
            }
            glGenBuffers(1, &mb.vertices);
            glBindBuffer(GL_ARRAY_BUFFER, mb.vertices);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(v.size() * sizeof(float)), v.data(), GL_STATIC_DRAW);
            glGenBuffers(1, &mb.indices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mb.indices);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(m->indices.size() * 2), m->indices.data(), GL_STATIC_DRAW);
        }
        MeshDraw d;
        d.mesh = &mb;
        // the transform, as the collision places the same mesh:
        // Location + R S (v - PrePivot)
        int32_t pitch, yaw, roll;
        w.vm.unrotator(w.var(a, "Rotation"), pitch, yaw, roll);
        float axes[3][3];
        rotationAxes(pitch, yaw, roll, axes);
        float sc = w.var(a, "DrawScale").f();
        Vec3 s3, loc, pp;
        w.vm.unvector(w.var(a, "DrawScale3D"), s3.x, s3.y, s3.z);
        w.vm.unvector(w.var(a, "Location"), loc.x, loc.y, loc.z);
        w.vm.unvector(w.var(a, "PrePivot"), pp.x, pp.y, pp.z);
        const float k3[3] = {sc * s3.x, sc * s3.y, sc * s3.z};
        const float ppa[3] = {pp.x, pp.y, pp.z};
        float origin[3] = {loc.x, loc.y, loc.z};
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) {
                d.model[c * 4 + r] = axes[c][r] * k3[c];
                origin[r] -= d.model[c * 4 + r] * ppa[c];
            }
        d.model[3] = d.model[7] = d.model[11] = 0;
        d.model[12] = origin[0];
        d.model[13] = origin[1];
        d.model[14] = origin[2];
        d.model[15] = 1;
        // the baked light of this placing: its StaticMeshInstance's colours
        ObjectRef inst = refOf(w.obj(a, "StaticMeshInstance"));
        if (inst) {
            std::vector<TagEntry> tags;
            size_t at = 0;
            if (Library::properties(inst, tags, at)) {
                try {
                    Reader r(inst.pkg->data, at, size_t(inst.exp().off + inst.exp().size));
                    int32_t n = r.idx();
                    if (n == int32_t(m->positions.size())) {
                        std::vector<uint8_t> rgb;
                        bool black = true;
                        for (int32_t i = 0; i < n; ++i) {
                            uint8_t c[4] = {r.u8(), r.u8(), r.u8(), r.u8()};
                            rgb.insert(rgb.end(), {c[0], c[1], c[2]});
                            black &= !c[0] && !c[1] && !c[2];
                        }
                        r.u32();            // revision
                        // the lights, each with a bit a vertex for where it reaches
                        std::vector<std::pair<Object*, std::vector<uint8_t>>> lights;
                        for (int32_t k = 0, nl = r.idx(); k < nl && k < 256; ++k) {
                            int32_t actor = r.idx();
                            int32_t len = r.idx();
                            r.need(size_t(len));
                            std::vector<uint8_t> mask(inst.pkg->data.begin() + long(r.p),
                                                      inst.pkg->data.begin() + long(r.p + size_t(len)));
                            r.p += size_t(len);
                            r.u32();        // applied
                            auto it = w.actorAt.find(actor);
                            if (it != w.actorAt.end()) lights.emplace_back(it->second, std::move(mask));
                        }
                        // Black throughout although lights reach it: the colours
                        // are a cache the engine fills, so fill it here.
                        if (black && !lights.empty()) {
                            rgb = bakeVertexLight(w, *m, d.model, lights);
                            ++relit;
                        }
                        glGenBuffers(1, &d.colors);
                        glBindBuffer(GL_ARRAY_BUFFER, d.colors);
                        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(rgb.size()), rgb.data(), GL_STATIC_DRAW);
                    }
                } catch (const FormatError&) {
                }
            }
        }
        BspModel::Region rg = bsp.regionAt(loc);
        if (rg.zone >= 0 && size_t(rg.zone) < bsp.zoneActors.size()) {
            auto it = w.actorAt.find(bsp.zoneActors[size_t(rg.zone)]);
            d.ambient = ambientOf(w, it != w.actorAt.end() ? it->second : w.info);
        }
        // a texture a section: the mesh's Materials, an actor's Skins over them
        ObjectRef meshRef = refOf(mo);
        std::vector<std::vector<TagEntry>> items;
        if (meshRef) {
            std::vector<TagEntry> tags;
            size_t end = 0;
            if (Library::properties(meshRef, tags, end))
                if (const TagEntry* t = findTag(*meshRef.pkg, tags, "Materials")) tagStructArray(*meshRef.pkg, *t, items);
        }
        const Value& skins = w.var(a, "Skins");
        for (size_t si = 0; si < m->sections.size(); ++si) {
            const auto& sec = m->sections[si];
            if (!sec.faces) continue;
            SurfaceMaterial mat;
            Object* skin = skins.isArr() && si < skins.arr().size() ? skins.arr()[si].o() : nullptr;
            ObjectRef sk = refOf(skin);
            if (sk) {
                mat = materials_.resolve(*sk.pkg, sk.idx);
            } else if (meshRef && si < items.size()) {
                mat = materials_.resolve(*meshRef.pkg, tagObject(*meshRef.pkg, items[si], "Material"));
            }
            int tw, th;
            unsigned tex = textureFor(mat, tw, th);
            if (mat.blend == Blend::Invisible) continue;
            d.parts.push_back({sec.firstIndex, sec.faces * 3, tex, mat});
            triangles += size_t(sec.faces);
        }
        meshDraws_.push_back(std::move(d));
    }
    meshes = meshDraws_.size();
}

namespace {

// The frame buffer blending for a material, and whether it writes depth.
void applyBlend(const SurfaceMaterial& m) {
    glDepthMask(m.zwrite ? GL_TRUE : GL_FALSE);
    switch (m.blend) {
    case Blend::Opaque: case Blend::Invisible:
        glDisable(GL_BLEND);
        return;
    case Blend::Alpha:
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        return;
    case Blend::Modulate:
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        return;
    case Blend::Translucent:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
        return;
    case Blend::Brighten:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        return;
    case Blend::Darken:
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
        return;
    }
}

}  // namespace

LevelRender::SkelDraw& LevelRender::skelFor(Object* a) {
    SkelDraw& d = skel_[a];
    if (d.mesh || d.failed) return d;
    Animator& an = *session_.animator;
    const SkeletalMesh* m = an.state(a).mesh;
    if (!m || m->wedges.size() > 65535) {
        d.failed = true;
        return d;
    }
    d.mesh = m;
    glGenBuffers(1, &d.vertices);
    glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m->wedges.size() * 8 * sizeof(float)), nullptr, GL_DYNAMIC_DRAW);
    // faces by material: the actor's Skins over the mesh's materials
    World& w = *session_.world;
    const Value& skins = w.var(a, "Skins");
    std::map<uint16_t, std::vector<uint16_t>> byMat;
    for (const SkeletalMesh::Face& f : m->faces) byMat[f.material].insert(byMat[f.material].end(), f.wedge, f.wedge + 3);
    std::vector<uint16_t> idx;
    for (auto& [mi, list] : byMat) {
        SurfaceMaterial mat;
        Object* skin = skins.isArr() && mi < skins.arr().size() ? skins.arr()[mi].o() : nullptr;
        ObjectRef sk = refOf(skin);
        if (sk) mat = materials_.resolve(*sk.pkg, sk.idx);
        else if (mi < m->materials.size()) mat = materials_.resolve(*m->package, m->materials[mi]);
        if (mat.blend == Blend::Invisible) continue;
        int tw, th;
        unsigned tex = textureFor(mat, tw, th);
        d.parts.push_back({int(idx.size()), int(list.size()), tex, mat});
        idx.insert(idx.end(), list.begin(), list.end());
    }
    glGenBuffers(1, &d.indices);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 2), idx.data(), GL_STATIC_DRAW);
    d.scratch.resize(m->wedges.size() * 8);
    ++characters;
    return d;
}

// A character's light at each of its points, as the engine lights an actor:
// the MaxLights lights strongest where it stands, each falling off and lighting
// by N.L as a static mesh's light does (bakeVertexLight), without shadows. In
// the units of the baked colours: the shader doubles it and adds the ambient.
std::vector<Vec3> LevelRender::characterLight(Object* a, const SkeletalMesh& mesh, const std::vector<Vec3>& pts,
                                              const float r[3][3], Vec3 loc) {
    World& w = *session_.world;
    std::vector<Vec3> out(pts.size(), Vec3{0.5f, 0.5f, 0.5f});
    if (w.flag(a, "bUnlit")) return out;
    if (!lightsBuilt_) {
        lightsBuilt_ = true;
        if (Class* lc = session_.vm->findClass("Light"))
            for (Object* l : w.actors)
                if (!l->deleted && l->isA(lc) && w.var(l, "LightType").i() != 0 && w.var(l, "LightBrightness").f() > 0)
                    lights_.push_back(l);
    }
    struct Lit {
        bool sun;
        Vec3 pos, dir, col;
        float bright, radius, strength;
    };
    std::vector<Lit> near;
    for (Object* l : lights_) {
        if (l->deleted) continue;
        Lit li;
        li.sun = l->cls->name == Name("Sunlight");
        w.vm.unvector(w.var(l, "Location"), li.pos.x, li.pos.y, li.pos.z);
        li.bright = w.var(l, "LightBrightness").f();
        li.radius = w.var(l, "LightRadius").f() * 25.0f;
        float d = length(li.pos - loc);
        if (!li.sun && d >= li.radius) continue;
        float x = li.sun ? 0 : d / li.radius;
        li.strength = li.bright * (1 - x * x) * (1 - x * x);
        int32_t pr, yr, rr;
        w.vm.unrotator(w.var(l, "Rotation"), pr, yr, rr);
        float ax[3][3];
        rotationAxes(pr, yr, rr, ax);
        li.dir = {ax[0][0], ax[0][1], ax[0][2]};
        float h = float(w.var(l, "LightHue").i()) / 255.0f, sat = 1.0f - float(w.var(l, "LightSaturation").i()) / 255.0f;
        float hi = std::floor(h * 6), f = h * 6 - hi, pv = 1 - sat, qv = 1 - f * sat, tv = 1 - (1 - f) * sat;
        switch (int(hi) % 6) {
        case 0: li.col = {1, tv, pv}; break;
        case 1: li.col = {qv, 1, pv}; break;
        case 2: li.col = {pv, 1, tv}; break;
        case 3: li.col = {pv, qv, 1}; break;
        case 4: li.col = {tv, pv, 1}; break;
        default: li.col = {1, pv, qv};
        }
        near.push_back(li);
    }
    size_t most = size_t(std::max(1, w.var(a, "MaxLights").i()));
    std::sort(near.begin(), near.end(), [](const Lit& x, const Lit& y) { return x.strength > y.strength; });
    if (near.size() > most) near.resize(most);
    // normals from the posed faces, turned to face away from the middle
    std::vector<Vec3> nrm(pts.size());
    for (const SkeletalMesh::Face& f : mesh.faces) {
        uint16_t p0 = mesh.wedges[f.wedge[0]].point, p1 = mesh.wedges[f.wedge[1]].point, p2 = mesh.wedges[f.wedge[2]].point;
        Vec3 n = cross(pts[p1] - pts[p0], pts[p2] - pts[p0]);
        nrm[p0] = nrm[p0] + n;
        nrm[p1] = nrm[p1] + n;
        nrm[p2] = nrm[p2] + n;
    }
    Vec3 mid{};
    for (const Vec3& p : pts) mid = mid + p;
    if (!pts.empty()) mid = mid * (1.0f / float(pts.size()));
    float outward = 0;
    for (size_t i = 0; i < pts.size(); ++i) outward += dot(nrm[i], pts[i] - mid);
    float sign = outward < 0 ? -1.0f : 1.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        Vec3 p = pts[i], n = nrm[i] * sign;
        Vec3 wp{loc.x + r[0][0] * p.x + r[0][1] * p.y + r[0][2] * p.z, loc.y + r[1][0] * p.x + r[1][1] * p.y + r[1][2] * p.z,
                loc.z + r[2][0] * p.x + r[2][1] * p.y + r[2][2] * p.z};
        Vec3 wn{r[0][0] * n.x + r[0][1] * n.y + r[0][2] * n.z, r[1][0] * n.x + r[1][1] * n.y + r[1][2] * n.z,
                r[2][0] * n.x + r[2][1] * n.y + r[2][2] * n.z};
        float ln = length(wn);
        if (ln > 0) wn = wn * (1 / ln);
        Vec3 c{};
        for (const Lit& li : near) {
            float k;
            if (li.sun) {
                k = std::max(0.0f, -dot(wn, li.dir));
            } else {
                Vec3 d = li.pos - wp;
                float dist = length(d);
                if (dist <= 0 || dist >= li.radius) continue;
                float x = dist / li.radius;
                k = (1 - x * x) * (1 - x * x) * std::max(0.0f, dot(wn, d) / dist);
            }
            c = c + li.col * (0.65f * li.bright / 255.0f * k);
        }
        out[i] = {std::min(c.x, 1.0f), std::min(c.y, 1.0f), std::min(c.z, 1.0f)};
    }
    return out;
}

void LevelRender::drawSkeletal(const float mvp[16], Vec3 eye) {
    World& w = *session_.world;
    Animator& an = *session_.animator;
    glUseProgram(meshProgram_);
    glActiveTexture(GL_TEXTURE0);
    GLint uM = glGetUniformLocation(meshProgram_, "uMvp"), uB = glGetUniformLocation(meshProgram_, "uBaked");
    GLint uC = glGetUniformLocation(meshProgram_, "uCut"), uA = glGetUniformLocation(meshProgram_, "uAmb");
    glUniform1f(uB, 1.0f);
    for (Object* a : w.actors) {
        if (a->deleted || w.var(a, "DrawType").i() != 2 || w.flag(a, "bHidden") || !w.obj(a, "Mesh")) continue;
        Vec3 o;
        w.vm.unvector(w.var(a, "Location"), o.x, o.y, o.z);
        if (length(o - eye) > 8000) continue;
        SkelDraw& d = skelFor(a);
        if (!d.mesh) continue;
        // the zone's ambient and the actor's own glow; unlit, full bright
        Vec3 ambient{};
        {
            const BspModel& bsp = session_.collision->bsp;
            BspModel::Region rg = bsp.regionAt(o);
            if (rg.zone >= 0 && size_t(rg.zone) < bsp.zoneActors.size()) {
                auto it = w.actorAt.find(bsp.zoneActors[size_t(rg.zone)]);
                ambient = ambientOf(w, it != w.actorAt.end() ? it->second : w.info);
            }
            float glow = float(w.var(a, "AmbientGlow").i()) / 255.0f;
            ambient = ambient + Vec3{glow, glow, glow};
        }
        // the pose, skinned, into the actor's space; the actor's transform
        // goes to the shader
        std::vector<Vec3> pts = d.mesh->skin(an.pose(a));
        for (Vec3& p : pts) p = d.mesh->toActor(p);
        float r[3][3], model[16] = {0};
        Vec3 loc;
        an.meshToWorld(a, r, loc);
        std::vector<Vec3> light = characterLight(a, *d.mesh, pts, r, loc);
        for (size_t i = 0; i < d.mesh->wedges.size(); ++i) {
            const SkeletalMesh::Wedge& wd = d.mesh->wedges[i];
            const Vec3& p = pts[wd.point];
            const Vec3& c = light[wd.point];
            float* v = &d.scratch[i * 8];
            v[0] = p.x;
            v[1] = p.y;
            v[2] = p.z;
            v[3] = wd.u;
            v[4] = wd.v;
            v[5] = c.x;
            v[6] = c.y;
            v[7] = c.z;
        }
        glBindBuffer(GL_ARRAY_BUFFER, d.vertices);
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(d.scratch.size() * sizeof(float)), d.scratch.data());
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k) model[c * 4 + k] = r[k][c];
        model[12] = loc.x;
        model[13] = loc.y;
        model[14] = loc.z;
        model[15] = 1;
        float m[16];
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) {
                float sum = 0;
                for (int j = 0; j < 4; ++j) sum += mvp[j * 4 + k] * model[c * 4 + j];
                m[c * 4 + k] = sum;
            }
        glUniformMatrix4fv(uM, 1, GL_FALSE, m);
        glUniform3f(uA, ambient.x, ambient.y, ambient.z);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(12));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), reinterpret_cast<void*>(20));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.indices);
        for (const MeshDraw::Part& part : d.parts) {
            applyBlend(part.mat);
            glBindTexture(GL_TEXTURE_2D, part.texture);
            glUniform1f(uC, part.mat.alphaRef);
            glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_SHORT, reinterpret_cast<void*>(size_t(part.first) * 2));
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

void LevelRender::draw(Vec3 loc, const int32_t rot[3], int width, int height, float fov) {
    glViewport(0, 0, width, height);
    glClearColor(0.35f, 0.45f, 0.55f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // The view: Unreal's X ahead, Y right, Z up, into GL's eye space, right
    // +x, up +y, looking down -z.
    float ax[3][3];
    rotationAxes(rot[0], rot[1], rot[2], ax);
    float view[16] = {
        ax[1][0], ax[2][0], -ax[0][0], 0,
        ax[1][1], ax[2][1], -ax[0][1], 0,
        ax[1][2], ax[2][2], -ax[0][2], 0,
        0, 0, 0, 1,
    };
    for (int r = 0; r < 3; ++r)
        view[12 + r] = -(view[r] * loc.x + view[4 + r] * loc.y + view[8 + r] * loc.z);
    float aspect = float(width) / float(std::max(1, height));
    float fx = 1.0f / std::tan(fov * 3.14159265f / 360.0f), fy = fx * aspect;
    float zn = 4.0f, zf = 65536.0f;
    float proj[16] = {fx, 0, 0, 0, 0, fy, 0, 0, 0, 0, (zf + zn) / (zn - zf), -1, 0, 0, 2 * zf * zn / (zn - zf), 0};
    float mvp[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) sum += proj[k * 4 + r] * view[c * 4 + k];
            mvp[c * 4 + r] = sum;
        }
    glUseProgram(program_);
    glUniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp);
    glUniform1i(uTex_, 0);
    glUniform1i(uLm_, 1);
    // Opaque first, then the blended, which do not write depth.
    for (int pass = 0; pass < 2; ++pass) {
        for (const Batch& b : batches_) {
            if (b.mat.blended() != (pass == 1)) continue;
            applyBlend(b.mat);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, b.texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, b.lightMap);
            glUniform1f(uLit_, b.unlit ? 0.0f : 1.0f);
            glUniform1f(uCut_, b.mat.alphaRef);
            glBindBuffer(GL_ARRAY_BUFFER, b.buffer);
            const GLsizei stride = 10 * sizeof(float);
            glEnableVertexAttribArray(GLuint(aPos_));
            glVertexAttribPointer(GLuint(aPos_), 3, GL_FLOAT, GL_FALSE, stride, nullptr);
            glEnableVertexAttribArray(GLuint(aUv_));
            glVertexAttribPointer(GLuint(aUv_), 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(12));
            glEnableVertexAttribArray(GLuint(aUv2_));
            glVertexAttribPointer(GLuint(aUv2_), 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(20));
            glEnableVertexAttribArray(GLuint(aAmb_));
            glVertexAttribPointer(GLuint(aAmb_), 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(28));
            glDrawArrays(GL_TRIANGLES, 0, b.count);
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    for (const TerrainDraw& td : terrains_) {
        glUseProgram(td.program);
        glUniformMatrix4fv(glGetUniformLocation(td.program, "uMvp"), 1, GL_FALSE, mvp);
        for (size_t i = 0; i < td.textures.size(); ++i) {
            std::string k = std::to_string(i);
            glActiveTexture(GLenum(GL_TEXTURE0 + i));
            glBindTexture(GL_TEXTURE_2D, td.textures[i]);
            glUniform1i(glGetUniformLocation(td.program, ("uT" + k).c_str()), GLint(i));
            glUniform4fv(glGetUniformLocation(td.program, ("uU" + k).c_str()), 1, td.uv[i].data());
            glUniform4fv(glGetUniformLocation(td.program, ("uV" + k).c_str()), 1, td.uv[i].data() + 4);
        }
        for (const TerrainDraw::Band& b : td.bands) {
            glBindBuffer(GL_ARRAY_BUFFER, b.vertices);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.indices);
            const GLsizei stride = 17 * sizeof(float);
            const int sizes[5] = {3, 3, 3, 4, 4}, offsets[5] = {0, 3, 6, 9, 13};
            for (int a = 0; a < 5; ++a) {
                glEnableVertexAttribArray(GLuint(a));
                glVertexAttribPointer(GLuint(a), sizes[a], GL_FLOAT, GL_FALSE, stride,
                                      reinterpret_cast<void*>(size_t(offsets[a]) * sizeof(float)));
            }
            glDrawElements(GL_TRIANGLES, b.count, GL_UNSIGNED_SHORT, nullptr);
        }
    }
    drawSkeletal(mvp, loc);
    // the static meshes, opaque then blended
    glUseProgram(meshProgram_);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(meshProgram_, "uTex"), 0);
    GLint uM = glGetUniformLocation(meshProgram_, "uMvp"), uA = glGetUniformLocation(meshProgram_, "uAmb");
    GLint uB = glGetUniformLocation(meshProgram_, "uBaked"), uC = glGetUniformLocation(meshProgram_, "uCut");
    for (int pass = 0; pass < 2; ++pass) {
        for (const MeshDraw& d : meshDraws_) {
            float m[16];
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) {
                    float sum = 0;
                    for (int k = 0; k < 4; ++k) sum += mvp[k * 4 + r] * d.model[c * 4 + k];
                    m[c * 4 + r] = sum;
                }
            glUniformMatrix4fv(uM, 1, GL_FALSE, m);
            glUniform3f(uA, d.ambient.x, d.ambient.y, d.ambient.z);
            glUniform1f(uB, d.colors ? 1.0f : 0.0f);
            glBindBuffer(GL_ARRAY_BUFFER, d.mesh->vertices);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(12));
            if (d.colors) {
                glBindBuffer(GL_ARRAY_BUFFER, d.colors);
                glEnableVertexAttribArray(2);
                glVertexAttribPointer(2, 3, GL_UNSIGNED_BYTE, GL_TRUE, 0, nullptr);
            } else {
                glDisableVertexAttribArray(2);
                glVertexAttrib3f(2, 0.5f, 0.5f, 0.5f);
            }
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, d.mesh->indices);
            for (const MeshDraw::Part& part : d.parts) {
                if (part.mat.blended() != (pass == 1)) continue;
                applyBlend(part.mat);
                glBindTexture(GL_TEXTURE_2D, part.texture);
                glUniform1f(uC, part.mat.alphaRef);
                glDrawElements(GL_TRIANGLES, part.count, GL_UNSIGNED_SHORT,
                               reinterpret_cast<void*>(size_t(part.first) * 2));
            }
        }
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
}

Image readFramebuffer(int width, int height) {
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(size_t(width) * size_t(height) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    // GL's rows run bottom to top
    std::vector<uint8_t> row(size_t(width) * 4);
    for (int y = 0; y < height / 2; ++y) {
        uint8_t* a = img.rgba.data() + size_t(y) * row.size();
        uint8_t* b = img.rgba.data() + size_t(height - 1 - y) * row.size();
        std::memcpy(row.data(), a, row.size());
        std::memcpy(a, b, row.size());
        std::memcpy(b, row.data(), row.size());
    }
    return img;
}

namespace {

uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        made = true;
    }
    c = ~c;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

void be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}

}  // namespace

void writePng(const std::string& path, const Image& img) {
    // the image rows with their filter byte, in deflate's stored blocks
    std::vector<uint8_t> raw;
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), img.rgba.begin() + y * img.width * 4, img.rgba.begin() + (y + 1) * img.width * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t at = 0; at < raw.size(); at += 65535) {
        size_t n = std::min<size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(at), raw.begin() + long(at + n));
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(out, uint32_t(data.size()));
        std::vector<uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        out.insert(out.end(), td.begin(), td.end());
        be32(out, crc32(td.data(), td.size()));
    };
    std::vector<uint8_t> ihdr;
    be32(ihdr, uint32_t(img.width));
    be32(ihdr, uint32_t(img.height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
}

}  // namespace ffa
