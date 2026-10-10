#include "world/Gui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_set>

namespace ffa {

namespace {

// eMenuState
enum { MSAT_Blurry, MSAT_Watched, MSAT_Focused, MSAT_Pressed, MSAT_Disabled };
// EInputKey and EInputAction, as far as the GUI minds them
enum { IK_LeftMouse = 1, IK_Enter = 13, IK_Escape = 27, IK_Left = 37, IK_Up = 38, IK_Right = 39, IK_Down = 40 };
enum { IST_Press = 1, IST_Release = 3 };

struct Gui {
    Object* controller = nullptr;
    float width = 640, height = 480;        // the screen, as last drawn
    float mouseX = 0, mouseY = 0;
    Object* watched = nullptr;              // the control under the mouse
    Object* pressed = nullptr;              // the one the button went down on
    std::vector<Object*> timers;
    std::unordered_set<Object*> made;       // controls made from templates
    Class *component = nullptr, *multi = nullptr, *page = nullptr, *button = nullptr, *label = nullptr,
          *image = nullptr, *tabs = nullptr;
};

Gui& gui(World& w) {
    if (!w.gui) {
        auto g = std::make_shared<Gui>();
        g->component = w.vm.findClass("GUIComponent");
        g->multi = w.vm.findClass("GUIMultiComponent");
        g->page = w.vm.findClass("GUIPage");
        g->button = w.vm.findClass("GUIButton");
        g->label = w.vm.findClass("GUILabel");
        g->image = w.vm.findClass("GUIImage");
        g->tabs = w.vm.findClass("GUITabControl");
        w.gui = g;
    }
    return *static_cast<Gui*>(w.gui.get());
}

World* worldOf(NativeCall& c) { return World::of(c.vm); }

bool strcasecmpEq(const std::string& a, const std::string& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower(x) == std::tolower(y); });
}

bool has(Object* o, const char* name) { return o && o->cls->findProp(Name(name)); }

Value color(Object* style, const char* array, int state) {
    Prop* p = style->cls->findProp(Name(array));
    if (!p || state < 0 || state >= p->dim) return Value();
    return style->props[size_t(p->slot + state)];
}
// Element k of a static array variable.
Value& element(Object* o, const char* name, int k) {
    Prop* p = o->cls->findProp(Name(name));
    return o->props[size_t(p->slot + std::clamp(k, 0, p->dim - 1))];
}

// Where a control is: WinLeft, WinTop, WinWidth and WinHeight of 1 or less
// are parts of the screen, or with bBoundToParent and bScaleToParent of the
// control it is on; more are pixels.
float actual(World& w, Object* c, int axis, bool size, int depth = 0) {
    Gui& g = gui(w);
    const char* names[2][2] = {{"WinLeft", "WinWidth"}, {"WinTop", "WinHeight"}};
    float v = w.var(c, names[axis][size ? 1 : 0]).f();
    Object* owner = w.obj(c, "MenuOwner");
    if (depth > 32 || owner == c) owner = nullptr;
    float screen = axis == 0 ? g.width : g.height;
    if (size) {
        if (std::fabs(v) > 1) return v;
        if (owner && w.flag(c, "bScaleToParent")) return v * actual(w, owner, axis, true, depth + 1);
        return v * screen;
    }
    if (owner && w.flag(c, "bBoundToParent")) {
        float base = actual(w, owner, axis, false, depth + 1);
        return base + (std::fabs(v) <= 1 ? v * actual(w, owner, axis, true, depth + 1) : v);
    }
    return std::fabs(v) <= 1 ? v * screen : v;
}

struct Box {
    float x = 0, y = 0, w = 0, h = 0;
    bool in(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
Box boxOf(World& w, Object* c) {
    return {actual(w, c, 0, false), actual(w, c, 1, false), actual(w, c, 0, true), actual(w, c, 1, true)};
}

// A multi-component's controls, in the order they are drawn: by RenderWeight,
// the lightest first.
std::vector<Object*> controlsOf(World& w, Object* c) {
    std::vector<Object*> out;
    if (!has(c, "Controls")) return out;
    const Value& v = w.var(c, "Controls");
    if (!v.isArr()) return out;
    for (const Value& e : v.arr())
        if (Object* o = e.o(); o && !o->deleted) out.push_back(o);
    std::stable_sort(out.begin(), out.end(), [&](Object* a, Object* b) {
        return w.var(a, "RenderWeight").f() < w.var(b, "RenderWeight").f();
    });
    return out;
}

// What is drawn and clicked on a control, in order: its controls, but of a
// tab control its tab buttons and the active tab's panel only, as the
// engine's draws it; Shrek's sound tab shows its controls again whenever it
// is drawn.
std::vector<Object*> childrenOf(World& w, Object* c) {
    Gui& g = gui(w);
    if (!g.tabs || !c->isA(g.tabs)) return controlsOf(w, c);
    std::vector<Object*> out;
    if (const Value& v = w.var(c, "TabStack"); v.isArr())
        for (const Value& e : v.arr())
            if (Object* t = e.o(); t && !t->deleted) out.push_back(t);
    if (Object* active = w.obj(c, "ActiveTab"))
        if (Object* panel = w.obj(active, "MyPanel"); panel && !panel->deleted) out.push_back(panel);
    return out;
}

Object* activePage(World& w) {
    Gui& g = gui(w);
    return g.controller ? w.obj(g.controller, "ActivePage") : nullptr;
}

// The control the mouse is over on a page: the last drawn that takes input.
Object* hitTest(World& w, Object* c, float x, float y) {
    if (!w.flag(c, "bVisible")) return nullptr;
    std::vector<Object*> kids = childrenOf(w, c);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it)
        if (Object* hit = hitTest(w, *it, x, y)) return hit;
    Gui& g = gui(w);
    if (c->isA(g.page) || !w.flag(c, "bAcceptsInput")) return nullptr;
    if (w.var(c, "MenuState").i() == MSAT_Disabled) return nullptr;
    return boxOf(w, c).in(x, y) ? c : nullptr;
}

void stateChange(World& w, Object* c, int s) { w.vm.call(c, "MenuStateChange", {Value::Int(s)}); }

void watch(World& w, Object* hit) {
    Gui& g = gui(w);
    if (hit == g.watched) return;
    if (g.watched && !g.watched->deleted && w.var(g.watched, "MenuState").i() == MSAT_Watched)
        stateChange(w, g.watched, MSAT_Blurry);
    g.watched = hit;
    if (hit && w.var(hit, "MenuState").i() == MSAT_Blurry) stateChange(w, hit, MSAT_Watched);
}

// The controls of the active page that take input, for the pad and the
// arrows: Shrek's buttons are all bNeverFocus, so they move what is watched,
// as the mouse would.
void collect(World& w, Object* c, std::vector<Object*>& out) {
    if (!w.flag(c, "bVisible")) return;
    for (Object* k : childrenOf(w, c)) collect(w, k, out);
    Gui& g = gui(w);
    if (!c->isA(g.page) && w.flag(c, "bAcceptsInput") && w.var(c, "MenuState").i() != MSAT_Disabled &&
        boxOf(w, c).w > 0)
        out.push_back(c);
}

void step(World& w, int key) {
    Gui& g = gui(w);
    Object* page = activePage(w);
    if (!page) return;
    std::vector<Object*> all;
    collect(w, page, all);
    if (all.empty()) return;
    if (!g.watched || std::find(all.begin(), all.end(), g.watched) == all.end()) {
        watch(w, all.front());
        return;
    }
    Box from = boxOf(w, g.watched);
    float fx = from.x + from.w / 2, fy = from.y + from.h / 2;
    float dx = key == IK_Left ? -1.0f : key == IK_Right ? 1.0f : 0.0f, dy = key == IK_Up ? -1.0f : key == IK_Down ? 1.0f : 0.0f;
    Object* best = nullptr;
    float bestScore = 1e30f;
    for (Object* o : all) {
        if (o == g.watched) continue;
        Box b = boxOf(w, o);
        float ox = b.x + b.w / 2 - fx, oy = b.y + b.h / 2 - fy;
        float along = ox * dx + oy * dy;
        if (along <= 1) continue;
        float across = std::fabs(ox * dy - oy * dx);
        float score = along + 2 * across;
        if (score < bestScore) {
            bestScore = score;
            best = o;
        }
    }
    if (best) watch(w, best);
}

// --- drawing

void setColor(World& w, Object* canvas, const Value& c) {
    if (c.isStruct()) w.var(canvas, "DrawColor") = c;
}

void drawControl(World& w, Object* canvas, Object* c);

void drawImage(World& w, Object* canvas, Object* mat, int imgStyle, const Box& b) {
    if (!mat) return;
    w.vm.call(canvas, "SetPos", {Value::Float(b.x), Value::Float(b.y)});
    if (imgStyle == 0)
        w.vm.call(canvas, "DrawTileScaled", {Value::Obj(mat), Value::Float(1), Value::Float(1)});
    else
        w.vm.call(canvas, "DrawTileStretched", {Value::Obj(mat), Value::Float(b.w), Value::Float(b.h)});
}

// GUIStyles' Draw(Canvas, MenuState, Left, Top, Width, Height), as the
// engine runs it: the Canvas set to the state's colour and render style, then
// the style's OnDraw, else its image for the state, stretched. The controls
// call it as the engine's own, not a script function of the same name a style
// may declare.
void styleDraw(World& w, Object* canvas, Object* style, int state, const Box& b) {
    if (!style || !canvas) return;
    setColor(w, canvas, color(style, "ImgColors", state));
    w.var(canvas, "Style") = Value::Int(element(style, "RStyles", state).i());
    if (w.vm.call(style, "OnDraw", {Value::Obj(canvas), Value::Int(state), Value::Float(b.x), Value::Float(b.y),
                                    Value::Float(b.w), Value::Float(b.h)})
            .b())
        return;
    drawImage(w, canvas, element(style, "Images", state).o(), element(style, "ImgStyle", state).i(), b);
}

// DrawText(Canvas, MenuState, Left, Top, Width, Height, Align, Text): the
// Canvas set to the state's font, for the screen's width, colour and render
// style, then the style's OnDrawText, else the text across the box. Shrek's
// styles draw it themselves and leave the font as it is set.
void styleText(World& w, Object* canvas, Object* style, int state, const Box& b, int align, const String& text) {
    if (!style || !canvas) return;
    Gui& g = gui(w);
    if (Object* font = element(style, "Fonts", state).o())
        if (Object* f = w.vm.call(font, "GetFont", {Value::Obj(canvas), Value::Int(int(g.width))}).o())
            w.var(canvas, "Font") = Value::Obj(f);
    setColor(w, canvas, color(style, "FontColors", state));
    w.var(canvas, "Style") = Value::Int(element(style, "RStyles", state).i());
    if (w.vm.call(style, "OnDrawText", {Value::Obj(canvas), Value::Int(state), Value::Float(b.x), Value::Float(b.y),
                                        Value::Float(b.w), Value::Float(b.h), Value::Int(align), Value::Str(text)})
            .b())
        return;
    if (text.empty()) return;
    w.vm.call(canvas, "DrawTextJustified", {Value::Str(text), Value::Int(align), Value::Float(b.x), Value::Float(b.y),
                                            Value::Float(b.x + b.w), Value::Float(b.y + b.h)});
}

// A tab control's buttons, in a row along its top: TabHeight high, of the
// screen when 1 or less, each as wide as its caption in its style's font with
// half its height either side. Placed in pixels from the control's corner,
// the buttons being bBoundToParent. The panels keep their own places.
void placeTabs(World& w, Object* canvas, Object* c) {
    Gui& g = gui(w);
    float h = w.var(c, "TabHeight").f();
    if (h <= 1) h *= g.height;
    float x = 0;
    for (Object* t : childrenOf(w, c)) {
        if (!has(t, "MyPanel")) continue;
        float width = 2 * h;
        Object* style = w.obj(t, "Style");
        int state = w.var(t, "MenuState").i();
        if (style && w.textWidth)
            if (Object* font = element(style, "Fonts", state).o())
                if (Object* f = w.vm.call(font, "GetFont", {Value::Obj(canvas), Value::Int(int(g.width))}).o())
                    width = w.textWidth(f, w.var(t, "Caption").s()) + h;
        w.var(t, "WinLeft") = Value::Float(x);
        w.var(t, "WinTop") = Value::Float(0);
        w.var(t, "WinWidth") = Value::Float(width);
        w.var(t, "WinHeight") = Value::Float(h);
        x += width;
    }
}

// What a control draws of its own, by its kind: a button its style and its
// caption, a label its caption, an image its image, the rest their style.
void drawOwn(World& w, Object* canvas, Object* c, Object* style, int state, const Box& b) {
    Gui& g = gui(w);
    if (g.button && c->isA(g.button)) {
        styleDraw(w, canvas, style, state, b);
        styleText(w, canvas, style, state, b, 1, w.var(c, "Caption").s());
    } else if (g.label && c->isA(g.label)) {
        String text = w.var(c, "Caption").s();
        int align = w.var(c, "TextAlign").i();
        if (style) {
            styleText(w, canvas, style, state, b, align, text);
        } else if (!text.empty() && g.controller) {
            Object* font = w.vm.call(g.controller, "GetMenuFont", {w.var(c, "TextFont")}).o();
            Object* f = font ? w.vm.call(font, "GetFont", {Value::Obj(canvas), Value::Int(int(g.width))}).o() : nullptr;
            if (f) w.var(canvas, "Font") = Value::Obj(f);
            setColor(w, canvas, w.var(c, state == MSAT_Focused ? "FocusedTextColor" : "TextColor"));
            w.var(canvas, "Style") = Value::Int(w.var(c, "TextStyle").i());
            w.vm.call(canvas, "DrawTextJustified", {Value::Str(text), Value::Int(align), Value::Float(b.x),
                                                    Value::Float(b.y), Value::Float(b.x + b.w), Value::Float(b.y + b.h)});
        }
    } else if (g.image && c->isA(g.image)) {
        setColor(w, canvas, w.var(c, "ImageColor"));
        w.var(canvas, "Style") = Value::Int(w.var(c, "ImageRenderStyle").i());
        drawImage(w, canvas, w.obj(c, "Image"), w.var(c, "ImageStyle").i(), b);
    } else if (g.tabs && c->isA(g.tabs)) {
        // not its style: its BackgroundStyle or BackgroundImage, if it has one
        if (Object* bg = w.obj(c, "BackgroundStyle")) styleDraw(w, canvas, bg, state, b);
        if (Object* img = w.obj(c, "BackgroundImage")) drawImage(w, canvas, img, 1, b);
    } else {
        styleDraw(w, canvas, style, state, b);
    }
}

void drawControl(World& w, Object* canvas, Object* c) {
    Gui& g = gui(w);
    if (!w.flag(c, "bVisible")) return;
    Box b = boxOf(w, c);
    if (has(c, "Bounds")) {
        float bv[4] = {b.x, b.y, b.x + b.w, b.y + b.h};
        for (int k = 0; k < 4; ++k) element(c, "Bounds", k) = Value::Float(bv[k]);
    }
    // A page's Background first, then the control's OnDraw, which draws over
    // it and is true when the control drew itself (ShInGameMenuGUIPage's
    // draws the book on the wood and is not); then its own drawing, then the
    // controls on it.
    w.vm.call(c, "OnPreDraw", {Value::Obj(canvas)});
    Object* style = w.obj(c, "Style");
    int state = w.var(c, "MenuState").i();
    if (c->isA(g.page))
        if (Object* bg = w.obj(c, "Background")) {
            setColor(w, canvas, w.var(c, "BackgroundColor"));
            w.var(canvas, "Style") = Value::Int(w.var(c, "BackgroundRStyle").i());
            drawImage(w, canvas, bg, 1, b);
        }
    bool drew = w.vm.call(c, "OnDraw", {Value::Obj(canvas)}).b();
    if (!drew && !c->isA(g.page)) drawOwn(w, canvas, c, style, state, b);
    if (g.tabs && c->isA(g.tabs)) placeTabs(w, canvas, c);
    for (Object* k : childrenOf(w, c)) drawControl(w, canvas, k);
}

// Make a page's controls from its class's templates: each variable of the
// page that holds a GUIComponent the page's package made for it gets a copy
// of its own, with the template's delegates bound to the page, and the copies
// are the page's Controls, in the order the variables are declared.
void initializeControls(World& w, Object* self) {
    Gui& g = gui(w);
    std::vector<Object*> controls;
    if (has(self, "Controls") && w.var(self, "Controls").isArr())
        for (const Value& e : w.var(self, "Controls").arr())
            if (Object* o = e.o()) controls.push_back(o);
    std::map<Object*, Object*> copies;
    auto copyOf = [&](Object* t) -> Object* {
        if (!t || t == self || g.made.count(t) || !t->isA(g.component)) return t;
        // a template lives in its package, its outer a class or the package
        if (t->outer && !t->outer->isClass() && t->outer->outer) return t;
        if (t->isA(g.page)) return t;
        auto it = copies.find(t);
        if (it != copies.end()) return it->second;
        Object* c = w.vm.spawn(t->cls, Name(), self->outer);
        c->props = t->props;
        for (Value& v : c->props)
            if (v.isDlg()) {
                Delegate d = v.d();
                if (d.func.isNone()) continue;
                if (!d.obj || d.obj->isClass() || d.obj == t->outer || (d.obj->cls == self->cls && d.obj != self)) {
                    d.obj = self;
                    v = Value::Dlg(d);
                }
            }
        g.made.insert(c);
        copies[t] = c;
        return c;
    };
    for (Prop* p : self->cls->layout()) {
        if (p->kind != Kind::Object) continue;
        static const std::unordered_set<std::string> skip = {"MenuOwner", "Controller", "ParentPage", "FocusedControl",
                                                             "Style", "FriendlyLabel", "FocusInstead"};
        if (skip.count(p->name.str())) continue;
        for (int k = 0; k < p->dim; ++k) {
            Value& v = self->props[size_t(p->slot + k)];
            Object* t = v.o();
            Object* c = copyOf(t);
            if (c == t) continue;
            v = Value::Obj(c);
            if (std::find(controls.begin(), controls.end(), c) == controls.end()) controls.push_back(c);
        }
    }
    for (Object*& c : controls) c = copyOf(c);
    if (has(self, "Controls")) {
        Array a;
        for (Object* c : controls) a.push_back(Value::Obj(c));
        w.var(self, "Controls") = Value::Arr(a);
    }
}

}  // namespace

void setupGui(World& w, const std::string& controllerClass) {
    Gui& g = gui(w);
    if (!w.player || controllerClass.empty()) return;
    Class* cc = w.vm.findClass(controllerClass.substr(controllerClass.find('.') + 1));
    if (!cc) return;
    Object* gc = w.vm.spawn(cc, Name(), w.player->outer);
    g.controller = gc;
    if (has(w.player, "GUIController")) w.var(w.player, "GUIController") = Value::Obj(gc);
    if (has(gc, "ViewportOwner")) w.var(gc, "ViewportOwner") = Value::Obj(w.player);
    if (Class* im = w.vm.findClass("InteractionMaster"); im && has(w.player, "InteractionMaster")) {
        Object* m = w.vm.spawn(im, Name(), w.player->outer);
        w.var(w.player, "InteractionMaster") = Value::Obj(m);
        if (has(gc, "Master")) w.var(gc, "Master") = Value::Obj(m);
    }
    try {
        w.vm.call(gc, "InitializeController");
    } catch (const std::exception& ex) {
        w.failures[std::string("GUI: ") + ex.what()]++;
    }
}

bool guiActive(World& w) {
    if (!w.gui) return false;
    Gui& g = gui(w);
    return g.controller && w.flag(g.controller, "bActive") && activePage(w);
}

void guiMouse(World& w, float x, float y) {
    if (!guiActive(w)) return;
    Gui& g = gui(w);
    g.mouseX = x;
    g.mouseY = y;
    w.var(g.controller, "MouseX") = Value::Float(x);
    w.var(g.controller, "MouseY") = Value::Float(y);
    try {
        watch(w, hitTest(w, activePage(w), x, y));
    } catch (const std::exception& ex) {
        w.failures[std::string("GUI: ") + ex.what()]++;
    }
}

bool guiKey(World& w, int key, int action) {
    if (!guiActive(w)) return false;
    Gui& g = gui(w);
    try {
        Object* page = activePage(w);
        // the page's OnKeyEvent first, then the focused control's
        for (Object* o : {page, w.obj(g.controller, "FocusedControl")}) {
            if (!o) continue;
            std::vector<Value> args = {Value::Int(key), Value::Int(action), Value::Float(0)};
            if (w.vm.eventOut(o, "OnKeyEvent", args).b()) return true;
        }
        if (key == IK_LeftMouse) {
            if (action == IST_Press && g.watched) {
                g.pressed = g.watched;
                stateChange(w, g.pressed, MSAT_Pressed);
            } else if (action == IST_Release && g.pressed) {
                Object* p = g.pressed;
                g.pressed = nullptr;
                bool over = p == hitTest(w, page, g.mouseX, g.mouseY);
                if (!p->deleted && w.var(p, "MenuState").i() == MSAT_Pressed)
                    stateChange(w, p, over ? MSAT_Watched : MSAT_Blurry);
                if (over) w.vm.call(p, "OnClick", {Value::Obj(p)});
            }
            return true;
        }
        if (action != IST_Press) return true;
        if (key == IK_Escape) {
            w.vm.call(g.controller, "CloseMenu", {Value::Bool(true)});
        } else if (key == IK_Enter) {
            if (Object* p = g.watched; p && !p->deleted) w.vm.call(p, "OnClick", {Value::Obj(p)});
        } else if (key >= IK_Left && key <= IK_Down) {
            step(w, key);
        }
    } catch (const std::exception& ex) {
        w.failures[std::string("GUI: ") + ex.what()]++;
    }
    return true;
}

void guiTick(World& w, float dt) {
    if (!w.gui) return;
    Gui& g = gui(w);
    std::vector<Object*> due;
    for (Object* c : g.timers) {
        if (c->deleted) continue;
        float left = w.var(c, "TimerCountdown").f() - dt;
        w.var(c, "TimerCountdown") = Value::Float(left);
        if (left <= 0) due.push_back(c);
    }
    for (Object* c : due) {
        if (w.flag(c, "bTimerRepeat"))
            w.var(c, "TimerCountdown") = Value::Float(w.var(c, "TimerCountdown").f() + w.var(c, "TimerInterval").f());
        else
            g.timers.erase(std::remove(g.timers.begin(), g.timers.end(), c), g.timers.end());
        try {
            w.vm.event(c, "Timer");
        } catch (const std::exception& ex) {
            w.failures[std::string("GUI timer: ") + ex.what()]++;
        }
    }
    g.timers.erase(std::remove_if(g.timers.begin(), g.timers.end(), [](Object* c) { return c->deleted; }), g.timers.end());
}

void drawGui(World& w, Object* canvas, int width, int height) {
    if (!w.gui) return;
    Gui& g = gui(w);
    g.width = float(width);
    g.height = float(height);
    if (!guiActive(w)) return;
    const Value& stack = w.var(g.controller, "MenuStack");
    if (!stack.isArr()) return;
    std::vector<Object*> pages;
    for (const Value& e : stack.arr())
        if (Object* p = e.o(); p && !p->deleted) pages.push_back(p);
    for (Object* p : pages) {
        try {
            w.vm.call(canvas, "Reset");
            drawControl(w, canvas, p);
        } catch (const std::exception& ex) {
            w.failures[std::string("GUI: ") + ex.what()]++;
        }
    }
    // the mouse cursor, unless hidden
    if (w.flag(g.controller, "bHideMouseCursor")) return;
    int index = g.watched && !g.watched->deleted ? w.var(g.watched, "MouseCursorIndex").i() : 0;
    const Value& cursors = w.var(g.controller, "MouseCursors");
    if (!cursors.isArr() || cursors.arr().empty()) return;
    if (index < 0 || size_t(index) >= cursors.arr().size()) index = 0;
    Object* cursor = cursors.arr()[size_t(index)].o();
    if (!cursor) return;
    w.vm.call(canvas, "Reset");
    w.var(canvas, "Style") = Value::Int(5);        // STY_Alpha
    w.vm.call(canvas, "SetPos", {Value::Float(g.mouseX), Value::Float(g.mouseY)});
    w.vm.call(canvas, "DrawTileScaled", {Value::Obj(cursor), Value::Float(1), Value::Float(1)});
}

std::string guiResolution(World& w) {
    Gui& g = gui(w);
    return std::to_string(int(g.width)) + "x" + std::to_string(int(g.height));
}

void registerGuiNatives(VM& vm) {
    auto& n = vm.natives;
    n["guicontroller.getstyle"] = [](NativeCall& c) {
        World* w = worldOf(c);
        std::string want = utf8(c.s(0));
        if (!w || want.empty()) return Value::Obj(nullptr);
        const Value& v = w->var(c.self, "StyleStack");
        if (v.isArr())
            for (const Value& e : v.arr())
                if (Object* s = e.o(); s && strcasecmpEq(utf8(w->var(s, "KeyName").s()), want)) return Value::Obj(s);
        return Value::Obj(nullptr);
    };
    n["guicontroller.getmenufont"] = [](NativeCall& c) {
        World* w = worldOf(c);
        std::string want = utf8(c.s(0));
        if (!w || want.empty()) return Value::Obj(nullptr);
        const Value& v = w->var(c.self, "FontStack");
        if (v.isArr())
            for (const Value& e : v.arr())
                if (Object* f = e.o(); f && strcasecmpEq(utf8(w->var(f, "KeyName").s()), want)) return Value::Obj(f);
        return Value::Obj(nullptr);
    };
    n["guicontroller.mouseemulation"] = [](NativeCall&) { return Value(); };
    n["guicontroller.resetkeyboard"] = [](NativeCall&) { return Value(); };
    n["guicontroller.getmaplist"] = [](NativeCall&) { return Value(); };
    n["guicontroller.getcurrentres"] = [](NativeCall& c) { return Value::Str(widen(guiResolution(*worldOf(c)))); };
    // ActualLeft, ActualTop, ActualWidth, ActualHeight
    n["guicomponent.actualleft"] = [](NativeCall& c) { return Value::Float(actual(*worldOf(c), c.self, 0, false)); };
    n["guicomponent.actualtop"] = [](NativeCall& c) { return Value::Float(actual(*worldOf(c), c.self, 1, false)); };
    n["guicomponent.actualwidth"] = [](NativeCall& c) { return Value::Float(actual(*worldOf(c), c.self, 0, true)); };
    n["guicomponent.actualheight"] = [](NativeCall& c) { return Value::Float(actual(*worldOf(c), c.self, 1, true)); };
    // SetTimer(Interval, optional bRepeat): Timer() when it runs out
    n["guicomponent.settimer"] = [](NativeCall& c) {
        World& w = *worldOf(c);
        Gui& g = gui(w);
        w.var(c.self, "TimerInterval") = Value::Float(c.f(0));
        w.var(c.self, "TimerCountdown") = Value::Float(c.f(0));
        w.var(c.self, "bTimerRepeat") = Value::Bool(c.b(1));
        g.timers.erase(std::remove(g.timers.begin(), g.timers.end(), c.self), g.timers.end());
        if (c.f(0) > 0) g.timers.push_back(c.self);
        return Value();
    };
    n["guicomponent.killtimer"] = [](NativeCall& c) {
        Gui& g = gui(*worldOf(c));
        g.timers.erase(std::remove(g.timers.begin(), g.timers.end(), c.self), g.timers.end());
        return Value();
    };
    n["guimulticomponent.initializecontrols"] = [](NativeCall& c) {
        initializeControls(*worldOf(c), c.self);
        return Value();
    };
    // GetFont(Canvas, XRes): the font for the screen's width, the one of the
    // largest resolution not over it, or the first
    n["guifont.getfont"] = [](NativeCall& c) {
        World& w = *worldOf(c);
        int x = c.i(1), pick = 0;
        if (!w.flag(c.self, "bFixedSize")) {
            const Value& r = w.var(c.self, "FontArrayResolutions");
            if (r.isArr())
                for (size_t i = 0; i < r.arr().size(); ++i)
                    if (r.arr()[i].i() <= x) pick = int(i);
        }
        return w.vm.call(c.self, "LoadFont", {Value::Int(pick)});
    };
    n["guistyles.draw"] = [](NativeCall& c) {
        styleDraw(*worldOf(c), c.o(0), c.self, c.i(1), Box{c.f(2), c.f(3), c.f(4), c.f(5)});
        return Value();
    };
    n["guistyles.drawtext"] = [](NativeCall& c) {
        styleText(*worldOf(c), c.o(0), c.self, c.i(1), Box{c.f(2), c.f(3), c.f(4), c.f(5)}, c.i(6), c.s(7));
        return Value();
    };
    n["interaction.initialize"] = [](NativeCall& c) {
        if (World* w = worldOf(c)) w->vm.event(c.self, "Initialized");
        return Value();
    };
}

}  // namespace ffa
