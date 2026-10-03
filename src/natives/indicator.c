/* Natives for subphase 3.10 indicators and needles (see docs/PHASE3_PLAN.md and
 * docs/subphases/3.10.md).
 *
 * Every routine is transliterated so that registers, segment registers and memory end up
 * exactly as the original leaves them, including the words its CALLs and PUSHes leave below
 * SP (the stack is written through a virtual SP; the real SP is only moved by native_ret).
 * Each routine also adds the emulated cycles the original would take on the path it ran
 * (the cycle model of cpu8086.c: 2 per instruction plus the opcode cost, plus 7 for a memory
 * operand, plus 4 per bit shifted), so the emulated timeline stays the same with the natives
 * on. */
#include "panel.h"
#include "game/state.h"

static uint32_t cyc; /* original cycles of the routine being run */

/* ---- memory and register helpers ------------------------------------------------ */

static uint8_t rd8(Pc *pc, uint16_t seg, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(seg, off)); }
static uint16_t rd16(Pc *pc, uint16_t seg, uint16_t off) { return mem_read16(pc, seg, off); }
static void wr8(Pc *pc, uint16_t seg, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(seg, off), v); }
static void wr16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v) { mem_write16(pc, seg, off, v); }

/* PUSH through the virtual stack pointer *sp. */
static void vpush(Pc *pc, uint16_t *sp, uint16_t v)
{
    *sp = (uint16_t)(*sp - 2);
    wr16(pc, pc->cpu.sregs[S_SS], *sp, v);
}

static uint8_t lo8(uint16_t v) { return (uint8_t)v; }
static uint8_t hi8(uint16_t v) { return (uint8_t)(v >> 8); }
static uint16_t with_lo(uint16_t v, uint8_t b) { return (uint16_t)((v & 0xFF00) | b); }
static uint16_t with_hi(uint16_t v, uint8_t b) { return (uint16_t)((v & 0x00FF) | b << 8); }

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])

/* Conditional jump cost: taken 16, not taken 4. */
#define JCC(taken) (cyc += (taken) ? 16 : 4)

/* CGA interleave: true when the offset is in the odd-line bank (signed compare, as the
 * original's JG/JLE against 1F40). */
static bool odd_bank(uint16_t off) { return (int16_t)off > 0x1F40; }

/* ---- blit_or 0050:4DF6 ------------------------------------------------------------ */

static void blit_or_body(Pc *pc)
{
    uint16_t ds = SREG(DS), es = SREG(ES);
    uint16_t si = REG(SI), di = REG(DI), ax = REG(AX), bx = REG(BX);
    uint8_t dl = lo8(REG(DX)), dh = hi8(REG(DX)), cl = lo8(REG(CX));
    bool odd = odd_bank(di);
    cyc += 6; /* cmp dl,1 */
    JCC(dl == 1);
    cyc += 6; /* cmp di,1F40 */
    if (dl == 1) {
        cyc += 4; /* mov bx,di */
        bx = di;
        JCC(odd);
        for (;;) {
            ax = rd16(pc, ds, si);
            si += 2;
            wr16(pc, es, bx, rd16(pc, es, bx) | ax);
            bx = (uint16_t)(bx + (odd ? 0xE000 : 0x1FB0));
            dh--;
            cyc += 44;
            JCC(odd == (dh != 0)); /* even row: JE ends; odd row: JNE continues */
            if (!dh)
                break;
            odd = !odd;
        }
    } else {
        JCC(odd);
        for (;;) {
            cl = dl;
            bx = di;
            cyc += 8;
            do {
                ax = rd16(pc, ds, si);
                si += 2;
                wr16(pc, es, bx, rd16(pc, es, bx) | ax);
                bx += 2;
                cl--;
                cyc += 44;
                JCC(cl != 0);
            } while (cl);
            di = (uint16_t)(di + (odd ? 0xE000 : 0x1FB0));
            dh--;
            cyc += 11;
            JCC(odd == (dh != 0));
            if (!dh)
                break;
            odd = !odd;
        }
    }
    cyc += 22; /* sti, ret */
    pc->cpu.flags |= F_IF;
    REG(AX) = ax;
    REG(BX) = bx;
    REG(CX) = with_lo(REG(CX), cl);
    REG(DX) = with_hi(REG(DX), dh);
    REG(SI) = si;
    REG(DI) = di;
}

/* ---- blit_sprite 0050:4DE0 -------------------------------------------------------- */

static void blit_sprite_body(Pc *pc)
{
    uint16_t bx = REG(BX), ds = SREG(DS);
    REG(AX) = 0xB800;
    SREG(ES) = 0xB800;
    REG(DX) = rd16(pc, ds, (uint16_t)(bx + 2));
    REG(DI) = rd16(pc, ds, bx);
    REG(SI) = (uint16_t)(bx + 4);
    cyc += 4 + 4 + 17 + 17 + 17 + 4 + 6;
    blit_or_body(pc);
}

/* ---- indicator_erase 0050:218E ---------------------------------------------------- */

static void indicator_erase_body(Pc *pc, uint16_t sp)
{
    uint16_t bx = REG(BX), ds = SREG(DS), es = SREG(ES);
    vpush(pc, &sp, bx);
    vpush(pc, &sp, ds);
    uint16_t si = rd16(pc, ds, (uint16_t)(bx + 8));
    uint16_t dx = rd16(pc, ds, (uint16_t)(bx + 0xA));
    cyc += 15 + 16 + 17 + 17 + 5;
    JCC(dx == 0);
    if (dx) {
        uint16_t src = rd16(pc, 0, 0x122); /* clean copy of the panel */
        uint16_t ax = REG(AX);
        uint8_t dl = lo8(dx), dh = hi8(dx), cl = lo8(REG(CX));
        cyc += 5 + 4 + 11 + 6;
        JCC(dl == 1);
        if (dl == 1) {
            uint16_t b = si;
            bool odd = odd_bank(b);
            cyc += 4 + 6;
            JCC(!odd);
            for (;;) {
                ax = rd16(pc, src, b);
                wr16(pc, es, b, ax);
                b = (uint16_t)(b + (odd ? 0xE000 : 0x1FB0));
                dh--;
                cyc += 46;
                JCC(odd == (dh == 0)); /* odd row: JE ends; even row: JNE continues */
                if (!dh) {
                    if (!odd)
                        cyc += 17; /* jmp 21C9 */
                    break;
                }
                odd = !odd;
            }
        } else {
            do {
                uint16_t b = si;
                cl = dl;
                cyc += 8;
                do {
                    ax = rd16(pc, src, b);
                    wr16(pc, es, b, ax);
                    b += 2;
                    cl--;
                    cyc += 46;
                    JCC(cl != 0);
                } while (cl);
                si += 0x1FB0;
                cyc += 12;
                JCC(!odd_bank(b));
                if (odd_bank(b)) {
                    si += 0xC050;
                    cyc += 6;
                }
                dh--;
                cyc += 5;
                JCC(dh != 0);
            } while (dh);
        }
        REG(AX) = ax;
        REG(CX) = with_lo(REG(CX), cl);
        dx = (uint16_t)(dl | dh << 8);
    }
    cyc += 14 + 12 + 20; /* pop ds, pop bx, ret */
    REG(SI) = si;
    REG(DX) = dx;
}

/* ---- draw_indicator 0050:2111 ------------------------------------------------------ */

static void draw_indicator_body(Pc *pc, uint16_t sp)
{
    uint16_t ds = rd16(pc, 0, 0x120);
    uint16_t bp = REG(BP), bx = bp;
    uint8_t al = rd8(pc, ds, (uint16_t)(bx + 6));
    SREG(DS) = ds;
    REG(AX) = al;
    REG(BX) = bx;
    cyc += 5 + 4 + 11 + 4 + 17 + 18;
    JCC(al != rd8(pc, ds, (uint16_t)(bx + 7)));
    if (al == rd8(pc, ds, (uint16_t)(bx + 7))) {
        uint8_t force = gs_indicator_force(pc);
        cyc += 19;
        JCC(force == 0);
        if (!force) {
            cyc += 20;
            return;
        }
        gs_set_indicator_force(pc, (uint8_t)(force - 1));
        cyc += 12;
    }
    pc->cpu.flags &= (uint16_t)~F_IF;
    REG(AX) = 0xB800;
    SREG(ES) = 0xB800;
    cyc += 2 + 4 + 4 + 21;
    vpush(pc, &sp, 0x2137);
    indicator_erase_body(pc, sp);
    sp += 2;

    uint16_t si = rd16(pc, ds, bx), di = rd16(pc, ds, (uint16_t)(bx + 2));
    uint8_t frame = rd8(pc, ds, (uint16_t)(bx + 6));
    wr8(pc, ds, (uint16_t)(bx + 7), frame);
    bx = (uint16_t)(rd16(pc, ds, (uint16_t)(bx + 4)) + frame * 2);
    uint16_t cx = rd16(pc, ds, bx); /* frame word: x byte, sprite index, lines up */
    bx = (uint16_t)(((cx & 0xF0) >> 3) + di);
    di = rd16(pc, ds, bx); /* sprite pointer */
    si = (uint16_t)(si + (cx & 0xF));
    uint8_t ch = hi8(cx);
    cyc += 17 + 17 + 5 + 17 + 18 + 8 + 17 + 5 + 17 + 4 + 6 + 24 + 5 + 17 + 4 + 6 + 5 + 5;
    ch--;
    JCC(ch & 0x80);
    if (!(ch & 0x80)) {
        bool odd = odd_bank(si);
        cyc += 6;
        JCC(!odd);
        for (;;) { /* move up one line per count */
            si = (uint16_t)(si + (odd ? 0xE000 : 0x1FB0));
            ch--;
            cyc += 11;
            JCC(odd == !!(ch & 0x80)); /* odd: JS ends; even: JNS continues */
            if (ch & 0x80)
                break;
            odd = !odd;
        }
    }
    /* xchg di,si: DI = screen position, SI = sprite (size word, then words) */
    wr16(pc, ds, (uint16_t)(bp + 8), si);
    uint16_t size = rd16(pc, ds, di);
    wr16(pc, ds, (uint16_t)(bp + 0xA), size);
    cyc += 6 + 18 + 15 + 18 + 4 + 17;
    REG(AX) = size;
    REG(BX) = bx;
    REG(CX) = with_hi(cx, ch);
    REG(DX) = size;
    REG(SI) = (uint16_t)(di + 2);
    REG(DI) = si;
    blit_or_body(pc); /* tail jump: its RET is ours */
}

/* ---- needles -------------------------------------------------------------------------
 * A needle descriptor (CX): +0 / +2 screen offsets of the hub for the upper / lower half,
 * +4 drawn code (FF = none), +5 screen offset and +7 size word of the drawn sprite,
 * +9 angle (draw_needle2 descriptors used as second needles). The code is the needle
 * direction 0..3F within a quadrant (from the table at DS:09BD) plus quadrant bits: 40 =
 * lower half (drawn downwards), 80 = left half (sprite mirrored by the bit-reversal table
 * at DS:12B4 and drawn right to left). */

static void needle_draw_normal(Pc *pc)
{
    uint16_t ds = SREG(DS), es = SREG(ES), cx = REG(CX);
    uint16_t si = REG(SI), di = REG(DI), ax = 0, bp = 0;
    uint16_t dx = rd16(pc, ds, si);
    si += 2;
    wr16(pc, ds, (uint16_t)(cx + 7), dx);
    wr16(pc, ds, (uint16_t)(cx + 5), di);
    cyc += 15 + 4 + 4 + 18 + 18;
    uint8_t dl = lo8(dx), dh = hi8(dx), cl;
    do {
        cl = dl;
        bp = di;
        cyc += 8;
        do {
            ax = rd16(pc, ds, si);
            si += 2;
            wr16(pc, es, bp, rd16(pc, es, bp) | ax);
            bp += 2;
            cl--;
            cyc += 44;
            JCC(cl != 0);
        } while (cl);
        ax = gs_needle_row_step(pc);
        cyc += 18;
        JCC(!odd_bank(bp));
        if (odd_bank(bp)) {
            ax = gs_needle_row_step2(pc);
            cyc += 12;
        }
        di += ax;
        dh--;
        cyc += 10;
        JCC(dh != 0);
    } while (dh);
    cyc += 20;
    REG(AX) = ax;
    REG(BX) = cx;
    REG(CX) = with_lo(cx, cl);
    REG(DX) = with_hi(dx, dh);
    REG(SI) = si;
    REG(DI) = di;
    REG(BP) = bp;
}

static void needle_draw_mirrored(Pc *pc)
{
    uint16_t ds = SREG(DS), es = SREG(ES), cx = REG(CX);
    uint16_t si = REG(SI), di = REG(DI), ax = 0, bp = 0;
    uint16_t dx = rd16(pc, ds, si);
    si += 2;
    wr16(pc, ds, (uint16_t)(cx + 7), dx);
    di--;
    wr16(pc, ds, (uint16_t)(cx + 5), di);
    cyc += 15 + 4 + 4 + 18 + 2 + 18 + 4;
    uint8_t dl = lo8(dx), dh = hi8(dx), cl;
    do {
        cl = dl;
        bp = di;
        cyc += 8;
        do {
            uint16_t w = rd16(pc, ds, si);
            si += 2;
            ax = (uint16_t)(rd8(pc, ds, (uint16_t)(0x12B4 + lo8(w))) << 8 | rd8(pc, ds, (uint16_t)(0x12B4 + hi8(w))));
            wr16(pc, es, bp, rd16(pc, es, bp) | ax);
            bp -= 2;
            cl--;
            cyc += 76;
            JCC(cl != 0);
        } while (cl);
        ax = gs_needle_row_step(pc);
        cyc += 18;
        JCC(!odd_bank(bp));
        if (odd_bank(bp)) {
            ax = gs_needle_row_step2(pc);
            cyc += 12;
        }
        di += ax;
        dh--;
        cyc += 10;
        JCC(dh != 0);
    } while (dh);
    cyc += 20;
    REG(AX) = ax;
    REG(BX) = 0x12B4;
    REG(CX) = with_lo(cx, cl);
    REG(DX) = with_hi(dx, dh);
    REG(SI) = si;
    REG(DI) = di;
    REG(BP) = bp;
}

/* 0050:1F63: restores the needle drawn at BX from the clean panel copy. BX = CX on return. */
static void needle_erase_body(Pc *pc, uint16_t sp)
{
    uint16_t bx = REG(BX), ds = SREG(DS), es = SREG(ES);
    vpush(pc, &sp, REG(CX));
    vpush(pc, &sp, REG(AX));
    uint16_t si = rd16(pc, ds, (uint16_t)(bx + 5));
    uint16_t dx = rd16(pc, ds, (uint16_t)(bx + 7));
    uint8_t code = rd8(pc, ds, (uint16_t)(bx + 4));
    cyc += 15 + 15 + 17 + 17 + 17 + 6;
    JCC(code == 0xFF);
    if (code != 0xFF) {
        uint16_t di = (code & 0x80) ? 0xFFFE : 2;
        bool lower = code & 0x40;
        uint16_t src = rd16(pc, 0, 0x122);
        uint8_t dl = lo8(dx), dh = hi8(dx);
        cyc += 4 + 8; /* mov di,2; shl al,1 */
        JCC(!(code & 0x80));
        if (code & 0x80)
            cyc += 5;
        vpush(pc, &sp, ds);
        cyc += 16 + 5 + 4 + 11 + 8;
        JCC(lower);
        do {
            uint16_t b = si;
            uint8_t cl = dl;
            cyc += 8;
            do {
                wr16(pc, es, b, rd16(pc, src, b));
                b += di;
                cl--;
                cyc += 45;
                JCC(cl != 0);
            } while (cl);
            uint16_t step = lower ? 0x2000 : 0x1FB0;
            cyc += 10;
            JCC(!odd_bank(b));
            if (odd_bank(b)) {
                step = lower ? 0xE050 : 0xE000;
                cyc += 4;
            }
            si += step;
            dh--;
            cyc += 10;
            JCC(dh != 0);
        } while (dh);
        cyc += 14; /* pop ds */
        if (!lower)
            cyc += 17; /* jmp 1FCF */
        dx = (uint16_t)(dl | dh << 8);
        REG(DI) = di;
    }
    cyc += 12 + 12 + 4 + 20;
    REG(SI) = si;
    REG(DX) = dx;
    REG(BX) = REG(CX);
}

static bool needle_div_overflow(Pc *pc);

/* 0050:20E0: while [0914] != 0 (the altimeter), redraws indicator 13AC with the frame for
 * [0900] / 5F5 (0..4). */
static void needle_alt_indicator(Pc *pc, uint16_t sp)
{
    uint16_t ds = SREG(DS);
    uint8_t on = gs_alt_band_on(pc);
    cyc += 19;
    JCC(on == 0);
    if (!on) {
        cyc += 20;
        return;
    }
    uint16_t ax0 = REG(AX), bx0 = REG(BX), cx0 = REG(CX);
    vpush(pc, &sp, cx0);
    vpush(pc, &sp, bx0);
    vpush(pc, &sp, ax0);
    REG(BX) = 0x13AC;
    cyc += 15 + 15 + 15 + 4 + 21;
    vpush(pc, &sp, 0x20F0);
    indicator_erase_body(pc, sp);
    sp += 2;
    /* cwd / div cx. A negative [0900] overflows (INT 0); callers check
     * needle_div_overflow() first and run the original instead. */
    uint16_t ax = gs_alt_display(pc);
    uint16_t q = (uint16_t)(ax / 0x5F5), r = (uint16_t)(ax % 0x5F5);
    cyc += 12 + 4 + 5 + 146 + 6;
    JCC(q < 5);
    if (q >= 5) {
        q = 4;
        cyc += 4;
    }
    REG(AX) = (uint16_t)(q << 1);
    REG(BX) = rd16(pc, ds, (uint16_t)(0x13B8 + (q << 1)));
    REG(CX) = 0x5F5;
    REG(DX) = r;
    cyc += 8 + 4 + 17 + 21;
    vpush(pc, &sp, 0x210C);
    blit_sprite_body(pc);
    REG(AX) = ax0;
    REG(BX) = bx0;
    REG(CX) = cx0;
    cyc += 12 + 12 + 12 + 20;
}

/* Quadrant of an angle: the code (direction | quadrant bits), the hub offset in the
 * descriptor (0 or 2), and the cycles of the original's classification code. */
typedef struct NeedleQuad {
    uint8_t code, hub;
    int quad; /* 0 = angle 00-40, 1 = 41-80, 3 = 81-C0, 2 = C1-FF (the code's quadrant bits) */
    bool lower, mirrored;
    uint32_t cycles;
} NeedleQuad;

static NeedleQuad needle_quadrant(Pc *pc, uint8_t angle)
{
    uint16_t ds = SREG(DS);
    NeedleQuad q;
    if (angle < 0x41) {
        q.code = rd8(pc, ds, (uint16_t)(0x9BD + (uint8_t)(0x40 - angle)));
        q.quad = 0;
        q.cycles = 4 + 6 + 4 + 5 + 6 + 13 + 4;
    } else if (angle < 0x81) {
        q.code = rd8(pc, ds, (uint16_t)(0x9BD + angle - 0x40)) | 0x40;
        q.quad = 1;
        q.cycles = 4 + 6 + 16 + 6 + 13 + 4 + 6;
    } else if (angle < 0xC1) {
        q.code = rd8(pc, ds, (uint16_t)(0x9BD + (uint8_t)(0xC0 - angle))) | 0xC0;
        q.quad = 3;
        q.cycles = 16 + 6 + 4 + 5 + 6 + 13 + 4 + 6;
    } else {
        q.code = rd8(pc, ds, (uint16_t)(0x9BD + angle - 0xC0)) | 0x80;
        q.quad = 2;
        q.cycles = 16 + 6 + 16 + 6 + 13 + 4 + 6;
    }
    q.lower = q.quad == 1 || q.quad == 3;
    q.mirrored = q.quad >= 2;
    q.hub = q.lower ? 2 : 0;
    return q;
}

/* Common entry of draw_needle / draw_needle2: stores AH at flag_var (in the caller's DS),
 * loads DS = [0000:0120] and ES = B800. */
static NeedleQuad needle_prologue(Pc *pc, uint16_t flag_var)
{
    uint8_t angle = lo8(REG(AX));
    wr8(pc, SREG(DS), flag_var, hi8(REG(AX)));
    SREG(DS) = rd16(pc, 0, 0x120);
    SREG(ES) = 0xB800;
    REG(BX) = with_lo(REG(BX), angle);
    cyc += 4 + 18 + 5 + 4 + 11 + 4 + 4 + 4 + 4 + 6;
    NeedleQuad q = needle_quadrant(pc, angle);
    cyc += q.cycles;
    REG(AX) = with_lo(0xB800, q.code);
    REG(BX) = REG(CX);
    return q;
}

/* Hub offset, row steps and the sprite of the code from the table at DS:table. Quadrants
 * 1 and 3 reach the shared code through one more JMP. */
static void needle_setup(Pc *pc, NeedleQuad q, uint16_t table)
{
    uint16_t ds = SREG(DS), bx = REG(BX);
    bool lower = q.lower;
    REG(DI) = rd16(pc, ds, (uint16_t)(bx + q.hub));
    gs_set_needle_row_step(pc, lower ? 0x2000 : 0x1FB0);
    gs_set_needle_row_step2(pc, lower ? 0xE050 : 0xE000);
    wr8(pc, ds, (uint16_t)(bx + 4), q.code);
    REG(BX) = (uint16_t)((q.code & 0x3F) << 1);
    REG(SI) = rd16(pc, ds, (uint16_t)(table + REG(BX)));
    cyc += 17 + 19 + 19 + 18 + 5 + 4 + 6 + 8 + 17 + 17;
    if (q.quad & 1)
        cyc += 17;
}

/* ---- draw_needle2 0050:1DE8 -------------------------------------------------------- */

static void draw_needle2_body(Pc *pc, uint16_t sp)
{
    NeedleQuad q = needle_prologue(pc, 0x904);
    uint16_t ds = SREG(DS), bx = REG(BX);
    bool same = q.code == rd8(pc, ds, (uint16_t)(bx + 4));
    static const uint16_t erase_ret[4] = { 0x1E21, 0x1E5E, 0x1EBF, 0x1E91 };
    static const uint16_t alt_ret[4] = { 0x1E24, 0x1E61, 0x1EC2, 0x1E94 };
    int quad = q.quad;
    cyc += 18;
    JCC(!same);
    if (same) {
        bool force = gs_needle2_force(pc) & 1;
        cyc += 14;
        JCC(force);
        if (!force) {
            cyc += 17 + 20;
            return;
        }
    } else {
        cyc += 21;
        vpush(pc, &sp, erase_ret[quad]);
        needle_erase_body(pc, sp);
        sp += 2;
    }
    cyc += 21;
    vpush(pc, &sp, alt_ret[quad]);
    needle_alt_indicator(pc, sp);
    sp += 2;
    needle_setup(pc, q, 0x98F);
    if (q.mirrored)
        needle_draw_mirrored(pc);
    else
        needle_draw_normal(pc);
}

/* ---- draw_needle 0050:1D20 --------------------------------------------------------- */

static void draw_needle_body(Pc *pc, uint16_t sp)
{
    NeedleQuad q = needle_prologue(pc, 0x90F);
    uint16_t ds = SREG(DS), bx = REG(BX);
    static const uint16_t chk_ret[4] = { 0x1D4A, 0x1D78, 0x1DB6, 0x1D99 };
    int quad = q.quad;
    cyc += 21 + 18; /* call 1DDA, cmp al,[bx+4] */
    vpush(pc, &sp, chk_ret[quad]);
    bool same = q.code == rd8(pc, ds, (uint16_t)(bx + 4));
    JCC(!same);
    if (same) { /* pop ax / ret: returns from draw_needle */
        REG(AX) = chk_ret[quad];
        cyc += 12 + 20;
        return;
    }
    uint16_t sp1 = sp;
    cyc += 21;
    vpush(pc, &sp1, 0x1DE4);
    needle_erase_body(pc, sp1);

    /* 1EE3: redraw the second needle [090D] at its angle (+9) if AH was nonzero */
    sp1 = sp;
    vpush(pc, &sp1, 0x1DE7);
    uint16_t ax0 = REG(AX), bx0 = REG(BX), cx0 = REG(CX);
    vpush(pc, &sp1, ax0);
    vpush(pc, &sp1, bx0);
    vpush(pc, &sp1, cx0);
    uint8_t redraw = gs_needle_redraw2(pc);
    cyc += 21 + 15 + 15 + 15 + 12 + 5;
    JCC(redraw == 0);
    if (redraw) {
        uint16_t cx = gs_second_needle_desc(pc);
        REG(CX) = cx;
        REG(BX) = cx;
        REG(AX) = (uint16_t)(0x0100 | rd8(pc, ds, (uint16_t)(cx + 9)));
        cyc += 17 + 4 + 4 + 17 + 21;
        vpush(pc, &sp1, 0x1EFB);
        draw_needle2_body(pc, sp1);
    }
    REG(AX) = ax0;
    REG(BX) = bx0;
    REG(CX) = cx0;
    cyc += 12 + 12 + 12 + 20 + 20; /* pops, ret from 1EE3, ret from 1DDA */

    needle_setup(pc, q, 0x961);
    cyc += 18; /* mov [bx+5],di: the draw below stores +5 again */
    if (q.mirrored)
        needle_draw_mirrored(pc);
    else
        needle_draw_normal(pc);
}

/* ---- run the original instead (the divide overflow case) ---------------------------- */

/* draw_needle(2) reach DIV in 0050:20F7 with [0900] negative only when [0914] != 0; the
 * 8086 then raises INT 0. Rather than emulate that, such calls run the original. */
static bool needle_div_overflow(Pc *pc)
{
    return gs_alt_band_on(pc) != 0 && (gs_alt_display(pc) & 0x8000);
}

/* Single-steps the original routine at CS:IP until it returns. Natives are off meanwhile.
 * Returns the cycles it took. */
static uint64_t run_original(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    const uint8_t *map = c->hook_map;
    uint16_t sp0 = c->regs[R_SP];
    uint16_t ret_ip = rd16(pc, c->sregs[S_SS], sp0), ret_cs = c->sregs[S_CS];
    uint64_t start = c->cycles;
    c->hook_map = NULL;
    for (long n = 0; n < 5000000L; n++) {
        native_or_cpu_step(pc);
        uint16_t d = (uint16_t)(c->regs[R_SP] - sp0);
        if (c->ip == ret_ip && c->sregs[S_CS] == ret_cs && d >= 2 && d < 0x8000)
            break;
    }
    c->hook_map = map;
    uint64_t used = c->cycles - start;
    c->cycles = start;
    return used;
}

/* ---- public helpers (panel.h) ------------------------------------------------------ */

uint32_t panel_cycles(void) { return cyc; }

/* Runs body as if called from the current state with return address ret_ip. */
static void call_body(Pc *pc, uint16_t ret_ip, void (*body)(Pc *, uint16_t))
{
    uint16_t sp = REG(SP);
    vpush(pc, &sp, ret_ip);
    cyc = 0;
    body(pc, sp);
}

static void blit_or_sp(Pc *pc, uint16_t sp) { (void)sp; blit_or_body(pc); }
static void blit_sprite_sp(Pc *pc, uint16_t sp) { (void)sp; blit_sprite_body(pc); }

void panel_call_blit_or(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, blit_or_sp); }
void panel_call_blit_sprite(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, blit_sprite_sp); }
void panel_call_indicator_erase(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, indicator_erase_body); }
void panel_call_draw_indicator(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, draw_indicator_body); }

/* The needle helpers fall back to the original routine for the INT 0 case; that needs the
 * real call, so it is done with the CPU (push ret_ip, jump, step until return). */
static void call_needle(Pc *pc, uint16_t ret_ip, uint16_t entry, void (*body)(Pc *, uint16_t))
{
    if (!needle_div_overflow(pc)) {
        call_body(pc, ret_ip, body);
        return;
    }
    Cpu8086 *c = &pc->cpu;
    uint16_t ip = c->ip, cs = c->sregs[S_CS];
    cpu_push(c, ret_ip);
    c->ip = entry;
    c->sregs[S_CS] = GAME_CS;
    cyc = (uint32_t)run_original(pc);
    c->ip = ip;
    c->sregs[S_CS] = cs;
}

void panel_call_draw_needle(Pc *pc, uint16_t ret_ip) { call_needle(pc, ret_ip, 0x1D20, draw_needle_body); }
void panel_call_draw_needle2(Pc *pc, uint16_t ret_ip) { call_needle(pc, ret_ip, 0x1DE8, draw_needle2_body); }

/* ---- natives ------------------------------------------------------------------------ */

/* Charges what the original took beyond the entry's fixed .cycles. */
static void charge(Pc *pc, uint32_t fixed)
{
    pc->cpu.cycles += cyc;
    pc->cpu.cycles -= fixed;
}

#define CYC_BLIT_OR 154
#define CYC_BLIT_SPRITE 295
#define CYC_ERASE 132
#define CYC_DRAW_INDICATOR 118
#define CYC_DRAW_NEEDLE 181
#define CYC_DRAW_NEEDLE2 183

static void n_blit_or(Pc *pc)
{
    cyc = 0;
    blit_or_body(pc);
    charge(pc, CYC_BLIT_OR);
    native_ret(pc);
}

static void n_blit_sprite(Pc *pc)
{
    cyc = 0;
    blit_sprite_body(pc);
    charge(pc, CYC_BLIT_SPRITE);
    native_ret(pc);
}

static void n_indicator_erase(Pc *pc)
{
    cyc = 0;
    indicator_erase_body(pc, REG(SP));
    charge(pc, CYC_ERASE);
    native_ret(pc);
}

static void n_draw_indicator(Pc *pc)
{
    cyc = 0;
    draw_indicator_body(pc, REG(SP));
    charge(pc, CYC_DRAW_INDICATOR);
    native_ret(pc);
}

static void n_draw_needle(Pc *pc)
{
    if (needle_div_overflow(pc)) {
        cyc = (uint32_t)run_original(pc); /* includes the RET */
        charge(pc, CYC_DRAW_NEEDLE);
        return;
    }
    cyc = 0;
    draw_needle_body(pc, REG(SP));
    charge(pc, CYC_DRAW_NEEDLE);
    native_ret(pc);
}

static void n_draw_needle2(Pc *pc)
{
    if (needle_div_overflow(pc)) {
        cyc = (uint32_t)run_original(pc);
        charge(pc, CYC_DRAW_NEEDLE2);
        return;
    }
    cyc = 0;
    draw_needle2_body(pc, REG(SP));
    charge(pc, CYC_DRAW_NEEDLE2);
    native_ret(pc);
}

NativeEntry native_indicator[] = {
    { .name = "blit_or", .seg = GAME_CS, .off = 0x4DF6, .fn = n_blit_or, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_BLIT_OR },
    { .name = "blit_sprite", .seg = GAME_CS, .off = 0x4DE0, .fn = n_blit_sprite, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_BLIT_SPRITE },
    { .name = "indicator_erase", .seg = GAME_CS, .off = 0x218E, .fn = n_indicator_erase, .enabled = true,
      .cycles = CYC_ERASE },
    { .name = "draw_indicator", .seg = GAME_CS, .off = 0x2111, .fn = n_draw_indicator, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_DRAW_INDICATOR },
    { .name = "draw_needle", .seg = GAME_CS, .off = 0x1D20, .fn = n_draw_needle, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_DRAW_NEEDLE },
    { .name = "draw_needle2", .seg = GAME_CS, .off = 0x1DE8, .fn = n_draw_needle2, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_DRAW_NEEDLE2 },
    { .name = NULL },
};
