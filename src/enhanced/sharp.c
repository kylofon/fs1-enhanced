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

void sharp_render_cga(const Pc *pc, uint32_t *px)
{
    static uint8_t vram[VRAM_SIZE];
    static Sprite spr;
    static uint8_t all[VRAM_SIZE];
    n_needles = 0;
    panel_frame++;
    uint32_t cseg = m16(pc, 0x122);
    if (!pc->composite || (pc->cga_mode & 0x1E) != 0x1A || cseg == 0 || cseg == 0xB800 ||
        (cseg << 4) + VRAM_SIZE > 0xA0000) {
        pc_render_cga(pc, px);
        return;
    }
    memcpy(vram, pc->mem + 0xB8000, VRAM_SIZE);
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

bool sharp_view(SDL_Renderer *renderer, const Pc *pc, SDL_Texture *cga)
{
    panel_draw(renderer);
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
