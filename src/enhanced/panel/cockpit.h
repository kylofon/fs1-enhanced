/* Cockpit modules (S2.5, docs/PANEL.md): the instrument panel divided into modules (airspeed, altimeter,
 * radio stack, ...). A module with a draw function owns its rect: the rect is blanked in the CGA copy and the
 * older per-element paths (S2.1 needles, S2.2 values, S2.4 traced labels) leave it alone; the module draws
 * the whole instrument with the vector kit (kit.h) from the frame's state. Modules without a draw function
 * (not done yet) keep the older look. */
#ifndef PANEL_COCKPIT_H
#define PANEL_COCKPIT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "pc.h"

typedef struct CockpitRect {
    int x0, y0, x1, y1; /* hi-res pixels and scan lines, inclusive */
} CockpitRect;

/* A game glyph found in the video RAM this frame (S2.2 detection): top left, character, dim form. */
typedef struct CockpitText {
    int x, line;
    uint8_t ch, dim;
} CockpitText;

/* A panel needle found this frame (S2.1): descriptor (0x92F airspeed, 0x939/0x943 altimeter large/small,
 * 0x94D VSI, 0x957 tachometer), hub and tip in panel units (hi-res x, scan line y). */
typedef struct CockpitNeedle {
    uint16_t desc;
    float hx, hy, tx, ty;
} CockpitNeedle;

typedef struct CockpitFrame {
    const Pc *pc;
    const uint8_t *clean; /* the clean panel image (B800 layout, cseg:0) */
    const CockpitText *text;
    int n_text;
    const CockpitNeedle *needle;
    int n_needle;
} CockpitFrame;

typedef struct CockpitModule {
    const char *name; /* --module-off NAME */
    const char *what;
    CockpitRect r;
    void (*draw)(const CockpitFrame *f, const struct CockpitModule *m); /* NULL: not done yet */
    bool rest; /* the frame module: owns everything inside its rect that no other module's rect covers */
} CockpitModule;

/* --module-off NAME (comma separated, or "all"); --module-outline. */
void cockpit_set_off(const char *names);
void cockpit_set_outline(bool on);

/* Called by sharp_render_cga while the panel is on screen (flight). begin resets the frame; the S2.1/S2.2
 * paths then report what they found; owns tells them whether a point belongs to a drawn module (then they
 * leave it to the module); end blanks the drawn modules' rects in vram. */
void cockpit_begin(const Pc *pc, const uint8_t *clean);
bool cockpit_owns(float x, float line);
void cockpit_add_text(int x, int line, uint8_t ch, uint8_t dim);
void cockpit_add_needle(uint16_t desc, float hx, float hy, float tx, float ty);
void cockpit_end(uint8_t *vram);

/* True if (x, line) is inside the rect of an ordinary (non-rest) module, drawn or not. */
bool cockpit_in_module(int x, int line);

/* Called by sharp_view: draws the drawn modules (and outlines), if the panel was on this frame. */
void cockpit_draw(SDL_Renderer *r);

#endif
