/* SDL back end of the sharp renderer (docs/RENDER.md section 6): draws the committed 3D view display list
 * and the panel needles at window size on top of the CGA picture. Tiles of the view that no longer match what the recorder saw
 * (text, overlays, anything unrecorded) are drawn from the CGA texture again. */
#ifndef SHARP_H
#define SHARP_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "pc.h"

/* Width of a line in output pixels of the 640x480 logical screen; 0 draws hairlines with SDL_RenderLines. */
void sharp_set_line_width(float w);
/* --view-border: keep the white left border of the 3D window the game draws in daylight (default: left out). */
void sharp_set_view_border(bool on);

/* --dump-panel FILE: one text line per needle per frame (fitted ends, width, colour). */
void sharp_set_dump_panel(const char *path);

/* --needle-width F: factor on the needle thickness taken from the sprite (default 1). */
void sharp_set_needle_scale(float s);

/* Renders the CGA picture of the frame into px (the pc_render_cga() replacement while sharp is on): the
 * panel's needle sprites (S2.1) are taken out of the video RAM image first and drawn as vectors by
 * sharp_view. Also runs --dump-panel. */
void sharp_render_cga(const Pc *pc, uint32_t *px);

/* Draws the sharp view over the CGA texture just rendered with SDL_RenderTexture. Returns false when
 * nothing sharp was drawn (no list yet, overflowed frame, or the video mode is not the 640x200 one). */
bool sharp_view(SDL_Renderer *renderer, const Pc *pc, SDL_Texture *cga);

#endif
