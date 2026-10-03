/* Natives for subphase 3.1 maths helpers (see docs/PHASE3_PLAN.md and docs/subphases/3.1.md). */
#include "native.h"
#include "fixmath.h"

/* ---- flags, as the CPU core computes them --------------------------------------- */

#define ARITH_FLAGS (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

static void set_flags(Cpu8086 *c, uint16_t mask, uint16_t value)
{
    c->flags = (uint16_t)((c->flags & ~mask) | (value & mask));
}

static uint16_t szp(uint16_t r)
{
    uint8_t p = (uint8_t)r;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return (uint16_t)((r == 0 ? F_ZF : 0) | ((r & 0x8000) ? F_SF : 0) | ((p & 1) ? 0 : F_PF));
}

/* ADD a,b */
static void flags_add(Cpu8086 *c, uint16_t a, uint16_t b)
{
    uint32_t r32 = (uint32_t)a + b;
    uint16_t r = (uint16_t)r32;
    uint16_t f = szp(r);
    if (r32 > 0xFFFF)
        f |= F_CF;
    if ((r ^ a) & (r ^ b) & 0x8000)
        f |= F_OF;
    if ((a ^ b ^ r) & 0x10)
        f |= F_AF;
    set_flags(c, ARITH_FLAGS, f);
}

/* SUB/CMP a,b (NEG x is SUB 0,x) */
static void flags_sub(Cpu8086 *c, uint16_t a, uint16_t b)
{
    uint16_t r = (uint16_t)(a - b);
    uint16_t f = szp(r);
    if (a < b)
        f |= F_CF;
    if ((a ^ b) & (a ^ r) & 0x8000)
        f |= F_OF;
    if ((a ^ b ^ r) & 0x10)
        f |= F_AF;
    set_flags(c, ARITH_FLAGS, f);
}

/* OR r,r: CF = OF = AF = 0 */
static void flags_logic(Cpu8086 *c, uint16_t r)
{
    set_flags(c, ARITH_FLAGS, szp(r));
}

/* SHR v,1: AF is left alone */
static void flags_shr1(Cpu8086 *c, uint16_t v)
{
    uint16_t f = szp((uint16_t)(v >> 1));
    if (v & 1)
        f |= F_CF;
    if (v & 0x8000)
        f |= F_OF;
    set_flags(c, (uint16_t)(ARITH_FLAGS & ~F_AF), f);
}

/* ---- cycles ---------------------------------------------------------------------
 * The original's cost depends on the path. Each entry's .cycles is its cheapest path;
 * the native adds the difference for the path it takes, so runs with the natives on
 * keep the original's emulated timeline. Measured against the original on the CPU core. */

#define SINCOS_CYCLES 597  /* 4000h < angle < 8000h */
#define DIV_Q15_CYCLES 54  /* CX = 0, DX < 0 */

static uint32_t sincos_cycles(uint16_t angle)
{
    if (angle == 0x8000)
        return 84 + 597; /* NEG, JNS not taken, MOV BX,7FFFh, CALL, NEG AX, RET */
    if (angle & 0x8000)
        return 92 + sincos_cycles((uint16_t)-angle);
    return angle <= 0x4000 ? 616 : 597;
}

/* [dividend < 0][divisor < 0][0 = saturated, 1 = divided] */
static const uint16_t div_q15_cycles[2][2][2] = {
    { { 84, 226 }, { 94, 241 } },
    { { 122, 269 }, { 115, 257 } },
};

/* ---- natives -------------------------------------------------------------------- */

/* 0050:45FD sin_quadrant: BX = 0..4000h -> AX (table at DS:3381, linear interpolation).
 * Clobbers BX, DX, SI, DI; flags from the final ADD AX,SI. */
static void n_sin_quadrant(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    FxQuadrant q = fx_quadrant_regs(pc, c->sregs[S_DS], c->regs[R_BX]);
    flags_add(c, (uint16_t)(q.ax - q.si), q.si);
    c->regs[R_AX] = q.ax;
    c->regs[R_BX] = q.bx;
    c->regs[R_DX] = q.dx;
    c->regs[R_SI] = q.si;
    c->regs[R_DI] = q.di;
    native_ret(pc);
}

/* 0050:45C0 sincos: BX = angle -> AX = sin, CX = cos (Q15). Clobbers BX, DX, SI, DI and
 * BP (= the angle, or its negation for angles >= 8000h); flags from the last ADD or NEG. */
static void n_sincos(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t angle = c->regs[R_BX];
    FxSincos r = fx_sincos_regs(pc, c->sregs[S_DS], angle);
    c->cycles += sincos_cycles(angle) - SINCOS_CYCLES;

    /* The original's inner CALLs leave their return addresses below SP; write the
     * last one at each depth so the stack bytes match too. */
    uint16_t ss = c->sregs[S_SS], sp = c->regs[R_SP];
    if (angle & 0x8000) {
        sp = (uint16_t)(sp - 2);
        mem_write16(pc, ss, sp, 0x45FA); /* CALL sincos at 45F7 */
        angle = (uint16_t)-angle;
    }
    /* last CALL sin_quadrant: at 45EC (angle <= 4000h) or 45D2 (then JMP) */
    mem_write16(pc, ss, (uint16_t)(sp - 2), (angle & 0x8000) || angle > 0x4000 ? 0x45D5 : 0x45EF);

    if (r.neg_ax)
        flags_sub(c, 0, r.ax_pre);
    else
        flags_add(c, (uint16_t)(r.ax - r.si), r.si);
    c->regs[R_AX] = r.ax;
    c->regs[R_BX] = r.bx;
    c->regs[R_CX] = r.cx;
    c->regs[R_DX] = r.dx;
    c->regs[R_SI] = r.si;
    c->regs[R_DI] = r.di;
    c->regs[R_BP] = r.bp;
    native_ret(pc);
}

/* 0050:1BEE div_q15: signed DX:AX / CX -> DX (see fx_div_q15). Also leaves AX = the
 * unsigned quotient |n| / |d| when it divides, CX = |CX| unless saturated early,
 * BX = 0 for a negative dividend. */
static void n_div_q15(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ax = c->regs[R_AX], dx = c->regs[R_DX], cx = c->regs[R_CX];
    int32_t n = (int32_t)((uint32_t)dx << 16 | ax);
    int16_t d = (int16_t)cx;

    if (cx == 0) {
        c->cycles += (dx & 0x8000) ? 0 : 66 - DIV_Q15_CYCLES;
        flags_logic(c, dx);
        c->regs[R_DX] = (uint16_t)fx_div_q15(n, d);
        native_ret(pc);
        return;
    }
    int dneg = dx >> 15, cneg = cx >> 15;
    bool neg = false;
    if (dx & 0x8000) {
        uint32_t un = 0u - (uint32_t)n;
        ax = (uint16_t)un;
        dx = (uint16_t)(un >> 16);
        c->regs[R_BX] = 0;
        neg = true;
    }
    if (cx & 0x8000) {
        cx = (uint16_t)-cx;
        neg = !neg;
    }
    flags_sub(c, dx, cx); /* CMP DX,CX */
    c->regs[R_AX] = ax;
    c->regs[R_CX] = cx;
    if ((int16_t)dx >= (int16_t)cx) {
        c->cycles += div_q15_cycles[dneg][cneg][0] - DIV_Q15_CYCLES;
        c->regs[R_DX] = (uint16_t)fx_div_q15(n, d);
        native_ret(pc);
        return;
    }
    if (dx >= cx) {
        /* only n = 80000000h: the original's DIV faults. Resume the original at
         * its DIV so INT 0 happens exactly as it would (see fx_div_q15_faults). */
        c->regs[R_DX] = dx;
        c->ip = neg ? 0x1C20 : 0x1C15;
        return;
    }
    c->cycles += div_q15_cycles[dneg][cneg][1] - DIV_Q15_CYCLES;
    uint16_t q = (uint16_t)(((uint32_t)dx << 16 | ax) / cx);
    c->regs[R_AX] = q;
    flags_shr1(c, q);
    if (neg)
        flags_sub(c, 0, (uint16_t)(q >> 1));
    c->regs[R_DX] = (uint16_t)fx_div_q15(n, d);
    native_ret(pc);
}

NativeEntry native_math[] = {
    { .name = "sincos", .seg = GAME_CS, .off = 0x45C0, .fn = n_sincos, .enabled = true,
      .flag_mask = 0, .cycles = SINCOS_CYCLES },
    { .name = "sin_quadrant", .seg = GAME_CS, .off = 0x45FD, .fn = n_sin_quadrant, .enabled = true,
      .flag_mask = 0, .cycles = 253 },
    { .name = "div_q15", .seg = GAME_CS, .off = 0x1BEE, .fn = n_div_q15, .enabled = true,
      .flag_mask = 0, .cycles = DIV_Q15_CYCLES },
    { .name = NULL },
};
