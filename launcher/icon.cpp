// icon.cpp -- the app icon: a light plane seen from above over green country (a light sky over a green
// field), drawn at 16x16 and scaled by whole pixels so every size shows
// the same picture. app.ico holds the same drawing for Explorer; make_icon.py writes it.
#include "icon.h"

#include <wx/image.h>

namespace {

// The plane: '#' fuselage and wings, 'r' the red nose.
const char* const PLANE[16] = {
    "................",
    ".......rr.......",
    ".......##.......",
    ".......##.......",
    "..############..",
    ".##############.",
    "..############..",
    ".......##.......",
    ".......##.......",
    ".......##.......",
    "......####......",
    ".......##.......",
    "................",
    "................",
    "................",
    "................",
};

wxImage PlaneTile() {
    wxImage tile(16, 16);
    auto put = [&](int x, int y, unsigned long rgb) { tile.SetRGB(x, y, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF); };
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            unsigned long c = y < 9 ? 0x6FA8E0 : y == 9 ? 0xB8D8F0 : 0x3C8C3C;
            switch (PLANE[y][x]) {
            case '#': c = 0xF0F0F0; break;
            case 'r': c = 0xD03030; break;
            }
            put(x, y, c);
        }
    return tile;
}

}  // namespace

wxBitmap AppBitmap(int size) { return wxBitmap(PlaneTile().Scale(size, size, wxIMAGE_QUALITY_NEAREST)); }

wxIconBundle AppIcons() {
    wxIconBundle icons;
    for (int size : {16, 20, 24, 32, 48, 64, 256}) {
        wxIcon icon;
        icon.CopyFromBitmap(AppBitmap(size));
        icons.AddIcon(icon);
    }
    return icons;
}
