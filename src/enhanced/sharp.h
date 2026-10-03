/* SDL back end of the sharp renderer (docs/RENDER.md section 6): draws the committed 3D view display list
 * at window size on top of the CGA picture. Tiles of the view that no longer match what the recorder saw
 * (text, overlays, anything unrecorded) are drawn from the CGA texture again. */
#ifndef SHARP_H
#define SHARP_H

#include <SDL3/SDL.h>
#include <stdbool.h>

#include "pc.h"

/* Width of a line in output pixels of the 640x480 logical screen; 0 draws hairlines with SDL_RenderLines. */
void sharp_set_line_width(float w);

/* Draws the sharp view over the CGA texture just rendered with SDL_RenderTexture. Returns false when
 * nothing sharp was drawn (no list yet, overflowed frame, or the video mode is not the 640x200 one). */
bool sharp_view(SDL_Renderer *renderer, const Pc *pc, SDL_Texture *cga);

#endif
