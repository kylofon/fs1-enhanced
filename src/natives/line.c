/* Natives for subphase 3.4 line drawing (see docs/PHASE3_PLAN.md, docs/subphases/3.4.md).
 *
 * The rasteriser is a label-for-label translation of the original, so the pixels, the
 * final registers and the stack words written by its internal CALLs all match. Labels are
 * the original offsets (Lxxxx = 0050:xxxx). */
#include <stdbool.h>

#include "native.h"
#include "game/state.h"
#include "raster.h"

/* ---- 8086 arithmetic as the original's conditional jumps see it -------------------- */

/* Jcc after SUB/ADD/DEC test the exact (unwrapped) result against zero. */
static inline bool sub_g(uint16_t *a, uint16_t b) /* sub a,b; jg */
{
    int32_t r = (int32_t)(int16_t)*a - (int16_t)b;
    *a = (uint16_t)r;
    return r > 0;
}
static inline bool add_g(uint16_t *a, uint16_t b) /* add a,b; jg */
{
    int32_t r = (int32_t)(int16_t)*a + (int16_t)b;
    *a = (uint16_t)r;
    return r > 0;
}
static inline bool add_l(uint16_t *a, uint16_t b) /* add a,b; jl */
{
    int32_t r = (int32_t)(int16_t)*a + (int16_t)b;
    *a = (uint16_t)r;
    return r < 0;
}
static inline bool dec_l(uint16_t *a) /* dec a; jl */
{
    int32_t r = (int32_t)(int16_t)*a - 1;
    *a = (uint16_t)r;
    return r < 0;
}
static inline bool dec_g(uint16_t *a) /* dec a; jg */
{
    int32_t r = (int32_t)(int16_t)*a - 1;
    *a = (uint16_t)r;
    return r > 0;
}
static inline uint16_t sar1(uint16_t v) { return (uint16_t)((v >> 1) | (v & 0x8000)); }
static inline bool lt(uint16_t a, uint16_t b) { return (int16_t)a < (int16_t)b; }
static inline bool odd_row(uint16_t bx) { return (int16_t)bx >= 0x2000; } /* cmp bx,2000h; jge */

/* ---- bus helpers ---------------------------------------------------------------- */

typedef struct St {
    const RasterBus *b;
    uint16_t ax, bx, cx, dx, si, di, bp;
} St;

static inline uint8_t brd(St *s, uint16_t o) { return s->b->buf_rd(s->b->ctx, o); }
static inline void bwr(St *s, uint16_t o, uint8_t v) { s->b->buf_wr(s->b->ctx, o, v); }
static inline uint16_t brd16(St *s, uint16_t o) { return (uint16_t)(brd(s, o) | brd(s, (uint16_t)(o + 1)) << 8); }
static inline void bwr16(St *s, uint16_t o, uint16_t v)
{
    bwr(s, o, (uint8_t)v);
    bwr(s, (uint16_t)(o + 1), (uint8_t)(v >> 8));
}
static inline uint16_t drd16(St *s, uint16_t o)
{
    return (uint16_t)(s->b->dat_rd(s->b->ctx, o) | s->b->dat_rd(s->b->ctx, (uint16_t)(o + 1)) << 8);
}
static inline void dwr16(St *s, uint16_t o, uint16_t v)
{
    s->b->dat_wr(s->b->ctx, o, (uint8_t)v);
    s->b->dat_wr(s->b->ctx, (uint16_t)(o + 1), (uint8_t)(v >> 8));
}
static inline void call(St *s, uint16_t ret)
{
    if (s->b->call)
        s->b->call(s->b->ctx, ret);
}

#define AL ((uint8_t)s->ax)
#define CL ((uint8_t)s->cx)
#define CH ((uint8_t)(s->cx >> 8))
#define SET_AL(v) (s->ax = (uint16_t)((s->ax & 0xFF00) | (uint8_t)(v)))
/* the nibble writes: left pixel (high nibble, colour CH) and right pixel (low, CL) */
#define HI_RD() SET_AL((brd(s, s->bx) & 0x0F) | CH)
#define LO_RD() SET_AL((brd(s, s->bx) & 0xF0) | CL)
#define HI_AL() SET_AL((AL & 0x0F) | CH)
#define LO_AL() SET_AL((AL & 0xF0) | CL)
#define STORE() bwr(s, s->bx, AL)
#define SWAP(a, b) do { uint16_t t_ = (a); (a) = (b); (b) = t_; } while (0)

/* add bx,[row table + 2*y]; cx = colour pair; CF:AX = x0 >> 1. Returns CF (x0 odd). */
static bool setup_row(St *s)
{
    s->bp = (uint16_t)(s->bp << 1);
    s->bx = (uint16_t)(s->bx + drd16(s, (uint16_t)(s->bp + 0x380C)));
    s->cx = drd16(s, 0x30C8);
    bool cf = s->ax & 1;
    s->ax = sar1(s->ax);
    return cf;
}

/* ---- 0050:569B normal path -------------------------------------------------------- */

static void line_normal(St *s)
{
    if (lt(s->si, s->bx)) {
        SWAP(s->si, s->bx);
        SWAP(s->bp, s->di);
    } else if (s->si == s->bx) {
        goto L5907;
    }
    s->si = (uint16_t)(s->si - s->bx);
    s->ax = s->bx;
    s->bx = (uint16_t)(s->bx >> 1);
    {
        int32_t r = (int32_t)(int16_t)s->di - (int16_t)s->bp;
        s->di = (uint16_t)r;
        if (r == 0)
            goto L596D;
        if (r < 0) {
            bool cf = setup_row(s);
            int32_t sum = (int32_t)(int16_t)s->di + (int16_t)s->si;
            s->ax = (uint16_t)sum;
            if (!cf) {
                if (sum < 0)
                    goto L588B;
                goto L5814;
            }
            if (sum < 0)
                goto L58C5;
            goto L584D;
        }
    }
    if (!setup_row(s)) {
        if (!lt(s->si, s->di)) /* di <= si */
            goto L5725;
        goto L579C;
    }
    if (!lt(s->si, s->di))
        goto L575E;
    goto L57D4;

    /* x-major, y increasing */
L5725:
    s->dx = sar1(s->si);
    s->bp = s->si;
    if (odd_row(s->bx))
        goto L5780;
L5731:
    HI_RD();
    if (dec_l(&s->bp))
        goto L5799;
    if (sub_g(&s->dx, s->di))
        goto L576C;
    STORE();
    s->bx += 0x2000;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
L5748:
    LO_AL();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx++;
    if (sub_g(&s->dx, s->di))
        goto L5780;
    s->bx += 0xE050;
    s->dx += s->si;
    goto L5731;
L575E:
    s->dx = sar1(s->si);
    s->bp = s->si;
    SET_AL(brd(s, s->bx));
    if (odd_row(s->bx))
        goto L5748;
L576C:
    LO_AL();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx++;
    if (sub_g(&s->dx, s->di))
        goto L5731;
    s->bx += 0x2000;
    s->dx += s->si;
L5780:
    HI_RD();
    if (dec_l(&s->bp))
        goto L5799;
    if (sub_g(&s->dx, s->di))
        goto L5748;
    STORE();
    s->bx += 0xE050;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
    goto L576C;
L5799:
    STORE();
    return;

    /* y-major, y increasing */
L579C:
    s->dx = sar1(s->di);
    s->bp = s->di;
    if (odd_row(s->bx))
        goto L57FC;
L57A8:
    HI_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx += 0x2000;
    if (sub_g(&s->dx, s->si))
        goto L57FC;
    s->dx += s->di;
L57BD:
    LO_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    if (sub_g(&s->dx, s->si))
        goto L57E2;
    s->bx += 0xE051;
    s->dx += s->di;
    goto L57A8;
L57D4:
    s->dx = sar1(s->di);
    s->bp = s->di;
    if (odd_row(s->bx))
        goto L57BD;
    goto L57E6;
L57E2:
    s->bx += 0xE050;
L57E6:
    LO_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx += 0x2000;
    if (sub_g(&s->dx, s->si))
        goto L57BD;
    s->dx += s->di;
    s->bx++;
L57FC:
    HI_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx += 0xE050;
    if (sub_g(&s->dx, s->si))
        goto L57A8;
    s->dx += s->di;
    goto L57E6;

    /* x-major, y decreasing (di < 0) */
L5814:
    s->dx = sar1(s->si);
    s->bp = s->si;
    if (odd_row(s->bx))
        goto L586F;
L5820:
    HI_RD();
    if (dec_l(&s->bp))
        goto L5888;
    if (add_g(&s->dx, s->di))
        goto L585B;
    STORE();
    s->bx += 0x1FB0;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
L5837:
    LO_AL();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx++;
    if (add_g(&s->dx, s->di))
        goto L586F;
    s->bx += 0xE000;
    s->dx += s->si;
    goto L5820;
L584D:
    s->dx = sar1(s->si);
    s->bp = s->si;
    SET_AL(brd(s, s->bx));
    if (odd_row(s->bx))
        goto L5837;
L585B:
    LO_AL();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx++;
    if (add_g(&s->dx, s->di))
        goto L5820;
    s->bx += 0x1FB0;
    s->dx += s->si;
L586F:
    HI_RD();
    if (dec_l(&s->bp))
        goto L5888;
    if (add_g(&s->dx, s->di))
        goto L5837;
    STORE();
    s->bx += 0xE000;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
    goto L585B;
L5888:
    STORE();
    return;

    /* y-major, y decreasing */
L588B:
    s->dx = sar1(s->di);
    s->bp = (uint16_t)-s->di;
    if (odd_row(s->bx))
        goto L58EF;
L5899:
    HI_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx += 0x1FB0;
    if (add_l(&s->dx, s->si))
        goto L58EF;
    s->dx += s->di;
L58AE:
    LO_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    if (add_l(&s->dx, s->si))
        goto L58D5;
    s->bx += 0xE001;
    s->dx += s->di;
    goto L5899;
L58C5:
    s->dx = sar1(s->di);
    s->bp = (uint16_t)-s->di;
    if (odd_row(s->bx))
        goto L58AE;
    goto L58D9;
L58D5:
    s->bx += 0xE000;
L58D9:
    LO_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx += 0x1FB0;
    if (add_l(&s->dx, s->si))
        goto L58AE;
    s->dx += s->di;
    s->bx++;
L58EF:
    HI_RD();
    STORE();
    if (dec_l(&s->bp))
        return;
    s->bx += 0xE000;
    if (add_l(&s->dx, s->si))
        goto L5899;
    s->dx += s->di;
    goto L58D9;

    /* vertical (x0 == x1) */
L5907:
    if (lt(s->di, s->bp))
        SWAP(s->bp, s->di);
    s->di = (uint16_t)(s->di - s->bp);
    s->ax = s->bx;
    s->bx = (uint16_t)(s->bx >> 1);
    if (setup_row(s)) {
        if (odd_row(s->bx))
            goto L595D;
    L594E:
        LO_RD();
        STORE();
        if (dec_l(&s->di))
            return;
        s->bx += 0x2000;
    L595D:
        LO_RD();
        STORE();
        s->bx += 0xE050;
        if (!dec_l(&s->di))
            goto L594E;
        return;
    }
    if (odd_row(s->bx))
        goto L5938;
L5929:
    HI_RD();
    STORE();
    if (dec_l(&s->di))
        return;
    s->bx += 0x2000;
L5938:
    HI_RD();
    STORE();
    s->bx += 0xE050;
    if (!dec_l(&s->di))
        goto L5929;
    return;

    /* horizontal (y0 == y1), si = length */
L596D:
    if (setup_row(s))
        goto L5991;
L597D:
    HI_RD();
    if (dec_l(&s->si)) {
        STORE();
        return;
    }
    LO_AL();
    STORE();
    s->bx++;
    if (!dec_l(&s->si))
        goto L597D;
    return;
L5991:
    LO_RD();
    STORE();
    s->bx++;
    if (!dec_l(&s->si))
        goto L597D;
}

/* ---- 0050:5060 polygon-outline path ------------------------------------------------
 *
 * Scratch bytes in the back buffer segment (between the banks): */
#define P_MID 0x1F44   /* byte: 1 while the midpoint is still ahead */
#define P_SIDE 0x1F45  /* byte: which side of the edge is inside (endpoints swapped, y-major up) */
#define P_FLAG 0x1F46  /* word: 8000h = seed is a left (high) nibble */
#define P_HALF 0x1F48  /* word: pixels left after the midpoint */
#define SEEDS 0x3730   /* data: word list index, then {neighbour, pixel} word pairs at 3734 */

/* 538A: append {bp, previous head} to the seed list; bx = the pixel again. */
static void seed_store(St *s)
{
    dwr16(s, (uint16_t)(s->bx + SEEDS + 4), s->bp);
    s->bp = drd16(s, SEEDS);
    dwr16(s, (uint16_t)(s->bx + SEEDS + 6), s->bp);
    s->bx = (uint16_t)(s->bx + 4);
    dwr16(s, SEEDS, s->bx);
    s->bx = (uint16_t)(s->bp & 0x7FFF);
}

/* 5363 (from 532C/5345 after the neighbour is chosen) */
static void seed_flagged(St *s)
{
    uint16_t f = brd16(s, P_FLAG);
    s->bx |= f;
    uint16_t t = drd16(s, SEEDS);
    dwr16(s, SEEDS, s->bx);
    s->bx = t;
    s->bp |= f;
    seed_store(s);
}

static void seed_532C(St *s) /* x-major: neighbour in the row above/below */
{
    s->bp = s->bx;
    bwr(s, P_MID, 0);
    if (brd(s, P_SIDE) != 0) {
        int32_t r = (int16_t)s->bp - 0x50;
        s->bp = (uint16_t)r;
        if (s->bp & 0x8000) /* js */
            goto L53A7;
    }
    s->bp += 0x2000;
    seed_flagged(s);
L53A7:
    s->bp = brd16(s, P_HALF);
}

static void seed_5345(St *s)
{
    s->bp = s->bx;
    bwr(s, P_MID, 0);
    if (brd(s, P_SIDE) != 0) {
        s->bp -= 0x2000;
    } else {
        s->bp += 0xE050;
        if ((int16_t)s->bp >= 0x1090)
            goto L53A7;
    }
    seed_flagged(s);
L53A7:
    s->bp = brd16(s, P_HALF);
}

static void seed_5372(St *s) /* y-major, left nibble: neighbour is the nibble to the left/right */
{
    s->bp = s->bx;
    bwr(s, P_MID, 0);
    if (brd(s, P_SIDE) == 0)
        s->bp--;
    s->bx |= 0x8000;
    uint16_t t = drd16(s, SEEDS);
    dwr16(s, SEEDS, s->bx);
    s->bx = t;
    seed_store(s);
    s->bp = brd16(s, P_HALF);
}

static void seed_53AC(St *s) /* y-major, right nibble */
{
    s->bp = s->bx;
    bwr(s, P_MID, 0);
    if (brd(s, P_SIDE) != 0)
        s->bp++;
    uint16_t t = drd16(s, SEEDS);
    dwr16(s, SEEDS, s->bx);
    s->bx = t;
    s->bp |= 0x8000;
    seed_store(s);
    s->bp = brd16(s, P_HALF);
}

/* 53D3: pixel count; if more than 3 pixels, stop at the midpoint first. */
static void mid_setup(St *s)
{
    s->bp++;
    bwr(s, P_MID, 0);
    if ((int16_t)s->bp <= 3)
        return;
    bool cf = s->bp & 1;
    s->bp >>= 1;
    bwr16(s, P_HALF, s->bp);
    s->bp = (uint16_t)(s->bp + cf);
    bwr(s, P_MID, (uint8_t)(brd(s, P_MID) + 1));
}

/* The helpers called when the pixel count runs out. They return true when the line ends
 * (the original pops its return address into BP and returns from draw_line). */
static bool h_53EC(St *s, uint16_t ret) /* left nibble pending in AL, x-major */
{
    if (brd(s, P_MID) == 0) {
        s->bp = ret;
        STORE();
        return true;
    }
    SET_AL(brd(s, s->bx));
    bwr16(s, P_FLAG, 0x8000);
    seed_532C(s);
    return false;
}
static bool h_53FE(St *s, uint16_t ret)
{
    if (brd(s, P_MID) == 0) {
        s->bp = ret;
        LO_AL();
        STORE();
        return true;
    }
    STORE();
    bwr16(s, P_FLAG, 0);
    seed_5345(s);
    return false;
}
static bool h_5410(St *s, uint16_t ret)
{
    if (brd(s, P_MID) == 0) {
        s->bp = ret;
        LO_AL();
        STORE();
        return true;
    }
    STORE();
    bwr16(s, P_FLAG, 0);
    seed_532C(s);
    return false;
}
static bool h_5422(St *s, uint16_t ret)
{
    if (brd(s, P_MID) == 0) {
        s->bp = ret;
        STORE();
        return true;
    }
    SET_AL(brd(s, s->bx));
    bwr16(s, P_FLAG, 0x8000);
    seed_5345(s);
    return false;
}
static bool h_5440(St *s, uint16_t ret)
{
    if (brd(s, P_MID) == 0) {
        s->bp = ret;
        HI_RD();
        STORE();
        return true;
    }
    seed_5372(s);
    return false;
}
static bool h_544A(St *s, uint16_t ret)
{
    if (brd(s, P_MID) == 0) {
        s->bp = ret;
        LO_RD();
        STORE();
        return true;
    }
    seed_53AC(s);
    return false;
}

/* CALL helper at return address RET; leave draw_line if it says so. */
#define HCALL(fn, ret)        \
    do {                      \
        call(s, ret);         \
        if (fn(s, ret))       \
            return;           \
    } while (0)
#define MID_SETUP(ret)        \
    do {                      \
        call(s, ret);         \
        mid_setup(s);         \
    } while (0)
/* 53C6: y-major going up flips the inside side, then counts like 53D3 */
#define MID_SETUP_UP(ret)                                        \
    do {                                                         \
        call(s, ret);                                            \
        bwr(s, P_SIDE, (uint8_t)(brd(s, P_SIDE) ^ 1));           \
        s->dx = sar1(s->di);                                     \
        s->bp = (uint16_t)-s->di;                                \
        mid_setup(s);                                            \
    } while (0)

static void line_poly(St *s)
{
    bwr(s, P_SIDE, 0);
    if (s->bx == 0)
        s->bx++;
    if (s->bx == 0x9F)
        s->bx--;
    if (s->si == 0)
        s->si++;
    if (s->si == 0x9F)
        s->si--;
    if (!lt(s->bx, s->si)) {
        SWAP(s->si, s->bx);
        SWAP(s->bp, s->di);
        bwr(s, P_SIDE, 1);
    }
    s->si = (uint16_t)(s->si - s->bx);
    s->ax = s->bx;
    s->bx = (uint16_t)(s->bx >> 1);
    {
        int32_t r = (int32_t)(int16_t)s->di - (int16_t)s->bp;
        s->di = (uint16_t)r;
        if (r < 0) {
            bool cf = setup_row(s);
            int32_t sum = (int32_t)(int16_t)s->di + (int16_t)s->si;
            s->ax = (uint16_t)sum;
            if (!cf) {
                if (sum < 0)
                    goto L52A7;
                goto L521D;
            }
            if (sum < 0)
                goto L52E6;
            goto L5261;
        }
    }
    if (!setup_row(s)) {
        if (!lt(s->si, s->di))
            goto L5102;
        goto L518C;
    }
    if (!lt(s->si, s->di))
        goto L5146;
    goto L51D1;

    /* x-major, y increasing */
L5102:
    s->dx = sar1(s->si);
    s->bp = s->si;
    MID_SETUP(0x510B);
    if (odd_row(s->bx))
        goto L5170;
L5111:
    HI_RD();
    if (!dec_g(&s->bp))
        HCALL(h_53EC, 0x511D);
    if (sub_g(&s->dx, s->di))
        goto L5157;
    STORE();
    s->bx += 0x2000;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
L512B:
    if (dec_g(&s->bp)) {
        LO_AL();
        STORE();
    } else {
        HCALL(h_53FE, 0x5131);
    }
    s->bx++;
    if (sub_g(&s->dx, s->di))
        goto L5170;
    s->bx += 0xE050;
    s->dx += s->si;
    goto L5111;
L5146:
    s->dx = sar1(s->si);
    s->bp = s->si;
    MID_SETUP(0x514F);
    SET_AL(brd(s, s->bx));
    if (odd_row(s->bx))
        goto L512B;
L5157:
    if (dec_g(&s->bp)) {
        LO_AL();
        STORE();
    } else {
        HCALL(h_5410, 0x515D);
    }
    s->bx++;
    if (sub_g(&s->dx, s->di))
        goto L5111;
    s->bx += 0x2000;
    s->dx += s->si;
L5170:
    HI_RD();
    if (!dec_g(&s->bp))
        HCALL(h_5422, 0x517C);
    if (sub_g(&s->dx, s->di))
        goto L512B;
    STORE();
    s->bx += 0xE050;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
    goto L5157;

    /* y-major, y increasing */
L518C:
    s->dx = sar1(s->di);
    s->bp = s->di;
    MID_SETUP(0x5195);
    if (odd_row(s->bx))
        goto L5201;
L519B:
    if (dec_g(&s->bp)) {
        HI_RD();
        STORE();
    } else {
        HCALL(h_5440, 0x51A1);
    }
    s->bx += 0x2000;
    if (sub_g(&s->dx, s->si))
        goto L5201;
    s->dx += s->di;
L51B5:
    if (dec_g(&s->bp)) {
        LO_RD();
        STORE();
    } else {
        HCALL(h_544A, 0x51BB);
    }
    if (sub_g(&s->dx, s->si))
        goto L51E2;
    s->bx += 0xE051;
    s->dx += s->di;
    goto L519B;
L51D1:
    s->dx = sar1(s->di);
    s->bp = s->di;
    MID_SETUP(0x51DA);
    if (odd_row(s->bx))
        goto L51B5;
    goto L51E6;
L51E2:
    s->bx += 0xE050;
L51E6:
    if (dec_g(&s->bp)) {
        LO_RD();
        STORE();
    } else {
        HCALL(h_544A, 0x51EC);
    }
    s->bx += 0x2000;
    if (sub_g(&s->dx, s->si))
        goto L51B5;
    s->dx += s->di;
    s->bx++;
L5201:
    if (dec_g(&s->bp)) {
        HI_RD();
        STORE();
    } else {
        HCALL(h_5440, 0x5207);
    }
    s->bx += 0xE050;
    if (sub_g(&s->dx, s->si))
        goto L519B;
    s->dx += s->di;
    goto L51E6;

    /* x-major, y decreasing */
L521D:
    s->dx = sar1(s->si);
    s->bp = s->si;
    MID_SETUP(0x5226);
    if (odd_row(s->bx))
        goto L528B;
L522C:
    HI_RD();
    if (!dec_g(&s->bp))
        HCALL(h_53EC, 0x5238);
    if (add_g(&s->dx, s->di))
        goto L5272;
    STORE();
    s->bx += 0x1FB0;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
L5246:
    if (dec_g(&s->bp)) {
        LO_AL();
        STORE();
    } else {
        HCALL(h_53FE, 0x524C);
    }
    s->bx++;
    if (add_g(&s->dx, s->di))
        goto L528B;
    s->bx += 0xE000;
    s->dx += s->si;
    goto L522C;
L5261:
    s->dx = sar1(s->si);
    s->bp = s->si;
    MID_SETUP(0x526A);
    SET_AL(brd(s, s->bx));
    if (odd_row(s->bx))
        goto L5246;
L5272:
    if (dec_g(&s->bp)) {
        LO_AL();
        STORE();
    } else {
        HCALL(h_5410, 0x5278);
    }
    s->bx++;
    if (add_g(&s->dx, s->di))
        goto L522C;
    s->bx += 0x1FB0;
    s->dx += s->si;
L528B:
    HI_RD();
    if (!dec_g(&s->bp))
        HCALL(h_5422, 0x5297);
    if (add_g(&s->dx, s->di))
        goto L5246;
    STORE();
    s->bx += 0xE000;
    SET_AL(brd(s, s->bx));
    s->dx += s->si;
    goto L5272;

    /* y-major, y decreasing */
L52A7:
    MID_SETUP_UP(0x52AA);
    if (odd_row(s->bx))
        goto L5310;
L52B0:
    if (dec_g(&s->bp)) {
        HI_RD();
        STORE();
    } else {
        HCALL(h_5440, 0x52B6);
    }
    s->bx += 0x1FB0;
    if (add_l(&s->dx, s->si))
        goto L5310;
    s->dx += s->di;
L52CA:
    if (dec_g(&s->bp)) {
        LO_RD();
        STORE();
    } else {
        HCALL(h_544A, 0x52D0);
    }
    if (add_l(&s->dx, s->si))
        goto L52F1;
    s->bx += 0xE001;
    s->dx += s->di;
    goto L52B0;
L52E6:
    MID_SETUP_UP(0x52E9);
    if (odd_row(s->bx))
        goto L52CA;
    goto L52F5;
L52F1:
    s->bx += 0xE000;
L52F5:
    if (dec_g(&s->bp)) {
        LO_RD();
        STORE();
    } else {
        HCALL(h_544A, 0x52FB);
    }
    s->bx += 0x1FB0;
    if (add_l(&s->dx, s->si))
        goto L52CA;
    s->dx += s->di;
    s->bx++;
L5310:
    if (dec_g(&s->bp)) {
        HI_RD();
        STORE();
    } else {
        HCALL(h_5440, 0x5316);
    }
    s->bx += 0xE000;
    if (add_l(&s->dx, s->si))
        goto L52B0;
    s->dx += s->di;
    goto L52F5;
}

/* ---- C entry points --------------------------------------------------------------- */

void raster_line_regs(const RasterBus *b, RasterRegs *r)
{
    St s = { b, r->ax, r->bx, r->cx, r->dx, r->si, r->di, r->bp };
    line_normal(&s);
    *r = (RasterRegs){ s.ax, s.bx, s.cx, s.dx, s.si, s.di, s.bp };
}

void raster_poly_edge_regs(const RasterBus *b, RasterRegs *r)
{
    St s = { b, r->ax, r->bx, r->cx, r->dx, r->si, r->di, r->bp };
    line_poly(&s);
    *r = (RasterRegs){ s.ax, s.bx, s.cx, s.dx, s.si, s.di, s.bp };
}

void raster_line(const RasterBus *b, int x0, int y0, int x1, int y1)
{
    RasterRegs r = { 0, (uint16_t)x0, 0, 0, (uint16_t)x1, (uint16_t)y1, (uint16_t)y0 };
    if (b->dat_rd(b->ctx, 0x31D0))
        raster_poly_edge_regs(b, &r);
    else
        raster_line_regs(b, &r);
}

/* ---- natives ------------------------------------------------------------------------ */

typedef struct Bus {
    Pc *pc;
    uint16_t buf, dat; /* DS and ES inside draw_line */
    uint16_t ss, call_sp; /* where the helper CALLs push their return address */
} Bus;

static uint8_t bus_buf_rd(void *ctx, uint16_t o)
{
    Bus *b = ctx;
    return cpu_read8(&b->pc->cpu, cpu_linear(b->buf, o));
}
static void bus_buf_wr(void *ctx, uint16_t o, uint8_t v)
{
    Bus *b = ctx;
    cpu_write8(&b->pc->cpu, cpu_linear(b->buf, o), v);
}
static uint8_t bus_dat_rd(void *ctx, uint16_t o)
{
    Bus *b = ctx;
    return cpu_read8(&b->pc->cpu, cpu_linear(b->dat, o));
}
static void bus_dat_wr(void *ctx, uint16_t o, uint8_t v)
{
    Bus *b = ctx;
    cpu_write8(&b->pc->cpu, cpu_linear(b->dat, o), v);
}
static void bus_call(void *ctx, uint16_t ret)
{
    Bus *b = ctx;
    mem_write16(b->pc, b->ss, b->call_sp, ret);
}

/* draw_line with the CPU registers; ret_sp = SP while draw_line runs (its return address
 * on top). Sets DS and ES as the original leaves them. */
static void draw_line_cpu(Pc *pc, uint16_t ret_sp)
{
    Cpu8086 *c = &pc->cpu;
    bool poly = gs_poly_mode(pc) != 0;
    Bus bus = { pc, gs_view_buf_seg(pc), mem_read16(pc, 0, 0x120), c->sregs[S_SS],
                (uint16_t)(ret_sp - 2) };
    RasterBus rb = { &bus, bus_buf_rd, bus_buf_wr, bus_dat_rd, bus_dat_wr, bus_call };
    RasterRegs r = { c->regs[R_AX], c->regs[R_BX], c->regs[R_CX], c->regs[R_DX],
                     c->regs[R_SI], c->regs[R_DI], c->regs[R_BP] };
    c->sregs[S_DS] = bus.buf;
    c->sregs[S_ES] = bus.dat;
    if (poly)
        raster_poly_edge_regs(&rb, &r);
    else
        raster_line_regs(&rb, &r);
    c->regs[R_AX] = r.ax;
    c->regs[R_BX] = r.bx;
    c->regs[R_CX] = r.cx;
    c->regs[R_DX] = r.dx;
    c->regs[R_SI] = r.si;
    c->regs[R_DI] = r.di;
    c->regs[R_BP] = r.bp;
}

/* 0050:5691 draw_line */
static void n_draw_line(Pc *pc)
{
    draw_line_cpu(pc, pc->cpu.regs[R_SP]);
    native_ret(pc);
}

/* 0050:408F draw_line_list: AX -> {x0, y0, x1, y1} byte records, FF ends; then advances
 * scenery_ip (408C is scenery opcode 15). */
static void n_draw_line_list(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP];
    gs_set_line_list_ptr(pc, c->regs[R_AX]);
    for (;;) {
        uint16_t ds = c->sregs[S_DS];
        uint16_t bx = gs_line_list_ptr(pc);
        uint16_t ax = mem_read16(pc, ds, bx);
        c->regs[R_AX] = ax;
        c->regs[R_BX] = bx;
        if ((ax & 0xFF) == 0xFF)
            break;
        uint16_t cx = mem_read16(pc, ds, (uint16_t)(bx + 2));
        c->regs[R_CX] = cx;
        gs_set_line_list_ptr(pc, (uint16_t)(gs_line_list_ptr(pc) + 4));
        c->regs[R_BX] = ax & 0xFF;
        c->regs[R_BP] = ax >> 8;
        c->regs[R_SI] = cx & 0xFF;
        c->regs[R_DI] = cx >> 8;
        c->regs[R_AX] = cx >> 8;
        mem_write16(pc, c->sregs[S_SS], (uint16_t)(sp - 2), 0x40B9); /* call draw_line */
        draw_line_cpu(pc, (uint16_t)(sp - 2));
        c->regs[R_AX] = 0;
        c->sregs[S_DS] = mem_read16(pc, 0, 0x120);
    }
    gs_set_scenery_ip(pc, (uint16_t)(gs_scenery_ip(pc) + 1));
    native_ret(pc);
}

/* 0050:408C draw_horizon_list: draw_line_list with AX = 041F (horizon_list). Also scenery
 * opcode 15. */
static void n_draw_horizon_list(Pc *pc)
{
    pc->cpu.regs[R_AX] = 0x041F;
    n_draw_line_list(pc);
}

/* Cycles: draw_line costs about 283 + 72 per extra pixel (measured 283..11825); the mean
 * line in the campaign is 16-18 pixels. The two lists draw fixed shapes (war-mode lines
 * 12300, horizon outline 7109). Flags are not used by any caller. */
NativeEntry native_line[] = {
    { .name = "draw_line", .seg = GAME_CS, .off = 0x5691, .fn = n_draw_line, .enabled = true, .cycles = 1400 },
    { .name = "draw_line_list", .seg = GAME_CS, .off = 0x408F, .fn = n_draw_line_list, .enabled = true,
      .cycles = 12300 },
    { .name = "draw_horizon_list", .seg = GAME_CS, .off = 0x408C, .fn = n_draw_horizon_list, .enabled = true,
      .cycles = 7109 },
    { .name = NULL },
};
