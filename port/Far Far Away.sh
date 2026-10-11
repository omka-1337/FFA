#!/bin/bash
# PORTMASTER: farfaraway.zip, Far Far Away.sh

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/farfaraway

cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# The card is exFAT, which carries no exec bit.
chmod +x "$GAMEDIR/ffa-play"

# The game data is the player's own Shrek 2 PC, installed: the folder with
# System, Maps, Textures and the rest in it.
if [ ! -f "$GAMEDIR/gamedata/System/Default.ini" ]; then
  echo "No game data: copy your installed Shrek 2 into $GAMEDIR/gamedata"
  echo "(the folder holding System, Maps, Textures, Sounds, Music and Movies)."
  pm_message "Copy your installed Shrek 2 into ports/farfaraway/gamedata" 2>/dev/null
  sleep 5
  pm_finish
  exit 1
fi

# The engine reads the pad itself through SDL; gptokeyb is here for the
# hotkey that ends it.
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
$GPTOKEYB "ffa-play" &

./ffa-play "$GAMEDIR/gamedata" --fullscreen

pm_finish
