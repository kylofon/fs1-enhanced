/* Fixed-point maths helpers for natives (3.1), bit-identical to the original routines.
 *
 * Angles are 16-bit binary angles (10000h = 360 degrees); sin/cos are Q15. The sine
 * values come from the game's own word table at DS:3381 (129 points per quadrant plus
 * one guard word, read from game memory, not embedded), with the same linear
 * interpolation as the original. These helpers are not bound to addresses; the math.c
 * natives use them, so --verify checks them on every call. */
#ifndef FS1_FIXMATH_H
#define FS1_FIXMATH_H

#include "native.h"

#define FX_SINE_TABLE 0x3381

/* Registers left by sin_quadrant (0050:45FD). */
typedef struct FxQuadrant {
    uint16_t ax, bx, dx, si, di;
} FxQuadrant;

/* 0050:45FD on X = BX (0..4000h): the table at ds:3381 is cos(X) (T[0] = 7FFFh), so
 * sincos uses it with X = 90 degrees - angle for the sine. Interpolates between
 * T[X>>7] and T[(X>>7)+1] with the Q15 multiply idiom and keeps bits 7..22. */
static inline FxQuadrant fx_quadrant_regs(Pc *pc, uint16_t ds, uint16_t x)
{
    FxQuadrant r;
    r.bx = (uint16_t)(((uint16_t)(x << 1) >> 8) << 1);
    r.si = mem_read16(pc, ds, (uint16_t)(r.bx + FX_SINE_TABLE));
    r.di = (uint16_t)(mem_read16(pc, ds, (uint16_t)(r.bx + FX_SINE_TABLE + 2)) - r.si);
    uint32_t p = (uint32_t)((int32_t)(int16_t)(x & 0x7F) * (int16_t)r.di) << 1; /* IMUL; SHL; RCL */
    r.dx = (uint16_t)(p >> 16);
    r.ax = (uint16_t)((uint16_t)(p >> 8) + r.si); /* MOV AL,AH / MOV AH,DL / ADD AX,SI */
    return r;
}

/* Registers left by sincos (0050:45C0): ax = sin, cx = cos, and the clobbers. The last
 * step is either ADD AX,SI of sin_quadrant (neg_ax false) or NEG AX (neg_ax true),
 * which sets the flags; ax_pre is AX before that NEG. */
typedef struct FxSincos {
    uint16_t ax, bx, cx, dx, si, di, bp;
    bool neg_ax;
    uint16_t ax_pre;
} FxSincos;

static inline FxSincos fx_sincos_regs(Pc *pc, uint16_t ds, uint16_t angle)
{
    FxSincos r;
    FxQuadrant q;
    if (angle & 0x8000) { /* sin(-a) = -sin(a); -8000h becomes 7FFFh */
        uint16_t a = (uint16_t)-angle;
        if (a & 0x8000)
            a = 0x7FFF;
        r = fx_sincos_regs(pc, ds, a);
        r.ax_pre = r.ax;
        r.ax = (uint16_t)-r.ax;
        r.neg_ax = true;
        return r;
    }
    r.bp = angle;
    if (angle <= 0x4000) {
        r.cx = fx_quadrant_regs(pc, ds, angle).ax;
        q = fx_quadrant_regs(pc, ds, (uint16_t)(0x4000 - angle));
    } else {
        r.cx = (uint16_t)-fx_quadrant_regs(pc, ds, (uint16_t)(0x8000 - angle)).ax;
        q = fx_quadrant_regs(pc, ds, (uint16_t)(angle - 0x4000));
    }
    r.ax = q.ax;
    r.bx = q.bx;
    r.dx = q.dx;
    r.si = q.si;
    r.di = q.di;
    r.neg_ax = false;
    r.ax_pre = 0;
    return r;
}

/* sin/cos of a binary angle, Q15, from the game's table (DS = 0618). */
static inline void fx_sincos(Pc *pc, uint16_t angle, int16_t *sin_out, int16_t *cos_out)
{
    FxSincos r = fx_sincos_regs(pc, GAME_DS, angle);
    if (sin_out)
        *sin_out = (int16_t)r.ax;
    if (cos_out)
        *cos_out = (int16_t)r.cx;
}
static inline int16_t fx_sin(Pc *pc, uint16_t angle)
{
    int16_t s;
    fx_sincos(pc, angle, &s, NULL);
    return s;
}
static inline int16_t fx_cos(Pc *pc, uint16_t angle)
{
    int16_t c;
    fx_sincos(pc, angle, NULL, &c);
    return c;
}

/* Q15 multiply idiom IMUL r16; SHL AX,1; RCL DX,1: the full 32-bit result (DX:AX). */
static inline uint32_t fx_mul_q15_32(int16_t a, int16_t b)
{
    return (uint32_t)((int32_t)a * b) << 1;
}
/* Its DX: (a * b) >> 15, truncated toward minus infinity; 8000h * 8000h gives 8000h. */
static inline int16_t fx_mul_q15(int16_t a, int16_t b)
{
    return (int16_t)(fx_mul_q15_32(a, b) >> 16);
}

/* 0050:1BEE div_q15: signed DX:AX / CX, result DX = (|n| / |d|) >> 1 with the sign of
 * n / d (rounding toward zero), i.e. Q15 for |n| < |d| << 16.
 * Saturation: d == 0 gives 7FFFh for n >= 0 (by the sign of DX only) and 8001h for n < 0;
 * |n| >> 16 >= |d| (signed compare) gives 7FFFh or 8001h by the result sign.
 * The one case the original does not return from: n = 80000000h with |d| < 8000h,
 * where its DIV faults (INT 0). fx_div_q15_faults() tells that case; the helper then
 * saturates by the result sign (8001h for d > 0, 7FFFh for d < 0). */
static inline bool fx_div_q15_faults(int32_t n, int16_t d)
{
    return n == INT32_MIN && d != 0 && d != INT16_MIN;
}
static inline int16_t fx_div_q15(int32_t n, int16_t d)
{
    uint32_t un = (uint32_t)n;
    uint16_t dx = (uint16_t)(un >> 16), cx = (uint16_t)d;
    if (cx == 0)
        return (dx & 0x8000) ? (int16_t)-0x7FFF : 0x7FFF;
    bool neg = false;
    if (dx & 0x8000) {
        un = 0u - un;
        dx = (uint16_t)(un >> 16);
        neg = true;
    }
    if (cx & 0x8000) {
        cx = (uint16_t)-cx;
        neg = !neg;
    }
    if ((int16_t)dx >= (int16_t)cx || dx >= cx) /* second test: the faulting case */
        return neg ? (int16_t)-0x7FFF : 0x7FFF;
    uint16_t q = (uint16_t)((un / cx) >> 1);
    return neg ? (int16_t)-q : (int16_t)q;
}

#endif
