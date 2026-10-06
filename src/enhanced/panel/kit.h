/* Vector kit for the cockpit modules (S2.5, docs/PANEL.md). Coordinates are panel units: x in hi-res pixels
 * (0..640), y in CGA scan lines (0..200), both float; the kit maps them to the 640x480 logical output (y * 2.4).
 * Widths are output pixels. Everything is batched and drawn by kit_flush in call order. */
#ifndef PANEL_KIT_H
#define PANEL_KIT_H

#include <SDL3/SDL.h>
#include <stdint.h>

#define KIT_SY 2.4f /* output pixels per scan line */

/* Panel palette: one look for every module. */
extern const SDL_FColor kit_white, kit_grey, kit_black, kit_face, kit_bezel;

/* The colour a field of the pattern shows on the composite monitor (n nibbles, first = even x). */
SDL_FColor kit_composite(const uint8_t *nib, int n);

void kit_line(float x0, float y0, float x1, float y1, float w, SDL_FColor c);
void kit_box(float x0, float y0, float x1, float y1, float w, SDL_FColor c);      /* outline */
void kit_fill_box(float x0, float y0, float x1, float y1, SDL_FColor c);
/* Ellipse centred at cx,cy with radii rx (hi-res pixels) and ry (scan lines). Angles in degrees, 0 = up,
 * clockwise, as on a gauge dial. */
void kit_arc(float cx, float cy, float rx, float ry, float a0, float a1, float w, SDL_FColor c);
void kit_ellipse(float cx, float cy, float rx, float ry, float w, SDL_FColor c);
void kit_fill_ellipse(float cx, float cy, float rx, float ry, SDL_FColor c);
/* n ticks evenly from a0 to a1 (inclusive), from radius fraction r_in to 1 of the ellipse. */
void kit_ticks(float cx, float cy, float rx, float ry, float a0, float a1, int n, float r_in, float w, SDL_FColor c);
/* A point on the ellipse at angle a (degrees, 0 = up, clockwise), scaled by f. */
void kit_dial_point(float cx, float cy, float rx, float ry, float a, float f, float *x, float *y);
/* A needle from the hub to the tip (panel units): a tapered quad, w0 output pixels at the hub, w1 at the tip. */
void kit_needle(float hx, float hy, float tx, float ty, float w0, float w1, SDL_FColor c);

/* Panel font: the 8x8 ROM font (font[ch][row], MSB = left) set by kit_set_font. h = cap height in output
 * pixels; the width follows the font's aspect. align: 0 left, 1 centre, 2 right of x. rot in degrees about x,y
 * (clockwise). Returns the text width in panel units. */
void kit_set_font(const uint8_t (*font)[8]);
float kit_text(float x, float y, float h, float rot, int align, const char *s, SDL_FColor c);

void kit_flush(SDL_Renderer *r);

#endif
