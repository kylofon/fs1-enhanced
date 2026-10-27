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
/* through the current DS, as the transform natives address their data */
static int16_t s16(Pc *pc, uint16_t off)
{
    uint16_t ds = pc->cpu.sregs[S_DS];
    return (int16_t)(m8(pc, ds, off) | m8(pc, ds, (uint16_t)(off + 1)) << 8);
}

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
    it->va = it->vb = -1;
    return it;
}

static void add_fill(float x, float y, float w, float h, uint16_t pattern)
{
    DlItem *it = add(DL_FILL, 0, pattern);
    if (it)
        it->u.rect.x = x, it->u.rect.y = y, it->u.rect.w = w, it->u.rect.h = h;
}

static bool vert_to(DlFrame *f, DlPt p)
{
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
static int chk_kept;         /* polygons not filled by the game, closed with the last frame's winding */
static struct { uint32_t frame; uint16_t pat; uint8_t col; int px; } chk_unf[6];

static void sm_report(void);
static void sm_stats(void);
static void smooth_check(void);
static void chk_report(void)
{
    sm_stats();
    sm_report();
    if (!chk_n_pairs)
        fprintf(stderr, "dl-check: every back-buffer write was recorded\n");
    fprintf(stderr, "dl-check: %d polygons with a visible outline were not filled by the game (%d more were filled with "
                    "the last frame's winding)\n", chk_unfilled_vis, chk_kept);
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
    int16_t v1, v2; /* S3.2: their vertices (unclipped), -1 = none */
    uint8_t pj;
    uint32_t frame; /* build_no when staged: vertex indices are only valid in that frame */
} Stage;

static Stage st;
static struct { bool valid; uint16_t sw; DlPt p; } prev_end; /* [3165] and its float */
static struct { bool valid; uint8_t x, y; DlPt p; int16_t v; uint8_t pj; } dot_st;
static uint32_t build_no; /* commits so far = number of the frame being built */
static int16_t line_v[2] = { -1, -1 }; /* S3.2: vertices of 310F / 311B at clip_project_line's entry */
/* S3.3: a line whose ends both have vertices but that never reached the projection (dl_stage_p2) was rejected by
 * the game's clip as outside the view: kept as a hidden DL_F_OUT line for the world behind the cockpit edge. */
static struct { bool on; int16_t v[2]; uint16_t colour; bool edge; uint8_t pj; } rej;
static void rej_flush(void);
static uint8_t pj_index(Pc *pc);
static int16_t find_vtx(uint16_t x, uint16_t y, uint16_t z);
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
    rej.on = false; /* projected: drawn or captured */
    st.valid = true;
    st.x0 = (uint8_t)sw1, st.y0 = (uint8_t)(sw1 >> 8);
    st.x1 = (uint8_t)sw2, st.y1 = (uint8_t)(sw2 >> 8);
    st.p1 = p1_pending;
    st.p2 = eye_point(pc, 0x311B, st.x1, st.y1);
    st.v1 = line_v[0], st.v2 = line_v[1];
    line_v[0] = line_v[1] = -1;
    st.pj = pj_index(pc);
    st.frame = build_no;
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
    dot_st.v = find_vtx(x, y, z);
    dot_st.pj = pj_index(pc);
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
        if (st.frame != build_no)
            st.v1 = st.v2 = -1;
    } else {
        st.valid = false;
    }
}

/* ---- S3.2 vertices ------------------------------------------------------------------------ */

/* Every point the scenery puts into eye space becomes a vertex: its float eye vector (from the raw point, so
 * without the per-point scaling of world_to_eye_delta / rotate_point, which differs from frame to frame) and an
 * identity (scenery address) to find it again in the next frame. Lines and dots find their vertices by the eye
 * words: a point keeps the words rotate_point gave it while it is copied around (move start, cache slots,
 * previous end), and clip_project_line's entry still sees both ends unclipped. */

#define EYE_POS 0x30EB
#define VIEW_MATRIX 0x30FD
#define XFORM_IN 0x3139
#define FLAT_Y 0x315B
#define PROJ_PARAMS 0x3167
#define STEP_POINT 0x31B5
#define VIEW_DIR_FN 0x059B
#define RADAR_VIEW 0x0405

/* Open-addressing tables keyed by 64 bits; a new stamp empties one. */
enum { HBITS = 14, HSIZE = 1 << HBITS };
typedef struct HTab {
    struct { uint64_t k; int v; uint32_t stamp; } s[HSIZE];
    uint32_t stamp;
} HTab;
static HTab words_tab = { .stamp = 1 }, occ_tab = { .stamp = 1 }, key_tab = { .stamp = 1 };

static void h_clear(HTab *t)
{
    if (++t->stamp == 0) {
        memset(t->s, 0, sizeof t->s);
        t->stamp = 1;
    }
}

/* The value slot of key k (a new one is -1), or NULL (not found / table crowded). */
static int *h_slot(HTab *t, uint64_t k, bool insert)
{
    uint64_t h = (k * 0x9E3779B97F4A7C15ull) >> (64 - HBITS);
    for (int i = 0; i < 32; i++) {
        int j = (int)((h + (uint64_t)i) & (HSIZE - 1));
        if (t->s[j].stamp != t->stamp) {
            if (!insert)
                return NULL;
            t->s[j].stamp = t->stamp, t->s[j].k = k, t->s[j].v = -1;
            return &t->s[j].v;
        }
        if (t->s[j].k == k)
            return &t->s[j].v;
    }
    return NULL;
}

static uint64_t words_key(uint16_t x, uint16_t y, uint16_t z) { return x | (uint64_t)y << 16 | (uint64_t)z << 32; }

static int16_t find_vtx(uint16_t x, uint16_t y, uint16_t z)
{
    int *v = h_slot(&words_tab, words_key(x, y, z), false);
    return v ? (int16_t)*v : -1;
}

/* The rotated delta in the units of the clip and the projection: rotate_point's rows are x, 2y and z/2. */
static void rotate_f(Pc *pc, const float d[3], float e[3])
{
    float m[9];
    for (int i = 0; i < 9; i++)
        m[i] = (float)s16(pc, (uint16_t)(VIEW_MATRIX + 2 * i));
    e[0] = d[0] * m[0] + d[1] * m[3] + d[2] * m[6];
    e[1] = 2.0f * (d[0] * m[1] + d[1] * m[4] + d[2] * m[7]);
    e[2] = 0.5f * (d[0] * m[2] + d[1] * m[5] + d[2] * m[8]);
}

/* A new vertex of the building frame, found later by the eye words (x, y, z). base = identity without the
 * occurrence (0: none); the n-th use of the same base in a frame gets occurrence n. */
static void new_vtx(const float e[3], const float ef[3], uint64_t base, uint8_t flags, uint16_t x, uint16_t y, uint16_t z)
{
    DlFrame *f = cur();
    if (f->n_vtx >= DL_MAX_VTX)
        return;
    int i = f->n_vtx++;
    DlVtx *v = &f->vtx[i];
    memcpy(v->e, e, sizeof v->e);
    memcpy(v->e0, e, sizeof v->e0);
    v->key = 0;
    v->flags = flags;
    for (int k = 0; k < 3; k++)
        v->ef[k] = ef ? ef[k] : 0.0f;
    int *occ = base ? h_slot(&occ_tab, base, true) : NULL;
    if (occ)
        v->key = base | (uint64_t)(uint32_t)++*occ << 40;
    int *w = h_slot(&words_tab, words_key(x, y, z), true);
    if (w)
        *w = i; /* the latest point with these words */
}

static uint64_t point_base(uint16_t addr, unsigned step) { return 1ull << 63 | addr | (uint64_t)step << 16; }

static struct { bool valid; uint16_t w[3]; float d[3], fr[3]; uint64_t base; uint8_t flags; } pend; /* world_to_eye_delta -> rotate_point */

/* Op 24 sets eye_pos = (pos - origin) >> shift (0, 4, 8, 12 or 16 bits, by the scenery scale), truncated: in a
 * coarse local frame the eye stands still for several frames and then jumps a whole step, and the scenery in
 * it with it (in the original too). The vertices take the eye with its fraction instead, so far scenery moves
 * steadily. Valid while eye_pos still holds the words op 24 wrote. */
static struct { bool valid; uint16_t q[3]; float frac[3]; } origin;

void dl_origin_(Pc *pc)
{
    origin.valid = false;
    uint16_t di = (uint16_t)s16(pc, 0x30DD), shift;
    switch ((uint16_t)s16(pc, (uint16_t)(di + 0x316C))) { /* the handler scale_shift jumps to */
    case 0x4073: shift = 0; break;
    case 0x4063: shift = 4; break;
    case 0x4074: shift = 8; break;
    case 0x4079: shift = 12; break;
    case 0x4089: shift = 16; break;
    default: return;
    }
    if (!shift)
        return;
    for (int i = 0; i < 3; i++) {
        uint16_t p = (uint16_t)(0x30D1 + 4 * i), o = (uint16_t)(0x30DF + 4 * i);
        uint32_t v = ((uint32_t)(uint16_t)s16(pc, (uint16_t)(p + 2)) << 16 | (uint16_t)s16(pc, p)) -
                     ((uint32_t)(uint16_t)s16(pc, (uint16_t)(o + 2)) << 16 | (uint16_t)s16(pc, o));
        origin.q[i] = (uint16_t)s16(pc, (uint16_t)(EYE_POS + 2 * i));
        origin.frac[i] = (float)(v & ((1u << shift) - 1)) / (float)(1u << shift);
    }
    origin.valid = true;
}
static struct { bool valid; float sigma; uint16_t op; unsigned k; } steps;     /* dotted / dashed line */

void dl_eye_delta_(Pc *pc, uint16_t start, uint16_t end, uint16_t x, uint16_t y, uint16_t z)
{
    pend.valid = false;
    uint16_t n = (uint16_t)(end - start); /* x, z and y unless the flat y was used */
    if (n != 4 && n != 6)
        return;
    float p[3] = { s16(pc, start), n == 6 ? s16(pc, (uint16_t)(start + 2)) : s16(pc, FLAT_Y), s16(pc, (uint16_t)(end - 2)) };
    uint16_t eye[3];
    for (int i = 0; i < 3; i++)
        eye[i] = (uint16_t)s16(pc, (uint16_t)(EYE_POS + 2 * i));
    bool fine = origin.valid && eye[0] == origin.q[0] && eye[1] == origin.q[1] && eye[2] == origin.q[2];
    for (int i = 0; i < 3; i++) {
        pend.fr[i] = fine ? origin.frac[i] : 0.0f;
        pend.d[i] = p[i] - (float)(int16_t)eye[i] - pend.fr[i];
    }
    pend.flags = fine ? DL_V_ORIGIN : 0;
    pend.w[0] = x, pend.w[1] = y, pend.w[2] = z;
    pend.base = point_base(start, 0);
    pend.valid = true;
}

void dl_rotated_(Pc *pc)
{
    uint16_t in[3];
    for (int i = 0; i < 3; i++)
        in[i] = (uint16_t)s16(pc, (uint16_t)(XFORM_IN + 2 * i));
    float d[3], e[3];
    uint64_t base = 0;
    uint8_t flags = 0;
    if (pend.valid && pend.w[0] == in[0] && pend.w[1] == in[1] && pend.w[2] == in[2]) {
        memcpy(d, pend.d, sizeof d);
        base = pend.base, flags = pend.flags;
    } else { /* not from world_to_eye_delta (war planes): the delta itself, no identity */
        for (int i = 0; i < 3; i++)
            d[i] = (float)(int16_t)in[i];
    }
    float fr[3] = { 0, 0, 0 }, ef[3];
    if (flags & DL_V_ORIGIN)
        memcpy(fr, pend.fr, sizeof fr);
    pend.valid = false;
    rotate_f(pc, d, e);
    rotate_f(pc, fr, ef);
    const uint16_t *r = pc->cpu.regs;
    new_vtx(e, ef, base, flags, r[R_BX], r[R_CX], r[R_DX]);
}

/* The current step point (31B5..) of a dotted/dashed line: the words times the scale of its start point. */
static void step_vtx(Pc *pc)
{
    uint16_t w[3];
    float e[3];
    for (int i = 0; i < 3; i++) {
        w[i] = (uint16_t)s16(pc, (uint16_t)(STEP_POINT + 2 * i));
        e[i] = steps.sigma * (float)(int16_t)w[i];
    }
    new_vtx(e, NULL, point_base(steps.op, ++steps.k), 0, w[0], w[1], w[2]);
}

/* After the set-up: the steps start at the second end point (SI - 6), already scaled like the first. */
void dl_steps_begin_(Pc *pc, uint16_t si)
{
    steps.valid = false;
    uint16_t b = (uint16_t)(si - 6);
    float d[3], e[3], ew = 0, ww = 0;
    for (int i = 0; i < 3; i++)
        d[i] = (float)s16(pc, (uint16_t)(b + 2 * i)) - (float)s16(pc, (uint16_t)(EYE_POS + 2 * i));
    rotate_f(pc, d, e);
    for (int i = 0; i < 3; i++) {
        float w = (float)s16(pc, (uint16_t)(STEP_POINT + 2 * i));
        ew += e[i] * w, ww += w * w;
    }
    if (ww <= 0)
        return;
    steps.sigma = ew / ww;
    steps.op = (uint16_t)(si - 13);
    steps.k = 0;
    steps.valid = true;
    step_vtx(pc);
}

void dl_step_(Pc *pc)
{
    if (steps.valid)
        step_vtx(pc);
}

static void rej_flush(void)
{
    if (!rej.on)
        return;
    rej.on = false;
    DlFrame *f = cur();
    if (f->n_items >= DL_MAX_ITEMS - 512) /* headroom: a rejected line never makes the frame incomplete */
        return;
    DlItem *it = add(DL_LINE, (uint8_t)(DL_F_OUT | DL_F_HIDDEN | (rej.edge ? DL_F_EDGE : 0)), rej.colour);
    if (it)
        it->va = rej.v[0], it->vb = rej.v[1], it->pj = rej.pj;
}

#define CAPTURE_MODE 0x30CA

void dl_line_begin_(Pc *pc)
{
    rej_flush();
    line_v[0] = find_vtx((uint16_t)s16(pc, 0x310F), (uint16_t)s16(pc, 0x3111), (uint16_t)s16(pc, 0x3113));
    line_v[1] = find_vtx((uint16_t)s16(pc, 0x311B), (uint16_t)s16(pc, 0x311D), (uint16_t)s16(pc, 0x311F));
    if (line_v[0] >= 0 && line_v[1] >= 0 && d8(pc, CAPTURE_MODE) == 0) {
        rej.on = true;
        rej.v[0] = line_v[0], rej.v[1] = line_v[1];
        rej.colour = d16(pc, DRAW_COLOUR), rej.edge = d8(pc, POLY_MODE) != 0, rej.pj = pj_index(pc);
    }
}

/* The projection parameters in force, as an index into the frame's sets. */
static uint8_t pj_index(Pc *pc)
{
    DlFrame *f = cur();
    float p[4] = { (float)(int8_t)d8(pc, PROJ_PARAMS), (float)(int8_t)d8(pc, PROJ_PARAMS + 1), (float)d8(pc, PROJ_PARAMS + 2),
                   (float)d8(pc, PROJ_PARAMS + 3) };
    for (int i = 0; i < f->n_pj; i++)
        if (!memcmp(f->pj[i], p, sizeof p))
            return (uint8_t)i;
    if (f->n_pj >= DL_MAX_PJ)
        return DL_MAX_PJ; /* none: not re-projected */
    memcpy(f->pj[f->n_pj], p, sizeof p);
    return (uint8_t)f->n_pj++;
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
    int16_t va = -1, vb = -1;
    if (in_range && st.valid && st.x0 == x0 && st.y0 == y0 && st.x1 == x1 && st.y1 == y1) {
        a = st.p1, b = st.p2;
        va = st.v1, vb = st.v2;
    } else if (in_range && st.valid && st.x0 == x1 && st.y0 == y1 && st.x1 == x0 && st.y1 == y0) {
        a = st.p1, b = st.p2; /* drawn from its far end: kept in the scenery's direction (outlines run one way) */
        va = st.v1, vb = st.v2;
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
    if (it) {
        it->u.line.a = a, it->u.line.b = b;
        if (va >= 0 && vb >= 0)
            it->va = va, it->vb = vb, it->pj = st.pj;
    }
}

void dl_dot_(Pc *pc, int x, int y, uint16_t seg)
{
    if (seg != view_seg(pc))
        return;
    sync(pc, "plot_pixel");
    DlPt p = { (float)x + 0.5f, (float)y + 0.5f };
    uint8_t flags = DL_F_INT;
    int16_t v = -1;
    if (dot_st.valid && dot_st.x == x && dot_st.y == y) {
        p = dot_st.p;
        flags = 0;
        v = dot_st.v;
    }
    dot_st.valid = false;
    chk_claim_rows(pc, y, y, x / 2, x / 2);
    DlItem *it = add(DL_DOT, flags, d16(pc, DRAW_COLOUR));
    if (it) {
        it->u.line.a = p;
        if (v >= 0)
            it->va = v, it->pj = dot_st.pj;
    }
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
    int16_t va = -1, vb = -1;
    if (c->valid && c->addr == HORIZON_LIST && c->s.x0 == d8(pc, HORIZON_LIST) && c->s.y0 == d8(pc, HORIZON_LIST + 1) &&
        c->s.x1 == d8(pc, HORIZON_LIST + 2) && c->s.y1 == d8(pc, HORIZON_LIST + 3)) {
        a = c->s.p1, b = c->s.p2;
        if (c->s.frame == build_no && c->s.v1 >= 0 && c->s.v2 >= 0)
            va = c->s.v1, vb = c->s.v2;
    }
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
        if (va >= 0)
            it->va = va, it->vb = vb, it->pj = c->s.pj;
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

enum { MAX_EDGES = 512 };
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
            vert_to(f, c);
        }
    }
    return true;
}

/* Joins outline segments into rings appended to f->verts (rings separated by a NaN vertex, no trailing one); the
 * segments are in drawing order, each possibly drawn from its far end. Returns the number of rings. */
static int build_rings(DlFrame *f, DlPt (*seg)[2], int n_edges)
{
    int first = f->n_verts, rings = 0;
    bool used[MAX_EDGES] = { false };
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
        chain[lo] = seg[s0][0];
        chain[hi] = seg[s0][1];
        for (bool grew = true; grew;) {
            grew = false;
            for (int k = 0; k < n_edges; k++) {
                if (used[k])
                    continue;
                DlPt a = seg[k][0], b = seg[k][1];
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
            vert_to(f, ch[c].p[k]);
        if (end_ring(f, ring_from)) {
            rings++;
            vert_to(f, (DlPt){ DL_RING_BREAK_X, 0 });
        }
    }
    if (f->n_verts > first && isnan(f->verts[f->n_verts - 1].x))
        f->n_verts--; /* the trailing break */
    return rings;
}

/* Position of a border point along the view border, clockwise on the screen from the top left corner (the
 * scenery's clip rectangle is x 1..159, y 1..105; the corners are taken at the view's own corners). */
#define PERIM (2.0f * (VIEW_W + VIEW_H))
static float border_t(DlPt p)
{
    float d[4] = { fabsf(p.y - 1), fabsf(p.x - (VIEW_W - 1)), fabsf(p.y - (VIEW_H - 1)), fabsf(p.x - 1) };
    int k = 0;
    for (int i = 1; i < 4; i++)
        if (d[i] < d[k])
            k = i;
    float x = fminf(fmaxf(p.x, 0), VIEW_W), y = fminf(fmaxf(p.y, 0), VIEW_H);
    switch (k) {
    case 0: return x;                                   /* top, left to right */
    case 1: return VIEW_W + y;                          /* right, downwards */
    case 2: return VIEW_W + VIEW_H + (VIEW_W - x);      /* bottom, right to left */
    default: return 2 * VIEW_W + VIEW_H + (VIEW_H - y); /* left, upwards */
    }
}

/* Directed outline segments (the scenery's order) into rings: chains joined end to start; a chain cut by the
 * clip rectangle is continued along the view border in direction wind (+1 clockwise on the screen, -1 the other
 * way) to the next chain's start, as the flood fill fills the polygon's side up to the border. Returns the
 * number of rings, or -1 when a chain does not end on the border (then build_rings is used). */
static int stitch_rings(DlFrame *f, DlPt (*seg)[2], int n_edges, int wind)
{
    enum { MAX_CHAINS = 64, SCRATCH = 4096 };
    static DlPt scratch[SCRATCH];
    struct Chain { DlPt *p; int n; bool open, used; float t_in, t_out; } ch[MAX_CHAINS];
    bool used[MAX_EDGES] = { false };
    int n_ch = 0, sc = 0, first = f->n_verts, rings = 0;
    for (int s0 = 0; s0 < n_edges; s0++) {
        if (used[s0])
            continue;
        if (n_ch >= MAX_CHAINS)
            return -1;
        used[s0] = true;
        DlPt chain[2 * MAX_EDGES + 2];
        int lo = MAX_EDGES, hi = MAX_EDGES + 1;
        chain[lo] = seg[s0][0], chain[hi] = seg[s0][1];
        for (bool grew = true; grew;) {
            grew = false;
            for (int k = 0; k < n_edges; k++) {
                /* closed: ends meet after at least three edges (an edge clipped to a stub under a pixel long at
                 * the view border is not a ring) */
                if (used[k] || (hi - lo >= 3 && near_pt(chain[hi], chain[lo])))
                    continue;
                if (near_pt(chain[hi], seg[k][0]))
                    chain[++hi] = seg[k][1];
                else if (near_pt(chain[lo], seg[k][1]))
                    chain[--lo] = seg[k][0];
                else
                    continue;
                used[k] = grew = true;
            }
        }
        int n = hi - lo + 1;
        if (sc + n > SCRATCH)
            return -1;
        memcpy(&scratch[sc], &chain[lo], (size_t)n * sizeof(DlPt));
        struct Chain *c = &ch[n_ch++];
        *c = (struct Chain){ &scratch[sc], n, !(n >= 4 && near_pt(chain[lo], chain[hi])), false, 0, 0 };
        sc += n;
        if (c->open) {
            if (!border_side(c->p[0]) || !border_side(c->p[n - 1]))
                return -1;
            c->t_in = border_t(c->p[0]), c->t_out = border_t(c->p[n - 1]);
        }
    }
    static const DlPt corner[4] = { { 0, 0 }, { VIEW_W, 0 }, { VIEW_W, VIEW_H }, { 0, VIEW_H } };
    static const float corner_t[4] = { 0, VIEW_W, VIEW_W + VIEW_H, 2 * VIEW_W + VIEW_H };
    for (int c0 = 0; c0 < n_ch; c0++) {
        if (ch[c0].used)
            continue;
        int from = f->n_verts;
        if (!ch[c0].open) { /* a whole ring */
            ch[c0].used = true;
            for (int k = 0; k + 1 < ch[c0].n; k++)
                vert_to(f, ch[c0].p[k]);
        } else {
            for (int c = c0, guard = 0; guard <= n_ch; guard++) {
                ch[c].used = true;
                for (int k = 0; k < ch[c].n; k++)
                    vert_to(f, ch[c].p[k]);
                float te = ch[c].t_out, best = 1e30f; /* along the border to the nearest chain start */
                int next = -1;
                for (int j = 0; j < n_ch; j++) {
                    if (!ch[j].open || (ch[j].used && j != c0))
                        continue;
                    float d = wind > 0 ? ch[j].t_in - te : te - ch[j].t_in;
                    d = fmodf(d + 2 * PERIM, PERIM);
                    if (d < best)
                        best = d, next = j;
                }
                { /* the corners passed on the way, in order */
                    float ds[4];
                    int ord[4];
                    for (int i = 0; i < 4; i++) {
                        float d = wind > 0 ? corner_t[i] - te : te - corner_t[i];
                        ds[i] = fmodf(d + 2 * PERIM, PERIM), ord[i] = i;
                    }
                    for (int i = 0; i < 4; i++)
                        for (int j = i + 1; j < 4; j++)
                            if (ds[ord[j]] < ds[ord[i]]) {
                                int t = ord[i];
                                ord[i] = ord[j], ord[j] = t;
                            }
                    for (int i = 0; i < 4; i++)
                        if (ds[ord[i]] > 0 && ds[ord[i]] < best)
                            vert_to(f, corner[ord[i]]);
                }
                if (next < 0 || next == c0)
                    break;
                c = next;
            }
        }
        float a2 = 0;
        for (int j = from; j < f->n_verts; j++) {
            DlPt p = f->verts[j], q = f->verts[j + 1 < f->n_verts ? j + 1 : from];
            a2 += p.x * q.y - q.x * p.y;
        }
        if (f->n_verts - from < 3 || fabsf(a2) < 1.0f) {
            f->n_verts = from;
            continue;
        }
        rings++;
        vert_to(f, (DlPt){ DL_RING_BREAK_X, 0 });
    }
    if (f->n_verts > first && isnan(f->verts[f->n_verts - 1].x))
        f->n_verts--;
    return rings;
}

/* How badly the rings at verts[first..first+count) cover what the outline and the flood fill changed (against
 * poly_snap): their area that did not change plus the changed pixels outside them. */
static void rings_size(const DlFrame *f, int first, int count, float *area, float *perim);
static float fill_mismatch(Pc *pc, const DlFrame *f, int first, int count)
{
    const uint8_t *buf = pc->cpu.mem + cpu_linear(view_seg(pc), 0);
    const DlPt *v = f->verts + first;
    int score = 0, n_in = 0;
    for (int y = 0; y < VIEW_H; y++) {
        int base = row_off(pc, y);
        for (int x = 0; x < VIEW_W; x++) {
            int o = base + x / 2;
            if (o >= BUF_SIZE || !((buf[o] ^ poly_snap[o]) & (x & 1 ? 0x0F : 0xF0)))
                continue;
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            bool in = false; /* even-odd over all rings */
            for (int k = 0, from = 0; k <= count; k++) {
                if (k < count && !isnan(v[k].x))
                    continue;
                for (int i = from, j = k - 1; i < k; j = i++)
                    if ((v[i].y > py) != (v[j].y > py) && px < v[j].x + (v[i].x - v[j].x) * (py - v[j].y) / (v[i].y - v[j].y))
                        in = !in;
                from = k + 1;
            }
            score += in ? 1 : -1;
            n_in += in;
        }
    }
    float area, perim;
    rings_size(f, first, count, &area, &perim);
    return area - (float)n_in + (float)(n_in - score); /* n_in - score = the changed pixels outside */
}

/* Area and perimeter (in fat pixels along the major axis) of the rings at verts[first..first+count). */
static void rings_size(const DlFrame *f, int first, int count, float *area, float *perim)
{
    *area = *perim = 0;
    for (int k = 0, from = 0; k <= count; k++) {
        if (k == count || isnan(f->verts[first + k].x)) {
            float a2 = 0;
            for (int j = from; j < k; j++) {
                DlPt p = f->verts[first + j], q = f->verts[first + (j + 1 < k ? j + 1 : from)];
                a2 += p.x * q.y - q.x * p.y;
                *perim += fabsf(p.x - q.x) > fabsf(p.y - q.y) ? fabsf(p.x - q.x) : fabsf(p.y - q.y);
            }
            *area += fabsf(a2) * 0.5f;
            from = k + 1;
        }
    }
}

/* The winding of the same polygon (an outline vertex with the same scenery key) in the last committed frame, 0 when
 * it was not filled there. */
static int8_t prev_wind(const DlFrame *f, int e_from, int e_to)
{
    uint64_t key = 0;
    for (int i = e_from; i < e_to && !key; i++)
        if (f->items[i].kind == DL_LINE && (f->items[i].flags & DL_F_EDGE) && f->items[i].va >= 0)
            key = f->vtx[f->items[i].va].key;
    const DlFrame *p = &frames[building ^ 1];
    if (!key || !committed)
        return 0;
    for (int i = 0; i < p->n_items; i++) {
        const DlItem *it = &p->items[i];
        if (it->kind != DL_POLY || !it->u.poly.wind)
            continue;
        for (int j = it->u.poly.e_from; j < it->u.poly.e_to && j < p->n_items; j++)
            if (p->items[j].kind == DL_LINE && (p->items[j].flags & DL_F_EDGE) && p->items[j].va >= 0 &&
                p->items[j].va < p->n_vtx && p->vtx[p->items[j].va].key == key)
                return it->u.poly.wind;
    }
    return 0;
}

void dl_poly_end_(Pc *pc)
{
    rej_flush();
    int slot = poly_slot;
    poly_slot = -1;
    DlFrame *f = cur();
    if (slot < 0 || slot >= f->n_items || f->items[slot].kind != DL_POLY)
        return;
    f->items[slot].u.poly.e_from = (uint16_t)poly_edges_from, f->items[slot].u.poly.e_to = (uint16_t)f->n_items;
    f->items[slot].colour = d8(pc, COLOUR_BYTE); /* S3.3: also for an empty one (its outline may be all outside) */
    /* The flood fill filled nothing (night, radar view, or a blocked seed). A seed outside the polygon's part on
     * the screen blinks it out for a frame: one filled in the last frame is closed with that frame's winding. */
    int8_t keep_wind = span_fills == 0 ? prev_wind(f, poly_edges_from, f->n_items) : 0;
    chk_kept += chk_on && keep_wind;
    if (span_fills == 0 && !keep_wind) {
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
            if (e->kind != DL_LINE || !(e->flags & DL_F_EDGE) || (e->flags & DL_F_OUT))
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
    int first = f->n_verts;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    static DlPt seg[MAX_EDGES][2];
    int n_edges = 0;
    for (int i = poly_edges_from; i < f->n_items && n_edges < MAX_EDGES; i++) {
        const DlItem *e = &f->items[i];
        if (e->kind != DL_LINE || !(e->flags & DL_F_EDGE) || (e->flags & DL_F_OUT))
            continue;
        DlPt a = e->u.line.a, b = e->u.line.b;
        seg[n_edges][0] = a, seg[n_edges++][1] = b;
        minx = fminf(minx, fminf(a.x, b.x)), maxx = fmaxf(maxx, fmaxf(a.x, b.x));
        miny = fminf(miny, fminf(a.y, b.y)), maxy = fmaxf(maxy, fmaxf(a.y, b.y));
    }
    int lx0, ly0, lx1, ly1;
    int total = diff_pixels(pc, poly_snap, &lx0, &ly0, &lx1, &ly1); /* what the outline and the flood fill changed */
    /* Outlines in the scenery's direction (every edge staged) are closed along the border on the side the game
     * filled: both windings are tried, the one whose area matches the changed pixels is kept (and stored for the
     * re-projection between frames). */
    bool directed = n_edges > 0;
    for (int i = poly_edges_from; i < f->n_items && directed; i++)
        if (f->items[i].kind == DL_LINE && (f->items[i].flags & DL_F_EDGE) && (f->items[i].flags & DL_F_INT))
            directed = false;
    int rings = -1;
    poly->u.poly.wind = 0;
    if (directed && keep_wind) {
        poly->u.poly.wind = keep_wind;
        rings = stitch_rings(f, seg, n_edges, keep_wind);
    } else if (directed) {
        float bad[2];
        for (int w = 0; w < 2; w++) {
            int r = stitch_rings(f, seg, n_edges, w ? -1 : 1);
            bad[w] = r < 0 ? 1e30f : fill_mismatch(pc, f, first, f->n_verts - first);
            f->n_verts = first;
        }
        if (bad[0] < 1e30f || bad[1] < 1e30f) {
            poly->u.poly.wind = (int8_t)(bad[1] < bad[0] ? -1 : 1);
            rings = stitch_rings(f, seg, n_edges, poly->u.poly.wind);
        }
    }
    if (rings < 0)
        rings = keep_wind ? 0 : build_rings(f, seg, n_edges);
    if (rings) {
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
    for (int k = 0; k < poly->u.poly.count && rings; k++) { /* a ring closed along the border reaches beyond its edges */
        DlPt v = f->verts[first + k];
        if (!isnan(v.x))
            minx = fminf(minx, v.x), maxx = fmaxf(maxx, v.x), miny = fminf(miny, v.y), maxy = fmaxf(maxy, v.y);
    }
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
    fprintf(dump, "frame %u view_row %d items %d verts %d vtx %d matched %d t %llu dt %llu%s\n", f->number, f->view_row,
            f->n_items, f->n_verts, f->n_vtx, f->matched, (unsigned long long)f->t, (unsigned long long)f->dt,
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
            if (it->flags & DL_F_OUT) {
                fprintf(dump, "out v=%d,%d col=%04X%s\n", it->va, it->vb, it->colour, it->flags & DL_F_EDGE ? " edge" : "");
                break;
            }
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

/* S3.2: each vertex of the frame just built looks for itself in the previous committed frame (same identity,
 * not implausibly far: a different point that took the identity over) and takes its position there as e0. */
static uint64_t prev_t;
static uint32_t prev_view;
static long sm_frames, sm_vtx, sm_matched;
static double sm_dt;

static long jk_n[2], jk_bad[2]; /* [in an op 24 frame]: vertices with three positions, of them with a jerk */

static void jerk_count(const float *e2, const float *e1, const float *e0, uint8_t flags)
{
    if (e2[2] <= 0 || e1[2] <= 0 || e0[2] <= 0 || fabsf(e0[0]) > e0[2] || fabsf(e0[1]) > e0[2])
        return;
    int o = (flags & DL_V_ORIGIN) != 0;
    float dx = 79.0f * (e0[0] / e0[2] - 2 * e1[0] / e1[2] + e2[0] / e2[2]);
    float dy = 52.0f * (e0[1] / e0[2] - 2 * e1[1] / e1[2] + e2[1] / e2[2]);
    jk_n[o]++;
    if (fabsf(dx) + fabsf(dy) > 2.0f)
        jk_bad[o]++;
}

static void match_vertices(Pc *pc, DlFrame *f)
{
    uint32_t view = d16(pc, VIEW_DIR_FN) | (uint32_t)d8(pc, RADAR_VIEW) << 16;
    f->t = pc->cpu.cycles;
    f->dt = committed && f->t > prev_t && f->t - prev_t < (uint64_t)(1.5 * PC_CPU_HZ) && view == prev_view ? f->t - prev_t : 0;
    f->matched = 0;
    const DlFrame *p = &frames[building ^ 1];
    for (int i = 0; i < f->n_vtx && f->dt; i++) {
        DlVtx *v = &f->vtx[i];
        int *j = v->key ? h_slot(&key_tab, v->key, false) : NULL;
        if (!j || *j < 0 || *j >= p->n_vtx)
            continue;
        const float *a = p->vtx[*j].e, *b = v->e;
        float dd = 0, na = 0, nb = 0;
        for (int k = 0; k < 3; k++)
            dd += (a[k] - b[k]) * (a[k] - b[k]), na += a[k] * a[k], nb += b[k] * b[k];
        if (dd > (na > nb ? na : nb))
            continue;
        memcpy(v->e0, a, sizeof v->e0);
        v->flags |= DL_V_MATCHED;
        f->matched++;
        const DlVtx *pv = &p->vtx[*j];
        if (chk_on && (pv->flags & DL_V_MATCHED)) /* screen motion of the last two intervals: a jump shows as a change */
            jerk_count(pv->e0, pv->e, v->e, v->flags);
    }
    h_clear(&key_tab);
    for (int i = 0; i < f->n_vtx; i++) {
        int *j = f->vtx[i].key ? h_slot(&key_tab, f->vtx[i].key, true) : NULL;
        if (j)
            *j = i;
    }
    if (f->dt)
        sm_frames++, sm_dt += (double)f->dt, sm_vtx += f->n_vtx, sm_matched += f->matched;
    prev_t = f->t, prev_view = view;
    h_clear(&words_tab), h_clear(&occ_tab);
    pend.valid = steps.valid = false;
    line_v[0] = line_v[1] = -1;
}

/* S3.2: a horizon drawn through other far points than in the last frame starts from the last frame's line. */
static void sky_from_prev(DlFrame *f, const DlFrame *p)
{
    DlItem *it = NULL;
    const DlItem *o = NULL;
    for (int i = 0; i < f->n_items && !it; i++)
        if (f->items[i].kind == DL_SKYGROUND)
            it = &f->items[i];
    for (int i = 0; committed && i < p->n_items && !o; i++)
        if (p->items[i].kind == DL_SKYGROUND)
            o = &p->items[i];
    if (!it || !o)
        return;
    it->u.sky.lerp = false;
    if (it->va >= 0 && it->vb >= 0 && (f->vtx[it->va].flags & f->vtx[it->vb].flags & DL_V_MATCHED))
        return;
    if (o->u.sky.pat_l == it->u.sky.pat_l && o->u.sky.pat_r == it->u.sky.pat_r)
        it->u.sky.a0 = o->u.sky.a, it->u.sky.b0 = o->u.sky.b;
    else if (o->u.sky.pat_l == it->u.sky.pat_r && o->u.sky.pat_r == it->u.sky.pat_l)
        it->u.sky.a0 = o->u.sky.b, it->u.sky.b0 = o->u.sky.a; /* drawn the other way round */
    else
        return;
    it->u.sky.lerp = true;
}

void dl_commit_view_(Pc *pc)
{
    rej_flush();
    sync(pc, "blit_view_to_screen");
    DlFrame *f = cur();
    f->view_row = view_row_of(pc);
    f->number = frames[building ^ 1].number + 1;
    match_vertices(pc, f);
    sky_from_prev(f, &frames[building ^ 1]);
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
    frames[building].n_items = frames[building].n_verts = frames[building].n_vtx = frames[building].n_pj = 0;
    frames[building].incomplete = false;
    build_no++;
    committed = true;
    if (chk_on)
        smooth_check();
    st.valid = prev_end.valid = dot_st.valid = rej.on = false;
    poly_slot = sky_mark = -1;
}

static void sm_stats(void)
{
    if (sm_frames)
        fprintf(stderr, "smooth: %ld frames, %.0f ms apart on average, %.1f%% of %ld vertices found in the previous frame\n",
                sm_frames, sm_dt / (double)sm_frames / PC_CPU_HZ * 1000.0, 100.0 * (double)sm_matched / (double)sm_vtx, sm_vtx);
    for (int o = 0; o < 2; o++)
        if (jk_n[o])
            fprintf(stderr, "smooth: %s: %ld of %ld visible points change their screen motion by more than 2 px from one interval to the next\n",
                    o ? "in coarse local frames (op 24)" : "elsewhere", jk_bad[o], jk_n[o]);
}

/* ---- S3.2 the view between two simulation frames --------------------------------------------- */

static int smooth_mode = DL_SMOOTH_ON;
void dl_set_smooth(int mode) { smooth_mode = mode; }

static DlFrame lerp_f;
static float lerp_v[DL_MAX_VTX][3];

/* The part t0..t1 of p->q inside the view pyramid |x| <= z, |y| <= z (the game's clip planes). */
static bool clip_pyramid(const float *p, const float *q, float *t0, float *t1)
{
    static const float pl[4][3] = { { -1, 0, 1 }, { 1, 0, 1 }, { 0, -1, 1 }, { 0, 1, 1 } };
    float a = 0, b = 1;
    for (int i = 0; i < 4; i++) {
        float fp = pl[i][0] * p[0] + pl[i][1] * p[1] + pl[i][2] * p[2];
        float fq = pl[i][0] * q[0] + pl[i][1] * q[1] + pl[i][2] * q[2];
        if (fp < 0 && fq < 0)
            return false;
        if (fp < 0)
            a = fmaxf(a, fp / (fp - fq));
        else if (fq < 0)
            b = fminf(b, fp / (fp - fq));
    }
    *t0 = a, *t1 = b;
    return a < b;
}

static void at(const float *p, const float *q, float t, float *r)
{
    for (int k = 0; k < 3; k++)
        r[k] = p[k] + (q[k] - p[k]) * t;
}

/* The screen point of eye vector e (z > 0), like the recorder's proj(). */
static bool project_e(const DlFrame *f, int pj, const float *e, DlPt *out)
{
    if (pj >= f->n_pj || !(e[2] > 1e-6f * (fabsf(e[0]) + fabsf(e[1]) + e[2])))
        return false;
    const float *p = f->pj[pj];
    out->x = p[2] + p[0] * e[0] / e[2];
    out->y = p[3] + p[1] * e[1] / e[2];
    return true;
}

/* The planes (n . e >= 0, for e[2] > 0) of the screen rectangle x0..x1, y0..y1 under projection set p. */
static void rect_planes(const float *p, float x0, float x1, float y0, float y1, float pl[4][3])
{
    const float r[4][3] = { { p[0], 0, p[2] - x0 }, { -p[0], 0, x1 - p[2] }, { 0, p[1], p[3] - y0 }, { 0, -p[1], y1 - p[3] } };
    memcpy(pl, r, sizeof r);
}

static float plane_d(const float *n, const float *e) { return n[0] * e[0] + n[1] * e[1] + n[2] * e[2]; }

static bool clip_planes(const float pl[4][3], const float *p, const float *q, float *t0, float *t1)
{
    float a = 0, b = 1;
    for (int i = 0; i < 4; i++) {
        float fp = plane_d(pl[i], p), fq = plane_d(pl[i], q);
        if (fp < 0 && fq < 0)
            return false;
        if (fp < 0)
            a = fmaxf(a, fp / (fp - fq));
        else if (fq < 0)
            b = fminf(b, fp / (fp - fq));
    }
    *t0 = a, *t1 = b;
    return a < b;
}

static bool game_eye; /* --dl-check: the game's own (truncated) eye, to compare with the recorded floats */

/* A line clipped to the whole view rectangle (0..VIEW_W, 0..VIEW_H), not to the game's clip (x 1..159, y 0..105),
 * so lines reach the screen edge like the fills; --dl-check compares with the game's clip. */
static bool line_at(const DlFrame *f, DlItem *it)
{
    const float *p = lerp_v[it->va], *q = lerp_v[it->vb];
    float t0, t1, a[3], b[3], pl[4][3];
    if (it->pj >= f->n_pj)
        return false;
    rect_planes(f->pj[it->pj], 0, VIEW_W, 0, VIEW_H, pl);
    if (!(game_eye ? clip_pyramid(p, q, &t0, &t1) : clip_planes(pl, p, q, &t0, &t1)))
        return false;
    at(p, q, t0, a), at(p, q, t1, b);
    return project_e(f, it->pj, a, &it->u.line.a) && project_e(f, it->pj, b, &it->u.line.b);
}

/* The horizon only needs two points of its line in front of the eye: clipped at a small z, not to the view. */
static void sky_at(const DlFrame *f, DlItem *it)
{
    float p[3], q[3];
    memcpy(p, lerp_v[it->va], sizeof p), memcpy(q, lerp_v[it->vb], sizeof q);
    float eps = 1e-3f * (fabsf(p[0]) + fabsf(p[1]) + fabsf(p[2]) + fabsf(q[0]) + fabsf(q[1]) + fabsf(q[2]));
    if (p[2] < eps && q[2] < eps)
        return;
    if (p[2] < eps)
        at(q, p, (q[2] - eps) / (q[2] - p[2]), p);
    else if (q[2] < eps)
        at(p, q, (p[2] - eps) / (p[2] - q[2]), q);
    DlPt a, b;
    if (project_e(f, it->pj, p, &a) && project_e(f, it->pj, q, &b) && (fabsf(a.x - b.x) > 1e-3f || fabsf(a.y - b.y) > 1e-3f))
        it->u.sky.a = a, it->u.sky.b = b;
}

/* The line a0->b0 turned and moved towards a->b on the screen (normal angle and distance from the view centre). */
static bool line_nd(DlPt a, DlPt b, float *th, float *d)
{
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-3f)
        return false;
    float nx = -dy / l, ny = dx / l;
    *th = atan2f(ny, nx), *d = nx * (a.x - VIEW_W / 2.0f) + ny * (a.y - VIEW_H / 2.0f);
    return true;
}

static void sky_lerp(DlItem *it, float s)
{
    float t0, d0, t1, d1;
    if (!line_nd(it->u.sky.a0, it->u.sky.b0, &t0, &d0) || !line_nd(it->u.sky.a, it->u.sky.b, &t1, &d1))
        return;
    float dt = remainderf(t1 - t0, 2 * (float)M_PI), th = t0 + dt * s, d = d0 + (d1 - d0) * s;
    float nx = cosf(th), ny = sinf(th), px = VIEW_W / 2.0f + d * nx, py = VIEW_H / 2.0f + d * ny;
    it->u.sky.a = (DlPt){ px - 400 * ny, py + 400 * nx }; /* direction (ny, -nx) */
    it->u.sky.b = (DlPt){ px + 400 * ny, py - 400 * nx };
}

/* A polygon again from its outline at the new positions (only when every edge has its vertices). */
static void poly_at(DlFrame *f, DlItem *it)
{
    static DlPt seg[MAX_EDGES][2];
    int n = 0;
    for (int i = it->u.poly.e_from; i < it->u.poly.e_to && i < f->n_items; i++) {
        const DlItem *e = &f->items[i];
        if (e->kind != DL_LINE || !(e->flags & DL_F_EDGE))
            continue;
        if (e->va < 0 || e->vb < 0)
            return; /* an edge without vertices: the recorded rings stay */
        if (!(e->flags & DL_F_HIDDEN) && n < MAX_EDGES)
            seg[n][0] = e->u.line.a, seg[n++][1] = e->u.line.b;
    }
    int first = f->n_verts, rings = it->u.poly.wind ? stitch_rings(f, seg, n, it->u.poly.wind) : -1;
    if (rings < 0)
        rings = build_rings(f, seg, n);
    if (!rings) {
        it->flags |= DL_F_HIDDEN;
        return;
    }
    it->u.poly.first = (uint16_t)first;
    it->u.poly.count = (uint16_t)(f->n_verts - first);
    it->u.poly.rings = (uint16_t)rings;
}

/* Frame f with every vertex at e0 + (e - e0) * s. */

static const DlFrame *view_s(const DlFrame *f, float s)
{
    for (int i = 0; i < f->n_vtx; i++)
        for (int k = 0; k < 3; k++)
            lerp_v[i][k] = f->vtx[i].e0[k] + (f->vtx[i].e[k] - f->vtx[i].e0[k]) * s + (game_eye ? f->vtx[i].ef[k] : 0.0f);
    DlFrame *l = &lerp_f;
    memcpy(l->items, f->items, (size_t)f->n_items * sizeof *f->items);
    memcpy(l->verts, f->verts, (size_t)f->n_verts * sizeof *f->verts);
    memcpy(l->pj, f->pj, sizeof l->pj);
    l->n_items = f->n_items, l->n_verts = f->n_verts, l->n_pj = f->n_pj, l->n_vtx = 0;
    l->view_row = f->view_row, l->number = f->number, l->incomplete = false, l->t = f->t, l->dt = f->dt;
    for (int i = 0; i < l->n_items; i++) {
        DlItem *it = &l->items[i];
        if (it->va < 0 || it->va >= f->n_vtx || (it->kind != DL_DOT && (it->vb < 0 || it->vb >= f->n_vtx)))
            continue;
        if (it->kind == DL_LINE) {
            /* a line the game's clip rejected comes into view as the eye moves on: shown (and part of its polygon's
             * outline) from then on, so the coast and the fills do not stop at the view edge */
            if (!line_at(l, it))
                it->flags |= DL_F_HIDDEN;
            else if (it->flags & DL_F_OUT)
                it->flags &= (uint8_t)~(DL_F_HIDDEN | DL_F_OUT);
        } else if (it->kind == DL_DOT) {
            const float *e = lerp_v[it->va];
            if (!(fabsf(e[0]) <= e[2] && fabsf(e[1]) <= e[2] && project_e(l, it->pj, e, &it->u.line.a)))
                it->flags |= DL_F_HIDDEN;
        } else if (it->kind == DL_SKYGROUND) {
            sky_at(l, it);
        }
    }
    for (int i = 0; i < l->n_items; i++)
        if (l->items[i].kind == DL_SKYGROUND && l->items[i].u.sky.lerp)
            sky_lerp(&l->items[i], s);
    for (int i = 0; i < l->n_items; i++)
        if (l->items[i].kind == DL_POLY && !(l->items[i].flags & (DL_F_EMPTY | DL_F_LEAK)))
            poly_at(l, &l->items[i]);
    return l->incomplete ? f : l;
}

/* The time base is the display clock (now: the emulated time the display frame stands for), counted from the
 * display frame that first showed this view: the CPU cycle counter itself is no clock for this, since the
 * scenery pass is one native step that charges its ~0.2 s at once and the next display frames run no CPU to
 * pay it back, so a commit time measured in cycles lies ahead of the display by up to a whole interval. */
static float view_a(const DlFrame *f, uint64_t now)
{
    if (smooth_mode == DL_SMOOTH_OFF)
        return 1.0f;
    static uint32_t shown_no;
    static uint64_t shown_at;
    if (f->number != shown_no || now < shown_at)
        shown_no = f->number, shown_at = now;
    float a = 1.0f; /* display time since the view first showed, in intervals between the last two views */
    if (f->dt)
        a = fminf(1.0f, (float)(now - shown_at) / (float)f->dt);
    return a;
}

const DlFrame *dl_view_at(uint64_t now)
{
    const DlFrame *f = dl_view();
    if (!f || f->incomplete || smooth_mode == DL_SMOOTH_OFF || !f->n_vtx)
        return f;
    return view_s(f, view_a(f, now));
}

/* ---- S3.3 the world behind the cockpit's top edge ------------------------------------------------ */

static DlFrame gap_f;

static bool same_vtx(const DlFrame *f, int a, int b)
{
    if (a == b)
        return true;
    const float *p = f->vtx[a].e, *q = f->vtx[b].e;
    float d = 0, n = 0;
    for (int k = 0; k < 3; k++)
        d += (p[k] - q[k]) * (p[k] - q[k]), n += p[k] * p[k];
    return d <= 1e-8f * n;
}

static long gp_why[3]; /* --dl-check: polygons not rebuilt (no vertices, unfilled by the game, open outline) */
static bool gp_count;
#define GP_WHY(k) (gp_count ? (void)gp_why[k]++ : (void)0)

/* Polygon it again from its whole outline (drawn and rejected edges), clipped as a 3D polygon: only when the edges
 * close into loops of vertices. A polygon the game drew something of but did not fill stays unfilled. */
static bool gap_poly(DlFrame *g, const DlFrame *f, const DlItem *it, float y0, float y1)
{
    static int16_t ea[MAX_EDGES], eb[MAX_EDGES];
    int n = 0, pj = -1;
    bool drawn = false;
    for (int i = it->u.poly.e_from; i < it->u.poly.e_to && i < f->n_items; i++) {
        const DlItem *e = &f->items[i];
        if (e->kind != DL_LINE || !(e->flags & DL_F_EDGE))
            continue;
        if (e->va < 0 || e->vb < 0 || e->va >= f->n_vtx || e->vb >= f->n_vtx || n >= MAX_EDGES || e->pj >= f->n_pj)
            return GP_WHY(0), false;
        if (pj < 0)
            pj = e->pj;
        drawn |= !(e->flags & DL_F_OUT);
        ea[n] = e->va, eb[n++] = e->vb;
    }
    if (n < 3 || ((it->flags & DL_F_EMPTY) && drawn))
        return GP_WHY(1), false;
    float pl[4][3];
    rect_planes(f->pj[pj], 0, VIEW_W, y0, y1, pl);
    static bool used[MAX_EDGES];
    static float ring_a[MAX_EDGES + 8][3], ring_b[MAX_EDGES + 8][3];
    memset(used, 0, (size_t)n * sizeof *used);
    int first = g->n_verts, rings = 0;
    for (int s0 = 0; s0 < n; s0++) { /* loops: each edge's end is the next edge's start */
        if (used[s0])
            continue;
        int m = 0, c = s0;
        for (;;) {
            used[c] = true;
            memcpy(ring_a[m++], lerp_v[ea[c]], sizeof ring_a[0]);
            if (same_vtx(f, eb[c], ea[s0]))
                break;
            int next = -1;
            for (int k = 0; k < n && next < 0; k++) {
                int j = (c + 1 + k) % n;
                if (!used[j] && same_vtx(f, ea[j], eb[c]))
                    next = j;
            }
            if (next < 0) {
                g->n_verts = first;
                return GP_WHY(2), false; /* an open outline */
            }
            c = next;
        }
        for (int i = 0; i < 4 && m > 0; i++) { /* Sutherland-Hodgman in eye space */
            int k = 0;
            for (int j = 0; j < m; j++) {
                const float *p = ring_a[j], *q = ring_a[(j + 1) % m];
                float fp = plane_d(pl[i], p), fq = plane_d(pl[i], q);
                if (fp >= 0)
                    memcpy(ring_b[k++], p, sizeof ring_b[0]);
                if ((fp >= 0) != (fq >= 0) && k < MAX_EDGES + 8)
                    at(p, q, fp / (fp - fq), ring_b[k++]);
                if (k >= MAX_EDGES + 7)
                    break;
            }
            memcpy(ring_a, ring_b, (size_t)k * sizeof ring_a[0]);
            m = k;
        }
        int from = g->n_verts;
        for (int j = 0; j < m; j++) {
            DlPt pt;
            if (project_e(f, pj, ring_a[j], &pt))
                vert_to(g, pt);
        }
        if (g->n_verts - from < 3) {
            g->n_verts = from;
            continue;
        }
        rings++;
        vert_to(g, (DlPt){ DL_RING_BREAK_X, 0 });
    }
    if (g->n_verts > first && isnan(g->verts[g->n_verts - 1].x))
        g->n_verts--;
    if (!rings)
        return false;
    DlItem *o = &g->items[g->n_items++];
    *o = *it;
    o->flags = 0;
    o->u.poly.first = (uint16_t)first, o->u.poly.count = (uint16_t)(g->n_verts - first), o->u.poly.rings = (uint16_t)rings;
    return true;
}

static long gp_frames, gp_polys, gp_closed, gp_lines, gp_out; /* --dl-check */

static const DlFrame *gap_s(const DlFrame *f, float s, float y0, float y1)
{
    for (int i = 0; i < f->n_vtx; i++)
        for (int k = 0; k < 3; k++)
            lerp_v[i][k] = f->vtx[i].e0[k] + (f->vtx[i].e[k] - f->vtx[i].e0[k]) * s;
    DlFrame *g = &gap_f;
    memcpy(g->pj, f->pj, sizeof g->pj);
    g->n_items = g->n_verts = g->n_vtx = 0, g->n_pj = f->n_pj;
    g->view_row = f->view_row, g->number = f->number, g->incomplete = false;
    for (int i = 0; i < f->n_items && g->n_items < DL_MAX_ITEMS; i++) {
        const DlItem *it = &f->items[i];
        if (it->kind == DL_POLY) {
            bool closed = gap_poly(g, f, it, y0, y1);
            if (gp_count && it->u.poly.e_to > it->u.poly.e_from)
                gp_polys++, gp_closed += closed;
            continue;
        }
        if ((it->kind != DL_LINE && it->kind != DL_DOT) || it->va < 0 || it->va >= f->n_vtx || it->pj >= f->n_pj)
            continue;
        float pl[4][3];
        rect_planes(f->pj[it->pj], 0, VIEW_W, y0, y1, pl);
        DlItem o = *it;
        o.flags &= (uint8_t)~(DL_F_HIDDEN | DL_F_OUT);
        if (it->kind == DL_DOT) {
            const float *e = lerp_v[it->va];
            bool in = true;
            for (int k = 0; k < 4; k++)
                in &= plane_d(pl[k], e) >= 0;
            if (!in || !project_e(f, it->pj, e, &o.u.line.a))
                continue;
        } else {
            if (it->vb < 0 || it->vb >= f->n_vtx)
                continue;
            const float *p = lerp_v[it->va], *q = lerp_v[it->vb];
            float t0, t1, a[3], b[3];
            if (!clip_planes(pl, p, q, &t0, &t1))
                continue;
            at(p, q, t0, a), at(p, q, t1, b);
            if (!project_e(f, it->pj, a, &o.u.line.a) || !project_e(f, it->pj, b, &o.u.line.b))
                continue;
        }
        g->items[g->n_items++] = o;
        if (gp_count && it->kind == DL_LINE)
            gp_lines++, gp_out += (it->flags & DL_F_OUT) != 0;
    }
    return g->incomplete ? NULL : g;
}

const DlFrame *dl_gap_at(uint64_t now, float y0, float y1)
{
    const DlFrame *f = dl_view();
    if (!f || f->incomplete || !f->n_vtx)
        return NULL;
    return gap_s(f, view_a(f, now), y0, y1);
}

/* --dl-check: the frame re-projected at its own time must give the recorded positions back. */
static long sc_items, sc_off, sc_hidden, sc_polys, sc_poly_off;
static float sc_max;

static float dist_line(DlPt p, DlPt a, DlPt b) /* distance of p from the infinite line a-b */
{
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
    return l < 1e-6f ? 0 : fabsf((p.x - a.x) * dy - (p.y - a.y) * dx) / l;
}

/* --dl-check: the view drawn at the interpolation steps of a frame into a raster of colour keys (fills, sky and
 * ground, polygons; no lines; a leaked polygon's box is unknown); a step that changes far more pixels than the
 * median step is a blink. Step 0 is the frame change (against the last frame's last step). */
enum { FL_STEPS = 16 };
#define FL_UNKNOWN 0xFFFFFFFFu
static uint32_t fl_ras[2][VIEW_H][VIEW_W];
static bool fl_have_prev;
static long fl_blinks, fl_views;

static void fl_span(uint32_t (*r)[VIEW_W], const DlPt *p, int n, uint32_t key)
{
    for (int y = 0; y < VIEW_H; y++) {
        float yc = (float)y + 0.5f, xs[512];
        int m = 0;
        for (int i = 0; i < n && m < 512; i++) {
            DlPt a = p[i], b = p[(i + 1) % n];
            if ((a.y <= yc) != (b.y <= yc))
                xs[m++] = a.x + (yc - a.y) * (b.x - a.x) / (b.y - a.y);
        }
        for (int i = 1; i < m; i++)
            for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) {
                float t = xs[j];
                xs[j] = xs[j - 1], xs[j - 1] = t;
            }
        for (int i = 0; i + 1 < m; i += 2)
            for (int x = (int)ceilf(fmaxf(xs[i] - 0.5f, 0)); x < VIEW_W && (float)x + 0.5f < xs[i + 1]; x++)
                r[y][x] = key;
    }
}

static void fl_raster(const DlFrame *f, uint32_t (*r)[VIEW_W])
{
    memset(r, 0, sizeof fl_ras[0]);
    for (int i = 0; i < f->n_items; i++) {
        const DlItem *it = &f->items[i];
        if (it->kind == DL_POLY && (it->flags & DL_F_LEAK) && !(it->flags & DL_F_HIDDEN)) { /* drawn from the CGA picture */
            for (int y = it->u.poly.y0; y <= it->u.poly.y1 && y < VIEW_H; y++)
                for (int x = it->u.poly.x0 * 4; x < it->u.poly.x1 * 4 + 4 && x < VIEW_W; x++)
                    r[y][x] = FL_UNKNOWN;
            continue;
        }
        if (it->flags & (DL_F_HIDDEN | DL_F_EMPTY | DL_F_LEAK))
            continue;
        if (it->kind == DL_FILL) {
            DlPt q[4] = { { it->u.rect.x, it->u.rect.y }, { it->u.rect.x + it->u.rect.w, it->u.rect.y },
                          { it->u.rect.x + it->u.rect.w, it->u.rect.y + it->u.rect.h }, { it->u.rect.x, it->u.rect.y + it->u.rect.h } };
            fl_span(r, q, 4, 0x10000u | it->colour);
        } else if (it->kind == DL_SKYGROUND) {
            DlPt a = it->u.sky.a, b = it->u.sky.b;
            for (int y = 0; y < VIEW_H; y++)
                for (int x = 0; x < VIEW_W; x++) {
                    float c = (b.x - a.x) * ((float)y + 0.5f - a.y) - (b.y - a.y) * ((float)x + 0.5f - a.x);
                    r[y][x] = 0x10000u | (c <= 0 ? it->u.sky.pat_l : it->u.sky.pat_r);
                }
        } else if (it->kind == DL_POLY) {
            const DlPt *v = f->verts + it->u.poly.first;
            for (int k = 0, from = 0; k <= it->u.poly.count; k++)
                if (k == it->u.poly.count || isnan(v[k].x)) {
                    if (k - from >= 3)
                        fl_span(r, v + from, k - from, 0x20000u | (it->colour & 0xFF));
                    from = k + 1;
                }
        }
    }
}

static int fl_diff(uint32_t (*a)[VIEW_W], uint32_t (*b)[VIEW_W])
{
    int n = 0;
    for (int y = 0; y < VIEW_H; y++)
        for (int x = 1; x < VIEW_W - 1; x++)
            n += a[y][x] != FL_UNKNOWN && b[y][x] != FL_UNKNOWN && a[y][x] != b[y][x];
    return n;
}

static void flicker_check(const DlFrame *f)
{
    static int d[FL_STEPS + 1];
    int cur = 0;
    fl_views++;
    for (int k = 0; k <= FL_STEPS; k++) {
        const DlFrame *l = view_s(f, (float)k / FL_STEPS);
        if (l == f)
            return;
        fl_raster(l, fl_ras[cur ^ 1]);
        cur ^= 1;
        /* step 0: the frame change, against the previous frame's last step */
        d[k] = (k || fl_have_prev) ? fl_diff(fl_ras[cur ^ 1], fl_ras[cur]) : 0;
    }
    fl_have_prev = true;
    /* the motion per step: the median of the steps inside the frame */
    int s[FL_STEPS];
    for (int k = 0; k < FL_STEPS; k++)
        s[k] = d[k + 1];
    for (int i = 1; i < FL_STEPS; i++)
        for (int j = i; j > 0 && s[j] < s[j - 1]; j--) {
            int t = s[j];
            s[j] = s[j - 1], s[j - 1] = t;
        }
    int med = s[FL_STEPS / 2];
    for (int k = 0; k <= FL_STEPS; k++)
        if (d[k] > 4 * med + 60) {
            fl_blinks++;
            fprintf(stderr, "smooth: blink in frame %u at step %d/%d: %d px changed (median step %d)%s\n", f->number, k,
                    FL_STEPS, d[k], med, k == 0 ? ", at the frame change" : "");
        }
}

static void smooth_check(void)
{
    const DlFrame *f = dl_view();
    if (f && !f->incomplete && f->n_vtx)
        flicker_check(f);
    gp_count = true;
    if (f && !f->incomplete && f->n_vtx && gap_s(f, 1.0f, VIEW_H - 2, VIEW_H + 12)) /* S3.3: the band of the stock corner */
        gp_frames++;
    gp_count = false;
    game_eye = true;
    const DlFrame *l = view_s(f, 1.0f);
    game_eye = false;
    if (l == f)
        return;
    for (int i = 0; i < f->n_items; i++) {
        const DlItem *o = &f->items[i], *n = &l->items[i];
        if (o->kind == DL_POLY && !(o->flags & (DL_F_EMPTY | DL_F_LEAK)) && o->u.poly.rings) {
            float a0, a1, p0, p1; /* the polygon rebuilt from its re-projected outline: same area */
            rings_size(f, o->u.poly.first, o->u.poly.count, &a0, &p0);
            if (n->flags & DL_F_HIDDEN)
                a1 = p1 = 0;
            else
                rings_size(l, n->u.poly.first, n->u.poly.count, &a1, &p1);
            sc_polys++;
            if (fabsf(a1 - a0) > 0.05f * a0 + p0)
                sc_poly_off++;
        }
        if (o->va < 0 || o->kind == DL_POLY || (o->flags & DL_F_OUT))
            continue;
        sc_items++;
        if (n->flags & DL_F_HIDDEN) {
            sc_hidden++;
            continue;
        }
        float d;
        if (o->kind == DL_SKYGROUND)
            d = fmaxf(dist_line(o->u.sky.a, n->u.sky.a, n->u.sky.b), dist_line(o->u.sky.b, n->u.sky.a, n->u.sky.b));
        else
            d = fmaxf(fabsf(o->u.line.a.x - n->u.line.a.x) + fabsf(o->u.line.a.y - n->u.line.a.y),
                      o->kind == DL_DOT ? 0 : fabsf(o->u.line.b.x - n->u.line.b.x) + fabsf(o->u.line.b.y - n->u.line.b.y));
        sc_max = fmaxf(sc_max, d);
        if (d > 1.5f)
            sc_off++;
    }
}

static void sm_report(void)
{
    if (fl_views)
        fprintf(stderr, "smooth: %ld blinks in %ld frames (a step changing far more pixels than the motion)\n", fl_blinks, fl_views);
    if (gp_frames)
        fprintf(stderr, "gap: %ld frames, per frame %.1f lines in the band under the view (%.1f rejected by the game), "
                        "%ld of %ld polygons in it (not rebuilt: %ld without vertices, %ld drawn but not filled by the game, "
                        "%ld open)\n", gp_frames, (double)gp_lines / (double)gp_frames,
                (double)gp_out / (double)gp_frames, gp_closed, gp_polys, gp_why[0], gp_why[1], gp_why[2]);
    if (sc_items)
        fprintf(stderr, "smooth: re-projected at their own time, %ld of %ld items are off by more than 1.5 px (max %.1f), %ld hidden\n",
                sc_off, sc_items, sc_max, sc_hidden);
    if (sc_polys)
        fprintf(stderr, "smooth: %ld of %ld polygons rebuilt at their own time differ in area by more than 5%% + perimeter\n",
                sc_poly_off, sc_polys);
}

/* ---- text rows (S2.2) ------------------------------------------------------------------------ */

static uint16_t text_w[0x4000];
static uint8_t text_ok[0x4000];

void dl_text_row_(Pc *pc, uint16_t es, uint16_t di, uint16_t w)
{
    (void)pc;
    if (es != 0xB800 || di >= 0x4000 - 1)
        return;
    text_w[di] = w;
    text_ok[di] = 1;
}

bool dl_text_rows(uint16_t **w, uint8_t **ok)
{
    *w = text_w, *ok = text_ok;
    return dl_on;
}
