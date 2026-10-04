/* Display-list recorder, see displaylist.h and docs/RENDER.md. Everything here only reads the emulated
 * memory (pc->cpu.mem, no trace bits); nothing is written to it and no cycle is charged. */
#include "enhanced/displaylist.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool dl_on;

static DlFrame frames[2]; /* building, committed */
static int building;
static bool committed;

const DlFrame *dl_view(void) { return committed ? &frames[building ^ 1] : NULL; }
static DlSnap snap;
const DlSnap *dl_view_snap(void) { return &snap; }

/* ---- game memory (read-only) -------------------------------------------------------------- */

static uint8_t m8(Pc *pc, uint16_t seg, uint16_t off) { return pc->cpu.mem[cpu_linear(seg, off)]; }
static uint8_t d8(Pc *pc, uint16_t off) { return m8(pc, GAME_DS, off); }
static uint16_t d16(Pc *pc, uint16_t off) { return (uint16_t)(d8(pc, off) | d8(pc, (uint16_t)(off + 1)) << 8); }

#define VIEW_BUF_SEG 0x3806
#define ROW_OFFSETS 0x380C
#define DRAW_COLOUR 0x30C8
#define POLY_MODE 0x31D0
#define COLOUR_BYTE 0x3159
#define HORIZON_LIST 0x041F
#define VIEW_W 160
#define VIEW_H 106
#define BUF_SIZE 0x4000

static uint16_t view_seg(Pc *pc) { return d16(pc, VIEW_BUF_SEG); }
static uint16_t row_off(Pc *pc, int y) { return d16(pc, (uint16_t)(ROW_OFFSETS + 2 * (y & 0x7F))); }

static int view_row_of(Pc *pc)
{
    uint16_t o = row_off(pc, 0);
    return ((o & 0x1FFF) / 80) * 2 + (o >= 0x2000);
}

/* ---- items -------------------------------------------------------------------------------- */

static DlFrame *cur(void) { return &frames[building]; }

static DlItem *add(DlKind kind, uint8_t flags, uint16_t colour)
{
    DlFrame *f = cur();
    if (f->n_items >= DL_MAX_ITEMS) {
        f->incomplete = true;
        return NULL;
    }
    DlItem *it = &f->items[f->n_items++];
    memset(it, 0, sizeof *it);
    it->kind = (uint8_t)kind;
    it->flags = flags;
    it->colour = colour;
    return it;
}

static void add_fill(float x, float y, float w, float h, uint16_t pattern)
{
    DlItem *it = add(DL_FILL, 0, pattern);
    if (it)
        it->u.rect.x = x, it->u.rect.y = y, it->u.rect.w = w, it->u.rect.h = h;
}

static bool vert(DlPt p)
{
    DlFrame *f = cur();
    if (f->n_verts >= DL_MAX_VERTS) {
        f->incomplete = true;
        return false;
    }
    f->verts[f->n_verts++] = p;
    return true;
}

/* ---- --dl-check: which bytes of the back buffer did a writer without a hook change? --------- */

static bool chk_on;
static bool chk_valid;
static uint8_t chk_shadow[BUF_SIZE], chk_claimed[BUF_SIZE];
static const char *chk_last = "frame start";
static struct ChkPair {
    const char *a, *b;
    int times;
    long bytes;
    uint16_t first;
} chk_pairs[64];
static int chk_n_pairs;
static bool chk_atexit_set;

/* Polygons whose flood fill (fat pixels changed since the last sync) differs much from the vector area. */
static struct { uint32_t frame; int x0, y0, x1, y1, changed; float area; } chk_poly[8];
static int chk_poly_n, chk_poly_total, chk_poly_all;
static int chk_unfilled_vis; /* polygons whose flood fill filled nothing although their outline is on screen */
static struct { uint32_t frame; uint16_t pat; uint8_t col; int px; } chk_unf[6];

static void chk_report(void)
{
    if (!chk_n_pairs)
        fprintf(stderr, "dl-check: every back-buffer write was recorded\n");
    fprintf(stderr, "dl-check: %d polygons with a visible outline were not filled by the game\n", chk_unfilled_vis);
    for (int i = 0; i < chk_unfilled_vis && i < 6; i++)
        fprintf(stderr, "dl-check:   frame %u: %d outline pixels, fill pattern %04X, colour %02X\n", chk_unf[i].frame, chk_unf[i].px,
                chk_unf[i].pat, chk_unf[i].col);
    fprintf(stderr, "dl-check: %d of %d filled polygons differ from their vector area (fill vs outline)\n", chk_poly_total,
            chk_poly_all);
    for (int i = 0; i < chk_poly_n; i++)
        fprintf(stderr, "dl-check:   frame %u box %d,%d-%d,%d: %d pixels filled, vector area %.0f\n", chk_poly[i].frame,
                chk_poly[i].x0, chk_poly[i].y0, chk_poly[i].x1, chk_poly[i].y1, chk_poly[i].changed, chk_poly[i].area);
    for (int i = 0; i < chk_n_pairs; i++) {
        struct ChkPair *p = &chk_pairs[i];
        uint16_t o = p->first;
        fprintf(stderr, "dl-check: unrecorded writes between '%s' and '%s': %ld bytes in %d frame-steps, first at %04X "
                "(y %d, byte %d)\n", p->a, p->b, p->bytes, p->times, o, ((o & 0x1FFF) / 80) * 2 + (o >= 0x2000), (o & 0x1FFF) % 80);
    }
}

static void chk_claim_rows(Pc *pc, int y0, int y1, int xb0, int xb1)
{
    if (!chk_on)
        return;
    if (y0 < 0)
        y0 = 0;
    if (y1 >= VIEW_H)
        y1 = VIEW_H - 1;
    if (xb0 < 0)
        xb0 = 0;
    if (xb1 > 79)
        xb1 = 79;
    for (int y = y0; y <= y1; y++) {
        int base = row_off(pc, y);
        for (int x = xb0; x <= xb1; x++)
            if (base + x < BUF_SIZE)
                chk_claimed[base + x] = 1;
    }
}

/* Fat pixels of the view rows that differ from the state at the previous sync. */
static int chk_changed_pixels(Pc *pc)
{
    const uint8_t *buf = pc->cpu.mem + cpu_linear(view_seg(pc), 0);
    int n = 0;
    for (int i = 0; i < BUF_SIZE; i++)
        if ((i & 0x1FFF) < VIEW_H / 2 * 80 && buf[i] != chk_shadow[i])
            n += ((buf[i] ^ chk_shadow[i]) & 0xF0 ? 1 : 0) + ((buf[i] ^ chk_shadow[i]) & 0x0F ? 1 : 0);
    return n;
}

/* Compares the back buffer with its state at the previous sync; bytes that changed without being claimed
 * by a hooked writer are reported against the pair (previous hook, this hook). Then re-synchronises. */
static void sync(Pc *pc, const char *name)
{
    if (!chk_on)
        return;
    const uint8_t *buf = pc->cpu.mem + cpu_linear(view_seg(pc), 0);
    if (chk_valid) {
        int n = 0;
        uint16_t first = 0;
        for (int i = 0; i < BUF_SIZE; i++) {
            if (buf[i] == chk_shadow[i])
                continue;
            if ((i & 0x1FFF) < VIEW_H / 2 * 80 && !chk_claimed[i]) { /* view rows only */
                if (!n++)
                    first = (uint16_t)i;
            }
            chk_claimed[i] = 0; /* a claim is used up by the write it announced; unwritten claims stay */
        }
        if (n) {
            struct ChkPair *p = NULL;
            for (int i = 0; i < chk_n_pairs && !p; i++)
                if (chk_pairs[i].a == chk_last && chk_pairs[i].b == name)
                    p = &chk_pairs[i];
            if (!p && chk_n_pairs < (int)(sizeof chk_pairs / sizeof *chk_pairs)) {
                p = &chk_pairs[chk_n_pairs++];
                p->a = chk_last, p->b = name, p->first = first;
            }
            if (p)
                p->times++, p->bytes += n;
        }
    }
    memcpy(chk_shadow, buf, BUF_SIZE);
    chk_valid = true;
    chk_last = name;
}

/* ---- projection staging ------------------------------------------------------------------- */

typedef struct Stage {
    bool valid;
    uint8_t x0, y0, x1, y1; /* the integer endpoints the original computed */
    DlPt p1, p2;
} Stage;

static Stage st;
static struct { bool valid; uint16_t sw; DlPt p; } prev_end; /* [3165] and its float */
static struct { bool valid; uint8_t x, y; DlPt p; } dot_st;
static DlPt p1_pending;

static float proj(Pc *pc, uint16_t v, uint16_t z, bool y_axis)
{
    int8_t scale = (int8_t)d8(pc, (uint16_t)(y_axis ? 0x3168 : 0x3167));
    uint8_t centre = d8(pc, (uint16_t)(y_axis ? 0x316A : 0x3169));
    return (float)centre + (float)scale * (float)(int16_t)v / (float)(int16_t)z;
}

/* The projected float, or the pixel centre when it does not belong to integer pixel n. */
static DlPt pt_for(float fx, float fy, uint8_t ix, uint8_t iy)
{
    DlPt p = { fx, fy };
    if (!(fabsf(floorf(fx) - (float)ix) <= 1.0f && fabsf(floorf(fy) - (float)iy) <= 1.0f))
        p.x = (float)ix + 0.5f, p.y = (float)iy + 0.5f;
    return p;
}

static DlPt eye_point(Pc *pc, uint16_t base, uint8_t ix, uint8_t iy)
{
    uint16_t z = d16(pc, (uint16_t)(base + 4));
    if ((int16_t)z <= 0)
        return pt_for(-1e9f, -1e9f, ix, iy);
    return pt_for(proj(pc, d16(pc, base), z, false), proj(pc, d16(pc, (uint16_t)(base + 2)), z, true), ix, iy);
}

void dl_stage_p1_(Pc *pc, bool from_prev, uint16_t sw)
{
    uint8_t ix = (uint8_t)sw, iy = (uint8_t)(sw >> 8);
    if (from_prev && prev_end.valid && prev_end.sw == sw)
        p1_pending = prev_end.p;
    else if (from_prev)
        p1_pending = pt_for(-1e9f, -1e9f, ix, iy);
    else
        p1_pending = eye_point(pc, 0x310F, ix, iy);
}

void dl_stage_p2_(Pc *pc, uint16_t sw1, uint16_t sw2)
{
    st.valid = true;
    st.x0 = (uint8_t)sw1, st.y0 = (uint8_t)(sw1 >> 8);
    st.x1 = (uint8_t)sw2, st.y1 = (uint8_t)(sw2 >> 8);
    st.p1 = p1_pending;
    st.p2 = eye_point(pc, 0x311B, st.x1, st.y1);
    prev_end.valid = true, prev_end.sw = sw2, prev_end.p = st.p2;
}

void dl_stage_dot_(Pc *pc, uint16_t x, uint16_t y, uint16_t z, uint8_t sx, uint8_t sy)
{
    dot_st.valid = false;
    if ((int16_t)z <= 0)
        return;
    dot_st.valid = true;
    dot_st.x = sx, dot_st.y = sy;
    dot_st.p = pt_for(proj(pc, x, z, false), proj(pc, y, z, true), sx, sy);
}

/* Capture shadow: the staged floats of the records written to a capture list, keyed by address. */
static struct Cap {
    bool valid;
    uint16_t addr;
    Stage s;
} cap[512];

void dl_capture_(Pc *pc, uint16_t addr)
{
    (void)pc;
    struct Cap *c = &cap[addr & 511];
    c->valid = st.valid;
    c->addr = addr;
    c->s = st;
}

void dl_stage_from_capture_(Pc *pc, uint16_t addr)
{
    struct Cap *c = &cap[addr & 511];
    if (c->valid && c->addr == addr && d8(pc, addr) == c->s.x0 && d8(pc, (uint16_t)(addr + 1)) == c->s.y0 &&
        d8(pc, (uint16_t)(addr + 2)) == c->s.x1 && d8(pc, (uint16_t)(addr + 3)) == c->s.y1) {
        st = c->s;
        st.valid = true;
    } else {
        st.valid = false;
    }
}

/* ---- lines and dots ----------------------------------------------------------------------- */

static void claim_box(Pc *pc, int xa, int ya, int xb, int yb)
{
    if (xa > xb)
        xa ^= xb, xb ^= xa, xa ^= xb;
    if (ya > yb)
        ya ^= yb, yb ^= ya, ya ^= yb;
    chk_claim_rows(pc, ya, yb, xa / 2, xb / 2);
}

void dl_line_(Pc *pc, int x0, int y0, int x1, int y1, uint16_t seg)
{
    if (seg != view_seg(pc))
        return;
    sync(pc, "draw_line");
    bool edge = d8(pc, POLY_MODE) != 0;
    uint8_t flags = (uint8_t)(edge ? DL_F_EDGE : 0);
    DlPt a, b;
    bool in_range = x0 >= 0 && x0 < 256 && y0 >= 0 && y0 < 256 && x1 >= 0 && x1 < 256 && y1 >= 0 && y1 < 256;
    if (in_range && st.valid && st.x0 == x0 && st.y0 == y0 && st.x1 == x1 && st.y1 == y1) {
        a = st.p1, b = st.p2;
    } else if (in_range && st.valid && st.x0 == x1 && st.y0 == y1 && st.x1 == x0 && st.y1 == y0) {
        a = st.p2, b = st.p1;
    } else {
        a.x = (float)x0 + 0.5f, a.y = (float)y0 + 0.5f;
        b.x = (float)x1 + 0.5f, b.y = (float)y1 + 0.5f;
        flags |= DL_F_INT;
    }
    st.valid = false;
    if (edge) /* the polygon edge routine also marks seed pixels beside the line */
        claim_box(pc, (x0 < x1 ? x0 : x1) - 2, (y0 < y1 ? y0 : y1) - 1, (x0 > x1 ? x0 : x1) + 2, (y0 > y1 ? y0 : y1) + 1);
    else
        claim_box(pc, x0, y0, x1, y1);
    DlItem *it = add(DL_LINE, flags, d16(pc, DRAW_COLOUR));
    if (it)
        it->u.line.a = a, it->u.line.b = b;
}

void dl_dot_(Pc *pc, int x, int y, uint16_t seg)
{
    if (seg != view_seg(pc))
        return;
    sync(pc, "plot_pixel");
    DlPt p = { (float)x + 0.5f, (float)y + 0.5f };
    uint8_t flags = DL_F_INT;
    if (dot_st.valid && dot_st.x == x && dot_st.y == y) {
        p = dot_st.p;
        flags = 0;
    }
    dot_st.valid = false;
    chk_claim_rows(pc, y, y, x / 2, x / 2);
    DlItem *it = add(DL_DOT, flags, d16(pc, DRAW_COLOUR));
    if (it)
        it->u.line.a = p;
}

static int sky_mark = -1; /* item count at the start of horizon_fill, -1 outside it */

/* ---- fills -------------------------------------------------------------------------------- */

void dl_fill_view_(Pc *pc, uint16_t pattern, uint16_t words)
{
    sync(pc, "clear_view_buffer");
    int rows = words / 20; /* [3800] words per bank, 40 words per row */
    if (rows > VIEW_H)
        rows = VIEW_H;
    chk_claim_rows(pc, 0, rows - 1, 0, 79);
    if (rows >= VIEW_H) { /* the whole view: nothing before it shows */
        DlFrame *f = cur();
        f->n_items = f->n_verts = 0;
    }
    add_fill(0, 0, VIEW_W, (float)rows, pattern);
}

void dl_fill_rows_(Pc *pc, int first, int last, uint16_t pattern, uint16_t seg)
{
    if (seg != view_seg(pc))
        return;
    sync(pc, "fill_rows");
    if (sky_mark >= 0)
        chk_claim_rows(pc, 0, VIEW_H - 1, 0, 79);
    if (last < first || first < 0)
        return;
    chk_claim_rows(pc, first, last, 0, 79);
    add_fill(0, (float)first, VIEW_W, (float)(last - first + 1), pattern);
}

void dl_fill_bytes_(Pc *pc, uint16_t seg, uint16_t off, int n, uint8_t value, uint8_t mask)
{
    if (seg != view_seg(pc))
        return;
    sync(pc, "fill_bytes");
    int vr = view_row_of(pc);
    while (n > 0) {
        int bank = off >= 0x2000, o = off & 0x1FFF;
        int y = (o / 80) * 2 + bank - vr, xb = o % 80;
        int chunk = n < 80 - xb ? n : 80 - xb;
        chk_claim_rows(pc, y, y, xb, xb + chunk - 1);
        if (y < 0 || y >= VIEW_H) {
            /* outside the view (the line pair below it): never shown */
        } else if (mask == 0xFF) {
            add_fill((float)(xb * 2), (float)y, (float)(chunk * 2), 1, (uint16_t)(value | value << 8));
        } else if (mask == 0xF0 || mask == 0x0F) {
            int hi = mask == 0xF0;
            uint16_t nib = (uint16_t)((hi ? value >> 4 : value & 15) * 0x1111);
            add_fill((float)(xb * 2 + !hi), (float)y, 1, 1, nib); /* first byte only: a partial-byte write is one pixel */
        }
        n -= chunk;
        off = (uint16_t)(off + chunk);
    }
}

/* ---- sky and ground ----------------------------------------------------------------------- */

void dl_sky_begin_(Pc *pc)
{
    sync(pc, "horizon_fill");
    sky_mark = cur()->n_items;
    chk_claim_rows(pc, 0, VIEW_H - 1, 0, 79); /* the sloped band is filled by an unhooked routine */
}

static uint16_t corner_pattern(Pc *pc, int x, int y)
{
    uint16_t off = (uint16_t)(row_off(pc, y) + (x < 80 ? 0 : 78));
    uint16_t seg = view_seg(pc);
    return (uint16_t)(m8(pc, seg, off) | m8(pc, seg, (uint16_t)(off + 1)) << 8);
}

void dl_sky_ground_(Pc *pc)
{
    int mark = sky_mark;
    sky_mark = -1;
    if (mark < 0 || mark > cur()->n_items)
        return;
    chk_claim_rows(pc, 0, VIEW_H - 1, 0, 79); /* the sloped band is filled by an unhooked routine */
    sync(pc, "horizon_fill end");
    if (d8(pc, HORIZON_LIST) == 0xFF)
        return; /* no horizon: the single fill of the whole view stays */
    DlPt a = { (float)d8(pc, HORIZON_LIST) + 0.5f, (float)d8(pc, HORIZON_LIST + 1) + 0.5f };
    DlPt b = { (float)d8(pc, HORIZON_LIST + 2) + 0.5f, (float)d8(pc, HORIZON_LIST + 3) + 0.5f };
    struct Cap *c = &cap[HORIZON_LIST & 511];
    if (c->valid && c->addr == HORIZON_LIST && c->s.x0 == d8(pc, HORIZON_LIST) && c->s.y0 == d8(pc, HORIZON_LIST + 1) &&
        c->s.x1 == d8(pc, HORIZON_LIST + 2) && c->s.y1 == d8(pc, HORIZON_LIST + 3))
        a = c->s.p1, b = c->s.p2;
    /* Pattern on each side: read back at the view corner farthest from the line on that side. */
    static const int cx[4] = { 0, VIEW_W - 1, 0, VIEW_W - 1 }, cy[4] = { 0, 0, VIEW_H - 1, VIEW_H - 1 };
    float smin = 1e30f, smax = -1e30f;
    int imin = 0, imax = 0;
    for (int i = 0; i < 4; i++) {
        float s = (b.x - a.x) * ((float)cy[i] + 0.5f - a.y) - (b.y - a.y) * ((float)cx[i] + 0.5f - a.x);
        if (s < smin)
            smin = s, imin = i;
        if (s > smax)
            smax = s, imax = i;
    }
    DlFrame *f = cur();
    f->n_items = mark;
    DlItem *it = add(DL_SKYGROUND, 0, 0);
    if (it) {
        it->u.sky.a = a, it->u.sky.b = b;
        it->u.sky.pat_l = corner_pattern(pc, cx[imin], cy[imin]); /* s < 0: left of a->b with y down */
        it->u.sky.pat_r = corner_pattern(pc, cx[imax], cy[imax]);
    }
}

/* ---- polygons ----------------------------------------------------------------------------- */

static uint8_t poly_snap[BUF_SIZE]; /* the view buffer at poly_begin */

/* Fat pixels of the view rows that differ from base, and the box of those pixels. */
static int diff_pixels(Pc *pc, const uint8_t *base, int *x0, int *y0, int *x1, int *y1)
{
    const uint8_t *buf = pc->cpu.mem + cpu_linear(view_seg(pc), 0);
    int n = 0;
    *x0 = VIEW_W, *y0 = VIEW_H, *x1 = *y1 = 0;
    for (int i = 0; i < BUF_SIZE; i++) {
        if ((i & 0x1FFF) >= VIEW_H / 2 * 80 || buf[i] == base[i])
            continue;
        int y = ((i & 0x1FFF) / 80) * 2 + (i >= 0x2000), xb = (i & 0x1FFF) % 80;
        int d = buf[i] ^ base[i];
        int xl = xb * 2 + ((d & 0xF0) ? 0 : 1), xr = xb * 2 + ((d & 0x0F) ? 1 : 0);
        n += ((d & 0xF0) ? 1 : 0) + ((d & 0x0F) ? 1 : 0);
        if (xl < *x0)
            *x0 = xl;
        if (xr > *x1)
            *x1 = xr;
        if (y < *y0)
            *y0 = y;
        if (y > *y1)
            *y1 = y;
    }
    return n;
}

static int poly_slot = -1, poly_edges_from;
static int span_fills;

void dl_poly_begin_(Pc *pc)
{
    sync(pc, "poly_begin");
    span_fills = 0;
    memcpy(poly_snap, pc->cpu.mem + cpu_linear(view_seg(pc), 0), BUF_SIZE);
    DlItem *it = add(DL_POLY, DL_F_EMPTY, 0);
    poly_slot = it ? cur()->n_items - 1 : -1;
    poly_edges_from = cur()->n_items;
}

void dl_span_fill_(Pc *pc)
{
    (void)pc;
    span_fills++;
}

static int border_side(DlPt p) /* 1 left, 2 right, 4 top, 8 bottom */
{
    /* the clip rectangle of the scenery is x 1..159, y 0..105 (column 0 holds the white frame) */
    return (p.x <= 1.5f ? 1 : 0) | (p.x >= VIEW_W - 1.5f ? 2 : 0) | (p.y <= 1.5f ? 4 : 0) | (p.y >= VIEW_H - 1.5f ? 8 : 0);
}

static bool near_pt(DlPt a, DlPt b) { return fabsf(a.x - b.x) < 1.0f && fabsf(a.y - b.y) < 1.0f; }

/* Ends the ring begun at index `from` of the pool: closes a clipped outline along the view border when its
 * two ends are on adjacent sides (one corner), drops degenerate rings. Returns false when no ring is left. */
static bool end_ring(DlFrame *f, int from)
{
    float a2 = 0; /* twice the signed area: a ring without area (an edge drawn back and forth) is dropped */
    for (int j = from; j < f->n_verts; j++) {
        DlPt p = f->verts[j], q = f->verts[j + 1 < f->n_verts ? j + 1 : from];
        a2 += p.x * q.y - q.x * p.y;
    }
    if (f->n_verts - from < 3 || fabsf(a2) < 1.0f) {
        f->n_verts = from;
        return false;
    }
    DlPt p0 = f->verts[from], pe = f->verts[f->n_verts - 1];
    int s0 = border_side(p0), se = border_side(pe);
    if (!near_pt(p0, pe) && s0 && se && !(s0 & se)) {
        int horiz = (s0 | se) & 3, vert_ = (s0 | se) & 12; /* one vertical side (left/right) and one horizontal */
        if (horiz && vert_ && (horiz == 1 || horiz == 2) && (vert_ == 4 || vert_ == 8)) {
            DlPt c = { horiz == 1 ? 0.0f : (float)VIEW_W, vert_ == 4 ? 0.0f : (float)VIEW_H };
            vert(c);
        }
    }
    return true;
}

void dl_poly_end_(Pc *pc)
{
    int slot = poly_slot;
    poly_slot = -1;
    DlFrame *f = cur();
    if (slot < 0 || slot >= f->n_items || f->items[slot].kind != DL_POLY)
        return;
    if (span_fills == 0) { /* the flood fill filled nothing (night, radar view, or a blocked seed) */
        if (chk_on) {
            int a, b, c, d, n = diff_pixels(pc, poly_snap, &a, &b, &c, &d);
            if (n > 0) {
                if (chk_unfilled_vis < 6)
                    {
                        chk_unf[chk_unfilled_vis].frame = f->number + 1, chk_unf[chk_unfilled_vis].pat = d16(pc, 0x31CE);
                        chk_unf[chk_unfilled_vis].col = d8(pc, COLOUR_BYTE), chk_unf[chk_unfilled_vis].px = n;
                    }
                chk_unfilled_vis++;
            }
        }
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; /* it may still have marked seed pixels by the outline */
        for (int i = poly_edges_from; i < f->n_items; i++) {
            const DlItem *e = &f->items[i];
            if (e->kind != DL_LINE || !(e->flags & DL_F_EDGE))
                continue;
            x0 = fminf(x0, fminf(e->u.line.a.x, e->u.line.b.x)), x1 = fmaxf(x1, fmaxf(e->u.line.a.x, e->u.line.b.x));
            y0 = fminf(y0, fminf(e->u.line.a.y, e->u.line.b.y)), y1 = fmaxf(y1, fmaxf(e->u.line.a.y, e->u.line.b.y));
        }
        if (x1 >= x0)
            claim_box(pc, (int)x0 - 2, (int)y0 - 1, (int)x1 + 2, (int)y1 + 1);
        sync(pc, "poly_end");
        return;
    }
    DlItem *poly = &f->items[slot];
    int first = f->n_verts, rings = 0;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    /* The edges of the outline, in drawing order but each possibly drawn from its far end: join them into
     * rings by matching end points (greedy: extend the tail until no unused edge touches it). */
    enum { MAX_EDGES = 512 };
    int idx[MAX_EDGES], n_edges = 0;
    bool used[MAX_EDGES] = { false };
    for (int i = poly_edges_from; i < f->n_items && n_edges < MAX_EDGES; i++) {
        const DlItem *e = &f->items[i];
        if (e->kind != DL_LINE || !(e->flags & DL_F_EDGE))
            continue;
        idx[n_edges++] = i;
        DlPt a = e->u.line.a, b = e->u.line.b;
        minx = fminf(minx, fminf(a.x, b.x)), maxx = fmaxf(maxx, fmaxf(a.x, b.x));
        miny = fminf(miny, fminf(a.y, b.y)), maxy = fmaxf(maxy, fmaxf(a.y, b.y));
    }
    /* Chains of touching edges (grown at both ends), then chains cut by the clip rectangle are joined along
     * the view border: the flood fill of the original is bounded by the border where the outline was clipped. */
    enum { MAX_CHAINS = 64, SCRATCH = 4096 };
    static DlPt scratch[SCRATCH];
    struct Chain { DlPt *p; int n; bool dead; } ch[MAX_CHAINS];
    int n_ch = 0, sc = 0;
    for (int s0 = 0; s0 < n_edges && n_ch < MAX_CHAINS; s0++) {
        if (used[s0])
            continue;
        used[s0] = true;
        DlPt chain[2 * MAX_EDGES + 2];
        int lo = MAX_EDGES, hi = MAX_EDGES + 1; /* the chain is chain[lo..hi], grown at both ends */
        chain[lo] = f->items[idx[s0]].u.line.a;
        chain[hi] = f->items[idx[s0]].u.line.b;
        for (bool grew = true; grew;) {
            grew = false;
            for (int k = 0; k < n_edges; k++) {
                if (used[k])
                    continue;
                DlPt a = f->items[idx[k]].u.line.a, b = f->items[idx[k]].u.line.b;
                if (near_pt(chain[hi], a))
                    chain[++hi] = b;
                else if (near_pt(chain[hi], b))
                    chain[++hi] = a;
                else if (near_pt(chain[lo], a))
                    chain[--lo] = b;
                else if (near_pt(chain[lo], b))
                    chain[--lo] = a;
                else
                    continue;
                used[k] = grew = true;
            }
        }
        int n = hi - lo + 1;
        if (sc + n > SCRATCH)
            break;
        memcpy(&scratch[sc], &chain[lo], (size_t)n * sizeof(DlPt));
        ch[n_ch++] = (struct Chain){ &scratch[sc], n, false };
        sc += n;
    }
    for (;;) { /* join the pair of chain ends that share a border side with the smallest gap */
        float best = 1e30f;
        int bx = -1, by = -1, bex = 0, bey = 0;
        for (int x = 0; x < n_ch; x++)
            for (int y = x + 1; y < n_ch; y++) {
                if (ch[x].dead || ch[y].dead)
                    continue;
                for (int ex = 0; ex < 2; ex++)
                    for (int ey = 0; ey < 2; ey++) {
                        DlPt px = ch[x].p[ex ? ch[x].n - 1 : 0], py = ch[y].p[ey ? ch[y].n - 1 : 0];
                        float gap = fabsf(px.x - py.x) + fabsf(px.y - py.y);
                        if ((border_side(px) & border_side(py)) && gap < best)
                            best = gap, bx = x, by = y, bex = ex, bey = ey;
                    }
            }
        if (bx < 0 || n_ch >= MAX_CHAINS || sc + ch[bx].n + ch[by].n > SCRATCH)
            break;
        DlPt *np = &scratch[sc];
        int m = 0;
        for (int i = 0; i < ch[bx].n; i++) /* X ending at the joined end */
            np[m++] = ch[bx].p[bex ? i : ch[bx].n - 1 - i];
        for (int i = 0; i < ch[by].n; i++) /* then Y starting at its joined end */
            np[m++] = ch[by].p[bey ? ch[by].n - 1 - i : i];
        sc += m;
        ch[bx].dead = ch[by].dead = true;
        ch[n_ch++] = (struct Chain){ np, m, false };
    }
    for (int c = 0; c < n_ch; c++) {
        if (ch[c].dead)
            continue;
        int ring_from = f->n_verts;
        for (int k = 0; k < ch[c].n; k++)
            vert(ch[c].p[k]);
        if (end_ring(f, ring_from)) {
            rings++;
            vert((DlPt){ DL_RING_BREAK_X, 0 });
        }
    }
    if (f->n_verts > first && isnan(f->verts[f->n_verts - 1].x))
        f->n_verts--; /* the trailing break */
    if (rings) {
        poly->colour = d8(pc, COLOUR_BYTE);
        poly->u.poly.first = (uint16_t)first;
        poly->u.poly.count = (uint16_t)(f->n_verts - first);
        poly->u.poly.rings = (uint16_t)rings;
    }
    float area = 0, perim = 0; /* of the rings (perimeter in fat pixels along the major axis) */
    for (int k = 0, from = 0; k <= poly->u.poly.count && rings; k++) {
        if (k == poly->u.poly.count || isnan(f->verts[first + k].x)) {
            float a2 = 0;
            for (int j = from; j < k; j++) {
                DlPt p = f->verts[first + j], q = f->verts[first + (j + 1 < k ? j + 1 : from)];
                a2 += p.x * q.y - q.x * p.y;
                perim += fabsf(p.x - q.x) > fabsf(p.y - q.y) ? fabsf(p.x - q.x) : fabsf(p.y - q.y);
            }
            area += fabsf(a2) * 0.5f;
            from = k + 1;
        }
    }
    /* Everything the outline and the flood fill changed since poly_begin: a fill that leaked out of (or was
     * not closed by) the outline cannot be reproduced from the vectors, so the item carries the box of the
     * changed pixels and the back end shows that box from the CGA picture (DL_F_LEAK). */
    int lx0, ly0, lx1, ly1;
    int total = diff_pixels(pc, poly_snap, &lx0, &ly0, &lx1, &ly1);
    bool leak = total > 2 * (area + perim) + 30;
    if (leak) {
        poly->flags |= DL_F_LEAK;
        poly->u.poly.x0 = (uint8_t)lx0, poly->u.poly.y0 = (uint8_t)ly0;
        poly->u.poly.x1 = (uint8_t)lx1, poly->u.poly.y1 = (uint8_t)ly1;
    }
    if (rings || leak)
        poly->flags &= (uint8_t)~DL_F_EMPTY;
    if (chk_on && chk_valid) {
        int changed = chk_changed_pixels(pc); /* the interior only: the outline lines were synced before */
        chk_poly_all++;
        if (leak || area > 2 * changed + perim + 30) { /* leaked / blocked */
            if (chk_poly_n < 8) {
                chk_poly[chk_poly_n].frame = f->number + 1, chk_poly[chk_poly_n].changed = changed;
                chk_poly[chk_poly_n].x0 = (int)minx, chk_poly[chk_poly_n].y0 = (int)miny;
                chk_poly[chk_poly_n].x1 = (int)maxx, chk_poly[chk_poly_n].y1 = (int)maxy;
                chk_poly[chk_poly_n++].area = area;
            }
            chk_poly_total++;
        }
    }
    /* The flood fill wrote inside the outline (one row/byte around it) or, for a leak, inside its box. */
    if (leak)
        claim_box(pc, lx0, ly0, lx1, ly1);
    claim_box(pc, (int)minx - 2, (int)miny - 1, (int)maxx + 2, (int)maxy + 1);
    sync(pc, "poly_end");
}

/* ---- frames ------------------------------------------------------------------------------- */

static const char *dump_path;
static FILE *dump;

void dl_set_dump(const char *path) { dump_path = path; }

void dl_set_check(void)
{
    chk_on = true;
    if (!chk_atexit_set) {
        chk_atexit_set = true;
        atexit(chk_report);
    }
}

static void dump_frame(const DlFrame *f)
{
    if (!dump_path)
        return;
    if (!dump && !(dump = fopen(dump_path, "w"))) {
        dump_path = NULL;
        return;
    }
    fprintf(dump, "frame %u view_row %d items %d verts %d%s\n", f->number, f->view_row, f->n_items, f->n_verts,
            f->incomplete ? " INCOMPLETE" : "");
    for (int i = 0; i < f->n_items; i++) {
        const DlItem *it = &f->items[i];
        switch (it->kind) {
        case DL_FILL:
            fprintf(dump, "fill %g %g %g %g pat=%04X\n", it->u.rect.x, it->u.rect.y, it->u.rect.w, it->u.rect.h, it->colour);
            break;
        case DL_SKYGROUND:
            fprintf(dump, "sky a=%.2f,%.2f b=%.2f,%.2f pat_l=%04X pat_r=%04X\n", it->u.sky.a.x, it->u.sky.a.y,
                    it->u.sky.b.x, it->u.sky.b.y, it->u.sky.pat_l, it->u.sky.pat_r);
            break;
        case DL_LINE:
            fprintf(dump, "line %.2f,%.2f %.2f,%.2f col=%04X%s%s%s\n", it->u.line.a.x, it->u.line.a.y, it->u.line.b.x,
                    it->u.line.b.y, it->colour, it->flags & DL_F_INT ? " int" : "", it->flags & DL_F_EDGE ? " edge" : "",
                    it->flags & DL_F_CAPTURED ? " cap" : "");
            break;
        case DL_DOT:
            fprintf(dump, "dot %.2f,%.2f col=%04X%s\n", it->u.line.a.x, it->u.line.a.y, it->colour,
                    it->flags & DL_F_INT ? " int" : "");
            break;
        case DL_POLY:
            fprintf(dump, "poly col=%02X rings=%d verts=%d%s", it->colour, it->u.poly.rings, it->u.poly.count,
                    it->flags & DL_F_EMPTY ? " empty" : "");
            if (it->flags & DL_F_LEAK)
                fprintf(dump, " leak=%d,%d-%d,%d", it->u.poly.x0, it->u.poly.y0, it->u.poly.x1, it->u.poly.y1);
            for (int k = 0; k < it->u.poly.count && !(it->flags & DL_F_EMPTY); k++) {
                DlPt v = f->verts[it->u.poly.first + k];
                if (isnan(v.x))
                    fprintf(dump, " |");
                else
                    fprintf(dump, " %.2f,%.2f", v.x, v.y);
            }
            fprintf(dump, "\n");
            break;
        default:
            fprintf(dump, "kind %d\n", it->kind);
        }
    }
    fflush(dump);
}

void dl_commit_view_(Pc *pc)
{
    sync(pc, "blit_view_to_screen");
    DlFrame *f = cur();
    f->view_row = view_row_of(pc);
    f->number = frames[building ^ 1].number + 1;
    dump_frame(f);
    uint16_t seg = view_seg(pc); /* the blit copies [3802] words at [3808] and [3804] words at [380A] */
    for (int i = 0; i < 2; i++) {
        uint16_t off = d16(pc, i ? 0x380A : 0x3808), len = (uint16_t)(2 * d16(pc, i ? 0x3804 : 0x3802));
        if (off >= BUF_SIZE)
            len = 0;
        else if (len > BUF_SIZE - off)
            len = (uint16_t)(BUF_SIZE - off);
        snap.off[i] = off, snap.len[i] = len;
        for (uint16_t k = 0; k < len; k++)
            snap.bytes[off + k] = m8(pc, seg, (uint16_t)(off + k));
    }
    memset(chk_claimed, 0, BUF_SIZE);
    building ^= 1;
    frames[building].n_items = frames[building].n_verts = 0;
    frames[building].incomplete = false;
    committed = true;
    st.valid = prev_end.valid = dot_st.valid = false;
    poly_slot = sky_mark = -1;
}
