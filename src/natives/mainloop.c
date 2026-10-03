/* Subphase 3.21: the main loop and the rest of the original code on the frame's hot path.
 *
 * main_loop (0050:01F8-02CA) never returns. It is split into blocks: one C function is bound at
 * the loop head and at every return address of a CALL in the loop (natives "main_loop" and
 * "main_loop@XXXX"). Each call runs the loop's own code from there up to the next CALL, which it
 * performs (pushes the return address, IP = callee), or up to the loop head; so the callees
 * (natives or original) run as separate steps, exactly where the original calls them, and
 * interrupts are taken between them. Nothing of the loop is left as original code except the
 * timer_hooked == 0 hang at 0211, which the block enters and the original executes (a JMP $
 * that never ends, as in the original).
 *
 * The other natives here are on the frame path or are polling loops:
 *   draw_view_frame   5A9E  border of the 3D window (no native before)
 *   view_overlay_marks 0658 the gear/gun-sight marks in the back buffer (no native before)
 *   view_frame_loop   5AC9  draw_view_frame's border loop, and
 *   overlay_rows      0700  view_overlay_marks' row loop (reached when those two decline)
 *   crash_delay       063C  crash_handler's 65535-iteration MUL delay loop
 *   key_wait          3726  editor_get_input's wait for editor_key bit 7 (and the same loop in
 *   key_wait_menu1/2  5CFC, 5D68 startup_menus)
 *
 * Timing. Every native here may decline (NativeTryFn): it first works out the cycles the
 * original would take (table mainloop_cycles.h, from the cpu8086.c cost model) without
 * writing anything, and declines when pc_irq_horizon() says the timer, the keyboard or the end
 * of the front-end frame would fall inside them. The original then runs that stretch with
 * interrupts at its own instruction boundaries, so the timeline is the same as with the natives
 * off. The loops (paused main loop, crash delay, key waits) run as many whole iterations as fit
 * before the horizon in one step, and decline for the last partial one.
 *
 * --verify. The entries have stop ranges: the original is run from the entry until it leaves
 * the block (a CALL) or comes back to the loop head; under --verify (native_logged) a loop
 * native does exactly one iteration and never declines. Flags are not compared (flag_mask 0):
 * no callee reads the flags left by the loop's TEST/SAR; the loops set the flags of their
 * last instruction anyway.
 */
#include <SDL3/SDL.h>

#include "native.h"
#include "game/state.h"
#include "mainloop_cycles.h"

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])
#define ENTRY_CYCLES 1

/* ---- cycles -------------------------------------------------------------------------- */

static const MlInsCost *ins_at(uint16_t addr)
{
    size_t lo = 0, hi = SDL_arraysize(ml_ins_cost);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ml_ins_cost[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    SDL_assert(lo < SDL_arraysize(ml_ins_cost) && ml_ins_cost[lo].addr == addr);
    return &ml_ins_cost[lo];
}

/* cost when it falls through (any instruction) / when it jumps */
static uint32_t fall(uint16_t a) { return ins_at(a)->cost; }
static uint32_t jump(uint16_t a) { return ins_at(a)->taken; }
/* REP STOS with CX elements: the table holds the cost for CX = 2 (10 per element) */
static uint32_t rep_stos(uint16_t a, uint16_t cx) { return ins_at(a)->cost - 20u + 10u * cx; }

/* The step may go on for n more cycles (see the header). Under --verify always. */
static bool fits(Pc *pc, uint64_t n)
{
    return native_logged(pc) || pc->csched || n < pc_irq_horizon(pc);
}

/* For the loops: the horizon, but under the C scheduler (3.22) at least one pass, since
 * there is no original code to decline to (interrupts come between the steps). */
static bool past(Pc *pc, uint64_t n, uint64_t h, bool any)
{
    return n >= h && (any || !pc->csched);
}

static void finish(Pc *pc, uint64_t cycles)
{
    pc->cpu.cycles += cycles - ENTRY_CYCLES;
}

static void set_flag(Pc *pc, uint16_t f, bool on)
{
    pc->cpu.flags = (uint16_t)(on ? pc->cpu.flags | f : pc->cpu.flags & ~f);
}

static bool parity8(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

/* ---- main_loop -------------------------------------------------------------------------
 * A plan is the block from IP up to the next CALL or the loop head, worked out with reads
 * only; commit() then makes its writes. */

typedef struct Plan {
    uint64_t cyc;
    uint16_t ip;  /* where the block ends: a callee, 01F8, or 0211 (hang) */
    uint16_t ret; /* return address pushed, 0 = none */
    uint16_t ax;
    bool clear_editor, rol_blink, inc_frame;
} Plan;

#define ML_TOP 0x01F8
#define ML_HANG 0x0211

/* Return addresses of the plain CALLs in the loop: entering there means calling the next one. */
static const uint16_t plain_calls[] = { 0x0202, 0x0229, 0x022C, 0x022F, 0x0232, 0x0235, 0x0238, 0x023B, 0x023E,
                                        0x0241, 0x0244, 0x0247, 0x024A, 0x024D, 0x0250, 0x0253, 0x0256, 0x0259,
                                        0x025C, 0x026D, 0x0270, 0x0283, 0x0286, 0x0292, 0x029A, 0x029D, 0x02A0,
                                        0x02A3, 0x02A6, 0x02A9, 0x02AC, 0x02B8, 0x02BB, 0x02BE, 0x02C1 };

static void call_at(Pc *pc, Plan *p, uint16_t at)
{
    uint32_t a = cpu_linear(GAME_CS, (uint16_t)(at + 1));
    int16_t rel = (int16_t)(pc->mem[a] | pc->mem[a + 1] << 8);
    p->cyc += fall(at);
    p->ret = (uint16_t)(at + 3);
    p->ip = (uint16_t)(at + 3 + rel);
}

static void plan_block(Pc *pc, uint16_t ip, Plan *p)
{
    SDL_zerop(p);
    p->ax = REG(AX);
    uint8_t al;
    bool cf;
    for (size_t i = 0; i < SDL_arraysize(plain_calls); i++)
        if (ip == plain_calls[i]) {
            call_at(pc, p, ip);
            return;
        }
    switch (ip) {
    case 0x01F8: goto l01F8;
    case 0x0205: goto l0205;
    case 0x0224: goto l0224;
    case 0x025F: goto l025F;
    case 0x0273: goto l0273;
    case 0x027E: goto l027E;
    case 0x0289: goto l0289;
    case 0x0295: goto l0295;
    case 0x02AF: goto l02AF;
    case 0x02C4: goto l02C4;
    default: SDL_assert(!"main_loop: unknown entry"); return;
    }

l01F8:
    p->cyc += fall(0x01F8);
    if (gs_editor_active(pc) & 1) {
        p->cyc += fall(0x01FD);
        call_at(pc, p, 0x01FF); /* editor_main */
        return;
    }
    p->cyc += jump(0x01FD);
    goto l020A;
l0205:
    p->cyc += fall(0x0205);
    p->clear_editor = true;
l020A:
    p->cyc += fall(0x020A) + fall(0x020D);
    p->ax = gs_timer_hooked(pc);
    if (!p->ax) {
        p->cyc += fall(0x020F);
        p->ip = ML_HANG;
        return;
    }
    p->cyc += jump(0x020F) + fall(0x0213);
    if (gs_paused(pc)) {
        p->cyc += jump(0x0218);
        p->ip = ML_TOP;
        return;
    }
    p->cyc += fall(0x0218) + fall(0x021A);
    if (gs_slew_mode(pc)) {
        p->cyc += fall(0x021F);
        call_at(pc, p, 0x0221); /* slew_update */
        return;
    }
    p->cyc += jump(0x021F);
    call_at(pc, p, 0x0226); /* flight_integrate */
    return;
l0224:
    p->cyc += jump(0x0224);
    call_at(pc, p, 0x022C); /* build_view_matrix */
    return;

    /* the 4-phase panel dispatch on frame_counter */
l025F:
    p->cyc += fall(0x025F) + fall(0x0262);
    p->ax = gs_frame_counter(pc);
    al = (uint8_t)p->ax;
    cf = al & 1;
    al = (uint8_t)((int8_t)al >> 1);
    if (cf) {
        p->cyc += jump(0x0264);
        goto l028B;
    }
    p->cyc += fall(0x0264) + fall(0x0266);
    cf = al & 1;
    al = (uint8_t)((int8_t)al >> 1);
    p->ax = (uint16_t)((p->ax & 0xFF00) | al);
    if (cf) {
        p->cyc += jump(0x0268);
        call_at(pc, p, 0x0280); /* phase 2: gauge_upd_1419 */
        return;
    }
    p->cyc += fall(0x0268);
    call_at(pc, p, 0x026A); /* phase 0: gauge_upd_1401 */
    return;
l0273:
    p->cyc += fall(0x0273);
    if (!(gs_frame_counter(pc) & 4)) {
        p->cyc += jump(0x0279);
        goto l02B5;
    }
    p->cyc += fall(0x0279);
    call_at(pc, p, 0x027B); /* nav_search_start */
    return;
l027E:
    p->cyc += jump(0x027E);
    goto l02B5;
l0289:
    p->cyc += jump(0x0289);
    goto l02B5;
l028B:
    p->cyc += fall(0x028B);
    cf = al & 1;
    al = (uint8_t)((int8_t)al >> 1);
    p->ax = (uint16_t)((p->ax & 0xFF00) | al);
    if (cf) {
        p->cyc += jump(0x028D);
        call_at(pc, p, 0x0297); /* phase 3: gauge_upd_1425 */
        return;
    }
    p->cyc += fall(0x028D);
    call_at(pc, p, 0x028F); /* phase 1: gauge_upd_140D */
    return;
l0295:
    p->cyc += jump(0x0295);
    call_at(pc, p, 0x02A0); /* nav_compute */
    return;
l02AF:
    p->cyc += fall(0x02AF) + jump(0x02B3);
    p->rol_blink = true;
    goto l02C4;
l02B5:
    call_at(pc, p, 0x02B5); /* upd_vsi */
    return;
l02C4:
    p->cyc += fall(0x02C4) + jump(0x02C8);
    p->inc_frame = true;
    p->ip = ML_TOP;
}

static bool n_main_loop(Pc *pc)
{
    Plan p;
    uint16_t entry = pc->cpu.ip;
    plan_block(pc, entry, &p);
    if (!fits(pc, p.cyc))
        return false; /* an interrupt is due inside: the original runs this block */
    uint64_t cyc = p.cyc;
    if (entry == ML_TOP && p.ip == ML_TOP && !native_logged(pc)) {
        /* paused: the loop reads only; run as many passes as fit before the next interrupt */
        uint64_t h = pc_irq_horizon(pc);
        while (cyc + p.cyc < h)
            cyc += p.cyc;
    }
    if (p.clear_editor)
        gs_set_editor_active(pc, 0);
    if (p.rol_blink) {
        uint16_t b = gs_blink_bits(pc);
        gs_set_blink_bits(pc, (uint16_t)(b << 1 | b >> 15));
    }
    if (p.inc_frame)
        gs_set_frame_counter(pc, (uint16_t)(gs_frame_counter(pc) + 1));
    REG(AX) = p.ax;
    if (p.ret)
        cpu_push(&pc->cpu, p.ret);
    pc->cpu.ip = p.ip;
    finish(pc, cyc);
    return true;
}

/* ---- draw_view_frame (5A9E) ---------------------------------------------------------------
 * In daylight (time_of_day bit 0) and not in radar view: clears the top line pair and the line
 * pair at 1090h of the back buffer, and ORs F0h into the first byte of every even/odd line pair
 * from 1090h up to 0 (the left border). */
static bool n_draw_view_frame(Pc *pc)
{
    if (pc->cpu.flags & F_DF)
        return false;
    uint64_t cyc = fall(0x5A9E);
    bool draw = false;
    if (!(gs_time_of_day(pc) & 1)) {
        cyc += jump(0x5AA4);
    } else {
        cyc += fall(0x5AA4) + fall(0x5AA6);
        if (gs_radar_view(pc)) {
            cyc += jump(0x5AAB);
        } else {
            draw = true;
            cyc += fall(0x5AAB) + fall(0x5AAD) + fall(0x5AB0) + fall(0x5AB3) + fall(0x5AB6) + rep_stos(0x5ABA, 0x28) +
                   fall(0x5ABC) + fall(0x5ABF) + rep_stos(0x5AC2, 0x28) + fall(0x5AC4) + fall(0x5AC6);
            /* BX = 1090h down to 0 in steps of 50h: 54 passes */
            uint32_t pass = fall(0x5AC9) + fall(0x5ACC) + fall(0x5AD1);
            cyc += 54u * pass + 53u * jump(0x5AD4) + fall(0x5AD4);
        }
    }
    cyc += fall(0x5AD6);
    if (!fits(pc, cyc))
        return false;
    if (draw) {
        uint16_t es = gs_view_buf_seg(pc);
        for (uint16_t di = 0; di < 0x50; di += 2)
            mem_write16(pc, es, di, 0);
        for (uint16_t di = 0x1090; di < 0x10E0; di += 2)
            mem_write16(pc, es, di, 0);
        for (int bx = 0x1090; bx >= 0; bx -= 0x50) {
            uint32_t a = cpu_linear(es, (uint16_t)bx), b = cpu_linear(es, (uint16_t)(bx + 0x2000));
            cpu_write8(&pc->cpu, a, (uint8_t)(cpu_read8(&pc->cpu, a) | 0xF0));
            cpu_write8(&pc->cpu, b, (uint8_t)(cpu_read8(&pc->cpu, b) | 0xF0));
        }
        SREG(ES) = es;
        REG(AX) = 0x00F0;
        REG(CX) = 0;
        REG(DI) = 0x10E0;
        REG(BX) = 0xFFB0;
    }
    native_ret(pc);
    finish(pc, cyc);
    return true;
}

/* ---- view_overlay_marks (0658) ------------------------------------------------------------
 * Not in radar view: in war mode with the forward view, clears the gun-sight marks; otherwise
 * (the gear down, or a view other than forward) draws the marks of the record list at
 * DS:04EF + 4 * view_not_forward. Each record: count byte (0 ends), word x step, word (low byte
 * = first row - 2 / 2 index, high byte = start x), word y step, byte rows; then a filled span of
 * overlay_fill per row. overlay_fill is cleared after each record. Run twice: first only to
 * count the cycles (no writes), then for real. */
static uint64_t overlay_body(Pc *pc, bool commit)
{
    uint64_t cyc = fall(0x0658);
    uint16_t ax = REG(AX), bx = REG(BX), cx = REG(CX), dx = REG(DX), si = REG(SI), di = REG(DI), bp = REG(BP);
    uint16_t es = SREG(ES);
    if (gs_radar_view(pc)) {
        cyc += jump(0x065D) + fall(0x06DE);
        goto ret;
    }
    cyc += fall(0x065D) + fall(0x065F) + fall(0x0663);
    si = gs_rd16(pc, GS_VIEW_NOT_FORWARD);
    if (si == 0x10) {
        cyc += fall(0x0666) + fall(0x0668);
        if (gs_war_mode(pc)) {
            static const struct { uint16_t mask; uint16_t off[4]; } groups[] = {
                { 0x07F0, { 0x0757, 0x27A7, 0x2897, 0x0937 } },
                { 0x3FFE, { 0x2757, 0x07A7, 0x08E7, 0x28E7 } },
                { 0xFE1F, { 0x07F6, 0x27F6, 0x2846, 0x0896 } },
                { 0xF83F, { 0x07F8, 0x27F8, 0x2848, 0x0898 } },
            };
            static const uint16_t ins[] = { 0x066F, 0x0670, 0x0674, 0x0677, 0x067B, 0x067F, 0x0683, 0x0687, 0x068A,
                                            0x068E, 0x0692, 0x0696, 0x069A, 0x069D, 0x06A1, 0x06A5, 0x06A9, 0x06AD,
                                            0x06B0, 0x06B4, 0x06B8, 0x06BC, 0x06C0, 0x06C2, 0x06C5, 0x06C8, 0x06C9 };
            cyc += fall(0x066D);
            for (size_t i = 0; i < SDL_arraysize(ins); i++)
                cyc += fall(ins[i]);
            if (commit) {
                uint16_t seg = gs_view_buf_seg(pc);
                cpu_push(&pc->cpu, SREG(DS));
                for (size_t g = 0; g < SDL_arraysize(groups); g++)
                    for (int k = 0; k < 4; k++)
                        mem_write16(pc, seg, groups[g].off[k], mem_read16(pc, seg, groups[g].off[k]) & groups[g].mask);
                mem_write16(pc, seg, 0x0846, 0);
                mem_write16(pc, seg, 0x0848, 0);
                SREG(DS) = cpu_pop(&pc->cpu);
                REG(AX) = 0;
                REG(SI) = si;
                native_ret(pc);
            }
            return cyc;
        }
        cyc += jump(0x066D) + fall(0x06CA);
        if (!gs_gear_down(pc)) {
            cyc += jump(0x06CF) + fall(0x06DE);
            goto ret;
        }
        cyc += fall(0x06CF);
    } else {
        cyc += jump(0x0666);
    }
    cyc += fall(0x06D1) + fall(0x06D3) + fall(0x06D5);
    si = (uint16_t)((si << 2) + 0x04EF);
    for (;;) {
        /* 06D9: next record */
        uint8_t n = ds_read8(pc, si++);
        ax = (uint16_t)((ax & 0xFF00) | n);
        cyc += fall(0x06D9) + fall(0x06DA);
        if (!n) {
            cyc += fall(0x06DC) + fall(0x06DE);
            break;
        }
        cyc += jump(0x06DC);
        static const uint16_t head[] = { 0x06DF, 0x06E1, 0x06E3, 0x06E4, 0x06E5, 0x06E8, 0x06E9, 0x06EB, 0x06ED,
                                         0x06EF, 0x06F0, 0x06F3, 0x06F4, 0x06F6, 0x06F8, 0x06FA, 0x06FB, 0x06FC };
        for (size_t i = 0; i < SDL_arraysize(head); i++)
            cyc += fall(head[i]);
        uint16_t xstep = ds_read16(pc, si);
        uint16_t w = ds_read16(pc, (uint16_t)(si + 2));
        uint16_t ystep = ds_read16(pc, (uint16_t)(si + 4));
        uint8_t rows = ds_read8(pc, (uint16_t)(si + 6));
        si = (uint16_t)(si + 7);
        bx = (uint8_t)w;
        dx = (uint16_t)(w & 0xFF00);
        bp = rows;
        uint16_t next = si;
        uint16_t first = (uint16_t)(n << 8 | 0x80); /* push ax (AH = count, AL = 80h) */
        if (commit) {
            gs_wr16(pc, 0x0414, xstep);
            gs_wr16(pc, 0x0401, ystep);
            cpu_push(&pc->cpu, first);
            cpu_pop(&pc->cpu);
            cpu_push(&pc->cpu, next);
        }
        si = first;
        uint16_t fill = gs_overlay_fill(pc);
        es = gs_view_buf_seg(pc);
        static const uint16_t row[] = { 0x0700, 0x0704, 0x0707, 0x070B, 0x070D, 0x070E, 0x0710, 0x0714, 0x0716, 0x0718,
                                        0x071A, 0x071F };
        uint32_t row_cyc = 0;
        for (size_t i = 0; i < SDL_arraysize(row); i++)
            row_cyc += fall(row[i]);
        do {
            dx = (uint16_t)(dx + ystep);
            bx = (uint16_t)((bx & 0xFF00) | (uint8_t)(bx + 2));
            di = gs_rd16(pc, (uint16_t)(GS_ROW_OFFSETS + bx));
            di = (uint16_t)(di + (uint16_t)(int16_t)(int8_t)(dx >> 8));
            si = (uint16_t)(si + xstep);
            cx = (uint16_t)(si >> 8);
            ax = fill;
            cyc += row_cyc + rep_stos(0x071D, cx);
            if (commit)
                for (uint16_t k = 0; k < cx; k++)
                    cpu_write8(&pc->cpu, cpu_linear(es, (uint16_t)(di + k)), (uint8_t)ax);
            di = (uint16_t)(di + cx);
            cx = 0;
            bp--;
            cyc += bp ? jump(0x0720) : fall(0x0720);
        } while (bp);
        /* 0722 */
        si = next;
        if (commit) {
            cpu_pop(&pc->cpu);
            gs_set_overlay_fill(pc, 0);
        }
        cyc += fall(0x0722) + fall(0x0723) + fall(0x0729);
        if (si != 0x0537) {
            cyc += jump(0x072D) + fall(0x0731);
            break;
        }
        cyc += fall(0x072D) + jump(0x072F);
    }
ret:
    if (commit) {
        REG(AX) = ax;
        REG(BX) = bx;
        REG(CX) = cx;
        REG(DX) = dx;
        REG(SI) = si;
        REG(DI) = di;
        REG(BP) = bp;
        SREG(ES) = es;
        native_ret(pc);
    }
    return cyc;
}

static bool n_view_overlay_marks(Pc *pc)
{
    if (pc->cpu.flags & F_DF)
        return false;
    uint64_t cyc = overlay_body(pc, false);
    if (!fits(pc, cyc))
        return false;
    overlay_body(pc, true);
    finish(pc, cyc);
    return true;
}

/* ---- resumable loop bodies --------------------------------------------------------------
 * When draw_view_frame or view_overlay_marks decline (an interrupt is due inside), the
 * original runs their head and then reaches these loops, which run as many passes as fit
 * before the interrupt and leave IP at the loop head (or past the loop when it is done). */

/* draw_view_frame's border loop at 5AC9: OR AL into ES:[BX] and ES:[BX+2000h], BX -= 50h
 * while BX >= 0. */
static bool n_view_frame_loop(Pc *pc)
{
    uint32_t pass = fall(0x5AC9) + fall(0x5ACC) + fall(0x5AD1);
    uint64_t h = native_logged(pc) ? UINT64_MAX : pc_irq_horizon(pc);
    uint16_t bx = REG(BX), es = SREG(ES);
    uint8_t al = (uint8_t)REG(AX);
    uint64_t cyc = 0;
    bool any = false;
    for (;;) {
        bool more = (int16_t)(bx - 0x50) >= 0;
        uint64_t c = pass + (more ? jump(0x5AD4) : fall(0x5AD4));
        if (past(pc, cyc + c, h, any))
            break;
        uint32_t a = cpu_linear(es, bx), b = cpu_linear(es, (uint16_t)(bx + 0x2000));
        cpu_write8(&pc->cpu, a, (uint8_t)(cpu_read8(&pc->cpu, a) | al));
        cpu_write8(&pc->cpu, b, (uint8_t)(cpu_read8(&pc->cpu, b) | al));
        bx = (uint16_t)(bx - 0x50);
        cyc += c;
        any = true;
        if (!more || native_logged(pc))
            break;
    }
    if (!any)
        return false;
    REG(BX) = bx;
    pc->cpu.ip = (int16_t)bx >= 0 ? 0x5AC9 : 0x5AD6;
    finish(pc, cyc);
    return true;
}

/* view_overlay_marks' row loop at 0700: one filled span per row (see overlay_body). */
static bool n_overlay_rows(Pc *pc)
{
    if (pc->cpu.flags & F_DF)
        return false;
    static const uint16_t row[] = { 0x0700, 0x0704, 0x0707, 0x070B, 0x070D, 0x070E, 0x0710, 0x0714, 0x0716, 0x0718,
                                    0x071A, 0x071F };
    uint32_t row_cyc = 0;
    for (size_t i = 0; i < SDL_arraysize(row); i++)
        row_cyc += fall(row[i]);
    uint64_t h = native_logged(pc) ? UINT64_MAX : pc_irq_horizon(pc);
    uint16_t ax = REG(AX), bx = REG(BX), cx = REG(CX), dx = REG(DX), si = REG(SI), di = REG(DI), bp = REG(BP);
    uint16_t es = SREG(ES);
    uint16_t xstep = gs_rd16(pc, 0x0414), ystep = gs_rd16(pc, 0x0401);
    uint64_t cyc = 0;
    bool any = false;
    for (;;) {
        uint16_t ndx = (uint16_t)(dx + ystep), nsi = (uint16_t)(si + xstep);
        uint16_t ncx = (uint16_t)(nsi >> 8);
        uint64_t c = row_cyc + rep_stos(0x071D, ncx) + (bp != 1 ? jump(0x0720) : fall(0x0720));
        if (past(pc, cyc + c, h, any))
            break;
        dx = ndx;
        si = nsi;
        bx = (uint16_t)((bx & 0xFF00) | (uint8_t)(bx + 2));
        di = (uint16_t)(gs_rd16(pc, (uint16_t)(GS_ROW_OFFSETS + bx)) + (uint16_t)(int16_t)(int8_t)(dx >> 8));
        ax = gs_overlay_fill(pc);
        for (uint16_t k = 0; k < ncx; k++)
            cpu_write8(&pc->cpu, cpu_linear(es, (uint16_t)(di + k)), (uint8_t)ax);
        di = (uint16_t)(di + ncx);
        cx = 0;
        bp--;
        cyc += c;
        any = true;
        if (!bp || native_logged(pc))
            break;
    }
    if (!any)
        return false;
    REG(AX) = ax;
    REG(BX) = bx;
    REG(CX) = cx;
    REG(DX) = dx;
    REG(SI) = si;
    REG(DI) = di;
    REG(BP) = bp;
    pc->cpu.ip = bp ? 0x0700 : 0x0722;
    finish(pc, cyc);
    return true;
}

/* ---- crash_delay (063C) -------------------------------------------------------------------
 * crash_handler's delay: CX passes of three MUL CL and a LOOP. */
static bool n_crash_delay(Pc *pc)
{
    uint32_t pass = fall(0x063C) + fall(0x063E) + fall(0x0640);
    uint64_t h = native_logged(pc) ? UINT64_MAX : pc_irq_horizon(pc);
    uint16_t ax = REG(AX), cx = REG(CX);
    uint64_t cyc = 0;
    bool any = false;
    for (;;) {
        uint64_t c = pass + (cx == 1 ? fall(0x0642) : jump(0x0642));
        if (past(pc, cyc + c, h, any))
            break;
        for (int k = 0; k < 3; k++)
            ax = (uint16_t)((uint8_t)ax * (uint8_t)cx);
        cx--;
        cyc += c;
        any = true;
        if (!cx || native_logged(pc))
            break;
    }
    if (!any)
        return false;
    REG(AX) = ax;
    REG(CX) = cx;
    set_flag(pc, F_CF, ax >> 8);
    set_flag(pc, F_OF, ax >> 8);
    set_flag(pc, F_ZF, false);
    pc->cpu.ip = cx ? 0x063C : 0x0644;
    finish(pc, cyc);
    return true;
}

/* ---- key_wait (3726, 5CFC, 5D68) ----------------------------------------------------------
 * MOV AL,[editor_key] / OR AL,AL / JNS back: waits for the keyboard interrupt to set bit 7. */
static bool key_wait(Pc *pc, uint16_t head)
{
    uint16_t at_or = (uint16_t)(head + 3), at_jns = (uint16_t)(head + 5);
    uint8_t key = gs_editor_key(pc);
    uint64_t cyc;
    if (key & 0x80) {
        cyc = fall(head) + fall(at_or) + fall(at_jns);
        if (!fits(pc, cyc))
            return false;
        pc->cpu.ip = (uint16_t)(head + 7);
    } else {
        uint32_t pass = fall(head) + fall(at_or) + jump(at_jns);
        uint64_t h = native_logged(pc) ? (uint64_t)pass + 1 : pc_irq_horizon(pc);
        if (pc->csched && h <= pass)
            h = (uint64_t)pass + 1;
        if (pass >= h)
            return false;
        cyc = (uint64_t)pass * ((h - 1) / pass); /* whole passes before the next interrupt */
        pc->cpu.ip = head;
    }
    REG(AX) = (uint16_t)((REG(AX) & 0xFF00) | key);
    set_flag(pc, F_CF, false);
    set_flag(pc, F_OF, false);
    set_flag(pc, F_ZF, key == 0);
    set_flag(pc, F_SF, key & 0x80);
    set_flag(pc, F_PF, parity8(key));
    finish(pc, cyc);
    return true;
}

static bool n_key_wait(Pc *pc) { return key_wait(pc, 0x3726); }
static bool n_key_wait_menu1(Pc *pc) { return key_wait(pc, 0x5CFC); }
static bool n_key_wait_menu2(Pc *pc) { return key_wait(pc, 0x5D68); }

/* ---- entries ---------------------------------------------------------------------------- */

static const uint16_t ml_stops[] = { ML_TOP, ML_HANG, 0 };
static const uint16_t crash_stops[] = { 0x063C, 0 };
static const uint16_t frame_loop_stops[] = { 0x5AC9, 0 };
static const uint16_t overlay_rows_stops[] = { 0x0700, 0 };
static const uint16_t key_stops[] = { 0x3726, 0 };
static const uint16_t menu1_stops[] = { 0x5CFC, 0 };
static const uint16_t menu2_stops[] = { 0x5D68, 0 };

#define ML(nm, at)                                                                                              \
    { .name = nm, .seg = GAME_CS, .off = at, .try_fn = n_main_loop, .enabled = true, .cycles = ENTRY_CYCLES, .exact_cycles = true,   \
      .stop_lo = ML_TOP, .stop_hi = 0x02CB, .stops = ml_stops }

NativeEntry native_mainloop[] = {
    ML("main_loop", 0x01F8),
    ML("main_loop@0202", 0x0202), ML("main_loop@0205", 0x0205), ML("main_loop@0224", 0x0224),
    ML("main_loop@0229", 0x0229), ML("main_loop@022C", 0x022C), ML("main_loop@022F", 0x022F),
    ML("main_loop@0232", 0x0232), ML("main_loop@0235", 0x0235), ML("main_loop@0238", 0x0238),
    ML("main_loop@023B", 0x023B), ML("main_loop@023E", 0x023E), ML("main_loop@0241", 0x0241),
    ML("main_loop@0244", 0x0244), ML("main_loop@0247", 0x0247), ML("main_loop@024A", 0x024A),
    ML("main_loop@024D", 0x024D), ML("main_loop@0250", 0x0250), ML("main_loop@0253", 0x0253),
    ML("main_loop@0256", 0x0256), ML("main_loop@0259", 0x0259), ML("main_loop@025C", 0x025C),
    ML("main_loop@025F", 0x025F), ML("main_loop@026D", 0x026D), ML("main_loop@0270", 0x0270),
    ML("main_loop@0273", 0x0273), ML("main_loop@027E", 0x027E), ML("main_loop@0283", 0x0283),
    ML("main_loop@0286", 0x0286), ML("main_loop@0289", 0x0289), ML("main_loop@0292", 0x0292),
    ML("main_loop@0295", 0x0295), ML("main_loop@029A", 0x029A), ML("main_loop@029D", 0x029D),
    ML("main_loop@02A0", 0x02A0), ML("main_loop@02A3", 0x02A3), ML("main_loop@02A6", 0x02A6),
    ML("main_loop@02A9", 0x02A9), ML("main_loop@02AC", 0x02AC), ML("main_loop@02AF", 0x02AF),
    ML("main_loop@02B8", 0x02B8), ML("main_loop@02BB", 0x02BB), ML("main_loop@02BE", 0x02BE),
    ML("main_loop@02C1", 0x02C1), ML("main_loop@02C4", 0x02C4),
    { .name = "draw_view_frame", .seg = GAME_CS, .off = 0x5A9E, .try_fn = n_draw_view_frame, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true },
    { .name = "view_overlay_marks", .seg = GAME_CS, .off = 0x0658, .try_fn = n_view_overlay_marks, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true },
    { .name = "view_frame_loop", .seg = GAME_CS, .off = 0x5AC9, .try_fn = n_view_frame_loop, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true, .stop_lo = 0x5AC9, .stop_hi = 0x5AD6, .stops = frame_loop_stops },
    { .name = "overlay_rows", .seg = GAME_CS, .off = 0x0700, .try_fn = n_overlay_rows, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true, .stop_lo = 0x0700, .stop_hi = 0x0722,
      .stops = overlay_rows_stops },
    { .name = "crash_delay", .seg = GAME_CS, .off = 0x063C, .try_fn = n_crash_delay, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true, .stop_lo = 0x063C, .stop_hi = 0x0644, .stops = crash_stops },
    { .name = "key_wait", .seg = GAME_CS, .off = 0x3726, .try_fn = n_key_wait, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true, .stop_lo = 0x3726, .stop_hi = 0x372D, .stops = key_stops },
    { .name = "key_wait_menu1", .seg = GAME_CS, .off = 0x5CFC, .try_fn = n_key_wait_menu1, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true, .stop_lo = 0x5CFC, .stop_hi = 0x5D03, .stops = menu1_stops },
    { .name = "key_wait_menu2", .seg = GAME_CS, .off = 0x5D68, .try_fn = n_key_wait_menu2, .enabled = true,
      .cycles = ENTRY_CYCLES, .exact_cycles = true, .stop_lo = 0x5D68, .stop_hi = 0x5D6F, .stops = menu2_stops },
    { .name = NULL },
};
