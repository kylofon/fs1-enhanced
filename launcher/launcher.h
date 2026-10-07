// launcher.h -- the launcher window: choose the disk image, where to start and the display and
// sound options, then Play.
#pragma once

#include <wx/frame.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSpinCtrl;
class wxStaticBitmap;
class wxStaticText;
class wxTextCtrl;

extern const char* const APP_TITLE;

class LauncherFrame : public wxFrame {
public:
    LauncherFrame();

private:
    void CreateContent();
    void CreateMenus();
#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

    // Checks the disk image and the program and enables Play to match.
    void UpdateState();
    void BrowseDisk();
    void BrowseProgram();
    void Play();
    void About();
    void Preferences();
    void SetTopmost(bool on);
    void Save();

    wxTextCtrl* disk_ = nullptr;
    wxTextCtrl* program_ = nullptr;
    wxStaticBitmap* statusIcon_ = nullptr;
    wxStaticText* statusNote_ = nullptr;
    wxChoice* start_ = nullptr;
    wxSpinCtrl* lineWidth_ = nullptr;
    wxChoice* smooth_ = nullptr;
    wxCheckBox* rgb_ = nullptr;
    wxCheckBox* sound_ = nullptr;
    wxButton* play_ = nullptr;
    bool topmost_ = false;
    bool loading_ = true;  // no edits are recorded while the window is built
};
