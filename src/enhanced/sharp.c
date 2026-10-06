/* SDL back end of the sharp renderer, see sharp.h and docs/RENDER.md section 6. */
#include "enhanced/sharp.h"
#include "enhanced/displaylist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define MAX_HAIR 8192
typedef struct { SDL_FPoint a, b; SDL_FColor c; } Hair;
static Hair hair[MAX_HAIR];
static int n_hair;

static void line(DlPt p, DlPt q, SDL_FColor col)
{
    if (line_width <= 0) { /* hairline: kept for the flush, which draws it with SDL_RenderLine */
        if (n_hair < MAX_HAIR)
            hair[n_hair++] = (Hair){ to_out(p.x, p.y), to_out(q.x, q.y), col };
        return;
    }
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

/* Draws the batched geometry (and hairlines) so far, clipped to the view. */
static void flush_clip(SDL_Renderer *r, const SDL_Rect *clip)
{
    SDL_SetRenderClipRect(r, clip);
    if (n_idx)
        SDL_RenderGeometry(r, NULL, verts, n_verts, idx, n_idx);
    for (int i = 0; i < n_hair; i++) {
        SDL_SetRenderDrawColorFloat(r, hair[i].c.r, hair[i].c.g, hair[i].c.b, 1);
        SDL_RenderLine(r, hair[i].a.x, hair[i].a.y, hair[i].b.x, hair[i].b.y);
    }
    n_verts = n_idx = n_hair = 0;
}

static void flush(SDL_Renderer *r)
{
    SDL_Rect clip = { 0, (int)((float)view_y0 * SY + 0.5f), 640, (int)(VIEW_H * SY + 0.5f) };
    flush_clip(r, &clip);
}

/* ---- instrument panel needles (S2.1) ------------------------------------------------------- */

/* The panel is drawn into B800 by the original, needle sprites ORed over the clean panel. Nothing is
 * recorded: each frame the five needle descriptors are read from the game's own data (drawn code, screen
 * offset and size of the sprite, +4/+5/+7; docs/subphases/3.10.md of the faithful port), the sprite bits
 * are rebuilt exactly as the needle drawer ORed them, and a needle counts as present only if every one of
 * those bits is set in the video RAM. The bits are then replaced by the clean panel's, so the CGA texture
 * shows the panel without the needles, and each needle is drawn on top as a line fitted to its sprite
 * pixels. Anything that does not check out (stale descriptor, other screen) stays as the CGA picture. */

#define VRAM_SIZE 0x4000
#define MAX_TOUCH 1700 /* bytes one sprite can touch: 8 words x 100 rows x 2 */

typedef struct Needle {
    SDL_FPoint a, b; /* output (640x480) coordinates of the two ends: a at the hub */
    float width;     /* output pixels at the hub end */
    SDL_FColor col;
    uint16_t desc;
    uint8_t code;
} Needle;

typedef struct Sprite {
    uint8_t bits[VRAM_SIZE]; /* the sprite's bits per video RAM byte */
    uint16_t touched[MAX_TOUCH];
    int n;
} Sprite;

static Needle needles[8];
static int n_needles;
static FILE *panel_dump;
static long panel_frame;
static float needle_scale = 1.0f; /* --needle-width: factor on the thickness taken from the sprite */

void sharp_set_dump_panel(const char *path) { panel_dump = fopen(path, "w"); }
void sharp_set_needle_scale(float s) { needle_scale = s < 0 ? 0 : s; }

static const struct { uint16_t desc, table; } needle_defs[] = {
    { 0x92F, 0x961 }, /* airspeed */
    { 0x939, 0x961 }, /* altimeter, large hand */
    { 0x943, 0x98F }, /* altimeter, small hand */
    { 0x94D, 0x98F }, /* VSI */
    { 0x957, 0x98F }, /* tachometer */
};

static uint16_t m16(const Pc *pc, uint32_t lin) { return (uint16_t)(pc->mem[lin] | pc->mem[lin + 1] << 8); }

static void sprite_clear(Sprite *s)
{
    for (int i = 0; i < s->n; i++)
        s->bits[s->touched[i]] = 0;
    s->n = 0;
}

/* The bits the needle drawer ORed for descriptor desc (sprite table `table`) into s; false when there is
 * no needle or the video RAM does not contain them. *hub_o = video RAM offset of the needle's hub byte. */
static bool needle_bits(const Pc *pc, const uint8_t *vram, uint16_t desc, uint16_t table, Sprite *s, uint8_t *code_out,
                        int *hub_o)
{
    sprite_clear(s);
    uint32_t ds = (uint32_t)m16(pc, 0x120) << 4;
    if (ds == 0 || ds + 0x2000 > 0xA0000)
        return false;
    uint8_t code = pc->mem[ds + desc + 4];
    if (code == 0xFF)
        return false;
    uint16_t off = m16(pc, ds + desc + 5), size = m16(pc, ds + desc + 7);
    int dl = size & 255, dh = size >> 8;
    if (dl < 1 || dl > 8 || dh < 1 || dh > 100)
        return false;
    uint32_t sprite = ds + m16(pc, ds + table + (uint32_t)(code & 0x3F) * 2);
    if (sprite + 2 + (uint32_t)dl * dh * 2 > 0xA0000 || m16(pc, sprite) != size)
        return false;
    bool lower = code & 0x40, mirrored = code & 0x80;
    const uint8_t *rev = pc->mem + ds + 0x12B4;
    uint16_t di = off;
    sprite += 2;
    for (int r = 0; r < dh; r++) {
        uint16_t bp = di;
        for (int c = 0; c < dl; c++) {
            uint16_t w = m16(pc, sprite);
            sprite += 2;
            uint16_t ax = mirrored ? (uint16_t)(rev[w & 255] << 8 | rev[w >> 8]) : w;
            if (bp >= VRAM_SIZE - 1 || s->n + 2 > MAX_TOUCH)
                goto fail;
            s->bits[bp] |= (uint8_t)ax, s->bits[bp + 1] |= (uint8_t)(ax >> 8);
            s->touched[s->n++] = bp, s->touched[s->n++] = (uint16_t)(bp + 1);
            bp = (uint16_t)(bp + (mirrored ? -2 : 2));
        }
        bool odd = (int16_t)bp > 0x1F40;
        di = (uint16_t)(di + (lower ? (odd ? 0xE050 : 0x2000) : (odd ? 0xE000 : 0x1FB0)));
    }
    for (int i = 0; i < s->n; i++)
        if (s->bits[s->touched[i]] & ~vram[s->touched[i]])
            goto fail;
    *code_out = code;
    *hub_o = m16(pc, ds + desc + (lower ? 2 : 0));
    return true;
fail:
    sprite_clear(s);
    return false;
}

/* Output coordinates of the centre of hi-res pixel `bit` (7 = leftmost) of video RAM byte o. */
static bool vram_pixel_pos(int o, int bit, float *x, float *y)
{
    int line, col;
    if (o < 0x2000)
        line = o / 80 * 2, col = o % 80;
    else
        line = (o - 0x2000) / 80 * 2 + 1, col = (o - 0x2000) % 80;
    if (line >= 200)
        return false;
    *x = (float)(col * 8 + (7 - bit)) + 0.5f;
    *y = ((float)line + 0.5f) * SY;
    return true;
}

/* Line through the sprite pixels (principal axis in output space), ends at the extreme pixels, a = the end
 * nearer to the hub byte. Thickness = the area of the pixels over the length. */
static bool needle_fit(const Sprite *s, int hub_o, Needle *n)
{
    double sx = 0, sy = 0, cnt = 0, cx = 0, cy = 0, sxx = 0, syy = 0, sxy = 0, ux = 0, uy = 0, tmin = 1e9, tmax = -1e9;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0) {
            sx = sy = cnt = 0;
        } else if (pass == 1) {
            if (cnt < 4)
                return false;
            cx = sx / cnt, cy = sy / cnt, sxx = syy = sxy = 0;
        } else {
            double th = 0.5 * atan2(2 * sxy, sxx - syy);
            ux = cos(th), uy = sin(th), tmin = 1e9, tmax = -1e9;
        }
        for (int i = 0; i < s->n; i++) {
            int o = s->touched[i];
            for (int bit = 0; bit < 8 && s->bits[o]; bit++) {
                float x, y;
                if (!(s->bits[o] >> bit & 1) || !vram_pixel_pos(o, bit, &x, &y))
                    continue;
                if (pass == 0)
                    sx += x, sy += y, cnt++;
                else if (pass == 1)
                    sxx += (x - cx) * (x - cx), syy += (y - cy) * (y - cy), sxy += (x - cx) * (y - cy);
                else {
                    double t = (x - cx) * ux + (y - cy) * uy;
                    tmin = t < tmin ? t : tmin, tmax = t > tmax ? t : tmax;
                }
            }
        }
        if (pass == 2) {
            tmin -= 0.5, tmax += 0.5;
            SDL_FPoint p = { (float)(cx + ux * tmin), (float)(cy + uy * tmin) };
            SDL_FPoint q = { (float)(cx + ux * tmax), (float)(cy + uy * tmax) };
            float hx = 0, hy = 0; /* the hub: left edge of its byte, centre of its line */
            vram_pixel_pos(hub_o, 7, &hx, &hy);
            hx -= 0.5f;
            bool p_first = (p.x - hx) * (p.x - hx) + (p.y - hy) * (p.y - hy) <= (q.x - hx) * (q.x - hx) + (q.y - hy) * (q.y - hy);
            n->a = p_first ? p : q, n->b = p_first ? q : p;
            n->width = (float)(cnt * SY / (tmax - tmin)) * needle_scale;
        }
    }
    /* colour: the most frequent non-empty nibble of the sprite (one fat pixel = 4 hi-res pixels) */
    int hist[16] = { 0 }, best = 15;
    for (int i = 0; i < s->n; i++) {
        uint8_t v = s->bits[s->touched[i]];
        if (v)
            hist[v >> 4]++, hist[v & 15]++;
    }
    hist[0] = 0;
    for (int v = 1; v < 16; v++)
        if (hist[v] > hist[best])
            best = v;
    uint8_t nib = (uint8_t)best;
    n->col = pattern_colour(&nib, 1);
    return true;
}

/* ---- text (S2.2) --------------------------------------------------------------------------- */

/* The game's font is 4 x 5 cells of one fat pixel (a nibble, 4 x 1 hi-res pixels) per glyph, stored as 16 x 5
 * bits. The print_str family reports each glyph row it writes (dl_text_row); a row counts as text only while
 * the video RAM still holds exactly those bytes, so anything drawn over it, cleared or restored shows as
 * before. Valid rows go into a grid of fat-pixel cells and are zeroed in the video RAM copy (the original
 * writes the whole word, so the background under a glyph is black); sharp_view draws every cell as a
 * rectangle whose convex corners are cut, and bridges cells that touch only diagonally, so the glyphs
 * become smooth shapes at any window size. */

#define TCELL_W 160
#define TCELL_H 200
static uint8_t tcell[TCELL_W * TCELL_H]; /* fat pixels whose nibble has one bit (dim text): the nibble, 0 = none */
static uint8_t tbit[4 * TCELL_W * TCELL_H]; /* hi-res pixels of the other nibbles (the glyph strokes): 15 = lit */
static int n_tcell;

/* The font's glyph rows (masks FFFF and 8888, the two the game uses) for the detection below; 0 = not found. */
#define N_GLYPH 0x60
static uint16_t glyph_rows[2 * N_GLYPH][5];
static uint8_t glyph_ch[2 * N_GLYPH], glyph_dim[2 * N_GLYPH]; /* the character and whether it is the dim (8888) form */
static int n_glyph;

static bool font_load(const Pc *pc)
{
    uint32_t base = (uint32_t)m16(pc, (0x50u << 4) + 0x1247);
    static const uint16_t segs[2] = { 0x618, 0x686 };
    for (int k = 0; k < 2; k++) {
        const uint8_t *f = pc->mem + ((uint32_t)segs[k] << 4) + base;
        bool ok = true;
        for (int r = 0; r < 5; r++)
            ok = ok && f[r * 2] == 0 && f[r * 2 + 1] == 0; /* the space is empty */
        for (int ch = 'A'; ch <= 'Z' && ok; ch++) {
            int nz = 0;
            for (int r = 0; r < 10; r++)
                nz += f[(ch - 0x20) * 10 + r] != 0;
            ok = nz >= 4;
        }
        if (!ok)
            continue;
        n_glyph = 0;
        for (int ch = 0x21; ch < 0x60; ch++) { /* the game's codes above that are symbols (panel switches), not the ROM font's letters */
            for (int m = 0; m < 2; m++) {
                int cells = 0;
                uint16_t *g = glyph_rows[n_glyph];
                for (int r = 0; r < 5; r++) {
                    g[r] = (uint16_t)((f[(ch - 0x20) * 10 + r * 2] | f[(ch - 0x20) * 10 + r * 2 + 1] << 8) & (m ? 0x8888 : 0xFFFF));
                    for (int b = 0; b < 4; b++)
                        cells += (g[r] >> (b * 4) & 15) != 0;
                }
                if (cells >= 3) {
                    glyph_ch[n_glyph] = (uint8_t)ch, glyph_dim[n_glyph] = (uint8_t)m;
                    n_glyph++;
                }
            }
        }
        return true;
    }
    n_glyph = 0;
    return false;
}

/* Glyphs of the game's font found in the video RAM, drawn by the back end with the 8x8 text font (pc->font)
 * instead of the game's thin 16x5 shapes. */
#define MAX_HIT 2048
typedef struct TextHit {
    int x, line; /* hi-res pixel column and scan line of the glyph's top left */
    uint8_t dim;
    uint8_t bm[8];
} TextHit;
static TextHit hits[MAX_HIT];
static int n_hit;
static uint8_t claimed[VRAM_SIZE]; /* video RAM bytes that belong to a found glyph */

/* Text is found by looking for the game's glyphs in the video RAM: five stacked rows equal to a glyph's
 * (whatever drew them: the print_str family, a buffer blitted later, ...). Rows from first_line down only
 * (the 3D view above could hold a lookalike). */
static void text_detect(const Pc *pc, const uint8_t *real, int first_line)
{
    for (int line = first_line; line + 4 < TCELL_H; line++) {
        for (int b = 0; b < 79; b++) {
            int o[5];
            uint16_t row[5];
            for (int r = 0; r < 5; r++) {
                int l = line + r;
                o[r] = (l & 1) * 0x2000 + (l >> 1) * 80 + b;
                row[r] = (uint16_t)(real[o[r]] | real[o[r] + 1] << 8);
            }
            if (!row[0] && !row[1] && !row[2])
                continue;
            for (int g = 0; g < n_glyph; g++) {
                const uint16_t *gr = glyph_rows[g];
                if (gr[0] == row[0] && gr[1] == row[1] && gr[2] == row[2] && gr[3] == row[3] && gr[4] == row[4]) {
                    if (n_hit < MAX_HIT) {
                        TextHit *h = &hits[n_hit++];
                        h->x = b * 8, h->line = line, h->dim = glyph_dim[g];
                        memcpy(h->bm, pc->font[glyph_ch[g]], 8);
                    }
                    for (int r = 0; r < 5; r++)
                        claimed[o[r]] = claimed[o[r] + 1] = 1;
                    b++; /* the next cell starts two bytes on */
                    break;
                }
            }
        }
    }
}

static void text_collect(const Pc *pc, uint8_t *vram, int detect_from)
{
    uint16_t *w;
    uint8_t *ok;
    n_hit = 0;
    if (!dl_text_rows(&w, &ok))
        return;
    const uint8_t *real = pc->mem + 0xB8000;
    static uint16_t used[VRAM_SIZE];
    int n_used = 0;
    memset(claimed, 0, sizeof claimed);
    if (font_load(pc))
        text_detect(pc, real, detect_from);
    for (int o = 0; o < VRAM_SIZE - 1; o++) {
        if (!ok[o])
            continue;
        if (real[o] != (w[o] & 255) || real[o + 1] != (w[o] >> 8)) {
            ok[o] = 0; /* overwritten since: not text any more */
            continue;
        }
        if (w[o] && !(claimed[o] && claimed[o + 1]))
            used[n_used++] = (uint16_t)o;
    }
    for (int a = 0; a < VRAM_SIZE; a++)
        if (claimed[a])
            vram[a] = 0;
    if (!n_used)
        return;
    memset(tcell, 0, sizeof tcell);
    memset(tbit, 0, sizeof tbit);
    for (int k = 0; k < n_used; k++) {
        for (int i = 0; i < 2; i++) {
            int a = used[k] + i, within = a & 0x1FFF, line = within / 80 * 2 + (a >> 13), col = within % 80;
            if (within >= 8000 || line >= TCELL_H || claimed[a])
                continue;
            for (int k = 0; k < 2; k++) {
                int nib = k ? real[a] & 15 : real[a] >> 4, bits = __builtin_popcount(nib);
                if (bits == 1)
                    tcell[line * TCELL_W + col * 2 + k] = (uint8_t)nib;
                else
                    for (int j = 0; j < 4; j++)
                        if (nib >> (3 - j) & 1)
                            tbit[line * 4 * TCELL_W + col * 8 + k * 4 + j] = 15;
            }
            vram[a] = 0;
            n_tcell++;
        }
    }
}


/* ---- panel labels (S2.4) ------------------------------------------------------------------- */

/* The labels (COMPASS, FUEL, TRANSPONDER, gauge numerals ...) are part of the panel bitmap, drawn in a small
 * 5-line font of dotted strokes (a lit pixel every other column), which the composite decode turns into
 * colour fringes. They are found in the clean panel image as small clusters of lit pixels (5 lines tall, at
 * most 24 wide), gaps of one pixel inside a stroke are bridged, and the cluster is drawn by the back end as
 * smoothed white strokes on the hi-res pixel grid. A cluster counts only while the video RAM still holds all
 * its pixels; those are then cleared in the CGA copy (whatever else is drawn there stays). Text only: the
 * gauge faces, scales and arcs are larger than that and stay in the CGA picture. */

#define LBL_W 640
#define LBL_MAX_PIX 16384
#define LBL_MAX_GLYPH 1024
typedef struct LPix {
    uint16_t x;
    uint8_t y, real; /* real: lit in the clean panel; else a bridged gap */
} LPix;
static LPix lpix[LBL_MAX_PIX];
static uint16_t lglyph[LBL_MAX_GLYPH + 1]; /* first pixel of each glyph; one more as the end */
static int n_lglyph, n_lpix;
static uint32_t lbl_hash;
static uint8_t lbit[LBL_W * TCELL_H]; /* the strokes to draw: 15 = lit hi-res pixel */
static int n_lbit;

static int clean_lit(const uint8_t *clean, int x, int y)
{
    return clean[(y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 3)] >> (7 - (x & 7)) & 1;
}

static void label_scan(const uint8_t *clean, int first_line)
{
    static uint8_t seen[LBL_W * TCELL_H];
    static uint32_t stack[LBL_W * TCELL_H];
    static LPix comp[2048];
    n_lglyph = n_lpix = 0;
    memset(seen, 0, sizeof seen);
    for (int y0 = first_line; y0 < TCELL_H; y0++) {
        for (int x0 = 0; x0 < LBL_W; x0++) {
            if (seen[y0 * LBL_W + x0] || !clean_lit(clean, x0, y0))
                continue;
            int sp = 0, nc = 0, minx = x0, maxx = x0, miny = y0, maxy = y0;
            stack[sp++] = (uint32_t)(y0 * LBL_W + x0);
            seen[y0 * LBL_W + x0] = 1;
            while (sp) {
                int cy = (int)(stack[--sp] / LBL_W), cx = (int)(stack[sp] % LBL_W);
                if (nc < 2048)
                    comp[nc] = (LPix){ (uint16_t)cx, (uint8_t)cy, 1 };
                nc++;
                minx = cx < minx ? cx : minx, maxx = cx > maxx ? cx : maxx;
                miny = cy < miny ? cy : miny, maxy = cy > maxy ? cy : maxy;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -2; dx <= 2; dx++) {
                        int ny = cy + dy, nx = cx + dx;
                        if (ny < first_line || ny >= TCELL_H || nx < 0 || nx >= LBL_W || seen[ny * LBL_W + nx] || !clean_lit(clean, nx, ny))
                            continue;
                        seen[ny * LBL_W + nx] = 1;
                        stack[sp++] = (uint32_t)(ny * LBL_W + nx);
                    }
                }
            }
            if (maxy - miny != 4 || maxx - minx > 23 || nc > 2048 || n_lglyph >= LBL_MAX_GLYPH || n_lpix + nc + 16 > LBL_MAX_PIX)
                continue;
            lglyph[n_lglyph++] = (uint16_t)n_lpix;
            for (int i = 0; i < nc; i++)
                lpix[n_lpix++] = comp[i];
            for (int y = miny; y <= maxy; y++) /* a stroke drawn as every other pixel becomes solid */
                for (int x = minx; x + 2 <= maxx; x++)
                    if (clean_lit(clean, x, y) && !clean_lit(clean, x + 1, y) && clean_lit(clean, x + 2, y))
                        lpix[n_lpix++] = (LPix){ (uint16_t)(x + 1), (uint8_t)y, 0 };
        }
    }
    lglyph[n_lglyph] = (uint16_t)n_lpix;
}

static void label_apply(const uint8_t *clean, uint8_t *vram, int first_line)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < VRAM_SIZE; i++)
        h = (h ^ clean[i]) * 16777619u;
    if (h != lbl_hash || !n_lglyph) {
        label_scan(clean, first_line);
        lbl_hash = h;
    }
    memset(lbit, 0, sizeof lbit);
    n_lbit = 0;
    for (int g = 0; g < n_lglyph; g++) {
        bool intact = true;
        for (int i = lglyph[g]; i < lglyph[g + 1] && intact; i++)
            if (lpix[i].real)
                intact = (vram[(lpix[i].y & 1) * 0x2000 + (lpix[i].y >> 1) * 80 + (lpix[i].x >> 3)] >> (7 - (lpix[i].x & 7)) & 1) != 0;
        if (!intact)
            continue;
        for (int i = lglyph[g]; i < lglyph[g + 1]; i++) {
            const LPix *p = &lpix[i];
            lbit[p->y * LBL_W + p->x] = 15;
            n_lbit++;
            if (p->real)
                vram[(p->y & 1) * 0x2000 + (p->y >> 1) * 80 + (p->x >> 3)] &= (uint8_t)~(0x80 >> (p->x & 7));
        }
    }
}

static const uint8_t *g_cells;
static int g_w;
static int cell_on(int x, int y) { return x >= 0 && y >= 0 && x < g_w && y < TCELL_H && g_cells[y * g_w + x]; }

static void fan(const SDL_FPoint *p, int n, SDL_FColor c)
{
    reserve(n, 3 * (n - 2));
    int i = n_verts;
    for (int k = 0; k < n; k++)
        push(p[k], c);
    for (int k = 1; k < n - 1; k++)
        idx[n_idx++] = i, idx[n_idx++] = i + k, idx[n_idx++] = i + k + 1;
}

/* One grid of cells (gw wide, cw output pixels each) as smoothed shapes: every cell a rectangle with its convex
 * corners cut, a small rhombus where two cells touch only diagonally. */
static void draw_grid(const uint8_t *g, int gw, float cw, const SDL_FColor *ncol)
{
    g_cells = g, g_w = gw;
    const float ch = SY, kx = (cw < 2 ? 0.5f : 0.3f) * cw, ky = 0.35f * ch;
    for (int y = 0; y < TCELL_H; y++) {
        for (int x = 0; x < gw; x++) {
            int v = g[y * gw + x];
            if (!v)
                continue;
            int n = cell_on(x, y - 1), s = cell_on(x, y + 1), e = cell_on(x + 1, y), wst = cell_on(x - 1, y);
            float x0 = (float)x * cw, x1 = x0 + cw, y0 = (float)y * ch, y1 = y0 + ch;
            SDL_FPoint p[8];
            int np = 0;
            if (!n && !wst) p[np++] = (SDL_FPoint){ x0, y0 + ky }, p[np++] = (SDL_FPoint){ x0 + kx, y0 };
            else p[np++] = (SDL_FPoint){ x0, y0 };
            if (!n && !e) p[np++] = (SDL_FPoint){ x1 - kx, y0 }, p[np++] = (SDL_FPoint){ x1, y0 + ky };
            else p[np++] = (SDL_FPoint){ x1, y0 };
            if (!s && !e) p[np++] = (SDL_FPoint){ x1, y1 - ky }, p[np++] = (SDL_FPoint){ x1 - kx, y1 };
            else p[np++] = (SDL_FPoint){ x1, y1 };
            if (!s && !wst) p[np++] = (SDL_FPoint){ x0 + kx, y1 }, p[np++] = (SDL_FPoint){ x0, y1 - ky };
            else p[np++] = (SDL_FPoint){ x0, y1 };
            fan(p, np, ncol[v]);
            if (y + 1 < TCELL_H) { /* a cell that touches this one only at a corner: bridge the gap */
                if (cell_on(x + 1, y + 1) && !e && !s) {
                    SDL_FPoint q[4] = { { x1 - kx, y1 }, { x1, y1 - ky }, { x1 + kx, y1 }, { x1, y1 + ky } };
                    fan(q, 4, ncol[v]);
                }
                if (cell_on(x - 1, y + 1) && !wst && !s) {
                    SDL_FPoint q[4] = { { x0 - kx, y1 }, { x0, y1 - ky }, { x0 + kx, y1 }, { x0, y1 + ky } };
                    fan(q, 4, ncol[v]);
                }
            }
        }
    }
}

static void text_draw(SDL_Renderer *r)
{
    if (!n_tcell && !n_hit && !n_lbit)
        return;
    static SDL_FColor ncol[16];
    static bool ncol_ready;
    if (!ncol_ready) {
        for (int n = 1; n < 16; n++) /* no composite colour fringes: white, or grey for dim text (one bit per nibble) */
            ncol[n] = n == 15 ? (SDL_FColor){ 1, 1, 1, 1 } : (SDL_FColor){ 0.65f, 0.65f, 0.65f, 1 };
        ncol_ready = true;
    }
    n_verts = n_idx = n_hair = 0;
    draw_grid(tcell, TCELL_W, 4.0f, ncol);
    draw_grid(tbit, 4 * TCELL_W, 1.0f, ncol); /* strokes: lit hi-res pixels in the colour of a full nibble */
    if (n_lbit)
        draw_grid(lbit, LBL_W, 1.0f, ncol); /* panel labels */
    for (int i = 0; i < n_hit; i++) { /* the 8x8 font: each run of lit pixels one rectangle, 2 x 1.7 output pixels per pixel */
        const TextHit *h = &hits[i];
        SDL_FColor col = ncol[h->dim ? 8 : 15];
        float x0 = (float)h->x, y0 = (float)h->line * SY - 0.8f;
        for (int row = 0; row < 8; row++) {
            for (int cx = 0; cx < 8; cx++) {
                if (!(h->bm[row] >> (7 - cx) & 1))
                    continue;
                int end = cx;
                while (end + 1 < 8 && (h->bm[row] >> (6 - end) & 1))
                    end++;
                float ax = x0 + (float)cx * 2.0f, bx = x0 + (float)(end + 1) * 2.0f + 0.8f;
                float ay = y0 + (float)row * 1.7f, by = ay + 1.85f;
                quad_out((SDL_FPoint){ ax, ay }, (SDL_FPoint){ bx, ay }, (SDL_FPoint){ bx, by }, (SDL_FPoint){ ax, by }, col);
                cx = end;
            }
        }
    }
    flush_clip(r, NULL);
}


void sharp_render_cga(const Pc *pc, uint32_t *px)
{
    static uint8_t vram[VRAM_SIZE];
    static Sprite spr;
    static uint8_t all[VRAM_SIZE];
    n_needles = 0;
    panel_frame++;
    uint32_t cseg = m16(pc, 0x122);
    n_tcell = n_hit = n_lbit = 0;
    if (!pc->composite || (pc->cga_mode & 0x1E) != 0x1A) {
        pc_render_cga(pc, px);
        return;
    }
    memcpy(vram, pc->mem + 0xB8000, VRAM_SIZE);
    if (cseg == 0 || cseg == 0xB800 || (cseg << 4) + VRAM_SIZE > 0xA0000) { /* no panel: text only */
        text_collect(pc, vram, 0);
        pc_render_cga_vram(pc, vram, px);
        return;
    }
    const uint8_t *clean = pc->mem + (cseg << 4);
    static uint16_t done[8 * MAX_TOUCH];
    int n_done = 0;
    for (size_t k = 0; k < sizeof needle_defs / sizeof *needle_defs; k++) {
        uint8_t code;
        int hub_o;
        Needle *n = &needles[n_needles];
        for (int pass = 0; pass < 2; pass++) { /* the listed sprite table first, then the other one */
            uint16_t table = pass == 0 ? needle_defs[k].table : (needle_defs[k].table == 0x961 ? 0x98F : 0x961);
            if (needle_bits(pc, vram, needle_defs[k].desc, table, &spr, &code, &hub_o) && needle_fit(&spr, hub_o, n)) {
                n->desc = needle_defs[k].desc, n->code = code;
                n_needles++;
                for (int i = 0; i < spr.n && n_done < 8 * MAX_TOUCH; i++) {
                    all[spr.touched[i]] |= spr.bits[spr.touched[i]];
                    done[n_done++] = spr.touched[i];
                }
                if (panel_dump)
                    fprintf(panel_dump, "frame %ld desc %04X table %04X code %02X a %.1f,%.1f b %.1f,%.1f w %.2f col %.2f,%.2f,%.2f\n",
                            panel_frame, n->desc, table, code, n->a.x, n->a.y, n->b.x, n->b.y, n->width, n->col.r, n->col.g, n->col.b);
                break;
            }
        }
    }
    for (int i = 0; i < n_done; i++) { /* the needle bits out, the clean panel's in; each byte once */
        int o = done[i];
        if (all[o])
            vram[o] = (uint8_t)((vram[o] & ~all[o]) | (clean[o] & all[o])), all[o] = 0;
    }
    const DlFrame *vf = dl_view(); /* in flight the 3D view occupies the top: no glyph look-alikes there */
    if (vf && !vf->incomplete)
        label_apply(clean, vram, 106);
    text_collect(pc, vram, vf && !vf->incomplete ? 106 : 0);
    pc_render_cga_vram(pc, vram, px);
}

/* A needle: a quad from the hub end (full width) to the tip (a little narrower). */
static void needle_quad(const Needle *n)
{
    float dx = n->b.x - n->a.x, dy = n->b.y - n->a.y, len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-3f)
        return;
    dx /= len, dy /= len;
    float h0 = n->width * 0.55f, h1 = n->width * 0.3f;
    SDL_FPoint nx = { -dy, dx };
    quad_out((SDL_FPoint){ n->a.x + nx.x * h0, n->a.y + nx.y * h0 }, (SDL_FPoint){ n->b.x + nx.x * h1, n->b.y + nx.y * h1 },
             (SDL_FPoint){ n->b.x - nx.x * h1, n->b.y - nx.y * h1 }, (SDL_FPoint){ n->a.x - nx.x * h0, n->a.y - nx.y * h0 }, n->col);
}

static void panel_draw(SDL_Renderer *r)
{
    n_verts = n_idx = n_hair = 0;
    for (int i = 0; i < n_needles; i++)
        needle_quad(&needles[i]);
    flush_clip(r, NULL);
}

static bool view_draw(SDL_Renderer *renderer, const Pc *pc, SDL_Texture *cga)
{
    const DlFrame *f = dl_view();
    if (!f || f->incomplete || !pc->composite || (pc->cga_mode & 0x1E) != 0x1A)
        return false;
    view_y0 = f->view_row;
    n_verts = n_idx = n_hair = 0;

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
            if (it->flags & DL_F_LEAK) { /* the fill left its outline: show the changed box from the CGA picture */
                flush(renderer);
                int x0 = it->u.poly.x0, y0 = it->u.poly.y0, x1 = it->u.poly.x1, y1 = it->u.poly.y1;
                SDL_FRect src = { (float)(x0 * 4), (float)(view_y0 + y0), (float)((x1 - x0 + 1) * 4), (float)(y1 - y0 + 1) };
                SDL_FRect dst = { src.x, src.y * SY, src.w, src.h * SY };
                SDL_RenderTexture(renderer, cga, &src, &dst);
            } else if (!(it->flags & DL_F_EMPTY)) {
                poly(f, it);
            }
            break;
        case DL_LINE:
            line(it->u.line.a, it->u.line.b, col_pair(it->colour));
            break;
        case DL_DOT:
            dot(it->u.line.a, col_pair(it->colour));
            break;
        default:
            break;
        }
    }

    flush(renderer);
    SDL_SetRenderClipRect(renderer, NULL);
    fallback_tiles(renderer, pc, cga, dl_view_snap(), view_y0);
    return true;
}

bool sharp_view(SDL_Renderer *renderer, const Pc *pc, SDL_Texture *cga)
{
    panel_draw(renderer);
    bool shown = view_draw(renderer, pc, cga);
    text_draw(renderer);
    return shown;
}
