// app.cpp -- the FS1 Enhanced launcher: the disk image, where to start and the display options,
// then fs1-enhanced.
#include <wx/app.h>

#include "launcher.h"

class LauncherApp : public wxApp {
public:
    bool OnInit() override {
        SetAppName("FS1 Enhanced");
        SetVendorName("Krzysztof Kania");
        if (!wxApp::OnInit()) return false;
        auto* frame = new LauncherFrame;
        SetTopWindow(frame);
        frame->Show();
        return true;
    }
};

wxIMPLEMENT_APP(LauncherApp);
