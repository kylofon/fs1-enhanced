/* Natives for subphase 3.14 flight model: integration and environment (see docs/PHASE3_PLAN.md
 * and docs/subphases/3.14.md).
 *
 * Each routine is transliterated so that registers and memory end up exactly as the original
 * leaves them, including the words its CALLs and PUSHes leave below SP. Cycles: every native
 * charges the cost of the original instructions on the path it took, from the per-instruction
 * table in flight_motion_cycles.h (run() for a straight run of instructions, taken() for a
 * conditional jump or LOOP that jumps), plus the measured cost of sincos and div_q15.
 *
 * Interrupts. The original runs with interrupts enabled, and the timer handler (flight_forces)
 * changes state these routines read. A native holds interrupts until it returns, so when a
 * timer or keyboard interrupt would arrive while the original is still running, the native
 * undoes its writes and lets the CPU run the original instead (irq_hand_back). The two paths
 * with busy-wait loops that run for seconds (crash message: 65535 x 3 MULs; engine starter:
 * 65000 MULs) are always handed back. Under --verify the original runs without interrupts,
 * so there the natives never hand back, and the two busy-wait paths run the original
 * routine inside the native. */
#include "native.h"
#include "game/state.h"
#include "fixmath.h"
#include "flight_motion_cycles.h"

/* ---- cycles ------------------------------------------------------------------------- */

static uint32_t cyc; /* original cycles of the path taken so far */

static const FmInsCost *ins_at(uint16_t addr)
{
    size_t lo = 0, hi = sizeof fm_ins_cost / sizeof fm_ins_cost[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (fm_ins_cost[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return &fm_ins_cost[lo];
}

/* The original executes every instruction in [from, to) in order (jumps fall through). */
static void run(uint16_t from, uint16_t to)
{
    for (const FmInsCost *i = ins_at(from); i < fm_ins_cost + sizeof fm_ins_cost / sizeof fm_ins_cost[0] &&
                                            i->addr < to; i++)
        cyc += i->cost;
}

/* The conditional jump (or LOOP, or SAR r,CL with CL = 3) at addr, already counted by run(),
 * was taken. */
static void taken(uint16_t addr)
{
    const FmInsCost *i = ins_at(addr);
    cyc += (uint32_t)(i->taken - i->cost);
}

/* Each entry's .cycles; the natives add the rest of the original's cost themselves. */
#define ENTRY_CYCLES 1

static void charge(Pc *pc) { pc->cpu.cycles += cyc - ENTRY_CYCLES; }

/* sincos 0050:45C0 and div_q15 0050:1BEE costs, as measured in 3.1 (callee only, with RET). */
static uint32_t sincos_cycles(uint16_t angle)
{
    if (angle == 0x8000)
        return 84 + 597;
    if (angle & 0x8000)
        return 92 + sincos_cycles((uint16_t)-angle);
    return angle <= 0x4000 ? 616 : 597;
}
static const uint16_t div_q15_cycles[2][2][2] = {
    { { 84, 226 }, { 94, 241 } },
    { { 122, 269 }, { 115, 257 } },
};

/* ---- registers, memory, stack ------------------------------------------------------- */

typedef struct Regs {
    uint16_t ax, bx, cx, dx, si, di, bp;
} Regs;

static Regs regs_load(Pc *pc)
{
    const uint16_t *r = pc->cpu.regs;
    return (Regs){ r[R_AX], r[R_BX], r[R_CX], r[R_DX], r[R_SI], r[R_DI], r[R_BP] };
}

static void regs_store(Pc *pc, const Regs *g)
{
    uint16_t *r = pc->cpu.regs;
    r[R_AX] = g->ax;
    r[R_BX] = g->bx;
    r[R_CX] = g->cx;
    r[R_DX] = g->dx;
    r[R_SI] = g->si;
    r[R_DI] = g->di;
    r[R_BP] = g->bp;
}

static uint8_t lo8(uint16_t v) { return (uint8_t)v; }
static uint8_t hi8(uint16_t v) { return (uint8_t)(v >> 8); }
static uint16_t with_lo(uint16_t v, uint8_t b) { return (uint16_t)((v & 0xFF00) | b); }
static uint16_t with_hi(uint16_t v, uint8_t b) { return (uint16_t)((v & 0x00FF) | b << 8); }
static uint16_t sext8(uint8_t b) { return (uint16_t)(int16_t)(int8_t)b; }
static uint16_t sign_of(uint16_t v) { return (v & 0x8000) ? 0xFFFF : 0; } /* CWD */

static uint8_t rd8(Pc *pc, uint16_t off) { return ds_read8(pc, off); }
static uint16_t rd16(Pc *pc, uint16_t off) { return ds_read16(pc, off); }
/* Every memory write of a native goes through put8, which keeps the old byte so that a call
 * can be handed back to the original (see irq_hand_back). */
#define UNDO_MAX 256
static uint32_t undo_addr[UNDO_MAX];
static uint8_t undo_old[UNDO_MAX];
static int undo_n;
static bool undo_full;

static void put8(Pc *pc, uint32_t a, uint8_t v)
{
    if (undo_n < UNDO_MAX) {
        undo_addr[undo_n] = a;
        undo_old[undo_n++] = pc->mem[a];
    } else {
        undo_full = true;
    }
    cpu_write8(&pc->cpu, a, v);
}
static void put16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v)
{
    put8(pc, cpu_linear(seg, off), (uint8_t)v);
    put8(pc, cpu_linear(seg, (uint16_t)(off + 1)), (uint8_t)(v >> 8));
}
static void wr8(Pc *pc, uint16_t off, uint8_t v) { put8(pc, cpu_linear(GAME_DS, off), v); }
static void wr16(Pc *pc, uint16_t off, uint16_t v) { put16(pc, GAME_DS, off, v); }
static uint32_t rd32(Pc *pc, uint16_t off) { return rd16(pc, off) | (uint32_t)rd16(pc, (uint16_t)(off + 2)) << 16; }
static void wr32(Pc *pc, uint16_t off, uint32_t v)
{
    wr16(pc, off, (uint16_t)v);
    wr16(pc, (uint16_t)(off + 2), (uint16_t)(v >> 16));
}

/* Writes a word the original leaves below SP (a CALL's return address or a PUSH). */
static void stack_word(Pc *pc, uint16_t sp, uint16_t v) { put16(pc, pc->cpu.sregs[S_SS], sp, v); }

/* IMUL r16 then the Q15 idiom SHL AX,1 / RCL DX,1: DX:AX. */
static void mul_q15(Regs *r, uint16_t a, uint16_t b)
{
    uint32_t p = fx_mul_q15_32((int16_t)a, (int16_t)b);
    r->ax = (uint16_t)p;
    r->dx = (uint16_t)(p >> 16);
}

/* Plain IMUL r16: DX:AX = a * b. */
static void imul16(Regs *r, uint16_t a, uint16_t b)
{
    uint32_t p = (uint32_t)((int32_t)(int16_t)a * (int16_t)b);
    r->ax = (uint16_t)p;
    r->dx = (uint16_t)(p >> 16);
}

/* IMUL r/m8: AX = AL * b. */
static uint16_t imul8(uint8_t al, uint8_t b) { return (uint16_t)((int8_t)al * (int8_t)b); }

/* "CALL sincos" with the caller's SP = sp: return address at sp-2 plus the words the inner
 * calls of sincos leave (see math.c). AX = sin, CX = cos; BX, DX, SI, DI, BP clobbered. */
static void call_sincos(Pc *pc, Regs *r, uint16_t sp, uint16_t ret_ip, uint16_t angle)
{
    uint16_t s = (uint16_t)(sp - 2);
    stack_word(pc, s, ret_ip);
    uint16_t a = angle;
    if (a & 0x8000) {
        s = (uint16_t)(s - 2);
        stack_word(pc, s, 0x45FA);
        a = (uint16_t)-a;
    }
    stack_word(pc, (uint16_t)(s - 2), (a & 0x8000) || a > 0x4000 ? 0x45D5 : 0x45EF);
    FxSincos q = fx_sincos_regs(pc, GAME_DS, angle);
    r->ax = q.ax;
    r->bx = q.bx;
    r->cx = q.cx;
    r->dx = q.dx;
    r->si = q.si;
    r->di = q.di;
    r->bp = q.bp;
    cyc += sincos_cycles(angle);
}

/* "CALL div_q15": DX:AX / CX -> DX (Q15, saturating), with its register side effects.
 * The dividends here are constants (08C0-08C4, never written), so the INT 0 case of
 * fx_div_q15_faults cannot occur. */
static void call_div_q15(Regs *r)
{
    uint16_t ax = r->ax, dx = r->dx, cx = r->cx;
    int32_t n = (int32_t)((uint32_t)dx << 16 | ax);
    int16_t d = (int16_t)cx;
    if (cx == 0) {
        cyc += (dx & 0x8000) ? 54 : 66;
        r->dx = (uint16_t)fx_div_q15(n, d);
        return;
    }
    int dneg = dx >> 15, cneg = cx >> 15;
    if (dx & 0x8000) {
        uint32_t un = 0u - (uint32_t)n;
        ax = (uint16_t)un;
        dx = (uint16_t)(un >> 16);
        r->bx = 0;
    }
    if (cx & 0x8000)
        cx = (uint16_t)-cx;
    r->ax = ax;
    r->cx = cx;
    if ((int16_t)dx >= (int16_t)cx || dx >= cx) {
        cyc += div_q15_cycles[dneg][cneg][0];
    } else {
        cyc += div_q15_cycles[dneg][cneg][1];
        r->ax = (uint16_t)(((uint32_t)dx << 16 | ax) / cx);
    }
    r->dx = (uint16_t)fx_div_q15(n, d);
}

/* ---- running the original ---------------------------------------------------------- */

/* Single-steps the original routine at CS:IP until it returns, natives off (as --verify runs
 * the original). Adds its cycles to cyc. */
static void run_original(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    const uint8_t *map = c->hook_map;
    uint16_t sp0 = c->regs[R_SP];
    uint16_t ret_ip = mem_read16(pc, c->sregs[S_SS], sp0), ret_cs = c->sregs[S_CS];
    uint64_t start = c->cycles;
    c->hook_map = NULL;
    for (long n = 0; n < 5000000L; n++) {
        native_or_cpu_step(pc);
        uint16_t d = (uint16_t)(c->regs[R_SP] - sp0);
        if (c->ip == ret_ip && c->sregs[S_CS] == ret_cs && d >= 2 && d < 0x8000)
            break;
    }
    c->hook_map = map;
    cyc += (uint32_t)(c->cycles - start);
    c->cycles = start;
}

/* --verify attaches its write log only while it runs a routine; in normal play it is NULL. */
static bool under_verify(Pc *pc) { return pc->cpu.write_log != NULL; }

/* Starts a native call: no cycles, empty undo log. */
static void begin(void)
{
    cyc = 0;
    undo_n = 0;
    undo_full = false;
}

/* The original runs with interrupts enabled, and the timer handler (flight_forces) and the
 * keyboard handler change state these routines read. A native holds interrupts until it
 * returns, so when one would arrive while the original is still running, the native's result
 * could differ from the original's. In that case the call is handed back: the native's writes
 * are undone and the CPU continues in the original routine after its first instruction
 * (which the caller performs). True when that has to happen; under --verify interrupts are
 * off for the original too, so never. */
static bool irq_hand_back(Pc *pc)
{
    if (under_verify(pc) || undo_full || pc->csched)
        return false;
    const PitChannel *t = &pc->pit[0];
    bool timer = t->loaded && t->count - (int32_t)((pc->pit_cycle_frac + cyc) / 4) <= 0;
    bool key = !pc->kbd_full && pc->kbd_head != pc->kbd_tail && pc->cpu.cycles + cyc >= pc->kbd_next_cycle;
    if (!timer && !key)
        return false;
    for (int i = undo_n; i-- > 0;)
        cpu_write8(&pc->cpu, undo_addr[i], undo_old[i]);
    return true;
}

/* TEST r/m8,FF flags. */
static void flags_test8(Pc *pc, uint8_t v)
{
    uint8_t p = v;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    uint16_t f = (uint16_t)((v ? 0 : F_ZF) | ((v & 0x80) ? F_SF : 0) | ((p & 1) ? 0 : F_PF));
    uint16_t m = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF;
    pc->cpu.flags = (uint16_t)((pc->cpu.flags & ~m) | f);
}
static void flags_cmp8(Pc *pc, uint8_t a, uint8_t b);

/* Hands the call back after performing the routine's first instruction (see irq_hand_back). */
static void continue_original(Pc *pc, uint16_t entry)
{
    switch (entry) {
    case 0x1906: /* mov byte [08F6],1 */
        gs_set_not_slew(pc, 1);
        cyc = 0, run(0x1906, 0x190B), pc->cpu.ip = 0x190B;
        break;
    case 0x1AF7: /* test byte [slew_mode],FF */
        flags_test8(pc, gs_slew_mode(pc));
        cyc = 0, run(0x1AF7, 0x1AFC), pc->cpu.ip = 0x1AFC;
        break;
    case 0x1C35: /* test byte [08F6],FF */
        flags_test8(pc, gs_not_slew(pc));
        cyc = 0, run(0x1C35, 0x1C3A), pc->cpu.ip = 0x1C3A;
        break;
    case 0x2F56: /* mov ax,[30D6] */
        pc->cpu.regs[R_AX] = ds_read16(pc, (uint16_t)(GS_ALTITUDE + 1));
        cyc = 0, run(0x2F56, 0x2F59), pc->cpu.ip = 0x2F59;
        break;
    case 0x2F10: /* cmp byte [on_ground],0 */
        flags_cmp8(pc, gs_on_ground(pc), 0);
        cyc = 0, run(0x2F10, 0x2F15), pc->cpu.ip = 0x2F15;
        break;
    case 0x2D00: /* cmp byte [war_mode],1 */
        flags_cmp8(pc, gs_war_mode(pc), 1);
        cyc = 0, run(0x2D00, 0x2D05), pc->cpu.ip = 0x2D05;
        break;
    default: /* 0625: mov bl,[crash_code] */
        pc->cpu.regs[R_BX] = (uint16_t)((pc->cpu.regs[R_BX] & 0xFF00) | gs_crash_code(pc));
        cyc = 0, run(0x0625, 0x0629), pc->cpu.ip = 0x0629;
        break;
    }
    charge(pc);
}

/* ---- game state (DS = 0618) -------------------------------------------------------- */

enum {
    PREV_TIME_HI = 0x0898,
    MASS_K2 = 0x08C2,
    MASS_K3 = 0x08C4,
    MAG_R_ON = 0x1DD3,
    MAG_R_OK = 0x1DD5,
    SHEAR_2 = 0x2075,
    SHEAR_3 = 0x2077,
    WIND_2 = 0x207D,
    WIND_1 = 0x2081,
    WIND_SURFACE = 0x2085,
};

/* ---- flight_integrate 0050:1906 ------------------------------------------------------ */

/* 0050:3C9F: altitude = editor altitude (feet, +9) * 0.305 - ground elevation, at least 3. */
static void alt_from_editor(Pc *pc, Regs *r)
{
    run(0x3C9F, 0x3CB3);
    r->cx = 0x4E0C;
    uint32_t p = (uint32_t)(uint16_t)(gs_editor_alt(pc) + 9) * 0x4E0C;
    r->ax = (uint16_t)p;
    r->dx = (uint16_t)((p >> 16) - gs_ground_elev(pc));
    if ((int16_t)r->dx >= 3) {
        taken(0x3CB1);
    } else {
        run(0x3CB3, 0x3CB6);
        r->dx = 3;
    }
    run(0x3CB6, 0x3CC0);
    wr16(pc, (uint16_t)(GS_ALTITUDE + 1), r->dx);
    wr8(pc, GS_ALTITUDE, 0);
}

/* 0050:0D93 key_period_brakes, reached with on_ground set: airspeed -= 0100, floor 0. */
static void brakes(Pc *pc, Regs *r)
{
    run(0x0D93, 0x0DA2);
    r->ax = (uint16_t)(gs_airspeed(pc) - 0x100);
    if (r->ax & 0x8000) {
        run(0x0DA2, 0x0DA4);
        r->ax = 0;
    } else {
        taken(0x0DA0);
    }
    run(0x0DA4, 0x0DA8);
    wr16(pc, GS_AIRSPEED, r->ax);
}

static void n_flight_integrate(Pc *pc)
{
    Regs r = regs_load(pc);
    uint16_t sp = pc->cpu.regs[R_SP];
    begin();
    wr8(pc, GS_NOT_SLEW, 1);

    /* dt: the timer's 32-bit time base (tick18:pit_accum) since the last frame, / 32 */
    run(0x1906, 0x196D);
    uint16_t now_lo = gs_pit_accum(pc), now_hi = gs_tick18(pc);
    r.bx = rd16(pc, GS_PREV_TIME);
    r.cx = rd16(pc, (uint16_t)(GS_PREV_TIME + 2));
    wr16(pc, GS_PREV_TIME, now_lo);
    wr16(pc, (uint16_t)(GS_PREV_TIME + 2), now_hi);
    uint32_t elapsed = ((uint32_t)now_hi << 16 | now_lo) - ((uint32_t)r.cx << 16 | r.bx);
    uint16_t dt = (uint16_t)((int32_t)elapsed >> 5);
    wr16(pc, GS_FRAME_DT, dt);

    /* heading += turn rate * dt; distance flown = airspeed * dt */
    mul_q15(&r, (uint16_t)gs_turn_rate(pc), dt);
    uint16_t heading = (uint16_t)((uint16_t)gs_heading(pc) + r.dx);
    wr16(pc, GS_HEADING, heading);
    mul_q15(&r, dt, gs_airspeed(pc));
    uint16_t dist = r.dx;
    wr16(pc, GS_FRAME_DIST, dist);

    /* track direction = heading + crab term; sin/cos of it split the horizontal step */
    imul16(&r, (uint16_t)gs_crab_b(pc), (uint16_t)gs_crab_a(pc));
    call_sincos(pc, &r, sp, 0x196D, (uint16_t)(r.dx + heading));
    uint16_t sin_track = r.ax, cos_track = r.cx;
    wr16(pc, GS_SIN_TRACK, sin_track);
    wr16(pc, GS_COS_TRACK, cos_track);
    run(0x196D, 0x19FE);
    mul_q15(&r, (uint16_t)gs_cos_pitch(pc), dist);
    uint16_t horiz = r.dx;
    wr16(pc, GS_HORIZ_STEP, horiz);
    mul_q15(&r, sin_track, horiz);
    wr16(pc, GS_EAST_STEP, r.dx);
    mul_q15(&r, cos_track, horiz);
    wr16(pc, GS_NORTH_STEP, r.dx);
    mul_q15(&r, (uint16_t)gs_sin_pitch(pc), dist);
    wr16(pc, GS_CLIMB_STEP, r.dx);

    /* roll rate = roll moment / roll inertia; bank += roll rate * distance * 4 */
    mul_q15(&r, gs_inv_roll_inertia(pc), (uint16_t)gs_roll_moment(pc));
    uint16_t roll_rate = r.dx;
    wr16(pc, GS_ROLL_RATE, roll_rate);
    uint32_t p = fx_mul_q15_32((int16_t)roll_rate, (int16_t)dist) << 1;
    r.ax = (uint16_t)p;
    r.dx = (uint16_t)(p >> 16);
    wr16(pc, GS_BANK, (uint16_t)(rd16(pc, GS_BANK) + r.dx));

    /* position and altitude (32-bit, sign-extended steps) */
    wr32(pc, GS_POS_NORTH, rd32(pc, GS_POS_NORTH) + (uint32_t)(int32_t)(int16_t)rd16(pc, GS_NORTH_STEP));
    wr32(pc, GS_POS_EAST, rd32(pc, GS_POS_EAST) + (uint32_t)(int32_t)(int16_t)rd16(pc, GS_EAST_STEP));
    r.ax = (uint16_t)gs_climb_step(pc);
    r.dx = sign_of(r.ax);
    uint32_t alt = (uint32_t)gs_altitude(pc) + (uint32_t)(int32_t)(int16_t)r.ax;
    wr32(pc, GS_ALTITUDE, alt);
    if (alt & 0x80000000u) { /* below zero: put it on the ground (0300 = 3.0) */
        run(0x19FE, 0x1A0A);
        r.dx = 0;
        r.ax = 0x300;
        wr32(pc, GS_ALTITUDE, 0x300);
    } else {
        taken(0x19FC);
    }

    /* a few frames after a reset, the editor altitude is applied again */
    run(0x1A0A, 0x1A11);
    if (gs_alt_reset(pc)) {
        run(0x1A11, 0x1A18);
        stack_word(pc, (uint16_t)(sp - 2), 0x1A14);
        alt_from_editor(pc, &r);
        wr8(pc, GS_ALT_RESET, (uint8_t)(rd8(pc, GS_ALT_RESET) - 1));
    } else {
        taken(0x1A0F);
    }

    /* view pitch. The original loads DX = angle of attack and then multiplies AX (whatever
     * the code above left there) by [08C6]; the IMUL ignores DX. Kept as is. */
    run(0x1A18, 0x1A36);
    imul16(&r, r.ax, gs_view_trim_k(pc));
    wr16(pc, GS_VIEW_PITCH_TRIM, r.dx);
    r.ax = (uint16_t)(0x300 - (uint16_t)gs_pitch(pc) + r.dx);
    if (!gs_on_ground(pc)) { /* previous frame's value */
        run(0x1A36, 0x1A3A);
        r.ax = (uint16_t)(r.ax + (uint16_t)gs_flaps_view(pc));
    } else {
        taken(0x1A34);
    }
    run(0x1A3A, 0x1A60);
    r.ax = (uint16_t)(r.ax + (uint16_t)gs_touchdown_view(pc));
    wr16(pc, GS_VIEW_PITCH, r.ax);
    uint16_t bank = (uint16_t)gs_bank(pc);
    wr16(pc, GS_VIEW_BANK, (uint16_t)-bank);
    wr16(pc, GS_VIEW_HEADING, heading);

    /* on the ground when the 24-bit altitude is below 0301 (3.004) */
    uint16_t alt_lo = rd16(pc, GS_ALTITUDE);
    uint8_t alt_hi = rd8(pc, (uint16_t)(GS_ALTITUDE + 2));
    bool borrow1 = alt_lo < 0x301, borrow2 = alt_hi < (uint8_t)borrow1;
    r.ax = (uint16_t)(alt_lo - 0x301);
    r.dx = with_lo(r.dx, (uint8_t)(alt_hi - borrow1));
    uint8_t ground = 0;
    if (borrow2) {
        run(0x1A60, 0x1A62);
        ground = 0xFF;
    } else {
        taken(0x1A5E);
    }
    r.ax = with_lo(r.ax, ground);
    wr8(pc, GS_ON_GROUND, ground);
    run(0x1A62, 0x1A69);

    if (!ground) {
        /* airborne: on the first frame after leaving the ground, sometimes pick a new
         * take-off spot value from the position */
        taken(0x1A67);
        run(0x1AD2, 0x1AD9);
        if (!gs_on_ground_prev(pc)) {
            taken(0x1AD7);
            run(0x1AF5, 0x1AF6);
        } else {
            run(0x1AD9, 0x1AE8);
            r.ax = 0;
            wr16(pc, GS_TOUCHDOWN_VIEW, 0);
            wr8(pc, GS_ON_GROUND_PREV, 0);
            r.ax = rd16(pc, (uint16_t)(GS_POS_NORTH + 1));
            if (r.ax & 7) {
                taken(0x1AE6);
                run(0x1AF5, 0x1AF6);
            } else {
                run(0x1AE8, 0x1AF6);
                r.ax = (uint16_t)(((r.ax + rd16(pc, (uint16_t)(GS_POS_EAST + 1))) & 0xFF) - 0x80);
                wr16(pc, GS_TAKEOFF_SPOT, r.ax);
            }
        }
        goto done;
    }

    /* on the ground: wings level, nose not below the horizon */
    run(0x1A69, 0x1A79);
    r.ax = 0;
    wr16(pc, GS_BANK, 0);
    wr16(pc, GS_ROLL_RATE, 0);
    if ((uint16_t)gs_pitch(pc) & 0x8000) {
        run(0x1A79, 0x1A7C);
        wr16(pc, GS_PITCH, 0);
    } else {
        taken(0x1A77);
    }
    run(0x1A7C, 0x1A83);
    if (gs_alt_reset(pc)) { /* just reset: brake instead of the landing checks */
        run(0x1A83, 0x1A88);
        stack_word(pc, (uint16_t)(sp - 2), 0x1A86);
        brakes(pc, &r);
        goto touchdown;
    }
    taken(0x1A81);

    /* landing checks: gear up -> 0A (overridden by a hard or banked landing -> 04) */
    run(0x1A88, 0x1A8F);
    if (!gs_gear_down(pc)) {
        run(0x1A8F, 0x1A94);
        wr8(pc, GS_CRASH_CODE, 0x0A);
    } else {
        taken(0x1A8D);
    }
    run(0x1A94, 0x1A9B);
    uint8_t al = hi8((uint16_t)gs_vertical_speed(pc));
    if (!(al & 0x80)) { /* not sinking */
        r.ax = with_lo(r.ax, al);
        taken(0x1A99);
        goto touchdown;
    }
    run(0x1A9B, 0x1A9F);
    al = (uint8_t)(al + 6);
    if (al & 0x80) { /* sinking faster than 0600 */
        r.ax = with_lo(r.ax, al);
        taken(0x1A9D);
        goto hard;
    }
    run(0x1A9F, 0x1AA6);
    al = hi8((uint16_t)gs_view_bank(pc));
    if (al & 0x80) {
        run(0x1AA6, 0x1AA8);
        al = (uint8_t)-al;
    } else {
        taken(0x1AA4);
    }
    run(0x1AA8, 0x1AAC);
    r.ax = with_lo(r.ax, al);
    if (!((uint8_t)(al - 0x0A) & 0x80)) { /* |bank| >= 0A00 (14 degrees) */
        taken(0x1AAA);
        goto hard;
    }

touchdown:
    /* on touchdown: sound 8 and a view kick from the sink rate */
    run(0x1AAC, 0x1ABB);
    al = gs_on_ground(pc);
    {
        bool same = al == gs_on_ground_prev(pc);
        wr8(pc, GS_ON_GROUND_PREV, al);
        r.ax = 0;
        if (same) {
            taken(0x1AB9);
        } else {
            run(0x1ABB, 0x1AC6);
            wr8(pc, GS_SOUND_STATE, 8);
            r.ax = (uint16_t)(hi8((uint16_t)gs_vertical_speed(pc)) << 9);
        }
    }
    run(0x1AC6, 0x1ACB);
    wr16(pc, GS_TOUCHDOWN_VIEW, r.ax);
    run(0x1AF6, 0x1AF7);
    goto done;

hard:
    run(0x1ACC, 0x1AD2);
    wr8(pc, GS_CRASH_CODE, 4);

done:
    if (irq_hand_back(pc)) {
        continue_original(pc, 0x1906);
        return;
    }
    regs_store(pc, &r);
    pc->cpu.flags |= F_IF; /* the CLI/STI around the timer read */
    charge(pc);
    native_ret(pc);
}

/* ---- flight_params 0050:1AF7 --------------------------------------------------------- */

/* 0050:1BB0: re-arm after a stop in war mode: magnetos, fuel, panel, enemies. */
static void war_rearm(Pc *pc, Regs *r)
{
    run(0x1BB0, 0x1BE3);
    wr8(pc, GS_MAG_OK, 1);
    wr8(pc, (uint16_t)(GS_MAG_OK + 1), 1);
    wr8(pc, (uint16_t)(GS_FUEL_LEFT + 2), 0x23);
    wr8(pc, (uint16_t)(GS_FUEL_RIGHT + 2), 0x23);
    wr16(pc, 0x19A0, 0xFFFF);
    wr16(pc, 0x041B, 0xFFFF);
    wr8(pc, GS_HITS_TAKEN, 0);
    wr8(pc, GS_ENGINE_FAULTS, 0);
    wr16(pc, GS_OIL_TEMP_RATE, 3);
    uint16_t bx = 0x91;
    for (;;) {
        run(0x1BE3, 0x1BED);
        wr8(pc, (uint16_t)(GS_ENEMY_TABLE + 25 + bx), 1);
        bx = (uint16_t)(bx - 0x1D);
        if (bx & 0x8000)
            break;
        taken(0x1BEB);
    }
    run(0x1BED, 0x1BEE);
    r->bx = bx;
}

/* 0050:3595 war_ammo_dec with 0050:3555 (ammo -> three ASCII digits at 1ED3..1ED5). */
static void war_ammo_dec(Pc *pc, Regs *r, uint16_t sp)
{
    stack_word(pc, (uint16_t)(sp - 2), 0x1BA5);
    run(0x3595, 0x359B);
    r->ax = (uint16_t)(gs_ammo(pc) - 1);
    if (r->ax & 0x8000) {
        run(0x359B, 0x359D);
        r->ax = 0;
    } else {
        taken(0x3599);
    }
    run(0x359D, 0x35A3);
    wr16(pc, GS_AMMO, r->ax);
    stack_word(pc, (uint16_t)(sp - 4), 0x35A3);
    run(0x3555, 0x3575);
    uint32_t n = (uint32_t)sign_of(r->ax) << 16 | r->ax;
    uint16_t hundreds_rest = (uint16_t)(n % 1000);
    r->cx = with_lo(1000, (uint8_t)(n / 1000));
    r->bp = 100;
    r->cx = with_hi(r->cx, (uint8_t)(hundreds_rest / 100));
    uint8_t rest = (uint8_t)(hundreds_rest % 100);
    r->dx = rest;
    r->ax = (uint16_t)((rest / 10) << 8 | rest % 10); /* AAM */
    r->ax = (uint16_t)(r->ax + 0x3030);
    r->cx = (uint16_t)(r->cx + 0x3030);
    r->ax = (uint16_t)(r->ax << 8 | r->ax >> 8); /* XCHG AL,AH */
    run(0x35A3, 0x35AB);
    wr16(pc, 0x1ED4, r->ax);
    wr8(pc, 0x1ED3, hi8(r->cx));
}

static void n_flight_params(Pc *pc)
{
    Regs r = regs_load(pc);
    uint16_t sp = pc->cpu.regs[R_SP];
    begin();
    if (gs_slew_mode(pc)) {
        run(0x1AF7, 0x1AFF);
        goto done;
    }
    run(0x1AF7, 0x1AFE);
    taken(0x1AFC);

    /* air density = 7FFF - 5 * displayed altitude */
    run(0x1AFF, 0x1B29);
    r.cx = gs_alt_display(pc);
    r.ax = (uint16_t)(0x7FFF - (uint16_t)(r.cx * 5));
    wr16(pc, GS_AIR_DENSITY, r.ax);
    /* weight = empty + fuel; mass terms = constants / weight */
    r.ax = (uint16_t)(gs_empty_weight(pc) + gs_fuel_weight(pc));
    uint16_t weight = r.ax;
    wr16(pc, GS_WEIGHT, weight);
    static const uint16_t k[3] = { GS_MASS_CONSTANTS, MASS_K2, MASS_K3 }, out[3] = { GS_INV_MASS, GS_MASS_TERM2, GS_INV_ROLL_INERTIA };
    static const uint16_t ret[3] = { 0x1B29, 0x1B3A, 0x1B4B };
    for (int i = 0; i < 3; i++) {
        r.dx = rd16(pc, k[i]);
        r.ax = 0;
        r.cx = weight;
        stack_word(pc, (uint16_t)(sp - 2), ret[i]);
        call_div_q15(&r);
        wr16(pc, out[i], r.dx);
    }
    run(0x1B29, 0x1B67);
    /* vertical speed = airspeed * sin(pitch) */
    mul_q15(&r, gs_airspeed(pc), (uint16_t)gs_sin_pitch(pc));
    wr16(pc, GS_VERTICAL_SPEED, r.dx);

    /* with the rudder centred the bank returns to level by a fixed step */
    if (hi8((uint16_t)gs_rudder(pc))) {
        taken(0x1B65);
    } else {
        uint16_t bank = (uint16_t)gs_bank(pc);
        r.ax = bank;
        if (!bank) {
            run(0x1B67, 0x1B6E);
            taken(0x1B6C);
        } else if (bank & 0x8000) {
            run(0x1B67, 0x1B70);
            taken(0x1B6E);
            run(0x1B79, 0x1B7F);
            r.ax = (uint16_t)(bank + (uint16_t)gs_level_rate_pos(pc));
            if (r.ax & 0x8000) {
                taken(0x1B7D);
            } else {
                run(0x1B7F, 0x1B81);
                r.ax = 0;
            }
            run(0x1B81, 0x1B84);
            wr16(pc, GS_BANK, r.ax);
        } else {
            r.ax = (uint16_t)(bank + (uint16_t)gs_level_rate_neg(pc));
            if (r.ax & 0x8000) {
                run(0x1B67, 0x1B76);
                taken(0x1B74);
                run(0x1B7F, 0x1B81);
                r.ax = 0;
            } else {
                run(0x1B67, 0x1B78);
            }
            run(0x1B81, 0x1B84);
            wr16(pc, GS_BANK, r.ax);
        }
    }

    /* war mode: re-arm once stopped */
    run(0x1B84, 0x1B8B);
    if (!gs_ground_service(pc)) {
        taken(0x1B89);
        run(0x1BAF, 0x1BB0);
        goto done;
    }
    run(0x1B8B, 0x1B92);
    if (gs_airspeed(pc)) {
        taken(0x1B90);
    } else {
        run(0x1B92, 0x1B95);
        stack_word(pc, (uint16_t)(sp - 2), 0x1B95);
        war_rearm(pc, &r);
        run(0x1B95, 0x1B9C);
        if (gs_ground_service(pc) != 2) {
            taken(0x1B9A);
        } else {
            run(0x1B9C, 0x1BA5);
            wr16(pc, GS_AMMO, 0x65);
            war_ammo_dec(pc, &r, sp);
            run(0x1BA5, 0x1BAA);
            wr8(pc, GS_BOMBS_LEFT, '5');
        }
    }
    run(0x1BAA, 0x1BB0);
    wr8(pc, GS_GROUND_SERVICE, 0);

done:
    if (irq_hand_back(pc)) {
        continue_original(pc, 0x1AF7);
        return;
    }
    regs_store(pc, &r);
    charge(pc);
    native_ret(pc);
}

/* ---- slew_update 0050:1C35 ------------------------------------------------------------ */

/* 0050:1CFF: slew speed from a control word. n = |AX >> 3| >> 8 (0..16); the result is
 * 2^n - 1 for a positive control and -(2^(n-1)) ... (FFFF << (n-1)) for a negative one,
 * built bit by bit with RCL/CMC. Returns AX, DX = its sign (CWD). CL = 3. */
static void slew_speed(Regs *r)
{
    run(0x1CFF, 0x1D06);
    taken(0x1D01); /* SAR AX,CL with CL = 3 */
    r->cx = with_lo(r->cx, 3);
    uint16_t ax = (uint16_t)((int16_t)r->ax >> 3);
    uint16_t dx = sign_of(ax);
    uint8_t ah = hi8(ax);
    if (ax & 0x8000) {
        run(0x1D06, 0x1D08);
        ah = (uint8_t)-ah;
    } else {
        taken(0x1D04);
    }
    run(0x1D08, 0x1D09);
    bool cf = true;
    for (;;) {
        ah--;
        if (ah & 0x80)
            break;
        run(0x1D09, 0x1D0D);
        taken(0x1D0B);
        run(0x1D11, 0x1D16);
        bool out = dx & 0x8000;
        dx = (uint16_t)(dx << 1 | cf);
        cf = !out;
    }
    run(0x1D09, 0x1D11);
    r->ax = dx;
    r->dx = sign_of(dx);
}

static void n_slew_update(Pc *pc)
{
    Regs r = regs_load(pc);
    uint16_t sp = pc->cpu.regs[R_SP];
    begin();
    run(0x1C35, 0x1C3C);
    if (gs_not_slew(pc)) { /* first slew frame: centre the controls */
        run(0x1C3C, 0x1C52);
        r.ax = 0;
        wr8(pc, GS_NOT_SLEW, 0);
        wr16(pc, GS_ELEVATOR, 0);
        wr16(pc, GS_RUDDER, 0);
        wr16(pc, GS_AILERON, 0);
        r.ax = 0x4000;
        wr16(pc, GS_FLAPS, 0x4000);
        wr16(pc, GS_THROTTLE_TARGET, 0x4000);
    } else {
        taken(0x1C3A);
    }

    /* ailerons move east, elevator moves north (24-bit, integer part and above) */
    run(0x1C52, 0x1C58);
    stack_word(pc, (uint16_t)(sp - 2), 0x1C58);
    r.ax = (uint16_t)gs_aileron(pc);
    slew_speed(&r);
    uint32_t e = (rd16(pc, (uint16_t)(GS_POS_EAST + 1)) | (uint32_t)rd8(pc, (uint16_t)(GS_POS_EAST + 3)) << 16) + r.ax + ((uint32_t)lo8(r.dx) << 16);
    wr16(pc, (uint16_t)(GS_POS_EAST + 1), (uint16_t)e);
    wr8(pc, (uint16_t)(GS_POS_EAST + 3), (uint8_t)(e >> 16));
    run(0x1C58, 0x1C66);
    stack_word(pc, (uint16_t)(sp - 2), 0x1C66);
    r.ax = (uint16_t)gs_elevator(pc);
    slew_speed(&r);
    uint32_t n = (rd16(pc, (uint16_t)(GS_POS_NORTH + 1)) | (uint32_t)rd8(pc, (uint16_t)(GS_POS_NORTH + 3)) << 16) - r.ax - ((uint32_t)lo8(r.dx) << 16);
    wr16(pc, (uint16_t)(GS_POS_NORTH + 1), (uint16_t)n);
    wr8(pc, (uint16_t)(GS_POS_NORTH + 3), (uint8_t)(n >> 16));

    /* throttle moves up and down: altitude -= (throttle - 4000h) >> 2, clamped to 0..002EFFFF */
    run(0x1C66, 0x1C83);
    r.ax = with_hi(gs_throttle_target(pc), (uint8_t)(hi8(gs_throttle_target(pc)) - 0x40));
    r.ax = (uint16_t)((int16_t)r.ax >> 2);
    r.dx = sign_of(r.ax);
    uint32_t alt = (uint32_t)gs_altitude(pc) - (uint32_t)(int32_t)(int16_t)r.ax;
    if (alt & 0x80000000u) {
        run(0x1C83, 0x1C8F);
        alt = 0;
    } else {
        taken(0x1C81);
    }
    run(0x1C8F, 0x1C96);
    if ((int16_t)(alt >> 16) >= 0x2F) {
        run(0x1C96, 0x1CA2);
        alt = 0x002EFFFF;
    } else {
        taken(0x1C94);
    }
    wr32(pc, GS_ALTITUDE, alt);

    /* flaps tilt the view pitch (sign by whether the view is upright); past +-90 degrees the
     * view flips over: pitch = 180 - pitch, bank and heading + 180 */
    run(0x1CA2, 0x1CB6);
    r.ax = (uint16_t)(sext8((uint8_t)(hi8(gs_flaps(pc)) - 0x40)) << 2);
    r.bx = (uint16_t)((uint16_t)gs_view_bank(pc) + 0x4000);
    if (r.bx & 0x8000) {
        run(0x1CB6, 0x1CB8);
        r.ax = (uint16_t)-r.ax;
    } else {
        taken(0x1CB4);
    }
    run(0x1CB8, 0x1CBE);
    r.ax = (uint16_t)(r.ax + (uint16_t)gs_view_pitch(pc));
    bool flip;
    if (r.ax & 0x8000) {
        taken(0x1CBC);
        run(0x1CC5, 0x1CCA);
        flip = (int16_t)r.ax < (int16_t)0xC000;
        if (!flip)
            taken(0x1CC8);
    } else {
        run(0x1CBE, 0x1CC3);
        flip = (int16_t)r.ax > 0x4000;
        if (flip)
            run(0x1CC3, 0x1CC5);
        else
            taken(0x1CC1);
    }
    if (flip) {
        run(0x1CCA, 0x1CDB);
        r.ax = (uint16_t)(0x8000 - r.ax);
        wr16(pc, GS_VIEW_BANK, (uint16_t)(rd16(pc, GS_VIEW_BANK) + 0x8000));
        wr16(pc, GS_VIEW_HEADING, (uint16_t)(rd16(pc, GS_VIEW_HEADING) + 0x8000));
    }
    run(0x1CDB, 0x1CFF);
    wr16(pc, GS_VIEW_PITCH, r.ax);
    wr16(pc, GS_VIEW_BANK, (uint16_t)(rd16(pc, GS_VIEW_BANK) + rd16(pc, GS_SLEW_RATE_A)));
    wr16(pc, GS_VIEW_HEADING, (uint16_t)(rd16(pc, GS_VIEW_HEADING) - rd16(pc, GS_SLEW_RATE_B)));

    /* 0050:1CEC: position back into the editor values */
    r.ax = (uint16_t)(rd16(pc, (uint16_t)(GS_POS_NORTH + 2)) + 0x4000);
    wr16(pc, GS_EDITOR_NORTH, r.ax);
    r.ax = (uint16_t)(rd16(pc, (uint16_t)(GS_POS_EAST + 2)) + 0x4000);
    wr16(pc, GS_EDITOR_EAST, r.ax);

    if (irq_hand_back(pc)) {
        continue_original(pc, 0x1C35);
        return;
    }
    regs_store(pc, &r);
    charge(pc);
    native_ret(pc);
}

/* ---- wind: compute_wind 0050:2F56, apply_wind 0050:2F10 ------------------------------ */

static void n_compute_wind(Pc *pc)
{
    Regs r = regs_load(pc);
    uint16_t sp = pc->cpu.regs[R_SP];
    begin();
    /* layer by altitude: surface, 1, 2, 3 */
    r.ax = rd16(pc, (uint16_t)(GS_ALTITUDE + 1));
    uint16_t layer;
    run(0x2F56, 0x2F5F);
    if ((int16_t)r.ax <= (int16_t)gs_shear_tops_w(pc, 0)) {
        taken(0x2F5D);
        run(0x2F8C, 0x2F98);
        layer = WIND_SURFACE;
        wr8(pc, GS_GUST_PHASE, (uint8_t)(rd8(pc, GS_GUST_PHASE) + 1));
    } else {
        run(0x2F5F, 0x2F65);
        if ((int16_t)r.ax <= (int16_t)gs_shear_tops_w(pc, 1)) {
            taken(0x2F63);
            run(0x2F81, 0x2F8B);
            layer = WIND_1;
        } else {
            run(0x2F65, 0x2F6B);
            if ((int16_t)r.ax <= (int16_t)gs_shear_tops_w(pc, 2)) {
                taken(0x2F69);
                run(0x2F76, 0x2F80);
                layer = WIND_2;
            } else {
                run(0x2F6B, 0x2F75);
                layer = GS_WIND_LAYERS;
            }
        }
    }
    uint16_t speed = rd16(pc, layer); /* low byte speed, high byte turbulence */
    uint16_t dir = (uint16_t)(rd16(pc, (uint16_t)(layer + 2)) + gs_wind_dir_offset(pc));
    stack_word(pc, (uint16_t)(sp - 2), speed); /* PUSH CX */
    uint8_t phase = gs_gust_phase(pc);
    wr8(pc, GS_GUST_PHASE, phase >> 1);
    if (phase & 1) { /* every other surface-layer call: gust direction */
        run(0x2F98, 0x2FAA);
        dir = (uint16_t)(dir + gs_wind_gust_offset(pc));
    } else {
        run(0x2F98, 0x2FA3);
        taken(0x2FA1);
        run(0x2FA7, 0x2FAA);
    }
    call_sincos(pc, &r, (uint16_t)(sp - 2), 0x2FAA, dir);
    run(0x2FAA, 0x2FD0);
    r.bx = speed;
    wr8(pc, GS_TURBULENCE, hi8(speed));
    /* drift per frame = speed * sin|cos(direction) (high bytes) * dt */
    uint16_t dt = gs_frame_dt(pc);
    mul_q15(&r, imul8(hi8(r.ax), lo8(speed)), dt);
    wr16(pc, GS_WIND_DX, r.dx);
    mul_q15(&r, imul8(hi8(r.cx), lo8(speed)), dt);
    wr16(pc, GS_WIND_DY, r.dx);
    if (irq_hand_back(pc)) {
        continue_original(pc, 0x2F56);
        return;
    }
    regs_store(pc, &r);
    charge(pc);
    native_ret(pc);
}

static void n_apply_wind(Pc *pc)
{
    Regs r = regs_load(pc);
    begin();
    run(0x2F10, 0x2F17);
    if (gs_on_ground(pc)) {
        taken(0x2F15);
        run(0x2F55, 0x2F56);
        goto done;
    }
    run(0x2F17, 0x2F45);
    r.ax = (uint16_t)gs_wind_dx(pc);
    wr32(pc, GS_POS_EAST, rd32(pc, GS_POS_EAST) - (uint32_t)(int32_t)(int16_t)r.ax);
    r.ax = (uint16_t)gs_wind_dy(pc);
    r.dx = sign_of(r.ax);
    wr32(pc, GS_POS_NORTH, rd32(pc, GS_POS_NORTH) - (uint32_t)(int32_t)(int16_t)r.ax);
    /* turbulence: a pseudo-random bank jolt from position and frame count */
    uint8_t turb = gs_turbulence(pc);
    r.ax = imul8((uint8_t)(rd8(pc, GS_POS_NORTH) + rd8(pc, GS_FRAME_COUNTER)), turb);
    wr16(pc, GS_BANK, (uint16_t)(rd16(pc, GS_BANK) + r.ax));
    if ((int8_t)turb > 5) { /* strong: also a pitch jolt every 64 frames */
        run(0x2F45, 0x2F4D);
        if (gs_frame_counter(pc) & 0x3F) {
            taken(0x2F4B);
        } else {
            run(0x2F4D, 0x2F55);
            r.ax = (uint16_t)((int16_t)r.ax >> 2);
            wr16(pc, GS_PITCH, (uint16_t)(rd16(pc, GS_PITCH) + r.ax));
        }
    } else {
        taken(0x2F43);
    }
    run(0x2F55, 0x2F56);
done:
    if (irq_hand_back(pc)) {
        continue_original(pc, 0x2F10);
        return;
    }
    regs_store(pc, &r);
    charge(pc);
    native_ret(pc);
}

/* ---- engine_update 0050:2D00 --------------------------------------------------------- */

/* 0050:2EDB: engine stopped. */
static void engine_stop(Pc *pc, uint16_t sp)
{
    run(0x2D29, 0x2D2C);
    stack_word(pc, (uint16_t)(sp - 2), 0x2D2C);
    run(0x2EDB, 0x2EEC);
    wr16(pc, GS_ENGINE_SOUND, 0);
    wr8(pc, GS_RPM_DEFICIT, 0);
    wr8(pc, GS_ENGINE_RUNNING, 0);
    run(0x2D2C, 0x2D2E);
}

static void engine_start(Pc *pc)
{
    run(0x2D70, 0x2D7B);
    wr8(pc, GS_ENGINE_RUNNING, 1);
    wr16(pc, GS_ENGINE_SOUND, 0xA140);
}

/* CMP r/m8,imm flags (for handing over after the first instruction). */
static void flags_cmp8(Pc *pc, uint8_t a, uint8_t b)
{
    uint8_t res = (uint8_t)(a - b);
    uint16_t f = 0;
    if (a < b)
        f |= F_CF;
    if (!res)
        f |= F_ZF;
    if (res & 0x80)
        f |= F_SF;
    if ((a ^ b) & (a ^ res) & 0x80)
        f |= F_OF;
    if ((a ^ b ^ res) & 0x10)
        f |= F_AF;
    uint8_t p = res;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    if (!(p & 1))
        f |= F_PF;
    uint16_t m = F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF;
    pc->cpu.flags = (uint16_t)((pc->cpu.flags & ~m) | f);
}

static void n_engine_update(Pc *pc)
{
    Regs r = regs_load(pc);
    uint16_t sp = pc->cpu.regs[R_SP];
    begin();
    bool war = gs_war_mode(pc) == 1, reality = gs_reality_mode(pc) != 0;
    uint8_t mag_l = rd8(pc, GS_MAG_SWITCHES) & rd8(pc, GS_MAG_OK), mag_r = rd8(pc, (uint16_t)(GS_MAG_SWITCHES + 1)) & rd8(pc, (uint16_t)(GS_MAG_OK + 1));

    /* ignition and fuel (reality mode or war): stop the engine without spark or fuel */
    enum { STOP, CHECK_FUEL, CHECK_START } next;
    run(0x2D00, 0x2D07);
    if (war) {
        taken(0x2D05);
        next = CHECK_FUEL; /* fuel check below */
    } else {
        run(0x2D07, 0x2D0E);
        if (!reality) {
            taken(0x2D0C);
            next = CHECK_START;
        } else {
            run(0x2D0E, 0x2D15);
            if (gs_magnetos(pc) == 5) {
                taken(0x2D13);
                next = STOP;
            } else {
                run(0x2D15, 0x2D29);
                r.ax = with_hi(r.ax, mag_l);
                r.ax = with_lo(r.ax, (uint8_t)(mag_r | mag_l));
                if (mag_l | mag_r) {
                    taken(0x2D27);
                    next = CHECK_FUEL;
                } else {
                    next = STOP;
                }
            }
        }
    }
    if (next == CHECK_FUEL) {
        run(0x2D2E, 0x2D37);
        r.ax = with_lo(r.ax, rd8(pc, (uint16_t)(GS_FUEL_LEFT + 2)) | rd8(pc, (uint16_t)(GS_FUEL_RIGHT + 2)));
        if (!lo8(r.ax)) {
            taken(0x2D35);
            next = STOP;
        } else {
            next = CHECK_START;
        }
    }
    if (next == STOP) {
        engine_stop(pc, sp);
    } else {
        /* starting: automatic without reality mode, by windmilling above 75 knots, or with
         * the starter (magnetos 4) */
        run(0x2D37, 0x2D40);
        bool running = gs_engine_running(pc) != 0;
        r.ax = with_lo(r.ax, (uint8_t)(gs_reality_mode(pc) | gs_engine_running(pc)));
        if (!lo8(r.ax)) {
            taken(0x2D3E);
            engine_start(pc);
        } else if (run(0x2D40, 0x2D47), running) {
            taken(0x2D45);
        } else if (run(0x2D47, 0x2D4E), (int16_t)gs_ias_knots(pc) > 0x4B) {
            taken(0x2D4C);
            engine_start(pc);
        } else if (run(0x2D4E, 0x2D55), gs_magnetos(pc) != 4) {
            taken(0x2D53);
        } else {
            /* starter: ~1 s busy wait with interrupts live, then magnetos to both (panel
             * text), and in season 1 a start fails on half of the frames. Run as original. */
            cyc = 0;
            if (under_verify(pc)) {
                run_original(pc);
                charge(pc);
                return;
            }
            continue_original(pc, 0x2D00);
            return;
        }
    }

    /* power = throttle, less carb heat (0800, or 2000 while melting ice), clipped by damage
     * and by a single magneto (-0400); thrust = power * [08E0] */
    run(0x2D7B, 0x2D8C);
    r.bx = (uint16_t)((int16_t)gs_ias_knots(pc) >> 4);
    if ((int16_t)r.bx > 0x1F) {
        run(0x2D8C, 0x2D8F);
        r.bx = 0x1F;
    } else {
        taken(0x2D8A);
    }
    run(0x2D8F, 0x2D9E);
    r.cx = with_lo(r.cx, rd8(pc, (uint16_t)(GS_RPM_WINDMILL + r.bx)));
    r.bx = gs_throttle(pc);
    bool running = gs_engine_running(pc) != 0;
    if (!running) {
        taken(0x2D9C);
        run(0x2DBA, 0x2DBC);
        r.bx = 0;
    } else {
        run(0x2D9E, 0x2DA5);
        if (!gs_carb_heat(pc)) {
            taken(0x2DA3);
        } else {
            run(0x2DA5, 0x2DAF);
            r.cx = 0x800; /* overwrites CL (the windmill RPM): kept as is */
            if (gs_carb_ice(pc)) {
                run(0x2DAF, 0x2DB6);
                r.cx = 0x2000;
                wr8(pc, GS_CARB_ICE, (uint8_t)(rd8(pc, GS_CARB_ICE) - 1));
            } else {
                taken(0x2DAD);
            }
            run(0x2DB6, 0x2DBA);
            r.bx = (uint16_t)(r.bx - r.cx);
            if (r.bx & 0x8000) {
                run(0x2DBA, 0x2DBC);
                r.bx = 0;
            } else {
                taken(0x2DB8);
            }
        }
    }
    run(0x2DBC, 0x2DC3);
    if (gs_engine_faults(pc) & 3) {
        run(0x2DC3, 0x2DC7);
        r.bx &= 0x0FFF;
    } else {
        taken(0x2DC1);
    }
    run(0x2DC7, 0x2DD0);
    r.ax = with_lo(r.ax, mag_l);
    bool both = false;
    if (!mag_l) {
        taken(0x2DCE);
    } else {
        run(0x2DD0, 0x2DD9);
        r.ax = with_lo(r.ax, mag_r);
        if (mag_r) {
            taken(0x2DD7);
            both = true;
        }
    }
    if (!both) {
        run(0x2DD9, 0x2DDF);
        r.bx = (uint16_t)(r.bx - 0x400);
        if (r.bx & 0x8000) {
            run(0x2DDF, 0x2DE1);
            r.bx = 0;
        } else {
            taken(0x2DDD);
        }
    }
    run(0x2DE1, 0x2E01);
    wr16(pc, GS_ENGINE_POWER, r.bx);
    imul16(&r, gs_thrust_k(pc), r.bx);
    wr16(pc, GS_THRUST, r.dx);

    /* RPM target from power, at least the windmill RPM for the airspeed */
    r.bx = (uint8_t)((int8_t)hi8(r.bx) >> 2);
    uint8_t ch = rd8(pc, (uint16_t)(GS_RPM_BY_POWER + r.bx));
    if (running) {
        taken(0x2DFF);
    } else {
        run(0x2E01, 0x2E03);
        ch = 5;
    }
    r.cx = with_hi(r.cx, ch);
    uint8_t cl = lo8(r.cx);
    run(0x2E03, 0x2E07);
    if ((int8_t)ch >= (int8_t)cl) {
        taken(0x2E05);
        run(0x2E14, 0x2E1D);
        wr8(pc, GS_RPM_TARGET, ch);
        wr8(pc, GS_RPM_DEFICIT, 0);
    } else {
        run(0x2E07, 0x2E14);
        wr8(pc, GS_RPM_TARGET, cl);
        cl = (uint8_t)(cl - ch);
        r.cx = with_lo(r.cx, cl);
        wr8(pc, GS_RPM_DEFICIT, cl);
    }

    /* oil temperature heads for 64 (cold), AF (running) or EF (oil leak), +-[1DC4] per call */
    run(0x2E1D, 0x2E26);
    uint8_t al = 0x64;
    if (!running) {
        taken(0x2E24);
    } else {
        run(0x2E26, 0x2E35);
        al = 0xAF;
        wr16(pc, GS_OIL_TEMP_RATE, 7);
        if (!(gs_engine_faults(pc) & 1)) {
            taken(0x2E33);
        } else {
            run(0x2E35, 0x2E3D);
            al = 0xEF;
            wr16(pc, GS_OIL_TEMP_RATE, 0x19);
        }
    }
    run(0x2E3D, 0x2E57);
    uint16_t target = (uint16_t)(((uint16_t)al << 6) - 0x1900);
    bool up = (int16_t)target > (int16_t)gs_oil_temp(pc);
    r.ax = gs_oil_temp_rate(pc);
    if (up) {
        taken(0x2E55);
        run(0x2E5E, 0x2E62);
        wr16(pc, GS_OIL_TEMP, (uint16_t)(rd16(pc, GS_OIL_TEMP) + r.ax));
    } else {
        run(0x2E57, 0x2E5D);
        wr16(pc, GS_OIL_TEMP, (uint16_t)(rd16(pc, GS_OIL_TEMP) - r.ax));
    }

    /* oil pressure heads for 0 or 32 (x 59h), +-[1DC6] per call */
    run(0x2E62, 0x2E6B);
    al = 0;
    if (!running) {
        taken(0x2E69);
    } else {
        run(0x2E6B, 0x2E72);
        if (gs_engine_faults(pc) & 2) {
            taken(0x2E70);
        } else {
            run(0x2E72, 0x2E74);
            al = 0x32;
        }
    }
    run(0x2E74, 0x2E81);
    r.dx = with_lo(r.dx, 0x59);
    target = (uint16_t)(al * 0x59);
    up = (int16_t)target > (int16_t)gs_oil_press(pc);
    r.ax = gs_oil_press_rate(pc);
    if (up) {
        taken(0x2E7F);
        run(0x2E88, 0x2E8C);
        wr16(pc, GS_OIL_PRESS, (uint16_t)(rd16(pc, GS_OIL_PRESS) + r.ax));
    } else {
        run(0x2E81, 0x2E87);
        wr16(pc, GS_OIL_PRESS, (uint16_t)(rd16(pc, GS_OIL_PRESS) - r.ax));
    }

    /* fuel: each tank += flow * throttle (flow is negative; x4 in war), leaks -1 per call */
    run(0x2E8C, 0x2E93);
    if (!running) {
        taken(0x2E91);
        run(0x2EDA, 0x2EDB);
        goto done;
    }
    run(0x2E93, 0x2E9D);
    r.ax = with_lo(r.ax, gs_fuel_flow(pc));
    if (gs_war_mode(pc)) {
        run(0x2E9D, 0x2EA1);
        r.ax = (uint16_t)(r.ax << 2);
    } else {
        taken(0x2E9B);
    }
    run(0x2EA1, 0x2EAD);
    r.ax = imul8(lo8(r.ax), rd8(pc, (uint16_t)(GS_THROTTLE + 1)));
    r.dx = sign_of(r.ax);
    static const uint16_t tank[2] = { GS_FUEL_LEFT, GS_FUEL_RIGHT };
    static const uint16_t leak_test[2] = { 0x2EAB, 0x2EC5 }, leak_dec[2][2] = { { 0x2EAD, 0x2EB1 }, { 0x2EC7, 0x2ECB } };
    static const uint16_t add_run[2][2] = { { 0x2EB1, 0x2EBB }, { 0x2ECB, 0x2ED5 } };
    static const uint16_t neg_run[2][2] = { { 0x2EBB, 0x2EC0 }, { 0x2ED5, 0x2EDA } };
    static const uint16_t pos_jump[2] = { 0x2EB9, 0x2ED3 };
    for (int t = 0; t < 2; t++) {
        if (t)
            run(0x2EC0, 0x2EC7);
        uint16_t gauge = (uint16_t)(tank[t] + 2);
        if (gs_engine_faults(pc) & (4 << t)) {
            run(leak_dec[t][0], leak_dec[t][1]);
            wr8(pc, gauge, (uint8_t)(rd8(pc, gauge) - 1));
        } else {
            taken(leak_test[t]);
        }
        run(add_run[t][0], add_run[t][1]);
        uint32_t sum = (uint32_t)rd16(pc, tank[t]) + r.ax;
        wr16(pc, tank[t], (uint16_t)sum);
        uint8_t hi = (uint8_t)(rd8(pc, gauge) + lo8(r.dx) + (sum >> 16));
        wr8(pc, gauge, hi);
        if (hi & 0x80) {
            run(neg_run[t][0], neg_run[t][1]);
            wr8(pc, gauge, 0);
        } else {
            taken(pos_jump[t]);
        }
    }
    run(0x2EDA, 0x2EDB);
done:
    if (irq_hand_back(pc)) {
        continue_original(pc, 0x2D00);
        return;
    }
    regs_store(pc, &r);
    charge(pc);
    native_ret(pc);
}

/* ---- crash_handler 0050:0625 ---------------------------------------------------------- */

/* With no crash it returns. A crash shows the message (clear_screen, print_str), waits about
 * 3 s in a MUL loop with interrupts live, then recalls the user mode and lowers the gear; that
 * path runs as original code (see the top of the file). */
static void n_crash_handler(Pc *pc)
{
    begin();
    uint8_t code = gs_crash_code(pc);
    if (!code) {
        run(0x0625, 0x062D);
        taken(0x062B);
        run(0x0657, 0x0658);
        if (irq_hand_back(pc)) {
            continue_original(pc, 0x0625);
            return;
        }
        pc->cpu.regs[R_BX] &= 0xFF00;
        charge(pc);
        native_ret(pc);
        return;
    }
    if (under_verify(pc)) {
        run_original(pc);
        charge(pc);
        return;
    }
    continue_original(pc, 0x0625);
}

NativeEntry native_flight_motion[] = {
    { .name = "flight_integrate", .seg = GAME_CS, .off = 0x1906, .fn = n_flight_integrate, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = "flight_params", .seg = GAME_CS, .off = 0x1AF7, .fn = n_flight_params, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = "slew_update", .seg = GAME_CS, .off = 0x1C35, .fn = n_slew_update, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = "compute_wind", .seg = GAME_CS, .off = 0x2F56, .fn = n_compute_wind, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = "apply_wind", .seg = GAME_CS, .off = 0x2F10, .fn = n_apply_wind, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = "engine_update", .seg = GAME_CS, .off = 0x2D00, .fn = n_engine_update, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = "crash_handler", .seg = GAME_CS, .off = 0x0625, .fn = n_crash_handler, .enabled = true,
      .cycles = ENTRY_CYCLES },
    { .name = NULL },
};
