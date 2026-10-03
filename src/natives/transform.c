/* Natives for subphase 3.6 3D transform and clipping (see docs/PHASE3_PLAN.md and
 * docs/subphases/3.6.md).
 *
 * All memory goes through DS/SS as the original's does (some callers run with a pushed
 * DS). Stack slots below SP that the original writes with CALL/PUSH are written with the
 * same values, since --verify compares every byte the original writes. No caller relies
 * on the flags these routines leave, so no flag_mask is declared. */
#include "native.h"
#include "game/state.h"

#include <SDL3/SDL.h>

/* eye-space points (view_matrix+18.. and +30..) */
#define P1_X 0x310F
#define P1_Y 0x3111
#define P1_Z 0x3113
#define P2_X 0x311B
#define P2_Y 0x311D
#define P2_Z 0x311F
#define OUT_P1 0x315E
#define OUT_P2 0x315F
#define SINE_TABLE 0x3381

typedef struct Regs {
    uint16_t ax, bx, cx, dx, si, di, bp;
} Regs;

static inline uint16_t rd(Pc *pc, uint16_t off) { return mem_read16(pc, pc->cpu.sregs[S_DS], off); }
static inline void wr(Pc *pc, uint16_t off, uint16_t v) { mem_write16(pc, pc->cpu.sregs[S_DS], off, v); }
static inline uint8_t rd8(Pc *pc, uint16_t off)
{
    return cpu_read8(&pc->cpu, cpu_linear(pc->cpu.sregs[S_DS], off));
}
static inline void wr8(Pc *pc, uint16_t off, uint8_t v)
{
    cpu_write8(&pc->cpu, cpu_linear(pc->cpu.sregs[S_DS], off), v);
}
/* A word the original leaves on the stack below SP (return address or PUSH). */
static inline void stk(Pc *pc, uint16_t sp, uint16_t v) { mem_write16(pc, pc->cpu.sregs[S_SS], sp, v); }

static inline uint16_t sar16(uint16_t v, int n) { return (uint16_t)((int16_t)v >> n); }
static inline uint32_t sar32(uint32_t v, int n) { return (uint32_t)((int32_t)v >> n); }
static inline uint8_t hi8(uint16_t v) { return (uint8_t)(v >> 8); }

static void load_regs(const Cpu8086 *c, Regs *r)
{
    *r = (Regs){ c->regs[R_AX], c->regs[R_BX], c->regs[R_CX], c->regs[R_DX],
                 c->regs[R_SI], c->regs[R_DI], c->regs[R_BP] };
}

static void store_regs(Cpu8086 *c, const Regs *r)
{
    c->regs[R_AX] = r->ax;
    c->regs[R_BX] = r->bx;
    c->regs[R_CX] = r->cx;
    c->regs[R_DX] = r->dx;
    c->regs[R_SI] = r->si;
    c->regs[R_DI] = r->di;
    c->regs[R_BP] = r->bp;
}

/* Runs original code (natives off) from the current CS:IP until it reaches GAME_CS:ret_ip
 * with SP == sp_end. Used to hand a call on to routines owned by other areas
 * (plot_pixel_es) and for the rare paths not reimplemented here. */
static void run_original_until(Pc *pc, uint16_t ret_ip, uint16_t sp_end)
{
    Cpu8086 *c = &pc->cpu;
    const uint8_t *map = c->hook_map;
    c->hook_map = NULL;
    long n = 0;
    while (!(c->ip == ret_ip && c->sregs[S_CS] == GAME_CS && c->regs[R_SP] == sp_end)) {
        native_or_cpu_step(pc);
        if (++n == 5000000L) {
            SDL_Log("transform: nested original run did not return to %04X", ret_ip);
            break;
        }
    }
    c->hook_map = map;
}

/* ---- sin/cos (bit-exact copies of 0050:45FD and 0050:45C0) -----------------------
 * Private so this area does not depend on 3.1; can be swapped for src/natives/fixmath.h
 * after merge if that header also writes the stack slots. */

/* 45FD sin_quadrant: BX = 0..4000h -> AX; sets BX, DX, SI, DI like the original. */
static void sin_quadrant(Pc *pc, Regs *r)
{
    uint16_t x = r->bx;
    uint16_t bx = (uint16_t)(((uint16_t)(x << 1) >> 8) << 1);
    uint16_t si = rd(pc, (uint16_t)(bx + SINE_TABLE));
    uint16_t di = (uint16_t)(rd(pc, (uint16_t)(bx + SINE_TABLE + 2)) - si);
    uint32_t p = (uint32_t)((int32_t)(int16_t)(x & 0x7F) * (int16_t)di) << 1; /* IMUL, SHL/RCL */
    uint16_t dx = (uint16_t)(p >> 16);
    r->ax = (uint16_t)((uint16_t)((dx & 0xFF) << 8 | (uint16_t)p >> 8) + si);
    r->bx = bx;
    r->dx = dx;
    r->si = si;
    r->di = di;
}

/* 45C0 sincos: BX = angle -> AX, CX; clobbers BX, BP, SI, DI, DX. sp = SP at entry
 * (pointing at its return address); the calls it makes write below it. */
static void sincos_q15(Pc *pc, Regs *r, uint16_t sp)
{
    r->bp = r->bx;
    if (r->bx & 0x8000) {
        r->bx = (uint16_t)-r->bx;
        if (r->bx & 0x8000)
            r->bx = 0x7FFF;
        stk(pc, (uint16_t)(sp - 2), 0x45FA);
        sincos_q15(pc, r, (uint16_t)(sp - 2));
        r->ax = (uint16_t)-r->ax;
    } else if (r->bx <= 0x4000) {
        stk(pc, (uint16_t)(sp - 2), 0x45E5);
        sin_quadrant(pc, r);
        r->cx = r->ax;
        r->bx = (uint16_t)(0x4000 - r->bp);
        stk(pc, (uint16_t)(sp - 2), 0x45EF);
        sin_quadrant(pc, r);
    } else {
        r->bx = (uint16_t)-(uint16_t)(r->bx + 0x8000);
        stk(pc, (uint16_t)(sp - 2), 0x45D5);
        sin_quadrant(pc, r);
        r->ax = (uint16_t)-r->ax;
        r->cx = r->ax;
        r->bx = (uint16_t)(r->bp - 0x4000);
        sin_quadrant(pc, r); /* JMP: no return address */
    }
}

/* IMUL then SHL AX,1 / RCL DX,1: the Q15 product, high word returned, low word in *lo. */
static uint16_t mulq(uint16_t a, uint16_t b, uint16_t *lo)
{
    uint32_t p = (uint32_t)((int32_t)(int16_t)a * (int16_t)b) << 1;
    if (lo)
        *lo = (uint16_t)p;
    return (uint16_t)(p >> 16);
}

/* ---- build_view_matrix 0050:4621 ------------------------------------------------- */

/* 4718 select_view_angles: view_angles 30F7/30F9/30FB from view_pitch/bank/heading
 * (30F1/30F3/30F5) for the radar view or the view direction handler at [059B]. Only
 * memory matters to build_view_matrix: sincos overwrites every register it sets. */
static void select_view_angles(Pc *pc, uint16_t sp)
{
    stk(pc, (uint16_t)(sp - 2), 0x4624);
    uint16_t pitch = (uint16_t)gs_view_pitch(pc), bank = (uint16_t)gs_view_bank(pc), hdg = (uint16_t)gs_view_heading(pc);
    uint16_t a0, a2, a4;
    if (gs_radar_view(pc)) { /* radar_view: look down */
        a0 = 0x4000;
        a2 = 0;
        a4 = hdg;
    } else {
        switch (rd(pc, 0x059B)) {
        case 0x4737: a4 = hdg; a0 = pitch; a2 = bank; break;                                        /* ahead */
        case 0x474A: a4 = (uint16_t)(hdg + 0x1000); a0 = pitch; a2 = bank; break;
        case 0x47B5: a4 = (uint16_t)(hdg + 0xF000); a0 = pitch; a2 = bank; break;
        case 0x4755: a0 = (uint16_t)-bank; a2 = (uint16_t)(pitch - 0x300); a4 = (uint16_t)(hdg + 0x4000); break;
        case 0x479A: a0 = bank; a2 = (uint16_t)-(uint16_t)(pitch - 0x300); a4 = (uint16_t)(hdg + 0xC000); break;
        case 0x4770: a4 = (uint16_t)(hdg + 0x6000); a0 = (uint16_t)-pitch; a2 = (uint16_t)-bank; break;
        case 0x478A: a4 = (uint16_t)(hdg + 0x8000); a0 = (uint16_t)-pitch; a2 = (uint16_t)-bank; break;
        case 0x4792: a4 = (uint16_t)(hdg + 0xA000); a0 = (uint16_t)-pitch; a2 = (uint16_t)-bank; break;
        case 0x47C1: a0 = (uint16_t)(pitch + 0x4000); a2 = bank; a4 = hdg; break;                    /* down */
        default: { /* unknown handler: run it as original code (JMP AX) */
            Cpu8086 *c = &pc->cpu;
            uint16_t ip = c->ip, s = c->regs[R_SP], ax = c->regs[R_AX];
            c->regs[R_AX] = c->ip = rd(pc, 0x059B);
            c->regs[R_SP] = (uint16_t)(sp - 2);
            run_original_until(pc, 0x4624, sp);
            c->ip = ip;
            c->regs[R_SP] = s;
            c->regs[R_AX] = ax;
            return;
        }
        }
    }
    gs_set_view_angles_w(pc, 0, a0);
    gs_set_view_angles_w(pc, 1, a2);
    gs_set_view_angles_w(pc, 2, a4);
}

static void n_build_view_matrix(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP], s1 = (uint16_t)(sp - 2);
    Regs r;
    load_regs(c, &r);
    select_view_angles(pc, sp);

    r.bx = gs_view_angles_w(pc, 0);
    stk(pc, s1, 0x462B);
    sincos_q15(pc, &r, s1);
    uint16_t v3143 = r.ax, v3149 = r.cx;
    wr(pc, 0x3143, v3143);
    wr(pc, 0x3149, v3149);
    gs_set_view_matrix_w(pc, 5, (uint16_t)-r.ax);

    r.bx = gs_view_angles_w(pc, 1);
    stk(pc, s1, 0x463E);
    sincos_q15(pc, &r, s1);
    uint16_t v3145 = r.ax, v314B = r.cx;
    wr(pc, 0x3145, v3145);
    wr(pc, 0x314B, v314B);
    gs_set_view_matrix_w(pc, 3, mulq(v3145, v3149, NULL));
    gs_set_view_matrix_w(pc, 4, mulq(v314B, v3149, NULL));

    r.bx = gs_view_angles_w(pc, 2);
    stk(pc, s1, 0x4669);
    sincos_q15(pc, &r, s1);
    uint16_t v3147 = r.ax, v3157 = r.cx;
    wr(pc, 0x3147, v3147);
    wr(pc, 0x3157, v3157);
    gs_set_view_matrix_w(pc, 2, mulq(v3147, v3149, NULL));
    gs_set_view_matrix_w(pc, 8, mulq(v3149, v3157, NULL));
    uint16_t v3151 = mulq(v3157, v3145, NULL);
    wr(pc, 0x3151, v3151);
    uint16_t v314D = mulq(v3157, v314B, NULL);
    wr(pc, 0x314D, v314D);
    uint16_t v3155 = mulq(v314D, v3143, NULL);
    wr(pc, 0x3155, v3155);
    uint16_t v314F = mulq(v3147, v3145, NULL);
    wr(pc, 0x314F, v314F);
    gs_set_view_matrix_w(pc, 0, (uint16_t)(mulq(v314F, v3143, NULL) + v314D));
    gs_set_view_matrix_w(pc, 7, (uint16_t)(v314F + v3155));
    uint16_t v3153 = mulq(v3147, v314B, NULL);
    wr(pc, 0x3153, v3153);
    gs_set_view_matrix_w(pc, 1, (uint16_t)(mulq(v3143, v3153, NULL) - v3151));
    r.dx = (uint16_t)(mulq(v3151, v3143, &r.ax) - v3153);
    gs_set_view_matrix_w(pc, 6, r.dx);

    store_regs(c, &r);
    native_ret(pc);
}

/* ---- rotate_point 0050:481F ------------------------------------------------------
 * (3139,313B,313D) x view_matrix with 32-bit sums. Rows come back as BX:SI (x), CX:DI
 * (y, doubled) and DX:BP (z, halved). Unless bit 0 of [3380] was set, the three are then
 * shifted left 3 bits at a time (a 1 enters SI's bottom) until one high byte leaves
 * -4..3. */
static uint32_t row(uint16_t x, uint16_t y, uint16_t z, uint16_t a, uint16_t b, uint16_t cc)
{
    return (uint32_t)((int32_t)(int16_t)x * (int16_t)a) + (uint32_t)((int32_t)(int16_t)y * (int16_t)b) +
           (uint32_t)((int32_t)(int16_t)z * (int16_t)cc);
}

static void n_rotate_point(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP];
    uint16_t x = gs_xform_in_w(pc, 0), y = gs_xform_in_w(pc, 1), z = gs_xform_in_w(pc, 2);
    uint32_t r0 = sar32(row(x, y, z, gs_view_matrix_w(pc, 0), gs_view_matrix_w(pc, 3), gs_view_matrix_w(pc, 6)) << 1, 1);
    uint32_t r1 = row(x, y, z, gs_view_matrix_w(pc, 1), gs_view_matrix_w(pc, 4), gs_view_matrix_w(pc, 7)) << 1;
    uint32_t r2 = sar32(row(x, y, z, gs_view_matrix_w(pc, 2), gs_view_matrix_w(pc, 5), gs_view_matrix_w(pc, 8)) << 1, 2);
    stk(pc, (uint16_t)(sp - 2), (uint16_t)(r0 >> 16));
    stk(pc, (uint16_t)(sp - 4), (uint16_t)r0);
    stk(pc, (uint16_t)(sp - 6), (uint16_t)(r1 >> 16));
    stk(pc, (uint16_t)(sp - 8), (uint16_t)r1);
    uint16_t ax = (uint16_t)r2;

    uint8_t f = gs_no_normalise_queue(pc);
    gs_set_no_normalise_queue(pc, f >> 1);
    if (!(f & 1)) {
        for (;;) {
            uint8_t ah = (uint8_t)((uint8_t)(hi8((uint16_t)(r0 >> 16)) + 4) | (uint8_t)(hi8((uint16_t)(r1 >> 16)) + 4) |
                                   (uint8_t)(hi8((uint16_t)(r2 >> 16)) + 4)) & 0xF8;
            ax = (uint16_t)(ah << 8 | (uint8_t)(hi8((uint16_t)(r2 >> 16)) + 4));
            if (ah)
                break;
            r0 = (r0 << 1 | 1) << 2;
            r1 <<= 3;
            r2 <<= 3;
        }
    }
    c->regs[R_AX] = ax;
    c->regs[R_BX] = (uint16_t)(r0 >> 16);
    c->regs[R_SI] = (uint16_t)r0;
    c->regs[R_CX] = (uint16_t)(r1 >> 16);
    c->regs[R_DI] = (uint16_t)r1;
    c->regs[R_DX] = (uint16_t)(r2 >> 16);
    c->regs[R_BP] = (uint16_t)r2;
    native_ret(pc);
}

/* ---- world_to_eye_delta 0050:48E8 ------------------------------------------------
 * BX -> scenery point (byte opcode, then x, y, z words; y is replaced by [315B] when bit 0
 * of [315D] is set). Stores point - eye_pos (30EB..) scaled to fit in 3139/313B/313D:
 * on overflow the components are divided by 32 (the overflowed one recovered via CF),
 * otherwise by 2 or 4 when large, or multiplied by 8 until large enough. */
static uint16_t lodsw(Pc *pc, uint16_t *si)
{
    uint16_t v = rd(pc, *si);
    *si = (uint16_t)(*si + ((pc->cpu.flags & F_DF) ? -2 : 2));
    return v;
}

/* SUB a,b; OF? -> on overflow, CMC / RCR 1 / SAR 4: the true 17-bit difference / 32. */
static bool sub_ovf(uint16_t a, uint16_t b, uint16_t *res)
{
    uint16_t r = (uint16_t)(a - b);
    if (!(((a ^ b) & (a ^ r)) & 0x8000)) {
        *res = r;
        return false;
    }
    bool cf = a < b;
    *res = sar16((uint16_t)((cf ? 0 : 0x8000) | r >> 1), 4);
    return true;
}

static void n_world_to_eye_delta(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t si = (uint16_t)(c->regs[R_BX] + 1);
    uint16_t ax, bx, cx, dx = c->regs[R_DX];
    uint16_t e0 = gs_eye_pos_w(pc, 0), e1 = gs_eye_pos_w(pc, 1), e2 = gs_eye_pos_w(pc, 2);

    if (sub_ovf(lodsw(pc, &si), e0, &ax)) { /* 49A2: x overflow */
        bx = ax;
        ax = gs_flat_y(pc);
        uint8_t f = gs_flat_flag(pc);
        gs_set_flat_flag(pc, 0);
        if (f != 1)
            ax = lodsw(pc, &si);
        dx = sar16(e1, 1);
        cx = sar16((uint16_t)(sar16(ax, 1) - dx), 4);
        goto z_scaled;
    }
    bx = ax;
    ax = gs_flat_y(pc);
    {
        uint8_t f = gs_flat_flag(pc);
        gs_set_flat_flag(pc, f >> 1);
        if (!(f & 1))
            ax = lodsw(pc, &si);
    }
    if (sub_ovf(ax, e1, &ax)) { /* 49D5: y overflow */
        cx = ax;
        bx = sar16(bx, 5);
        goto z_scaled;
    }
    cx = ax;
    if (sub_ovf(lodsw(pc, &si), e2, &ax)) { /* 494B: z overflow */
        cx = sar16(cx, 5);
        bx = sar16(bx, 5);
        goto store;
    }
    {
        uint8_t dh = (uint8_t)((uint8_t)(hi8(bx) + 0x20) | (uint8_t)(hi8(cx) + 0x20) | (uint8_t)(hi8(ax) + 0x20));
        uint8_t dl = (uint8_t)(hi8(ax) + 0x20);
        gs_set_clip_steps(pc, 0x0A);
        if (dh & 0xC0) {
            dh &= 0xE0;
            int n = (dh == 0x40 || dh == 0xE0) ? 1 : 2;
            bx = sar16(bx, n);
            cx = sar16(cx, n);
            ax = sar16(ax, n);
        } else {
            for (;;) {
                dh = (uint8_t)((uint8_t)(hi8(bx) + 4) | (uint8_t)(hi8(cx) + 4) | (uint8_t)(hi8(ax) + 4)) & 0xF8;
                dl = (uint8_t)(hi8(ax) + 4);
                if (dh)
                    break;
                if (!(bx | cx | ax)) {
                    /* the point is the eye: the original loops at 497E forever; do the same */
                    c->regs[R_AX] = ax;
                    c->regs[R_BX] = bx;
                    c->regs[R_CX] = cx;
                    c->regs[R_SI] = si;
                    c->ip = 0x497E;
                    return;
                }
                bx = (uint16_t)(bx << 3);
                cx = (uint16_t)(cx << 3);
                ax = (uint16_t)(ax << 3);
            }
        }
        dx = (uint16_t)(dh << 8 | dl);
        goto store;
    }
z_scaled: /* 49EC */
    ax = lodsw(pc, &si);
    dx = sar16(e2, 1);
    ax = sar16((uint16_t)(sar16(ax, 1) - dx), 4);
store: /* 4996 */
    gs_set_xform_in_w(pc, 0, bx);
    gs_set_xform_in_w(pc, 1, cx);
    gs_set_xform_in_w(pc, 2, ax);
    c->regs[R_AX] = ax;
    c->regs[R_BX] = bx;
    c->regs[R_CX] = cx;
    c->regs[R_DX] = dx;
    c->regs[R_SI] = si;
    native_ret(pc);
}

/* ---- outcodes 0050:4A1A / 0050:4A4E ------------------------------------------------
 * Bits 3..0: x < -z, x > z, y < -z, y > z. Sets BX, CX, DX = x, y, z, SI = -z, AL. */
static void outcode(Pc *pc, Regs *r, uint16_t p, uint16_t out)
{
    r->bx = rd(pc, p);
    r->cx = rd(pc, (uint16_t)(p + 2));
    r->dx = rd(pc, (uint16_t)(p + 4));
    r->si = (uint16_t)-r->dx;
    int16_t x = (int16_t)r->bx, y = (int16_t)r->cx, z = (int16_t)r->dx, nz = (int16_t)r->si;
    uint8_t al = (uint8_t)((x < nz) << 3 | (x > z) << 2 | (y < nz) << 1 | (y > z));
    r->ax = (uint16_t)((r->ax & 0xFF00) | al);
    wr8(pc, out, al);
}

static void n_outcode_p1(Pc *pc)
{
    Regs r;
    load_regs(&pc->cpu, &r);
    outcode(pc, &r, P1_X, OUT_P1);
    store_regs(&pc->cpu, &r);
    native_ret(pc);
}

static void n_outcode_p2(Pc *pc)
{
    Regs r;
    load_regs(&pc->cpu, &r);
    outcode(pc, &r, P2_X, OUT_P2);
    store_regs(&pc->cpu, &r);
    native_ret(pc);
}

/* ---- clipping 0050:4AF7 (point 1) and 0050:4BCD (point 2) ---------------------------
 * One step of Cohen-Sutherland: point P is moved onto one plane (y = -z, y = z, x = -z
 * or x = z, picked from its outcode) along the line to Q, then its outcode is redone.
 * The intersection helpers 4A82 (P = 1) and 4AAB (P = 2) are the same routine:
 * BX = denominator, DX = numerator, CX = Q's other coordinate, SI = P's. t = DX/2/BX in
 * Q15 (IDIV), SI += t*(CX-SI), DX = zP + t*(zQ-zP); then DX:DI and SI:CX are shifted
 * left together (up to 16 times) while both high bytes stay in -16..15. */

typedef struct ClipPoint {
    uint16_t x, y, z, out;             /* this point's words and outcode byte */
    uint16_t helper_ip;                /* its intersection helper */
    uint16_t ret[4];                   /* return addresses of the four helper calls */
} ClipPoint;

static const ClipPoint clip_pt1 = { P1_X, P1_Y, P1_Z, OUT_P1, 0x4A82, { 0x4B26, 0x4B58, 0x4B8C, 0x4BBE } };
static const ClipPoint clip_pt2 = { P2_X, P2_Y, P2_Z, OUT_P2, 0x4AAB, { 0x4BFC, 0x4C2E, 0x4C62, 0x4C94 } };

static void clip_point(Pc *pc, const ClipPoint *p, const ClipPoint *q)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP];
    Regs r;
    load_regs(c, &r);
    uint8_t al = rd8(pc, p->out);
    r.ax = (uint16_t)((r.ax & 0xFF00) | al);
    bool on_x = (int8_t)al > 3;                  /* x planes, else y planes */
    bool neg = !(al & (on_x ? 4 : 1));           /* plane a = -z, else a = z */
    uint16_t pa = on_x ? p->x : p->y, po = on_x ? p->y : p->x;
    uint16_t qa = on_x ? q->x : q->y, qo = on_x ? q->y : q->x;
    uint16_t aP = rd(pc, pa), zP = rd(pc, p->z), aQ = rd(pc, qa), zQ = rd(pc, q->z);
    int path = (on_x ? 2 : 0) + (neg ? 0 : 1);
    if (neg) {
        r.bx = (uint16_t)(aP + zP - aQ - zQ);
        r.dx = (uint16_t)(zP + aP);
    } else {
        r.bx = (uint16_t)(aQ + zP - aP - zQ);
        r.dx = (uint16_t)(zP - aP);
    }
    r.cx = rd(pc, qo);
    r.si = rd(pc, po);
    stk(pc, (uint16_t)(sp - 2), p->ret[path]);

    /* intersection helper */
    int32_t n = (int32_t)((uint32_t)r.dx << 16) >> 1; /* XOR AX,AX / SAR DX,1 / RCR AX,1 */
    int32_t q15 = r.bx ? n / (int16_t)r.bx : 0;
    if (!r.bx || q15 > 32767 || q15 < -32768) {
        /* Divide overflow (t = 1 when Q lies on the plane): the IDIV raises INT 0, whose
         * handler (4DC6) saturates AX to +-7FFF and IRETs; it also leaves FLAGS/CS/IP
         * below SP. Run the helper as original code for that. */
        store_regs(c, &r);
        c->regs[R_SP] = (uint16_t)(sp - 2);
        c->ip = p->helper_ip;
        run_original_until(pc, p->ret[path], sp);
        load_regs(c, &r);
    } else {
        uint16_t t = (uint16_t)q15;
        r.cx = (uint16_t)(r.cx - r.si);
        r.si = (uint16_t)(r.si + mulq(t, r.cx, &r.cx));
        r.dx = (uint16_t)(mulq((uint16_t)(zQ - zP), t, &r.di) + zP);
        r.bp = 0x10;
        for (;;) {
            r.ax = (uint16_t)(r.dx + 0x1000);
            if (hi8(r.ax) > 0x1F)
                break;
            r.ax = (uint16_t)(r.si + 0x1000);
            if (hi8(r.ax) > 0x1F)
                break;
            r.dx = (uint16_t)(r.dx << 1 | r.di >> 15);
            r.di = (uint16_t)(r.di << 1);
            r.si = (uint16_t)(r.si << 1 | r.cx >> 15);
            r.cx = (uint16_t)(r.cx << 1);
            if (--r.bp == 0)
                break;
        }
    }

    wr(pc, p->z, r.dx);
    wr(pc, po, r.si);
    wr(pc, pa, neg ? (uint16_t)-r.dx : r.dx);
    outcode(pc, &r, p->x, p->out); /* JMP outcode_pN */
    store_regs(c, &r);
    native_ret(pc);
}

static void n_clip_p1_plane(Pc *pc) { clip_point(pc, &clip_pt1, &clip_pt2); }
static void n_clip_line_planes(Pc *pc) { clip_point(pc, &clip_pt2, &clip_pt1); }

/* ---- project_dot 0050:3D1A -------------------------------------------------------
 * Point 1 (eye space) -> screen: nothing unless z > 1 and |x|, |y| < z (ones' complement
 * for negatives). sy = ((y*8000h/z) >> 8) * [3168] * 2 >> 8 + [316A], sx likewise with
 * [3167]/[3169]. Then a JMP to plot_pixel_es (AH = sx, CL = sy), run here as original
 * code, or with capture_mode [30CA] set, an FE,sx,sy record at
 * [30C6] (advanced by 3). */
/* The entries' .cycles cover the usual early exit; the two divides cost this much more. */
#define PROJECT_DIVIDE_CYCLES 670

static uint16_t project_axis(uint16_t v, uint16_t z, uint8_t scale, uint8_t centre, uint16_t *rem)
{
    int32_t n = (int32_t)((uint32_t)v << 16) >> 1;
    int16_t q = (int16_t)(n / (int16_t)z); /* |v| < z: no divide error */
    *rem = (uint16_t)(int16_t)(n % (int16_t)z);
    uint16_t ax = (uint16_t)((int16_t)(int8_t)hi8((uint16_t)q) * (int8_t)scale); /* MOV AL,AH / IMUL byte */
    ax = (uint16_t)(ax << 1);
    return (uint16_t)((uint16_t)((uint8_t)(hi8(ax) + centre) << 8) | (ax & 0xFF));
}

static void project(Pc *pc, uint16_t x, uint16_t y, uint16_t z)
{
    Cpu8086 *c = &pc->cpu;
    c->regs[R_BX] = x;
    c->regs[R_CX] = y;
    uint16_t zm1 = (uint16_t)(z - 1);
    c->regs[R_BP] = zm1;
    if ((int16_t)z <= 1) {
        native_ret(pc);
        return;
    }
    uint16_t ax = (x & 0x8000) ? (uint16_t)~x : x; /* CWD / XOR AX,DX */
    c->regs[R_AX] = ax;
    c->regs[R_DX] = (x & 0x8000) ? 0xFFFF : 0;
    if ((int16_t)ax >= (int16_t)zm1) {
        native_ret(pc);
        return;
    }
    ax = (y & 0x8000) ? (uint16_t)~y : y;
    c->regs[R_AX] = ax;
    c->regs[R_DX] = (y & 0x8000) ? 0xFFFF : 0;
    if ((int16_t)ax >= (int16_t)zm1) {
        native_ret(pc);
        return;
    }
    c->regs[R_BP] = z;
    c->cycles += PROJECT_DIVIDE_CYCLES;
    uint16_t rem;
    uint16_t sy = project_axis(y, z, gs_proj_params_b(pc, 1), gs_proj_params_b(pc, 3), &rem);
    uint16_t cx = (uint16_t)((y & 0xFF00) | hi8(sy));
    uint16_t sx = project_axis(x, z, gs_proj_params_b(pc, 0), gs_proj_params_b(pc, 2), &rem);
    c->regs[R_AX] = sx;
    c->regs[R_DX] = rem;
    if (!gs_capture_mode(pc)) {
        c->regs[R_CX] = cx;
        uint16_t sp = c->regs[R_SP];
        uint16_t ret = mem_read16(pc, c->sregs[S_SS], sp);
        c->ip = 0x5660; /* JMP plot_pixel_es; its RET returns to our caller */
        run_original_until(pc, ret, (uint16_t)(sp + 2)); /* charges plot_pixel_es's cycles */
        return;
    }
    uint16_t bx = gs_capture_ptr(pc);
    wr8(pc, bx, 0xFE);
    cx = (uint16_t)(hi8(sx) | (cx & 0xFF) << 8); /* MOV CH,AH / XCHG CL,CH */
    wr(pc, (uint16_t)(bx + 1), cx);
    gs_set_capture_ptr(pc, (uint16_t)(bx + 3));
    c->regs[R_BX] = bx;
    c->regs[R_CX] = cx;
    native_ret(pc);
}

static void n_project_dot(Pc *pc) { project(pc, rd(pc, P1_X), rd(pc, P1_Y), rd(pc, P1_Z)); }

/* 3D26: second entry with BX, CX, BP = x, y, z (JMP from 41DD, scenery point records). */
static void n_project_dot_regs(Pc *pc)
{
    project(pc, pc->cpu.regs[R_BX], pc->cpu.regs[R_CX], pc->cpu.regs[R_BP]);
}

NativeEntry native_transform[] = {
    { .name = "build_view_matrix", .seg = GAME_CS, .off = 0x4621, .fn = n_build_view_matrix, .enabled = true,
      .cycles = 4550 },
    { .name = "rotate_point", .seg = GAME_CS, .off = 0x481F, .fn = n_rotate_point, .enabled = true, .cycles = 1800 },
    { .name = "world_to_eye_delta", .seg = GAME_CS, .off = 0x48E8, .fn = n_world_to_eye_delta, .enabled = true,
      .cycles = 420 },
    { .name = "outcode_p1", .seg = GAME_CS, .off = 0x4A1A, .fn = n_outcode_p1, .enabled = true, .cycles = 190 },
    { .name = "outcode_p2", .seg = GAME_CS, .off = 0x4A4E, .fn = n_outcode_p2, .enabled = true, .cycles = 190 },
    { .name = "clip_p1_plane", .seg = GAME_CS, .off = 0x4AF7, .fn = n_clip_p1_plane, .enabled = true,
      .cycles = 1300 },
    { .name = "clip_line_planes", .seg = GAME_CS, .off = 0x4BCD, .fn = n_clip_line_planes, .enabled = true,
      .cycles = 1300 },
    { .name = "project_dot", .seg = GAME_CS, .off = 0x3D1A, .fn = n_project_dot, .enabled = true, .cycles = 93 },
    { .name = "project_dot_regs", .seg = GAME_CS, .off = 0x3D26, .fn = n_project_dot_regs, .enabled = true,
      .cycles = 50 },
    { .name = NULL },
};
