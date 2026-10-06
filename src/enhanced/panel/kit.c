/* Vector kit for the cockpit modules, see kit.h. */
#include "enhanced/panel/kit.h"
#include "pc.h"

#include <math.h>
#include <stdlib.h>

const SDL_FColor kit_white = { 1, 1, 1, 1 };
const SDL_FColor kit_grey = { 0.62f, 0.62f, 0.62f, 1 };
const SDL_FColor kit_black = { 0, 0, 0, 1 };
const SDL_FColor kit_face = { 0.04f, 0.04f, 0.05f, 1 };
const SDL_FColor kit_bezel = { 0.78f, 0.78f, 0.80f, 1 };

/* D1: a little more saturation than the composite decode gives, nothing more. */
#define SATURATION 1.2f

SDL_FColor kit_composite(const uint8_t *nib, int n)
{
    int period = 4 * n;
    float r = 0, g = 0, b = 0;
    for (int x = 0; x < period; x++) {
        int pattern = 0;
        for (int k = -1; k <= 2; k++) {
            int p = (x + k + period) % period;
            pattern = pattern << 1 | (nib[p >> 2] >> (3 - (p & 3)) & 1);
        }
        uint32_t c = pc_composite_rgb(x & 3, pattern);
        r += (float)(c >> 16 & 255), g += (float)(c >> 8 & 255), b += (float)(c & 255);
    }
    r /= (float)period * 255, g /= (float)period * 255, b /= (float)period * 255;
    float l = 0.299f * r + 0.587f * g + 0.114f * b;
    r = l + (r - l) * SATURATION, g = l + (g - l) * SATURATION, b = l + (b - l) * SATURATION;
    return (SDL_FColor){ fminf(fmaxf(r, 0), 1), fminf(fmaxf(g, 0), 1), fminf(fmaxf(b, 0), 1), 1 };
}

/* ---- batch -------------------------------------------------------------------------------- */

static SDL_Vertex *verts;
static int *idx;
static int n_verts, n_idx, cap_verts, cap_idx;

static void reserve(int v, int i)
{
    if (n_verts + v > cap_verts) {
        cap_verts = (n_verts + v) * 2;
        verts = realloc(verts, (size_t)cap_verts * sizeof *verts);
    }
    if (n_idx + i > cap_idx) {
        cap_idx = (n_idx + i) * 2;
        idx = realloc(idx, (size_t)cap_idx * sizeof *idx);
    }
}

static SDL_FPoint out(float x, float y) { return (SDL_FPoint){ x, y * KIT_SY }; }

/* A quad in output coordinates. */
static void quad(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_FColor col)
{
    reserve(4, 6);
    int i = n_verts;
    verts[n_verts++] = (SDL_Vertex){ a, col, { 0, 0 } };
    verts[n_verts++] = (SDL_Vertex){ b, col, { 0, 0 } };
    verts[n_verts++] = (SDL_Vertex){ c, col, { 0, 0 } };
    verts[n_verts++] = (SDL_Vertex){ d, col, { 0, 0 } };
    idx[n_idx++] = i, idx[n_idx++] = i + 1, idx[n_idx++] = i + 2;
    idx[n_idx++] = i, idx[n_idx++] = i + 2, idx[n_idx++] = i + 3;
}

/* A segment in output coordinates, w0 wide at a and w1 at b, extended by e at both ends. */
static void seg(SDL_FPoint a, SDL_FPoint b, float w0, float w1, float e, SDL_FColor col)
{
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f)
        dx = 1, dy = 0, len = 1;
    dx /= len, dy /= len;
    a.x -= dx * e, a.y -= dy * e, b.x += dx * e, b.y += dy * e;
    float h0 = w0 * 0.5f, h1 = w1 * 0.5f;
    quad((SDL_FPoint){ a.x - dy * h0, a.y + dx * h0 }, (SDL_FPoint){ b.x - dy * h1, b.y + dx * h1 },
         (SDL_FPoint){ b.x + dy * h1, b.y - dx * h1 }, (SDL_FPoint){ a.x + dy * h0, a.y - dx * h0 }, col);
}

void kit_line(float x0, float y0, float x1, float y1, float w, SDL_FColor c)
{
    seg(out(x0, y0), out(x1, y1), w, w, w * 0.5f, c);
}

void kit_needle(float hx, float hy, float tx, float ty, float w0, float w1, SDL_FColor c)
{
    seg(out(hx, hy), out(tx, ty), w0, w1, 0, c);
}

void kit_fill_box(float x0, float y0, float x1, float y1, SDL_FColor c)
{
    SDL_FPoint a = out(x0, y0), b = out(x1, y1);
    quad(a, (SDL_FPoint){ b.x, a.y }, b, (SDL_FPoint){ a.x, b.y }, c);
}

void kit_box(float x0, float y0, float x1, float y1, float w, SDL_FColor c)
{
    kit_line(x0, y0, x1, y0, w, c);
    kit_line(x1, y0, x1, y1, w, c);
    kit_line(x1, y1, x0, y1, w, c);
    kit_line(x0, y1, x0, y0, w, c);
}

void kit_dial_point(float cx, float cy, float rx, float ry, float a, float f, float *x, float *y)
{
    float r = a * (float)M_PI / 180;
    *x = cx + sinf(r) * rx * f;
    *y = cy - cosf(r) * ry * f;
}

static int arc_steps(float rx, float ry, float a0, float a1)
{
    int n = (int)(fabsf(a1 - a0) / 360 * (rx + ry * KIT_SY) * 1.2f);
    return n < 8 ? 8 : n > 256 ? 256 : n;
}

void kit_arc(float cx, float cy, float rx, float ry, float a0, float a1, float w, SDL_FColor c)
{
    int n = arc_steps(rx, ry, a0, a1);
    float px, py;
    kit_dial_point(cx, cy, rx, ry, a0, 1, &px, &py);
    for (int i = 1; i <= n; i++) { /* segments overlap by half a width: no notches at the joints */
        float x, y;
        kit_dial_point(cx, cy, rx, ry, a0 + (a1 - a0) * (float)i / (float)n, 1, &x, &y);
        seg(out(px, py), out(x, y), w, w, w * 0.25f, c);
        px = x, py = y;
    }
}

void kit_ellipse(float cx, float cy, float rx, float ry, float w, SDL_FColor c) { kit_arc(cx, cy, rx, ry, 0, 360, w, c); }

void kit_fill_ellipse(float cx, float cy, float rx, float ry, SDL_FColor c)
{
    int n = arc_steps(rx, ry, 0, 360);
    reserve(n + 1, 3 * n);
    int i0 = n_verts;
    verts[n_verts++] = (SDL_Vertex){ out(cx, cy), c, { 0, 0 } };
    for (int i = 0; i < n; i++) {
        float x, y;
        kit_dial_point(cx, cy, rx, ry, 360.0f * (float)i / (float)n, 1, &x, &y);
        verts[n_verts++] = (SDL_Vertex){ out(x, y), c, { 0, 0 } };
        idx[n_idx++] = i0, idx[n_idx++] = i0 + 1 + i, idx[n_idx++] = i0 + 1 + (i + 1) % n;
    }
}

void kit_ticks(float cx, float cy, float rx, float ry, float a0, float a1, int n, float r_in, float w, SDL_FColor c)
{
    for (int i = 0; i < n; i++) {
        float a = n > 1 ? a0 + (a1 - a0) * (float)i / (float)(n - 1) : a0, x0, y0, x1, y1;
        kit_dial_point(cx, cy, rx, ry, a, r_in, &x0, &y0);
        kit_dial_point(cx, cy, rx, ry, a, 1, &x1, &y1);
        seg(out(x0, y0), out(x1, y1), w, w, 0, c);
    }
}

/* ---- font --------------------------------------------------------------------------------- */

static const uint8_t (*rom_font)[8];

void kit_set_font(const uint8_t (*font)[8]) { rom_font = font; }

/* Columns a glyph uses (the ROM font leaves the right column(s) empty): advance in font pixels. */
#define GLYPH_ADV 7

float kit_text(float x, float y, float h, float rot, int align, const char *s, SDL_FColor c)
{
    size_t len = SDL_strlen(s);
    float px = h / 7, py = h / 7; /* output pixels per font pixel (cap height = 7 rows) */
    float w_out = (float)len * GLYPH_ADV * px - px;
    if (!rom_font || !len)
        return w_out;
    float start = align == 1 ? -w_out / 2 : align == 2 ? -w_out : 0;
    float r = rot * (float)M_PI / 180, cr = cosf(r), sr = sinf(r);
    SDL_FPoint o = out(x, y);
    for (size_t k = 0; k < len; k++) {
        const uint8_t *g = rom_font[(uint8_t)s[k]];
        float gx = start + (float)k * GLYPH_ADV * px;
        for (int row = 0; row < 8; row++) {
            for (int cx = 0; cx < 8; cx++) {
                if (!(g[row] >> (7 - cx) & 1))
                    continue;
                int end = cx;
                while (end + 1 < 8 && (g[row] >> (6 - end) & 1))
                    end++;
                /* the run in text space (y down from the top of the caps), slightly overlapping its neighbours */
                float ax = gx + (float)cx * px - 0.1f, bx = gx + (float)(end + 1) * px + 0.1f;
                float ay = (float)row * py - 0.1f, by = ay + py + 0.2f;
                SDL_FPoint p[4] = { { ax, ay }, { bx, ay }, { bx, by }, { ax, by } };
                for (int i = 0; i < 4; i++) {
                    float tx = p[i].x, ty = p[i].y;
                    p[i] = (SDL_FPoint){ o.x + tx * cr - ty * sr, o.y + tx * sr + ty * cr };
                }
                quad(p[0], p[1], p[2], p[3], c);
                cx = end;
            }
        }
    }
    return w_out;
}

void kit_flush(SDL_Renderer *r)
{
    SDL_SetRenderClipRect(r, NULL);
    if (n_idx)
        SDL_RenderGeometry(r, NULL, verts, n_verts, idx, n_idx);
    n_verts = n_idx = 0;
}
