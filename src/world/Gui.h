// The GUI: the engine's side of GUI.u, the menus' pages and their controls.
//
// The engine makes the player's GUIController, of the class Default.ini names
// ([Engine.Engine] GUIController=ShGame.ShGUIController), and its script
// registers the styles and fonts. Pages open through OpenMenu, which script
// calls (the menu level's cutscene does GotoMenu SHGame.ShFEGUIPage). The
// rest is the engine's: a page's controls made from its class's templates
// (InitializeControls), where a control is on the screen (ActualLeft and its
// kin), the controls drawn each frame through their styles, the mouse and the
// keys given to them, and their timers.
#pragma once

#include <string>

#include "world/World.h"

namespace ffa {

// The player's GUIController, made and initialised.
void setupGui(World& w, const std::string& controllerClass);

// Whether a menu is open, and takes the input.
bool guiActive(World& w);

// A key, by the engine's numbers (EInputKey: 1 the left mouse button, 13
// Enter, 27 Escape, 37 to 40 the arrows), pressed (1) or let go (3); true
// when the GUI took it.
bool guiKey(World& w, int key, int action);
// The mouse at a place on the screen, in pixels.
void guiMouse(World& w, float x, float y);

// A frame of the GUI's timers.
void guiTick(World& w, float dt);

// The open pages and the mouse cursor, drawn on the Canvas the HUD drew on.
void drawGui(World& w, Object* canvas, int width, int height);

// The screen as the console tells it, GETCURRENTRES: 1280x720.
std::string guiResolution(World& w);

void registerGuiNatives(VM& vm);

}  // namespace ffa
