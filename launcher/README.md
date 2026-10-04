# FS1 Enhanced (the launcher)

`FS1 Enhanced.exe` starts `fs1-enhanced` with the options chosen in its window. It is built with
[wxWidgets](https://www.wxwidgets.org/) 3.2 from the platform's own controls and follows the launcher of
Aces of the Pacific Enhanced.

## The window

* **Game files**
  * **Disk image**: the 160K PC disk image of Microsoft Flight Simulator 1.05 (`.ima` / `.img`, 163840 bytes).
    Default: the first one in the launcher's folder or its `original` folder. Left empty, the game looks
    beside itself and in `original/` as it always does.
  * **Program**: `fs1-enhanced.exe` (default: beside the launcher). **Play** stays greyed out until the program
    is there and the disk image, if one is named, is a disk image.
* **Flight**
  * **Start location** (`--start N`): one of the game's ten *user modes*, presets of position, altitude, time
    and weather held on the disk and normally recalled in the editor (Esc, the User mode field). The game
    recalls the chosen one as soon as the start-up menus (A, B) are over. The first entry, Meigs Field, is the
    game's own start and passes nothing. The labels come from the positions the presets hold (Chicago, Los
    Angeles, Seattle, New York / Boston, the 1917 war scenery); the tooltip names the user mode number.
* **Display and sound**
  * **Line width** (`--line-width`, default 2): thickness of the sharp 3D view's lines in window pixels, 0 =
    hairlines.
  * **Start on the RGB monitor** (`--rgb`); F11 in the game cycles sharp, composite and RGB.
  * **Sound**: off starts the game with `SDL_AUDIO_DRIVER=dummy`.

Settings are kept in `%APPDATA%\FS1 Enhanced\settings.ini` (`~/.config/fs1-enhanced` on Linux). A disk image or
program left at its default is stored empty, so it follows the launcher if the folder moves.

## Building

Needs CMake 3.24, a C++17 compiler and wxWidgets 3.2 (MSYS2 `mingw64`: `mingw-w64-x86_64-wxwidgets3.2-msw`;
Debian and Ubuntu: `libwxgtk3.2-dev`). The main project builds it beside `fs1-enhanced.exe` by default
(`-DFS1_LAUNCHER=OFF` leaves it out). On Windows the build copies the DLLs the launcher needs beside it
(`copy_dlls.cmake`); keep them with the `.exe` in a release.

## Files

`app.cpp` the application, `launcher.cpp/.h` the window and the About box, `game.cpp/.h` the disk check, the start
locations table and starting the game, `settings.cpp/.h` `settings.ini`, `icon.cpp/.h` the icon drawn in code
(`make_icon.py` writes the same drawing to `app.ico`), `app.rc`, `app.manifest`, `version.h`, `copy_dlls.cmake`.

More options (smooth frame rate, gamepad, key bindings) are added as the game gets them (PLAN.md S3.2, S4, S5.1).
