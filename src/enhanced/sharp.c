/* SDL back end of the sharp renderer, see sharp.h and docs/RENDER.md section 6. */
#include "enhanced/sharp.h"
#include "enhanced/displaylist.h"

#include <math.h>
#include <stdlib.h>

#define VIEW_W 160
#define VIEW_H 106
#define SX 4.0f  /* output (640x480 logical) pixels per view x unit: 640 / 160 */
#define SY 2.4f  /* output pixels per CGA scan line: 480 / 200 */

static float line_width = 2.0f;

void sharp_set_line_width(float w) { line_width = w < 0 ? 0 : w; }

/* ---- colour ------------------------------------------------------------------------------- */

/* D1: a little more saturation than the composite decode gives, nothing more. */
#define SATURATION 1.2f

/* The colour a field of the pattern shows on the composite monitor: the pattern is n nibbles (one per
 * fat pixel, first = even x) repeated, decoded with the same NTSC window as the CGA picture and averaged. */
static SDL_FColor pattern_colour(const uint8_t *nib, int n)
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

static SDL_FColor col_pair(uint16_t w) /* nibble pair [30C8]: low byte odd x, high byte even x */
{
    uint8_t nib[2] = { (uint8_t)(w >> 12 & 15), (uint8_t)(w & 15) };
    return pattern_colour(nib, 2);
}

static SDL_FColor col_byte(uint16_t b) /* a colour byte: high nibble even x, low nibble odd x */
{
    uint8_t nib[2] = { (uint8_t)(b >> 4 & 15), (uint8_t)(b & 15) };
    return pattern_colour(nib, 2);
}

static SDL_FColor col_word(uint16_t p) /* a dither word: four fat pixels, low byte first */
{
    uint8_t nib[4] = { (uint8_t)(p >> 4 & 15), (uint8_t)(p & 15), (uint8_t)(p >> 12 & 15), (uint8_t)(p >> 8 & 15) };
    return pattern_colour(nib, 4);
}

/* ---- geometry batch ----------------------------------------------------------------------- */

static SDL_Vertex *verts;
static int *idx;
static int n_verts, n_idx, cap_verts, cap_idx;
static int view_y0; /* screen scan line of view row 0 */

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

static SDL_FPoint to_out(float x, float y) { return (SDL_FPoint){ x * SX, ((float)view_y0 + y) * SY }; }

static int push(SDL_FPoint p, SDL_FColor c)
{
    verts[n_verts] = (SDL_Vertex){ p, c, { 0, 0 } };
    return n_verts++;
}

static void tri(DlPt a, DlPt b, DlPt c, SDL_FColor col)
{
    reserve(3, 3);
    int i = push(to_out(a.x, a.y), col);
    push(to_out(b.x, b.y), col);
    push(to_out(c.x, c.y), col);
    idx[n_idx++] = i, idx[n_idx++] = i + 1, idx[n_idx++] = i + 2;
}

static void quad_out(SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FPoint d, SDL_FColor col)
{
    reserve(4, 6);
    int i = push(a, col);
    push(b, col), push(c, col), push(d, col);
    idx[n_idx++] = i, idx[n_idx++] = i + 1, idx[n_idx++] = i + 2;
    idx[n_idx++] = i, idx[n_idx++] = i + 2, idx[n_idx++] = i + 3;
}

static void fill_rect(float x, float y, float w, float h, SDL_FColor col)
{
    SDL_FPoint a = to_out(x, y), b = to_out(x + w, y + h);
    quad_out(a, (SDL_FPoint){ b.x, a.y }, b, (SDL_FPoint){ a.x, b.y }, col);
}

/* A line as a quad of line_width output pixels, extended by half a width at both ends so that chained
 * lines meet without gaps. */
static void line(DlPt p, DlPt q, SDL_FColor col)
{
    SDL_FPoint a = to_out(p.x, p.y), b = to_out(q.x, q.y);
    float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy), h = line_width * 0.5f;
    if (len < 1e-3f) {
        dx = 1, dy = 0, len = 1;
    }
    dx /= len, dy /= len;
    SDL_FPoint nx = { -dy * h, dx * h }, e = { dx * h, dy * h };
    a.x -= e.x, a.y -= e.y, b.x += e.x, b.y += e.y;
    quad_out((SDL_FPoint){ a.x + nx.x, a.y + nx.y }, (SDL_FPoint){ b.x + nx.x, b.y + nx.y },
             (SDL_FPoint){ b.x - nx.x, b.y - nx.y }, (SDL_FPoint){ a.x - nx.x, a.y - nx.y }, col);
}

static void dot(DlPt p, SDL_FColor col) { fill_rect(p.x - 0.5f, p.y - 0.5f, 1, 1, col); }

/* Convex polygon of the view rectangle on one side of the line a->b (side < 0: f <= 0, > 0: f >= 0). */
static void sky_half(DlPt a, DlPt b, int side, SDL_FColor col)
{
    DlPt in[8] = { { 0, 0 }, { VIEW_W, 0 }, { VIEW_W, VIEW_H }, { 0, VIEW_H } }, out[8];
    int n = 4, m = 0;
    for (int i = 0; i < n; i++) {
        DlPt p = in[i], q = in[(i + 1) % n];
        float fp = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) * (float)side;
        float fq = ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) * (float)side;
        if (fp <= 0)
            out[m++] = p;
        if ((fp < 0 && fq > 0) || (fp > 0 && fq < 0)) {
            float t = fp / (fp - fq);
            out[m++] = (DlPt){ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t };
        }
    }
    for (int i = 1; i + 1 < m; i++)
        tri(out[0], out[i], out[i + 1], col);
}

static void sky(const DlItem *it)
{
    DlPt a = it->u.sky.a, b = it->u.sky.b;
    if (fabsf(a.x - b.x) < 1e-4f && fabsf(a.y - b.y) < 1e-4f) {
        fill_rect(0, 0, VIEW_W, VIEW_H, col_word(it->u.sky.pat_l));
        return;
    }
    sky_half(a, b, 1, col_word(it->u.sky.pat_l));  /* f <= 0: left of a->b */
    sky_half(a, b, -1, col_word(it->u.sky.pat_r)); /* f >= 0 */
}

static float cross3(DlPt a, DlPt b, DlPt c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

/* Ear clipping of one ring (concave outlines happen: roofs, runway ends cut by the view border). */
static void ring(const DlPt *p, int n, SDL_FColor col)
{
    if (n < 3)
        return;
    int stack[256], m = n > 256 ? 256 : n;
    float area = 0;
    for (int i = 0; i < m; i++) {
        stack[i] = i;
        area += p[i].x * p[(i + 1) % m].y - p[(i + 1) % m].x * p[i].y;
    }
    float sgn = area < 0 ? -1.0f : 1.0f;
    while (m > 3) {
        int ear = -1;
        for (int i = 0; i < m && ear < 0; i++) {
            DlPt a = p[stack[(i + m - 1) % m]], b = p[stack[i]], c = p[stack[(i + 1) % m]];
            if (cross3(a, b, c) * sgn <= 0)
                continue;
            bool ok = true;
            for (int j = 0; j < m && ok; j++) {
                DlPt q = p[stack[j]];
                if (stack[j] == stack[(i + m - 1) % m] || stack[j] == stack[i] || stack[j] == stack[(i + 1) % m])
                    continue;
                if (cross3(a, b, q) * sgn >= 0 && cross3(b, c, q) * sgn >= 0 && cross3(c, a, q) * sgn >= 0)
                    ok = false;
            }
            if (ok)
                ear = i;
        }
        if (ear < 0)
            ear = 0; /* degenerate (touching or crossing edges): cut anyway */
        tri(p[stack[(ear + m - 1) % m]], p[stack[ear]], p[stack[(ear + 1) % m]], col);
        for (int i = ear; i + 1 < m; i++)
            stack[i] = stack[i + 1];
        m--;
    }
    tri(p[stack[0]], p[stack[1]], p[stack[2]], col);
}

static void poly(const DlFrame *f, const DlItem *it)
{
    SDL_FColor col = col_byte(it->colour);
    const DlPt *v = f->verts + it->u.poly.first;
    int n = it->u.poly.count, from = 0;
    for (int k = 0; k <= n; k++) {
        if (k == n || isnan(v[k].x)) {
            ring(v + from, k - from, col);
            from = k + 1;
        }
    }
}

/* ---- present ------------------------------------------------------------------------------ */

/* The view tiles of one scan line (16 pixels = 4 bytes) whose B800 bytes differ from what the blit wrote
 * are drawn again from the CGA texture. */
static void fallback_tiles(SDL_Renderer *r, const Pc *pc, SDL_Texture *cga, const DlSnap *snap, int y0)
{
    const uint8_t *vram = pc->mem + 0xB8000;
    for (int row = 0; row < VIEW_H; row++) {
        int y = y0 + row;
        if (y < 0 || y >= CGA_H)
            continue;
        int base = (y & 1) * 0x2000 + (y >> 1) * 80, run = -1;
        for (int xb = 0; xb <= 20; xb++) {
            bool bad = false;
            if (xb < 20) {
                int o = base + xb * 4;
                bool in = false;
                for (int i = 0; i < 2; i++)
                    in |= o >= snap->off[i] && o + 4 <= snap->off[i] + snap->len[i];
                bad = !in;
                for (int k = 0; k < 4 && !bad; k++)
                    bad = vram[o + k] != snap->bytes[o + k];
            }
            if (bad && run < 0)
                run = xb;
            if (!bad && run >= 0) {
                SDL_FRect src = { (float)(run * 16), (float)y, (float)((xb - run) * 16), 1 };
                SDL_FRect dst = { src.x, (float)y * SY, src.w, SY };
                SDL_RenderTexture(r, cga, &src, &dst);
                run = -1;
            }
        }
    }
}

bool sharp_view(SDL_Renderer *renderer, const Pc *pc, SDL_Texture *cga)
{
    const DlFrame *f = dl_view();
    if (!f || f->incomplete || !pc->composite || (pc->cga_mode & 0x1E) != 0x1A)
        return false;
    view_y0 = f->view_row;
    n_verts = n_idx = 0;

    for (int i = 0; i < f->n_items; i++) {
        const DlItem *it = &f->items[i];
        switch (it->kind) {
        case DL_FILL:
            fill_rect(it->u.rect.x, it->u.rect.y, it->u.rect.w, it->u.rect.h, col_word(it->colour));
            break;
        case DL_SKYGROUND:
            sky(it);
            break;
        case DL_POLY:
            if (!(it->flags & DL_F_EMPTY))
                poly(f, it);
            break;
        case DL_LINE:
            if (line_width > 0)
                line(it->u.line.a, it->u.line.b, col_pair(it->colour));
            break;
        case DL_DOT:
            dot(it->u.line.a, col_pair(it->colour));
            break;
        default:
            break;
        }
    }

    SDL_Rect clip = { 0, (int)((float)view_y0 * SY + 0.5f), 640, (int)(VIEW_H * SY + 0.5f) };
    SDL_SetRenderClipRect(renderer, &clip);
    if (n_idx)
        SDL_RenderGeometry(renderer, NULL, verts, n_verts, idx, n_idx);
    if (line_width <= 0) { /* hairlines */
        for (int i = 0; i < f->n_items; i++) {
            const DlItem *it = &f->items[i];
            if (it->kind != DL_LINE)
                continue;
            SDL_FColor c = col_pair(it->colour);
            SDL_SetRenderDrawColorFloat(renderer, c.r, c.g, c.b, 1);
            SDL_FPoint a = to_out(it->u.line.a.x, it->u.line.a.y), b = to_out(it->u.line.b.x, it->u.line.b.y);
            SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
        }
    }
    SDL_SetRenderClipRect(renderer, NULL);
    fallback_tiles(renderer, pc, cga, dl_view_snap(), view_y0);
    return true;
}
