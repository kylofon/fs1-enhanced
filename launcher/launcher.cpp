// launcher.cpp -- the launcher window.
#include "launcher.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbmp.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#ifdef __WXMSW__
#include <wx/msw/wrapcctl.h>
#include <shellapi.h>
#endif

#include "game.h"
#include "icon.h"
#include "settings.h"
#include "version.h"

const char* const APP_TITLE = "FS1 Enhanced";

namespace {

const char* const WEBSITE = "https://kkania.com";
const char* const SUPPORT = "https://buymeacoffee.com/krzysztofkania";
const char* const SECTION = "Game";

const int ID_ABOUT = wxID_HIGHEST + 1;
const int MAX_LINE_WIDTH = 8, DEFAULT_LINE_WIDTH = 2;

#ifdef __WXMSW__
HRESULT CALLBACK AboutCallback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(hwnd, L"open", reinterpret_cast<LPCWSTR>(lp), nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}
#else
// A label and a link on one line, for the portable About box.
void AddLink(wxWindow* parent, wxSizer* sizer, const wxString& label, const wxString& text, const wxString& url) {
    auto* line = new wxBoxSizer(wxHORIZONTAL);
    line->Add(new wxStaticText(parent, wxID_ANY, label + " "), 0, wxALIGN_CENTER_VERTICAL);
    line->Add(new wxHyperlinkCtrl(parent, wxID_ANY, text, url), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(line);
}
#endif

wxStaticText* GreyText(wxWindow* parent, const wxString& text = wxEmptyString) {
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    return label;
}

// A label, a text field and a Browse button in a row of a two-column grid.
wxTextCtrl* PathRow(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, const wxString& tip,
                    wxButton** browse) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* text = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(parent->FromDIP(300), -1));
    text->SetToolTip(tip);
    *browse = new wxButton(parent, wxID_ANY, "B&rowse...");
    row->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, parent->FromDIP(8));
    row->Add(*browse, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(row, 1, wxEXPAND);
    return text;
}

}  // namespace

LauncherFrame::LauncherFrame()
    : wxFrame(nullptr, wxID_ANY, APP_TITLE, wxDefaultPosition, wxDefaultSize,
              wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER | wxMAXIMIZE_BOX)) {
    SetIcons(AppIcons());
    topmost_ = settings::GetInt("Preferences", "AlwaysOnTop", 0) != 0;
    if (topmost_) SetWindowStyleFlag(GetWindowStyleFlag() | wxSTAY_ON_TOP);

    CreateContent();
    CreateMenus();

    // Settings from the last run.
    wxString disk = settings::GetString(SECTION, "Disk", "");
    wxString program = settings::GetString(SECTION, "Program", "");
    disk_->ChangeValue(disk.empty() ? DefaultDisk() : disk);
    program_->ChangeValue(program.empty() ? DefaultProgram() : program);
    const int mode = settings::GetInt(SECTION, "StartMode", 0);
    start_->SetSelection(0);
    for (int i = 0; i < START_LOCATION_COUNT; ++i)
        if (START_LOCATIONS[i].mode == mode) start_->SetSelection(i);
    lineWidth_->SetValue(wxMax(0, wxMin(MAX_LINE_WIDTH, settings::GetInt(SECTION, "LineWidth", DEFAULT_LINE_WIDTH))));
    smooth_->SetSelection(wxMax(0, wxMin(2, settings::GetInt(SECTION, "Smooth", 0))));
    rgb_->SetValue(settings::GetInt(SECTION, "Rgb", 0) != 0);
    sound_->SetValue(settings::GetInt(SECTION, "Sound", 1) != 0);
    loading_ = false;
    UpdateState();

    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
        if (event.GetActive()) UpdateState();  // files may have been copied in meanwhile
        event.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
        Save();
        Destroy();
    });

    SetClientSize(GetSizer()->GetMinSize());
    if (!settings::RestoreWindowPosition(SECTION, this)) Centre();
}

void LauncherFrame::CreateContent() {
    auto* page = new wxPanel(this);
    const int margin = FromDIP(12), gap = FromDIP(8), small = FromDIP(4);

    // Game files
    auto* filesBox = new wxStaticBoxSizer(wxVERTICAL, page, "Game files");
    wxWindow* fb = filesBox->GetStaticBox();
    auto* filesGrid = new wxFlexGridSizer(2, gap, gap);
    filesGrid->AddGrowableCol(1);
    wxButton* browseDisk = nullptr;
    wxButton* browseProgram = nullptr;
    disk_ = PathRow(fb, filesGrid, "&Disk image:",
                    "The 160K PC disk image of Microsoft Flight Simulator 1.05 (.ima or .img, 163840 bytes).",
                    &browseDisk);
    program_ = PathRow(fb, filesGrid, "&Program:", "fs1-enhanced, the game.", &browseProgram);
    auto* statusRow = new wxBoxSizer(wxHORIZONTAL);
    statusIcon_ = new wxStaticBitmap(fb, wxID_ANY, wxArtProvider::GetBitmapBundle(wxART_WARNING, wxART_MENU));
    statusNote_ = new wxStaticText(fb, wxID_ANY, wxEmptyString);
    statusRow->Add(statusIcon_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, small);
    statusRow->Add(statusNote_, 1, wxALIGN_CENTER_VERTICAL);
    filesBox->Add(filesGrid, 0, wxEXPAND | wxALL, gap);
    filesBox->Add(statusRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
    disk_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!loading_) UpdateState();
    });
    program_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        if (!loading_) UpdateState();
    });
    browseDisk->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseDisk(); });
    browseProgram->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { BrowseProgram(); });

    // Flight
    auto* flightBox = new wxStaticBoxSizer(wxVERTICAL, page, "Flight");
    wxWindow* flb = flightBox->GetStaticBox();
    auto* flightGrid = new wxFlexGridSizer(2, gap, gap);
    flightGrid->AddGrowableCol(1);
    flightGrid->Add(new wxStaticText(flb, wxID_ANY, "&Start location:"), 0, wxALIGN_CENTER_VERTICAL);
    start_ = new wxChoice(flb, wxID_ANY);
    for (int i = 0; i < START_LOCATION_COUNT; ++i) start_->Append(wxString::FromUTF8(START_LOCATIONS[i].label));
    start_->SetToolTip("Where the flight starts: one of the game's user modes (position, altitude, time and "
                       "weather). The game recalls it as soon as the start-up menus are over; you can still pick "
                       "another in the editor (Esc).");
    flightGrid->Add(start_, 1, wxEXPAND);
    flightBox->Add(flightGrid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, gap);
    flightBox->Add(GreyText(flb, "Pick A and B in the game's own menus as usual; the location is set after them."), 0,
                   wxALL, gap);
    start_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int i = start_->GetSelection();
        if (i >= 0) start_->SetToolTip(START_LOCATIONS[i].tip);
    });

    // Display and sound
    auto* optionsBox = new wxStaticBoxSizer(wxVERTICAL, page, "Display and sound");
    wxWindow* ob = optionsBox->GetStaticBox();
    auto* grid = new wxFlexGridSizer(2, gap, gap);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Line width:"), 0, wxALIGN_CENTER_VERTICAL);
    auto* widthRow = new wxBoxSizer(wxHORIZONTAL);
    lineWidth_ = new wxSpinCtrl(ob, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1),
                                wxSP_ARROW_KEYS, 0, MAX_LINE_WIDTH, DEFAULT_LINE_WIDTH);
    lineWidth_->SetToolTip("The thickness of the lines in the sharp 3D view, in pixels of the window. 0 draws "
                           "one-pixel hairlines.");
    widthRow->Add(lineWidth_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);
    widthRow->Add(GreyText(ob, "pixels of the window (0 = hairlines)"), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(widthRow, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(new wxStaticText(ob, wxID_ANY, "&Motion:"), 0, wxALIGN_CENTER_VERTICAL);
    const wxString motions[] = {"Smooth", "Smooth, without delay", "As the original"};
    smooth_ = new wxChoice(ob, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, motions);
    smooth_->SetToolTip("The game draws its 3D view about 5 times a second. Smooth moves the view on at every "
                        "frame of the screen, from the last two pictures the game drew (shown one game picture "
                        "late); without delay continues the motion past the last picture instead (no delay, may "
                        "overshoot when the motion changes). The flight itself is the same in every setting.");
    grid->Add(smooth_, 0, wxALIGN_CENTER_VERTICAL);
    optionsBox->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    rgb_ = new wxCheckBox(ob, wxID_ANY, "Start on the &RGB monitor (instead of the sharp view)");
    rgb_->SetToolTip("F11 in the game cycles the sharp view, the composite monitor and the RGB monitor.");
    optionsBox->Add(rgb_, 0, wxLEFT | wxRIGHT | wxTOP, gap);
    sound_ = new wxCheckBox(ob, wxID_ANY, "S&ound");
    sound_->SetToolTip("Off runs the game silently.");
    optionsBox->Add(sound_, 0, wxALL, gap);

    // Buttons
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    play_ = new wxButton(page, wxID_ANY, "&Play");
    auto* close = new wxButton(page, wxID_CLOSE, "Close");
    buttons->AddStretchSpacer();
    buttons->Add(play_, 0, wxRIGHT, gap);
    buttons->Add(close);
    play_->SetDefault();
    play_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Play(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(filesBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(flightBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(optionsBox, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);
    all->Add(buttons, 0, wxEXPAND | wxALL, margin);
    page->SetSizer(all);

    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(page, 1, wxEXPAND);
    SetSizer(frameSizer);
}

void LauncherFrame::CreateMenus() {
    auto* file = new wxMenu;
    file->Append(wxID_PREFERENCES, wxString::FromUTF8("&Preferences…"));
    file->AppendSeparator();
    file->Append(wxID_EXIT, "E&xit\tAlt+F4");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
#ifndef __WXMSW__
    auto* about = new wxMenu;
    about->Append(ID_ABOUT, wxString::FromUTF8("&About ") + APP_TITLE + wxString::FromUTF8("…"));
    bar->Append(about, "A&bout");
#endif
    SetMenuBar(bar);
#ifdef __WXMSW__
    // A top-level About item that acts straight away, which wxWidgets menus
    // can't express; MSWWindowProc handles its command.
    const HMENU native = static_cast<HMENU>(bar->GetHMenu());
    AppendMenuW(native, MF_STRING, ID_ABOUT, L"A&bout");
    DrawMenuBar(static_cast<HWND>(GetHWND()));
#endif

    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Preferences(); }, wxID_PREFERENCES);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { About(); }, ID_ABOUT);
}

#ifdef __WXMSW__
WXLRESULT LauncherFrame::MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) {
    if (msg == WM_COMMAND && LOWORD(wParam) == ID_ABOUT && lParam == 0) {
        CallAfter([this] { About(); });
        return 0;
    }
    return wxFrame::MSWWindowProc(msg, wParam, lParam);
}
#endif

void LauncherFrame::UpdateState() {
    const wxString disk = disk_->GetValue(), program = program_->GetValue();
    const bool haveProgram = wxFileName::FileExists(program);
    // An empty disk field leaves it to the game, which looks beside itself and in original/.
    const bool haveDisk = disk.empty() ? true : IsDiskImage(disk);
    wxString note;
    if (!haveDisk)
        note = wxFileName::FileExists(disk) ? "This isn't a 160K PC disk image (163840 bytes)."
                                            : "The disk image isn't there.";
    else if (!haveProgram)
        note = wxString::Format("%s isn't there.", wxFileName(program).GetFullName());
    else if (disk.empty())
        note = "No disk image chosen: the game will look for one beside itself.";
    else
        note = "Found the game's files.";
    const bool ok = haveProgram && haveDisk;
    statusIcon_->Show(!ok || disk.empty());
    statusNote_->SetLabel(note);
    play_->Enable(ok);
    play_->GetParent()->Layout();
}

void LauncherFrame::BrowseDisk() {
    wxFileName current(disk_->GetValue());
    wxFileDialog dialog(this, "Choose the Flight Simulator 1 disk image", current.GetPath(), current.GetFullName(),
                        "Disk images (*.ima;*.img)|*.ima;*.img|All files|*", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) disk_->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

void LauncherFrame::BrowseProgram() {
    wxFileName current(program_->GetValue());
#ifdef __WXMSW__
    const char* const filter = "Programs (*.exe)|*.exe|All files (*.*)|*.*";
#else
    const char* const filter = "All files|*";
#endif
    wxFileDialog dialog(this, "Choose fs1-enhanced", current.GetPath(), current.GetFullName(), filter,
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK) program_->SetValue(dialog.GetPath());  // raises wxEVT_TEXT
}

void LauncherFrame::Play() {
    Save();
    GameOptions options;
    options.program = wxFileName(program_->GetValue()).GetFullPath();
    options.disk = disk_->GetValue();
    const int i = start_->GetSelection();
    options.startMode = i >= 0 ? START_LOCATIONS[i].mode : 0;
    options.lineWidth = lineWidth_->GetValue();
    options.smooth = smooth_->GetSelection();
    options.rgb = rgb_->GetValue();
    options.sound = sound_->GetValue();
    wxString error;
    if (!LaunchGame(options, error)) wxMessageBox(error, APP_TITLE, wxOK | wxICON_ERROR, this);
}

void LauncherFrame::Save() {
    // A disk or program left at its default is stored empty, so it follows the launcher if it moves.
    const wxString disk = disk_->GetValue(), program = program_->GetValue();
    settings::SetString(SECTION, "Disk", disk == DefaultDisk() ? wxString() : disk);
    settings::SetString(SECTION, "Program",
                        wxFileName(program).SameAs(wxFileName(DefaultProgram())) ? wxString() : program);
    const int i = start_->GetSelection();
    settings::SetInt(SECTION, "StartMode", i >= 0 ? START_LOCATIONS[i].mode : 0);
    settings::SetInt(SECTION, "LineWidth", lineWidth_->GetValue());
    settings::SetInt(SECTION, "Smooth", smooth_->GetSelection());
    settings::SetInt(SECTION, "Rgb", rgb_->GetValue() ? 1 : 0);
    settings::SetInt(SECTION, "Sound", sound_->GetValue() ? 1 : 0);
    settings::SaveWindowPosition(SECTION, this);
}

void LauncherFrame::Preferences() {
    wxDialog dialog(this, wxID_ANY, "Preferences");
    auto* all = new wxBoxSizer(wxVERTICAL);
    auto* topmost = new wxCheckBox(&dialog, wxID_ANY, "Always on &top");
    topmost->SetValue(topmost_);
    topmost->SetToolTip("Keep the launcher above other windows.");
    all->Add(topmost, 0, wxALL, dialog.FromDIP(12));
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,
             dialog.FromDIP(12));
    dialog.SetSizerAndFit(all);
    dialog.SetMinSize(wxSize(dialog.FromDIP(300), -1));
    dialog.Fit();
    dialog.CentreOnParent();
    topmost->SetFocus();
    if (dialog.ShowModal() != wxID_OK) return;
    if (topmost->GetValue() != topmost_) SetTopmost(topmost->GetValue());
}

void LauncherFrame::SetTopmost(bool on) {
    topmost_ = on;
    const long style = GetWindowStyleFlag();
    SetWindowStyleFlag(on ? style | wxSTAY_ON_TOP : style & ~wxSTAY_ON_TOP);
    settings::SetInt("Preferences", "AlwaysOnTop", on ? 1 : 0);
}

void LauncherFrame::About() {
    const wxString title = wxString("About ") + APP_TITLE;
    const wxString heading = wxString(APP_TITLE) + " " + APP_VERSION_TEXT;
    const wxString blurb = "Starts fs1-enhanced, the enhanced SDL3 port of Microsoft Flight Simulator 1.05 (1982).";
#ifdef __WXMSW__
    // The Windows task dialog.
    const wxString content = wxString::Format(
        "%s\n\n"
        "Author: Krzysztof Kania\n"
        "Website: <a href=\"%s\">kkania.com</a>\n"
        "Support: <a href=\"%s\">buymeacoffee.com/krzysztofkania</a>",
        blurb, WEBSITE, SUPPORT);
    wxIcon icon;
    icon.CopyFromBitmap(AppBitmap(FromDIP(32)));
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof dialog;
    dialog.hwndParent = static_cast<HWND>(GetHWND());
    dialog.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_USE_HICON_MAIN | TDF_ALLOW_DIALOG_CANCELLATION |
                     TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.dwCommonButtons = TDCBF_OK_BUTTON;
    dialog.pszWindowTitle = title.wc_str();
    dialog.hMainIcon = static_cast<HICON>(icon.GetHICON());
    dialog.pszMainInstruction = heading.wc_str();
    dialog.pszContent = content.wc_str();
    dialog.pfCallback = AboutCallback;
    TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
#else
    wxDialog dialog(this, wxID_ANY, title);
    auto* body = new wxBoxSizer(wxHORIZONTAL);
    body->Add(new wxStaticBitmap(&dialog, wxID_ANY, AppBitmap(dialog.FromDIP(48))), 0, wxALL, dialog.FromDIP(12));

    auto* text = new wxBoxSizer(wxVERTICAL);
    auto* headingText = new wxStaticText(&dialog, wxID_ANY, heading);
    headingText->SetFont(dialog.GetFont().Bold().Scaled(1.3f));
    text->Add(headingText, 0, wxBOTTOM, dialog.FromDIP(8));
    text->Add(new wxStaticText(&dialog, wxID_ANY, blurb), 0, wxBOTTOM, dialog.FromDIP(12));
    text->Add(new wxStaticText(&dialog, wxID_ANY, "Author: Krzysztof Kania"));
    AddLink(&dialog, text, "Website:", "kkania.com", WEBSITE);
    AddLink(&dialog, text, "Support:", "buymeacoffee.com/krzysztofkania", SUPPORT);
    body->Add(text, 1, wxTOP | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));

    auto* all = new wxBoxSizer(wxVERTICAL);
    all->Add(body, 1, wxEXPAND);
    all->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0, wxEXPAND | wxALL, dialog.FromDIP(8));
    dialog.SetSizerAndFit(all);
    dialog.CentreOnParent();
    dialog.ShowModal();
#endif
}
