// game.cpp -- the disk image, the start locations and starting the game.
#include "game.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <string>
#include <vector>

// The user modes of the original disk, labelled from the position each one holds (Meigs Field is North 786,
// East 287 in the editor's units; the scenery area follows from the position). The mode number is in the
// tooltip so the labels can be checked in the editor.
const StartLocation START_LOCATIONS[] = {
    {0, "Chicago: Meigs Field, on the runway (the game's own start)", "User mode 0: the game's own start."},
    {4, "Chicago: Meigs Field, clouds and wind", "User mode 4: Meigs Field with a cloud layer and winds."},
    {2, "Chicago: Meigs Field, demonstration flight",
     "User mode 2: Meigs Field in the game's demo (the plane flies itself)."},
    {9, "Chicago scenery: south of Meigs, 3000 ft",
     "User mode 9: in the air at 3000 ft, banked, south of Meigs in the Chicago scenery."},
    {3, "Los Angeles", "User mode 3: the Los Angeles scenery."},
    {8, "Los Angeles: in the air, 4000 ft", "User mode 8: Los Angeles, 4000 ft, 130 knots."},
    {1, "Seattle", "User mode 1: the Seattle scenery."},
    {5, "New York / Boston", "User mode 5: the New York / Boston scenery."},
    {6, "New York / Boston: second place", "User mode 6: the New York / Boston scenery, another place."},
    {7, "Europe 1917: the war scenery", "User mode 7: the Western Front (war mode)."},
};
const int START_LOCATION_COUNT = static_cast<int>(sizeof START_LOCATIONS / sizeof START_LOCATIONS[0]);

namespace {

const wxFileOffset DISK_SIZE = 163840;

bool FirstDiskIn(const wxString& dir, wxString& found) {
    if (!wxDir::Exists(dir)) return false;
    wxDir d(dir);
    wxString name;
    for (bool more = d.GetFirst(&name, wxEmptyString, wxDIR_FILES); more; more = d.GetNext(&name)) {
        const wxString ext = wxFileName(name).GetExt().Lower();
        if (ext != "ima" && ext != "img") continue;
        const wxString path = wxFileName(dir, name).GetFullPath();
        if (IsDiskImage(path)) {
            found = path;
            return true;
        }
    }
    return false;
}

}  // namespace

bool IsDiskImage(const wxString& path) {
    if (path.empty() || !wxFileName::FileExists(path)) return false;
    return wxFileName(path).GetSize() == DISK_SIZE;
}

wxString LauncherDir() { return wxFileName(wxStandardPaths::Get().GetExecutablePath()).GetPath(); }

wxString DefaultProgram() {
    wxFileName name(LauncherDir(), "fs1-enhanced");
#ifdef __WXMSW__
    name.SetExt("exe");
#endif
    return name.GetFullPath();
}

wxString DefaultDisk() {
    wxString found;
    if (FirstDiskIn(LauncherDir(), found) || FirstDiskIn(wxFileName(LauncherDir(), "original").GetFullPath(), found))
        return found;
    return wxString();
}

bool LaunchGame(const GameOptions& o, wxString& error) {
    if (!wxFileName::FileExists(o.program)) {
        error = wxString::Format("The game's program isn't there:\n\n%s", o.program);
        return false;
    }
    std::vector<wxString> args{o.program, "--line-width", wxString::Format("%d", o.lineWidth)};
    if (o.rgb) args.push_back("--rgb");
    if (o.startMode > 0) {
        args.push_back("--start");
        args.push_back(wxString::Format("%d", o.startMode));
    }
    if (!o.disk.empty()) args.push_back(o.disk);

    std::vector<std::wstring> wide;
    for (const wxString& a : args) wide.push_back(a.ToStdWstring());
    std::vector<const wchar_t*> argv;
    for (const std::wstring& w : wide) argv.push_back(w.c_str());
    argv.push_back(nullptr);

    wxExecuteEnv env;
    env.cwd = wxFileName(o.program).GetPath();
    if (!o.sound) {
        // Sound off: SDL opens its silent audio driver.
        wxGetEnvMap(&env.env);
        env.env["SDL_AUDIO_DRIVER"] = "dummy";
    }
    long pid;
    {
        wxLogNull quiet;  // wxExecute would show its own error box
        pid = wxExecute(argv.data(), wxEXEC_ASYNC, nullptr, &env);
    }
    if (pid == 0) {
        error = wxString::Format("Couldn't start %s.\n\n%s", wxFileName(o.program).GetFullName(),
                                 wxSysErrorMsgStr(wxSysErrorCode()));
        return false;
    }
    return true;
}
