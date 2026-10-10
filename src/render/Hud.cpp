// The HUD: what the game's HUD draws on a Canvas, after the frame.
//
// Each frame the player's HUD gets PostRender(Canvas), as the engine sends
// it, on one Canvas object: SizeX and SizeY the window's, ClipX and ClipY
// the same, and Reset first, which puts back the rest. The HUD's script draws
// through Canvas's natives: a tile of a material at OrgX + CurX, OrgY + CurY,
// by DrawColor and Style, after which CurX moves on by its width; text in the
// Canvas's Font, glyph by glyph from the font's pages. The tiles are drawn in
// order over the frame. KnowWonder's cutscenes draw through it their
// letterbox borders, the hourglass over a skipped cutscene, and subtitles.
// The open menus are drawn after it on the same Canvas.
#include <GLES2/gl2.h>

#include <algorithm>
#include <cmath>

#include "render/LevelRender.h"
#include "world/Gui.h"
#include "world/World.h"

namespace ffa {

namespace {

LevelRender* current = nullptr;     // the renderer the natives draw into, during PostRender

unsigned compileShader(GLenum type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

// A Canvas variable, by name.
Value& field(VM& vm, Object* c, const char* name) {
    Prop* p = c->cls->findProp(Name(name));
    if (!p) throw vm.error(std::string("Canvas has no variable ") + name);
    return *vm.slot(c, p);
}
float ff(VM& vm, Object* c, const char* n) { return field(vm, c, n).f(); }
void setf(VM& vm, Object* c, const char* n, float v) { field(vm, c, n) = Value::Float(v); }

// DrawColor, R G B A a byte each, as 0 to 1.
void colorOf(VM& vm, Object* c, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 1;
    const Value& v = field(vm, c, "DrawColor");
    if (!v.isStruct()) return;
    const char* names[4] = {"R", "G", "B", "A"};
    for (int k = 0; k < 4; ++k)
        if (Prop* f = v.st().type->field(Name(names[k]))) out[k] = float(v.st().f[size_t(f->slot)].i()) / 255.0f;
}

// A rectangle of a texture, by the Canvas's colour and style; with `clip`,
// cut at its ClipX and ClipY, the texture with it.
void push(VM& vm, Object* c, unsigned tex, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
          bool clip) {
    if (!current || !tex || w == 0 || h == 0) return;
    if (clip) {
        float cx = ff(vm, c, "OrgX") + ff(vm, c, "ClipX"), cy = ff(vm, c, "OrgY") + ff(vm, c, "ClipY");
        if (x >= cx || y >= cy) return;
        if (x + w > cx) {
            u1 = u0 + (u1 - u0) * (cx - x) / w;
            w = cx - x;
        }
        if (y + h > cy) {
            v1 = v0 + (v1 - v0) * (cy - y) / h;
            h = cy - y;
        }
    }
    LevelRender::HudTile t;
    t.texture = tex;
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    t.u0 = u0;
    t.v0 = v0;
    t.u1 = u1;
    t.v1 = v1;
    colorOf(vm, c, t.color);
    t.style = field(vm, c, "Style").i();
    current->hudPush(t);
}

const FontData* fontOf(VM& vm, Object* c) { return current ? current->hudFont(field(vm, c, "Font").o()) : nullptr; }

// A line of text's width, and its height: the tallest glyph's, or A's.
void measure(VM& vm, Object* c, const String& text, float& w, float& h) {
    w = h = 0;
    const FontData* f = fontOf(vm, c);
    if (!f) return;
    for (char16_t ch : text)
        if (const FontGlyph* g = f->glyph(ch)) {
            w += float(g->width);
            h = std::max(h, float(g->height));
        }
    if (h == 0)
        if (const FontGlyph* g = f->glyph(u'A')) h = float(g->height);
}

void text(VM& vm, Object* c, const String& s, float x, float y, bool clip) {
    const FontData* f = fontOf(vm, c);
    if (!f) return;
    for (char16_t ch : s) {
        const FontGlyph* g = f->glyph(ch);
        if (!g) continue;
        int pw = 0, ph = 0;
        unsigned tex = current->hudPage(*f, g->page, pw, ph);
        if (pw > 0 && ph > 0)
            push(vm, c, tex, x, y, float(g->width), float(g->height), float(g->u) / float(pw), float(g->v) / float(ph),
                 float(g->u + g->width) / float(pw), float(g->v + g->height) / float(ph), clip);
        x += float(g->width);
    }
}

void advance(VM& vm, Object* c, float w, float h) {
    setf(vm, c, "CurX", ff(vm, c, "CurX") + w);
    setf(vm, c, "CurYL", std::max(ff(vm, c, "CurYL"), h));
}

void registerCanvasNatives(VM& vm) {
    auto& n = vm.natives;
    // DrawTile(Material, XL, YL, U, V, UL, VL): U V UL VL in the texture's texels
    n["canvas.drawtile"] = [](NativeCall& c) {
        int w = 0, h = 0;
        unsigned tex = current ? current->hudTexture(c.o(0), w, h) : 0;
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        float XL = c.f(1), YL = c.f(2), U = c.f(3), V = c.f(4), UL = c.f(5), VL = c.f(6);
        if (w > 0 && h > 0) push(c.vm, c.self, tex, x, y, XL, YL, U / float(w), V / float(h), (U + UL) / float(w), (V + VL) / float(h), false);
        advance(c.vm, c.self, XL, YL);
        return Value();
    };
    n["canvas.drawtileclipped"] = [](NativeCall& c) {
        int w = 0, h = 0;
        unsigned tex = current ? current->hudTexture(c.o(0), w, h) : 0;
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        float XL = c.f(1), YL = c.f(2), U = c.f(3), V = c.f(4), UL = c.f(5), VL = c.f(6);
        if (w > 0 && h > 0) push(c.vm, c.self, tex, x, y, XL, YL, U / float(w), V / float(h), (U + UL) / float(w), (V + VL) / float(h), true);
        advance(c.vm, c.self, XL, YL);
        return Value();
    };
    // DrawTileStretched(Material, XL, YL), DrawTileJustified(Material,
    // Justification, XL, YL): the whole texture over the rectangle
    n["canvas.drawtilestretched"] = [](NativeCall& c) {
        int w = 0, h = 0;
        unsigned tex = current ? current->hudTexture(c.o(0), w, h) : 0;
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        push(c.vm, c.self, tex, x, y, c.f(1), c.f(2), 0, 0, 1, 1, false);
        advance(c.vm, c.self, c.f(1), c.f(2));
        return Value();
    };
    n["canvas.drawtilejustified"] = [](NativeCall& c) {
        int w = 0, h = 0;
        unsigned tex = current ? current->hudTexture(c.o(0), w, h) : 0;
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        push(c.vm, c.self, tex, x, y, c.f(2), c.f(3), 0, 0, 1, 1, false);
        advance(c.vm, c.self, c.f(2), c.f(3));
        return Value();
    };
    // DrawTileScaled(Material, XScale, YScale): the texture at its size, scaled
    n["canvas.drawtilescaled"] = [](NativeCall& c) {
        int w = 0, h = 0;
        unsigned tex = current ? current->hudTexture(c.o(0), w, h) : 0;
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        float XL = float(w) * c.f(1), YL = float(h) * c.f(2);
        push(c.vm, c.self, tex, x, y, XL, YL, 0, 0, 1, 1, false);
        advance(c.vm, c.self, XL, YL);
        return Value();
    };
    // DrawText(Text, optional CR): centred across ClipX with bCenter; then on
    // along the line, or with CR down a line and back to the left
    n["canvas.drawtext"] = [](NativeCall& c) {
        String s = c.s(0);
        float XL, YL;
        measure(c.vm, c.self, s, XL, YL);
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        if (field(c.vm, c.self, "bCenter").b()) x = ff(c.vm, c.self, "OrgX") + (ff(c.vm, c.self, "ClipX") - XL) / 2;
        text(c.vm, c.self, s, x, y, false);
        if (c.b(1)) {
            setf(c.vm, c.self, "CurX", 0);
            setf(c.vm, c.self, "CurY", ff(c.vm, c.self, "CurY") + YL);
        } else {
            advance(c.vm, c.self, XL, YL);
        }
        return Value();
    };
    n["canvas.drawtextclipped"] = [](NativeCall& c) {
        float x = ff(c.vm, c.self, "OrgX") + ff(c.vm, c.self, "CurX"), y = ff(c.vm, c.self, "OrgY") + ff(c.vm, c.self, "CurY");
        text(c.vm, c.self, c.s(0), x, y, true);
        return Value();
    };
    // DrawTextJustified(String, Justification, X1, Y1, X2, Y2): left, centre or
    // right across the box, centred down it
    n["canvas.drawtextjustified"] = [](NativeCall& c) {
        String s = c.s(0);
        float XL, YL;
        measure(c.vm, c.self, s, XL, YL);
        int j = c.i(1);
        float x1 = c.f(2), y1 = c.f(3), x2 = c.f(4), y2 = c.f(5);
        float x = x1 + (j == 1 ? (x2 - x1 - XL) / 2 : j == 2 ? x2 - x1 - XL : 0);
        float y = y1 + (y2 - y1 - YL) / 2;
        text(c.vm, c.self, s, ff(c.vm, c.self, "OrgX") + x, ff(c.vm, c.self, "OrgY") + y, false);
        return Value();
    };
    // DrawActor(Actor, Wireframe, optional ClearZ, optional DisplayFOV): the
    // actor as the camera sees it, in turn with the tiles; the in-game
    // menu's book over its page
    n["canvas.drawactor"] = [](NativeCall& c) {
        if (!current || !c.o(0)) return Value();
        LevelRender::HudTile t;
        t.actor = c.o(0);
        t.clearZ = c.b(2);
        current->hudPush(t);
        return Value();
    };
    n["canvas.textsize"] = [](NativeCall& c) {
        float XL, YL;
        measure(c.vm, c.self, c.s(0), XL, YL);
        c.out(1, Value::Float(XL));
        c.out(2, Value::Float(YL));
        return Value();
    };
    n["canvas.strlen"] = [](NativeCall& c) {
        float XL, YL;
        measure(c.vm, c.self, c.s(0), XL, YL);
        c.out(1, Value::Float(XL));
        c.out(2, Value::Float(YL));
        return Value();
    };
    // WrapStringToArray(Text, out Array, dx, EOL): lines at EOL, and words
    // broken to lines no wider than dx
    n["canvas.wrapstringtoarray"] = [](NativeCall& c) {
        String all = c.s(0), eol = c.s(3);
        float dx = c.f(2);
        Array out;
        auto line = [&](const String& para) {
            String cur;
            size_t at = 0;
            while (at <= para.size()) {
                size_t sp = para.find(u' ', at);
                String word = para.substr(at, sp == String::npos ? String::npos : sp - at);
                String tryLine = cur.empty() ? word : cur + u" " + word;
                float w, h;
                measure(c.vm, c.self, tryLine, w, h);
                if (!cur.empty() && dx > 0 && w > dx) {
                    out.push_back(Value::Str(cur));
                    cur = word;
                } else {
                    cur = tryLine;
                }
                if (sp == String::npos) break;
                at = sp + 1;
            }
            out.push_back(Value::Str(cur));
        };
        size_t at = 0;
        while (true) {
            size_t e = eol.empty() ? String::npos : all.find(eol, at);
            line(all.substr(at, e == String::npos ? String::npos : e - at));
            if (e == String::npos) break;
            at = e + eol.size();
        }
        c.out(1, Value::Arr(std::move(out)));
        return Value();
    };
}

const char* kHudVertex = R"(
attribute vec2 aPos;
attribute vec2 aUv;
uniform vec2 uSize;
varying vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos.x / uSize.x * 2.0 - 1.0, 1.0 - aPos.y / uSize.y * 2.0, 0.0, 1.0);
}
)";

const char* kHudFragment = R"(
precision mediump float;
uniform sampler2D uTex;
uniform vec4 uColor;
uniform float uCut;
varying vec2 vUv;
void main() {
    vec4 c = texture2D(uTex, vUv) * uColor;
    if (c.a < uCut) discard;
    gl_FragColor = c;
}
)";

}  // namespace

// Both by the object, found in its package once: finding it by its path
// walks the package's exports, about a millisecond for a font, and the
// subtitles' WrapStringToArray asks for the font for every word, which came
// to 28 ms a frame while a subtitle showed.
unsigned LevelRender::hudTexture(Object* material, int& width, int& height) {
    auto hit = hudTextures_.find(material);
    if (hit != hudTextures_.end()) {
        width = hit->second.width;
        height = hit->second.height;
        return hit->second.texture;
    }
    width = height = 0;
    unsigned t = 0;
    ObjectRef r = refOf(material);
    if (r) {
        SurfaceMaterial m = materials_.resolve(*r.pkg, r.idx);
        if (m.texture) t = textureFor(m, width, height);
    }
    hudTextures_[material] = {t, width, height};
    return t;
}

const FontData* LevelRender::hudFont(Object* font) {
    auto hit = fontByObject_.find(font);
    if (hit != fontByObject_.end()) return hit->second;
    const FontData* out = nullptr;
    ObjectRef r = refOf(font);
    if (r && r.cls() == "Font") {
        auto key = std::make_pair(r.pkg, r.idx);
        auto it = fonts_.find(key);
        if (it == fonts_.end()) {
            FontData f;
            try {
                f = readFont(lib_, r);
            } catch (const std::exception&) {
            }
            it = fonts_.emplace(key, std::move(f)).first;
        }
        out = &it->second;
    }
    fontByObject_[font] = out;
    return out;
}

unsigned LevelRender::hudPage(const FontData& f, int page, int& width, int& height) {
    width = height = 0;
    if (page < 0 || size_t(page) >= f.pages.size() || !f.pages[size_t(page)]) return 0;
    SurfaceMaterial m;
    m.texture = f.pages[size_t(page)];
    return textureFor(m, width, height);
}

void LevelRender::drawHud(int width, int height) {
    World& w = *session_.world;
    VM& vm = *session_.vm;
    hud_.clear();
    Object* pc = session_.controller;
    Object* hud = pc && !pc->deleted && pc->cls->findProp(Name("myHUD")) ? w.obj(pc, "myHUD") : nullptr;
    if (hud && hud->deleted) hud = nullptr;
    if (!canvas_) {
        Class* cc = vm.findClass("Canvas");
        if (!cc) return;
        canvas_ = vm.spawn(cc);
        registerCanvasNatives(vm);
    }
    if (!w.textWidth)
        w.textWidth = [this](Object* font, const String& text) {
            float width = 0;
            if (const FontData* f = hudFont(font))
                for (char16_t ch : text)
                    if (const FontGlyph* gl = f->glyph(ch)) width += float(gl->width);
            return width;
        };
    current = this;
    // A movie the HUD plays covers the frame. Its pictures are not decoded
    // yet, so it is black for its length.
    for (auto& [m, play] : w.movies)
        if (play.playing) {
            HudTile t;
            t.texture = white_;
            t.w = float(width);
            t.h = float(height);
            t.color[0] = t.color[1] = t.color[2] = 0;
            hud_.push_back(t);
            break;
        }
    try {
        vm.call(canvas_, "Reset");
        field(vm, canvas_, "SizeX") = Value::Int(width);
        field(vm, canvas_, "SizeY") = Value::Int(height);
        setf(vm, canvas_, "ClipX", float(width));
        setf(vm, canvas_, "ClipY", float(height));
        if (w.player && field(vm, canvas_, "Viewport").o() == nullptr) field(vm, canvas_, "Viewport") = Value::Obj(w.player);
        if (hud) vm.event(hud, "PostRender", {Value::Obj(canvas_)});
    } catch (const std::exception& ex) {
        ++hudFailures;
        w.failures[std::string("HUD: ") + ex.what()]++;
    }
    // the menus over it, on the same Canvas (world/Gui.cpp)
    drawGui(w, canvas_, width, height);
    current = nullptr;
    hudTiles = hud_.size();
    if (hud_.empty()) return;
    if (!hudProgram_) {
        hudProgram_ = glCreateProgram();
        glAttachShader(hudProgram_, compileShader(GL_VERTEX_SHADER, kHudVertex));
        glAttachShader(hudProgram_, compileShader(GL_FRAGMENT_SHADER, kHudFragment));
        glBindAttribLocation(hudProgram_, 0, "aPos");
        glBindAttribLocation(hudProgram_, 1, "aUv");
        glLinkProgram(hudProgram_);
    }
    glUseProgram(hudProgram_);
    glUniform2f(glGetUniformLocation(hudProgram_, "uSize"), float(width), float(height));
    glUniform1i(glGetUniformLocation(hudProgram_, "uTex"), 0);
    GLint uColor = glGetUniformLocation(hudProgram_, "uColor"), uCut = glGetUniformLocation(hudProgram_, "uCut");
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (int a = 2; a < 5; ++a) glDisableVertexAttribArray(GLuint(a));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    auto hudState = [&] {
        glUseProgram(hudProgram_);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        for (int a = 2; a < 5; ++a) glDisableVertexAttribArray(GLuint(a));
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
    };
    for (const HudTile& t : hud_) {
        if (t.actor) {
            drawHudActor(t.actor, t.clearZ);
            hudState();
            continue;
        }
        // ERenderStyle: Normal and Alpha blend by the texture's alpha, Masked
        // cuts at half, Translucent and Additive add, Modulated multiplies
        float cut = -1;
        glEnable(GL_BLEND);
        switch (t.style) {
        case 2:
            cut = 0.5f;
            glDisable(GL_BLEND);
            break;
        case 3: case 6:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case 4:
            glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            break;
        default:
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        const float pos[8] = {t.x, t.y, t.x + t.w, t.y, t.x, t.y + t.h, t.x + t.w, t.y + t.h};
        const float uv[8] = {t.u0, t.v0, t.u1, t.v0, t.u0, t.v1, t.u1, t.v1};
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
        glBindTexture(GL_TEXTURE_2D, t.texture);
        glUniform4fv(uColor, 1, t.color);
        glUniform1f(uCut, cut);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

}  // namespace ffa
