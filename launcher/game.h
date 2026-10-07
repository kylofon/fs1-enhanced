// game.h -- the disk image, where fs1-enhanced is, the start locations and starting the game.
#pragma once

#include <wx/string.h>

// The folder the launcher runs from.
wxString LauncherDir();

// Where things are by default: fs1-enhanced(.exe) beside the launcher, and the first 160K disk image in
// the launcher's folder or its original/ folder (empty if there is none: the game then looks itself).
wxString DefaultProgram();
wxString DefaultDisk();

// Whether `path` is a 160K PC disk image of the right size (what the game accepts).
bool IsDiskImage(const wxString& path);

// The start locations: the game's user modes (the editor's "User mode" field, which holds a preset of
// position, altitude, time and weather). fs1-enhanced --start N recalls mode N once the menus are over;
// mode 0 is the game's own start and passes nothing.
struct StartLocation {
    int mode;
    const char* label;
    const char* tip;
};
extern const StartLocation START_LOCATIONS[];
extern const int START_LOCATION_COUNT;

struct GameOptions {
    wxString program;   // fs1-enhanced(.exe)
    wxString disk;      // the disk image (empty: the game looks beside itself and in original/)
    int startMode = 0;  // --start (0 = none)
    int lineWidth = 2;  // --line-width: output pixels of a sharp line (0 = hairlines)
    int smooth = 0;     // --smooth: 0 on (one simulation frame behind), 1 ahead (no delay), 2 off
    bool rgb = false;   // --rgb: start on the RGB monitor (F11 cycles sharp / composite / RGB)
    bool sound = true;  // false: SDL_AUDIO_DRIVER=dummy
};

// Starts the game. On failure returns false and says why in `error`.
bool LaunchGame(const GameOptions& options, wxString& error);
