/* Natives for subphase 3.17 war mode (see docs/PHASE3_PLAN.md and docs/subphases/3.17.md).
 *
 * The routines are transliterated instruction by instruction on the emulated registers, so
 * every register, segment register and memory byte (including the stack words written by
 * CALL and PUSH) ends as the original leaves it. Memory operands use the live DS/ES, as the
 * original does: after draw_line_list DS is the back buffer segment, and war_frame's last two
 * stores land there too.
 *
 * Cycles: every routine adds the emulated cycles the original takes on the path it ran (the
 * cpu8086.c model: 2 per instruction plus the opcode cost, plus 7 for a memory operand, plus
 * 4 per bit shifted) to pc->cpu.cycles; each entry's .cycles is the cost of the shortest
 * path and is subtracted again, so the total equals the original's.
 *
 * Calls into code that is not war mode (print_str, project_dot, rotate_point, plot_pixel,
 * draw_line_list, fmt_dec4) go through war_call(): it runs the target the way the CPU would
 * with the natives on. An enabled native at a reached address runs its C function (and is
 * charged its .cycles, as native.c's dispatch does); anything else is single-stepped as
 * original code. That way print_str (3.9, not merged yet) runs as original today and as C
 * once its native exists. */
#include "war.h"
#include "game/state.h"

#include <stddef.h>

/* ---- registers, memory --------------------------------------------------------------- */

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])
#define CYC(n) (pc->cpu.cycles += (n))
/* Conditional jump: taken 16, not taken 4. */
#define JCC(taken) CYC((taken) ? 16 : 4)

static uint8_t lo8(uint16_t v) { return (uint8_t)v; }
static uint8_t hi8(uint16_t v) { return (uint8_t)(v >> 8); }
static uint16_t with_lo(uint16_t v, uint8_t b) { return (uint16_t)((v & 0xFF00) | b); }
static uint16_t with_hi(uint16_t v, uint8_t b) { return (uint16_t)((v & 0x00FF) | b << 8); }
#define SET_LO(r, b) (REG(r) = with_lo(REG(r), (uint8_t)(b)))
#define SET_HI(r, b) (REG(r) = with_hi(REG(r), (uint8_t)(b)))

/* DS-relative access with the live DS. */
static uint8_t rd8(Pc *pc, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(SREG(DS), off)); }
static uint16_t rd16(Pc *pc, uint16_t off) { return mem_read16(pc, SREG(DS), off); }
static void wr8(Pc *pc, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(SREG(DS), off), v); }
static void wr16(Pc *pc, uint16_t off, uint16_t v) { mem_write16(pc, SREG(DS), off, v); }
/* ES-relative. */
static uint8_t erd8(Pc *pc, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(SREG(ES), off)); }
static uint16_t erd16(Pc *pc, uint16_t off) { return mem_read16(pc, SREG(ES), off); }
static void ewr8(Pc *pc, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(SREG(ES), off), v); }
static void ewr16(Pc *pc, uint16_t off, uint16_t v) { mem_write16(pc, SREG(ES), off, v); }

static int dstep(Pc *pc, int size) { return (pc->cpu.flags & F_DF) ? -size : size; }

/* LODSB / LODSW (15 cycles each). */
static void lodsb(Pc *pc)
{
    CYC(15);
    SET_LO(AX, rd8(pc, REG(SI)));
    REG(SI) = (uint16_t)(REG(SI) + dstep(pc, 1));
}

static void lodsw(Pc *pc)
{
    CYC(15);
    REG(AX) = rd16(pc, REG(SI));
    REG(SI) = (uint16_t)(REG(SI) + dstep(pc, 2));
}

/* REP STOSB with CX bytes. */
static void rep_stosb(Pc *pc)
{
    CYC(2 + 10u * REG(CX));
    for (; REG(CX); REG(CX)--) {
        ewr8(pc, REG(DI), lo8(REG(AX)));
        REG(DI) = (uint16_t)(REG(DI) + dstep(pc, 1));
    }
}

static void push(Pc *pc, uint16_t v) { cpu_push(&pc->cpu, v); }
static uint16_t pop(Pc *pc) { return cpu_pop(&pc->cpu); }

/* RET: 20 cycles. */
static void ret(Pc *pc)
{
    CYC(20);
    native_ret(pc);
}

/* "CALL body" from C: 21 cycles, push the return address, run the body (it ends with RET). */
static void call_body(Pc *pc, uint16_t ret_ip, void (*body)(Pc *))
{
    CYC(21);
    push(pc, ret_ip);
    body(pc);
}

/* Signed overflow of a 16-bit a - b. */
static bool sub_overflows(uint16_t a, uint16_t b)
{
    uint16_t r = (uint16_t)(a - b);
    return ((a ^ b) & (a ^ r) & 0x8000) != 0;
}

/* ---- calls into other code --------------------------------------------------------- */

/* Runs from 0050:target until the routine returns to the address on top of the stack, as the
 * CPU would with the natives on (see the top of the file): enabled natives reached on the way
 * run through native_call(), everything else is single-stepped. Interrupts are not delivered. */
static void run_from(Pc *pc, uint16_t target)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp0 = c->regs[R_SP];
    uint16_t ret_ip = mem_read16(pc, c->sregs[S_SS], sp0);
    const uint8_t *map = c->hook_map;
    c->sregs[S_CS] = GAME_CS;
    c->ip = target;
    for (long n = 0; n < 5000000L; n++) {
        bool done = false;
        if (map && c->sregs[S_CS] == GAME_CS && map[c->ip])
            done = native_call(pc, c->ip);
        if (!done) {
            c->hook_map = NULL; /* stepped here, so that no nested --verify starts */
            native_or_cpu_step(pc);
            c->hook_map = map;
        }
        uint16_t d = (uint16_t)(c->regs[R_SP] - sp0);
        if (c->ip == ret_ip && c->sregs[S_CS] == GAME_CS && d >= 2 && d < 0x8000)
            break;
    }
}

/* CALL target (21 cycles) with return address ret_ip, run through run_from. */
static void war_call(Pc *pc, uint16_t target, uint16_t ret_ip)
{
    CYC(21);
    push(pc, ret_ip);
    run_from(pc, target);
}

#define PRINT_STR 0x1225
#define PRINT_STR2 0x1282
#define FMT_DEC4 0x3555
#define DRAW_LINE_LIST 0x408F
#define PROJECT_DOT 0x3D1A
#define ROTATE_POINT 0x481F
#define PLOT_PIXEL_ES 0x5660
#define PLOT_PIXEL 0x5664

/* ---- game variables (DS = 0618) ----------------------------------------------------- */

#define V_ROW_OFFSETS 0x380C
#define V_SPRITES 0x3A10

#define ENEMY_FIRST 0x1F3B
#define ENEMY_SIZE 0x1D
#define BULLETS 0x1EE6
#define CAPTURE_LIST 0x041F

/* ---- 0050:3555 fmt_dec4, 3575 score, 3595 ammo ------------------------------------- */

/* 0050:3575: score += AX (minus 10000 above 10000), digits to 1EC5/1EC7. */
static void war_score_add(Pc *pc)
{
    CYC(15 * 3);
    push(pc, REG(CX));
    push(pc, REG(BP));
    push(pc, REG(DX));
    CYC(18);
    REG(AX) = (uint16_t)(REG(AX) + gs_score(pc));
    CYC(6);
    bool le = (int16_t)REG(AX) <= 0x2710;
    JCC(le);
    if (!le) {
        CYC(6);
        REG(AX) = (uint16_t)(REG(AX) - 0x2710);
    }
    CYC(12);
    gs_set_score(pc, REG(AX));
    war_call(pc, FMT_DEC4, 0x358A);
    CYC(18);
    wr16(pc, 0x1EC5, REG(CX));
    CYC(12);
    wr16(pc, 0x1EC7, REG(AX));
    CYC(12 * 3);
    REG(DX) = pop(pc);
    REG(BP) = pop(pc);
    REG(CX) = pop(pc);
    ret(pc);
}

/* 0050:3595: ammo - 1, clamped at 0; digits to 1ED3..1ED5 ("AMMO : nnn"). */
static void war_ammo_dec(Pc *pc)
{
    CYC(12 + 2);
    REG(AX) = (uint16_t)(gs_ammo(pc) - 1);
    bool ns = !(REG(AX) & 0x8000);
    JCC(ns);
    if (!ns) {
        CYC(5);
        REG(AX) = 0;
    }
    CYC(12);
    gs_set_ammo(pc, REG(AX));
    war_call(pc, FMT_DEC4, 0x35A3);
    CYC(12);
    wr16(pc, 0x1ED4, REG(AX));
    CYC(18);
    wr8(pc, 0x1ED3, hi8(REG(CX)));
    ret(pc);
}

/* ---- 0050:35AB explosion, 35DB damage -------------------------------------------------- */

/* 0050:35AB: CL dots in colour [1E55] around screen (2F+, 50+) of the view; the offsets are
 * products of bytes read from DS:0830 (live flight variables, so it looks random). */
static void war_explosion(Pc *pc)
{
    CYC(15);
    push(pc, REG(BX));
    CYC(12 + 12 + 4);
    REG(AX) = gs_explosion_colour(pc);
    gs_set_draw_colour(pc, REG(AX));
    REG(SI) = 0x0830;
    for (;;) {
        CYC(15);
        push(pc, REG(CX));
        lodsw(pc);
        CYC(6 + 6 + 4);
        REG(AX) = (uint16_t)((REG(AX) & 0x7F7F) + 0xC0C0);
        REG(CX) = REG(AX);
        lodsb(pc);
        CYC(82); /* imul cl */
        REG(AX) = (uint16_t)((int8_t)lo8(REG(AX)) * (int8_t)lo8(REG(CX)));
        CYC(4);
        SET_LO(CX, hi8(REG(AX)));
        lodsb(pc);
        CYC(82); /* imul ch */
        REG(AX) = (uint16_t)((int8_t)lo8(REG(AX)) * (int8_t)hi8(REG(CX)));
        CYC(8 + 6 + 6);
        SET_HI(AX, (uint8_t)((hi8(REG(AX)) << 1) + 0x50));
        SET_LO(CX, (uint8_t)(lo8(REG(CX)) + 0x2F));
        CYC(15);
        push(pc, REG(BX));
        war_call(pc, PLOT_PIXEL_ES, 0x35D3);
        CYC(12 + 12);
        REG(BX) = pop(pc);
        REG(CX) = pop(pc);
        CYC(5);
        SET_LO(CX, (uint8_t)(lo8(REG(CX)) - 1));
        bool more = lo8(REG(CX)) != 0;
        JCC(more);
        if (!more)
            break;
    }
    CYC(12);
    REG(BX) = pop(pc);
    ret(pc);
}

/* 0050:35DB: damage after an enemy hit. AL = hits taken [1E5C]; the thresholds drop once 4 or
 * more enemies are down. Bits of [1DC8]: 1, 2 (instruments), 4, 8 (only in the first 64 frames
 * of each 256); 1E83 = 1 stops the engine. */
static void war_damage(Pc *pc)
{
    CYC(12 + 19);
    SET_LO(AX, gs_hits_taken(pc));
    int8_t al = (int8_t)lo8(REG(AX));
    bool ge = (int8_t)gs_kills(pc) >= 4;
    JCC(ge);
    int level; /* 0 none, 1 from 3624, 2 from 3618, 3 from 3613, 4 from 360E, 5 from 3609 */
    if (!ge) {
        static const int8_t limits[5] = { 0x3C, 0x28, 0x23, 0x14, 0x07 };
        static const int levels[5] = { 5, 4, 3, 2, 1 };
        level = 0;
        for (int i = 0; i < 5; i++) {
            CYC(6);
            JCC(al > limits[i]);
            if (al > limits[i]) {
                level = levels[i];
                break;
            }
        }
        if (!level)
            CYC(17); /* jmp 3630 */
    } else {
        static const int8_t limits[3] = { 0x14, 0x07, 0x01 };
        static const int levels[3] = { 5, 4, 2 };
        level = 0;
        for (int i = 0; i < 3; i++) {
            CYC(6);
            JCC(al > limits[i]);
            if (al > limits[i]) {
                level = levels[i];
                break;
            }
        }
        if (!level)
            CYC(17);
    }
    if (level >= 5) {
        CYC(19);
        gs_set_wing_damage(pc, rd8(pc, GS_WING_DAMAGE) | 1);
    }
    if (level >= 4) {
        CYC(19);
        gs_set_engine_faults(pc, rd8(pc, GS_ENGINE_FAULTS) | 2);
    }
    if (level >= 3) {
        CYC(19);
        gs_set_engine_faults(pc, rd8(pc, GS_ENGINE_FAULTS) | 1);
    }
    if (level >= 2) {
        CYC(19);
        bool gt = (int8_t)rd8(pc, GS_FRAME_COUNTER) > 0x3F;
        JCC(gt);
        if (!gt) {
            CYC(19);
            gs_set_engine_faults(pc, rd8(pc, GS_ENGINE_FAULTS) | 8);
        }
    }
    if (level >= 1) {
        CYC(19);
        bool gt = (int8_t)rd8(pc, GS_FRAME_COUNTER) > 0x3F;
        JCC(gt);
        if (!gt) {
            CYC(19);
            gs_set_engine_faults(pc, rd8(pc, GS_ENGINE_FAULTS) | 4);
        }
    }
    ret(pc);
}

/* ---- 0050:326A..32CB enemy steering ------------------------------------------------ */

/* 0050:3296: target (CL:BP east, CH:DI altitude, AL:DX north, 24 bits each) minus the enemy at
 * BX, stored to 1E6F/1E71, 1E72/1E74, 1E75/1E77. Only the high bytes' signs steer. */
static void enemy_delta(Pc *pc)
{
    CYC(15 + 4);
    push(pc, REG(AX));
    REG(SI) = REG(BX);
    lodsw(pc);
    CYC(5 + 4 + 12);
    bool cf = REG(BP) < REG(AX);
    REG(BP) = (uint16_t)(REG(BP) - REG(AX));
    REG(AX) = REG(BP);
    wr16(pc, GS_DELTA_EAST, REG(AX));
    lodsb(pc);
    CYC(5 + 4 + 12);
    uint8_t cl = lo8(REG(CX)), al = lo8(REG(AX));
    uint8_t r = (uint8_t)(cl - al - cf);
    SET_LO(CX, r);
    SET_LO(AX, r);
    wr8(pc, (uint16_t)(GS_DELTA_EAST + 2), r);
    lodsw(pc);
    CYC(5 + 4 + 12);
    cf = REG(DI) < REG(AX);
    REG(DI) = (uint16_t)(REG(DI) - REG(AX));
    REG(AX) = REG(DI);
    wr16(pc, GS_DELTA_ALT, REG(AX));
    lodsb(pc);
    CYC(5 + 4 + 12);
    r = (uint8_t)(hi8(REG(CX)) - lo8(REG(AX)) - cf);
    SET_HI(CX, r);
    SET_LO(AX, r);
    wr8(pc, (uint16_t)(GS_DELTA_ALT + 2), r);
    lodsw(pc);
    CYC(5 + 4 + 12);
    cf = REG(DX) < REG(AX);
    REG(DX) = (uint16_t)(REG(DX) - REG(AX));
    REG(AX) = REG(DX);
    wr16(pc, GS_DELTA_NORTH, REG(AX));
    lodsb(pc);
    CYC(12 + 5 + 4 + 12);
    REG(CX) = pop(pc);
    r = (uint8_t)(lo8(REG(CX)) - lo8(REG(AX)) - cf);
    SET_LO(CX, r);
    SET_LO(AX, r);
    wr8(pc, (uint16_t)(GS_DELTA_NORTH + 2), r);
    ret(pc);
}

/* 0050:326A: delta towards the home position at SI (9 bytes: east, altitude, north). */
static void enemy_delta_home(Pc *pc)
{
    lodsw(pc);
    CYC(4);
    REG(BP) = REG(AX);
    lodsw(pc);
    CYC(4 + 4);
    SET_LO(CX, lo8(REG(AX)));
    SET_LO(DX, hi8(REG(AX)));
    lodsw(pc);
    CYC(4 + 4 + 4);
    SET_HI(DX, lo8(REG(AX)));
    REG(DI) = REG(AX);
    SET_HI(CX, hi8(REG(AX)));
    lodsw(pc);
    CYC(4);
    REG(DX) = REG(AX);
    lodsb(pc);
    CYC(17);
    enemy_delta(pc);
}

/* 0050:327F: delta towards the player (the middle 24 bits of pos_east, altitude, pos_north). */
static void enemy_delta_player(Pc *pc)
{
    CYC(17 * 5 + 12);
    REG(BP) = rd16(pc, (uint16_t)(GS_POS_EAST + 1));
    SET_LO(CX, rd8(pc, (uint16_t)(GS_POS_EAST + 3)));
    REG(DI) = rd16(pc, (uint16_t)(GS_ALTITUDE + 1));
    SET_HI(CX, rd8(pc, (uint16_t)(GS_ALTITUDE + 3)));
    REG(DX) = rd16(pc, (uint16_t)(GS_POS_NORTH + 1));
    SET_LO(AX, rd8(pc, (uint16_t)(GS_POS_NORTH + 3)));
    enemy_delta(pc);
}

/* One axis of 0050:32CB: DX:AX = the 24-bit delta, one's complement if negative. */
static void abs_axis(Pc *pc, uint16_t var)
{
    CYC(17 + 12 + 2 + 5);
    REG(DX) = rd16(pc, var);
    SET_LO(AX, rd8(pc, (uint16_t)(var + 2)));
    REG(AX) = (uint16_t)(int8_t)lo8(REG(AX));
    bool ns = !(REG(AX) & 0x80);
    JCC(ns);
    if (!ns) {
        CYC(5 + 5);
        REG(AX) = (uint16_t)~REG(AX);
        REG(DX) = (uint16_t)~REG(DX);
    }
}

/* 0050:32CB: CX:BP = |east| + |alt| + |north| of the last delta (one's complement). */
static void enemy_distance(Pc *pc)
{
    abs_axis(pc, GS_DELTA_EAST);
    CYC(4 + 4);
    REG(BP) = REG(DX);
    REG(CX) = REG(AX);
    for (int i = 0; i < 2; i++) {
        abs_axis(pc, i ? GS_DELTA_NORTH : GS_DELTA_ALT);
        CYC(5 + 5);
        uint32_t lo = (uint32_t)REG(BP) + REG(DX);
        REG(BP) = (uint16_t)lo;
        REG(CX) = (uint16_t)(REG(CX) + REG(AX) + (lo >> 16));
    }
    ret(pc);
}

/* ---- 0050:334A scope marker ------------------------------------------------------- */

/* 0050:33A4 (entry = 0x33A4, three doublings) or 33A8 (two): AX doubled; on overflow or with
 * AH outside -DL..DL the caller is abandoned (POP AX pops this call's return address) and
 * [BX+9] = 0. Returns false in that case. */
static bool scope_scale(Pc *pc, uint16_t entry, uint16_t ret_ip)
{
    CYC(21);
    push(pc, ret_ip);
    int n = entry == 0x33A4 ? 3 : 2;
    bool out = false;
    for (int i = 0; i < n && !out; i++) {
        CYC(5);
        out = ((REG(AX) ^ (REG(AX) << 1)) & 0x8000) != 0; /* OF of add ax,ax */
        REG(AX) = (uint16_t)(REG(AX) << 1);
        JCC(out);
    }
    if (!out) {
        CYC(5);
        int8_t ah = (int8_t)hi8(REG(AX));
        bool gt = ah > (int8_t)lo8(REG(DX));
        JCC(gt);
        if (gt) {
            out = true;
        } else {
            CYC(5 + 5);
            SET_LO(DX, (uint8_t)-lo8(REG(DX)));
            bool lt = ah < (int8_t)lo8(REG(DX));
            JCC(lt);
            out = lt;
        }
    }
    if (!out) {
        ret(pc);
        return true;
    }
    CYC(12 + 5 + 18);
    REG(AX) = pop(pc);
    SET_LO(AX, 0);
    wr8(pc, (uint16_t)(REG(BX) + 9), 0);
    ret(pc);
    return false;
}

/* 0050:334A: the enemy's dot on the panel scope (B800:17A8.., cleared by war_scope_clear):
 * column 87h + (4 * eye x 310F) / 256, line ABh - (8 * depth 3113) / 256; the colour shows
 * whether it is below, level with or above the player (eye y 3111 against -14h/14h). Sets
 * [BX+9] = FF when on the scope, 0 when outside. */
static void scope_marker(Pc *pc)
{
    CYC(4 + 12);
    SET_LO(DX, 0x15);
    REG(AX) = gs_eye_p1_w(pc, 2);
    if (!scope_scale(pc, 0x33A4, 0x3352))
        return;
    CYC(4 + 5 + 6 + 4 + 12);
    SET_LO(CX, hi8(REG(AX)));
    SET_LO(CX, (uint8_t)(0xAB - lo8(REG(CX))));
    SET_LO(DX, 0x16);
    REG(AX) = gs_eye_p1_w(pc, 0);
    if (!scope_scale(pc, 0x33A8, 0x3361))
        return;
    CYC(6 + 11 + 25 + 4 + 17 + 6);
    SET_HI(AX, (uint8_t)(hi8(REG(AX)) + 0x87));
    SREG(ES) = gs_screen_seg(pc);
    push(pc, gs_draw_colour(pc));
    REG(BP) = 0x8008;
    REG(DX) = gs_eye_p1_w(pc, 1);
    bool le = (int16_t)REG(DX) <= -0x14;
    JCC(le);
    if (!le) {
        CYC(6);
        bool ge = (int16_t)REG(DX) >= 0x14;
        JCC(ge);
        if (ge) {
            CYC(4);
            REG(BP) = 0x1001;
        } else {
            CYC(4 + 17);
            REG(BP) = 0xF00F;
        }
    }
    CYC(18 + 15 + 19);
    gs_set_draw_colour(pc, REG(BP));
    push(pc, REG(BX));
    bool skip = gs_view_not_forward(pc) != 0;
    JCC(skip);
    if (!skip) {
        CYC(19);
        skip = gs_radar_view(pc) == 1;
        JCC(skip);
        if (!skip)
            war_call(pc, PLOT_PIXEL, 0x339B);
    }
    CYC(12 + 26 + 4 + 17 + 18);
    REG(BX) = pop(pc);
    gs_set_draw_colour(pc, pop(pc));
    SET_LO(AX, 0xFF);
    wr8(pc, (uint16_t)(REG(BX) + 9), 0xFF);
    ret(pc);
}

/* ---- 0050:5BAA enemy sprite --------------------------------------------------------- */

/* 0050:5C63: BX one line up in the CGA interleave (+E000, and +3FB0 more from an even line). */
static void line_up(Pc *pc, uint16_t ret_ip)
{
    CYC(21 + 6);
    push(pc, ret_ip);
    REG(BX) = (uint16_t)(REG(BX) + 0xE000);
    bool ns = !(REG(BX) & 0x8000);
    JCC(ns);
    if (!ns) {
        CYC(6);
        REG(BX) = (uint16_t)(REG(BX) + 0x3FB0);
    }
    ret(pc);
}

static void sprite_and16(Pc *pc, uint16_t mask)
{
    CYC(19);
    ewr16(pc, REG(BX), (uint16_t)(erd16(pc, REG(BX)) & mask));
}

static void sprite_mov16(Pc *pc, uint16_t off, uint16_t v)
{
    CYC(19);
    ewr16(pc, (uint16_t)(REG(BX) + off), v);
}

/* The five sprites (jump targets in the table at DS:3A10), drawn bottom-up from ES:BX. Returns
 * false for an address that is not one of them. */
static bool draw_sprite(Pc *pc, uint16_t addr)
{
    switch (addr) {
    case 0x5BD1: /* 1 byte wide, 3 lines: far away */
        CYC(19);
        ewr8(pc, REG(BX), 0x6D);
        line_up(pc, 0x5BD8);
        CYC(19);
        ewr8(pc, REG(BX), erd8(pc, REG(BX)) & 0xF3);
        line_up(pc, 0x5BDF);
        CYC(19);
        ewr8(pc, REG(BX), erd8(pc, REG(BX)) & 0x6D);
        break;
    case 0x5BE4: /* 2 bytes, 3 lines */
        sprite_mov16(pc, 0, 0xDD6D);
        line_up(pc, 0x5BEC);
        sprite_and16(pc, 0x3FFF);
        line_up(pc, 0x5BF4);
        sprite_mov16(pc, 0, 0xDD6D);
        break;
    case 0x5BFA: /* 2 bytes, 4 lines */
        sprite_mov16(pc, 0, 0xDD6D);
        line_up(pc, 0x5C02);
        sprite_and16(pc, 0x3FFF);
        line_up(pc, 0x5C0A);
        sprite_and16(pc, 0x3FFF);
        CYC(17); /* jmp 5BF1 */
        line_up(pc, 0x5BF4);
        sprite_mov16(pc, 0, 0xDD6D);
        break;
    case 0x5C11: /* 3 bytes, 4 lines */
        sprite_mov16(pc, 0, 0xDD6D);
        CYC(19);
        ewr8(pc, (uint16_t)(REG(BX) + 2), 0xDD);
        line_up(pc, 0x5C1E);
        sprite_and16(pc, 0xF3FF);
        line_up(pc, 0x5C26);
        sprite_and16(pc, 0xF3FF);
        line_up(pc, 0x5C2E);
        sprite_mov16(pc, 0, 0xDD6D);
        CYC(19);
        ewr8(pc, (uint16_t)(REG(BX) + 2), 0xDD);
        break;
    case 0x5C39: /* 4 bytes, 4 lines: close */
        sprite_mov16(pc, 0, 0xDD6D);
        sprite_mov16(pc, 2, 0xDDDD);
        line_up(pc, 0x5C47);
        sprite_and16(pc, 0xF9FF);
        line_up(pc, 0x5C4F);
        sprite_and16(pc, 0xF9FF);
        line_up(pc, 0x5C57);
        sprite_mov16(pc, 0, 0xDD6D);
        sprite_mov16(pc, 2, 0xDDDD);
        break;
    default:
        return false;
    }
    ret(pc);
    return true;
}

/* 0050:5BAA: AH = x, CL = y of the enemy on the view; SI = size class * 2 (0 far .. 8 near).
 * Draws the sprite from the table at DS:3A10 into the back buffer, its bottom line at y. */
static void enemy_sprite(Pc *pc)
{
    CYC(6);
    bool out = hi8(REG(AX)) >= 0x99;
    JCC(out);
    if (!out) {
        CYC(6);
        bool in = lo8(REG(CX)) > 3;
        JCC(in);
        out = !in;
    }
    if (out) {
        ret(pc);
        return;
    }
    CYC(11 + 5 + 4 + 4 + 8 + 4 + 8 + 18 + 4 + 17 + 13);
    SREG(ES) = gs_view_buf_seg(pc);
    REG(BX) = (uint16_t)(lo8(REG(CX)) << 1);
    REG(DX) = (uint16_t)((hi8(REG(AX)) >> 1) + rd16(pc, (uint16_t)(REG(BX) + V_ROW_OFFSETS)));
    REG(BX) = REG(DX);
    REG(CX) = rd16(pc, (uint16_t)(REG(SI) + V_SPRITES));
    if (!draw_sprite(pc, REG(CX)))
        run_from(pc, REG(CX)); /* not one of the five: run it as it is */
}

/* ---- 0050:30AB war_enemy_update ---------------------------------------------------- */

/* Adds the signed speed byte [BX+speed] to the 24-bit coordinate [BX+coord], negated when the
 * delta's high byte [sign_var] is negative (3142..3177). */
static void enemy_step(Pc *pc, uint8_t speed, uint16_t sign_var, uint8_t coord)
{
    CYC(17 + 2 + 14);
    REG(AX) = (uint16_t)(int8_t)rd8(pc, (uint16_t)(REG(BX) + speed));
    bool ns = !(rd8(pc, sign_var) & 0x80);
    JCC(ns);
    if (!ns) {
        CYC(5);
        REG(AX) = (uint16_t)-REG(AX);
    }
    CYC(18 + 18);
    uint16_t a = (uint16_t)(REG(BX) + coord);
    uint32_t sum = (uint32_t)rd16(pc, a) + REG(AX);
    wr16(pc, a, (uint16_t)sum);
    a = (uint16_t)(a + 2);
    wr8(pc, a, (uint8_t)(rd8(pc, a) + hi8(REG(AX)) + (sum >> 16)));
}

/* One axis of 317A..31B3: AX = [SI] - eye, stored for rotate_point; false when out of range
 * (overflow or |AX| >= 10000), which returns from the routine. */
static bool eye_axis(Pc *pc, uint16_t eye, uint16_t out)
{
    lodsw(pc);
    CYC(18);
    uint16_t e = rd16(pc, eye);
    bool ov = sub_overflows(REG(AX), e);
    REG(AX) = (uint16_t)(REG(AX) - e);
    JCC(ov);
    if (ov)
        return false;
    CYC(2 + 12 + 5 + 5 + 6);
    REG(SI)++;
    wr16(pc, out, REG(AX));
    REG(DX) = (REG(AX) & 0x8000) ? 0xFFFF : 0;
    REG(AX) ^= REG(DX);
    bool ge = (int16_t)REG(AX) >= 0x2710;
    JCC(ge);
    return !ge;
}

/* 0050:47EA: rotate_point, then eye-space x, y, z to 310F/3111/3113. */
static void rotate_store(Pc *pc)
{
    war_call(pc, ROTATE_POINT, 0x47ED);
    CYC(18 * 3);
    gs_set_eye_p1_w(pc, 0, REG(BX));
    gs_set_eye_p1_w(pc, 1, REG(CX));
    gs_set_eye_p1_w(pc, 2, REG(DX));
    ret(pc);
}

/* The drawing half of war_enemy_update (317A..3269). */
static void enemy_draw(Pc *pc)
{
    CYC(4);
    REG(SI) = REG(BX);
    if (!eye_axis(pc, 0x30EB, 0x3139) || !eye_axis(pc, 0x30ED, 0x313B) || !eye_axis(pc, 0x30EF, 0x313D)) {
        ret(pc);
        return;
    }
    CYC(17 + 16 + 15 + 12);
    push(pc, SREG(DS));
    push(pc, REG(BX));
    gs_set_no_normalise_queue(pc, (uint8_t)(gs_no_normalise_queue(pc) + 1));
    call_body(pc, 0x31C1, rotate_store);
    CYC(12 + 14);
    REG(BX) = pop(pc);
    SREG(DS) = pop(pc);
    call_body(pc, 0x31C6, scope_marker);
    CYC(19 + 19 + 16 + 15);
    gs_set_capture_mode(pc, 0xFF);
    gs_set_capture_ptr(pc, CAPTURE_LIST);
    push(pc, SREG(DS));
    push(pc, REG(BX));
    war_call(pc, PROJECT_DOT, 0x31D6);
    CYC(12 + 14 + 19 + 19);
    REG(BX) = pop(pc);
    SREG(DS) = pop(pc);
    gs_set_capture_mode(pc, 0);
    bool none = gs_capture_ptr(pc) == CAPTURE_LIST;
    JCC(none);
    if (none) {
        ret(pc);
        return;
    }
    /* captured: AH = x, CH = y */
    CYC(15 + 16 + 4 + 19 + 15 + 5 + 12 + 6);
    push(pc, REG(BX));
    push(pc, SREG(DS));
    SET_LO(CX, hi8(REG(CX)));
    gs_set_draw_colour(pc, 0xF00F);
    push(pc, REG(AX));
    REG(DI) = 0;
    REG(AX) = gs_eye_p1_w(pc, 2);
    bool far = (int16_t)REG(AX) >= 0x2BC;
    JCC(!far);
    if (far) { /* a single dot */
        CYC(12);
        REG(AX) = pop(pc);
        war_call(pc, PLOT_PIXEL, 0x31FE);
        CYC(14 + 12);
        SREG(DS) = pop(pc);
        REG(BX) = pop(pc);
        ret(pc);
        return;
    }
    /* size class by depth: > 1C2 0, > C8 2, > 80 4 (and DI = 1), > 40 6, else 8 */
    static const int16_t depth[4] = { 0x1C2, 0xC8, 0x80, 0x40 };
    int16_t z = (int16_t)REG(AX);
    CYC(5);
    REG(SI) = 0;
    for (int i = 0; i < 4; i++) {
        CYC(6);
        JCC(z > depth[i]);
        if (z > depth[i])
            break;
        CYC(4);
        REG(SI) = (uint16_t)(2 * (i + 1));
        if (i == 1) {
            CYC(2);
            REG(DI)++;
        }
    }
    CYC(12);
    REG(AX) = pop(pc);
    /* near the middle of the view (x 46..54, y 23..3B): DI + 1 */
    uint8_t x = hi8(REG(AX)), y = lo8(REG(CX));
    bool off = false;
    CYC(6);
    off = x > 0x54;
    JCC(off);
    if (!off) {
        CYC(6);
        off = x < 0x46;
        JCC(off);
    }
    if (!off) {
        CYC(6);
        off = y > 0x3B;
        JCC(off);
    }
    if (!off) {
        CYC(6);
        off = y < 0x23;
        JCC(off);
    }
    if (!off) {
        CYC(2);
        REG(DI)++;
    }
    CYC(15);
    push(pc, REG(DI));
    call_body(pc, 0x323E, enemy_sprite);
    CYC(12 + 14 + 12 + 5 + 19);
    REG(DI) = pop(pc);
    SREG(DS) = pop(pc);
    REG(BX) = pop(pc);
    SET_LO(CX, 0);
    /* in gun range: DI = 2 (close and centred), view forward, and not a parked enemy */
    bool skip = rd8(pc, (uint16_t)(REG(BX) + 0x19)) != 1;
    JCC(skip);
    skip = false;
    if (rd8(pc, (uint16_t)(REG(BX) + 0x19)) == 1) {
        CYC(19);
        skip = (uint8_t)(rd8(pc, (uint16_t)(REG(BX) + 5)) - 1) & 0x80;
        JCC(skip);
    }
    if (!skip) {
        CYC(19);
        skip = gs_view_not_forward(pc) != 0;
        JCC(skip);
    }
    if (!skip) {
        CYC(6);
        skip = REG(DI) != 2;
        JCC(skip);
    }
    if (!skip) {
        CYC(5 + 18 + 19);
        SET_LO(CX, 1);
        gs_set_in_range(pc, 1);
        gs_set_in_range_msg(pc, rd8(pc, GS_IN_RANGE_MSG) | 1);
    }
    CYC(18);
    wr8(pc, (uint16_t)(REG(BX) + 0xA), lo8(REG(CX)));
    ret(pc);
}

/* 0050:30AB: one enemy record at BX (29 bytes, see the notes). State 0 = shot down, 1 = flying
 * home (or parked there), 2 = attacking. */
static void war_enemy_update(Pc *pc)
{
    uint16_t bx = REG(BX);
    CYC(17 + 5);
    SET_LO(AX, rd8(pc, (uint16_t)(bx + 0x19)));
    bool dead = lo8(REG(AX)) == 0;
    JCC(dead);
    if (dead) {
        ret(pc);
        return;
    }
    CYC(17 + 17 + 5);
    REG(CX) = rd16(pc, (uint16_t)(GS_POS_EAST + 2));
    REG(DX) = rd16(pc, (uint16_t)(GS_POS_NORTH + 2));
    SET_LO(AX, (uint8_t)(lo8(REG(AX)) - 1));
    bool home = lo8(REG(AX)) == 0;
    JCC(!home);
    if (home) {
        /* state 1: attack once the player is west of [BX+B] and north of [BX+D] */
        CYC(18);
        bool ns = !((uint16_t)(REG(CX) - rd16(pc, (uint16_t)(bx + 0xB))) & 0x8000);
        JCC(ns);
        if (!ns) {
            CYC(18);
            bool s = (uint16_t)(REG(DX) - rd16(pc, (uint16_t)(bx + 0xD))) & 0x8000;
            JCC(s);
            if (!s) {
                CYC(19);
                wr8(pc, (uint16_t)(bx + 0x19), 2);
            }
        }
        CYC(17);
        REG(SI) = rd16(pc, (uint16_t)(bx + 0x1B));
        call_body(pc, 0x30D2, enemy_delta_home);
        CYC(17);
    } else {
        /* state 2: give up once the player is east of [BX+F] or south of [BX+11] */
        CYC(18);
        bool west = (uint16_t)(REG(CX) - rd16(pc, (uint16_t)(bx + 0xF))) & 0x8000;
        JCC(west);
        bool give_up = !west;
        if (west) {
            CYC(18);
            give_up = (uint16_t)(REG(DX) - rd16(pc, (uint16_t)(bx + 0x11))) & 0x8000;
            JCC(give_up);
        }
        if (give_up) {
            CYC(19);
            wr8(pc, (uint16_t)(bx + 0x19), 1);
            ret(pc);
            return;
        }
        call_body(pc, 0x30E7, enemy_delta_player);
        call_body(pc, 0x30EA, enemy_distance);
        /* fire when |delta| <= [BX+16] */
        CYC(17 + 5 + 4 + 5 + 5);
        SET_LO(AX, rd8(pc, (uint16_t)(bx + 0x16)));
        REG(DX) = 0;
        SET_HI(AX, 0);
        bool cf = REG(AX) < REG(BP);
        REG(AX) = (uint16_t)(REG(AX) - REG(BP));
        REG(DX) = (uint16_t)(0 - REG(CX) - cf);
        bool fire = !(REG(DX) & 0x8000);
        JCC(fire);
        if (!fire) {
            CYC(17 + 17);
            SET_LO(AX, rd8(pc, (uint16_t)(bx + 0x18)));
        } else {
            CYC(19 + 19 + 12 + 15 + 15);
            gs_set_enemy_fire(pc, 1);
            wr8(pc, (uint16_t)(bx + 0x1A), 0xFF);
            gs_set_hits_taken(pc, (uint8_t)(rd8(pc, GS_HITS_TAKEN) + 1));
            push(pc, REG(AX));
            push(pc, REG(BX));
            call_body(pc, 0x310E, war_damage);
            CYC(12 + 12);
            REG(BX) = pop(pc);
            REG(AX) = pop(pc);
        }
    }

    /* 3110: after firing, [BX+1A] counts down from FF: dive while it is negative (every other
     * frame, not below 0), then climb; otherwise move towards the target at the record's
     * speeds. */
    CYC(19);
    uint8_t t = rd8(pc, (uint16_t)(bx + 0x1A));
    JCC(t == 0);
    if (t != 0) {
        CYC(12);
        t--;
        wr8(pc, (uint16_t)(bx + 0x1A), t);
        bool ns = !(t & 0x80);
        JCC(ns);
        uint16_t a = (uint16_t)(bx + 3);
        if (ns) {
            CYC(19 + 19 + 17);
            uint32_t sum = (uint32_t)rd16(pc, a) + 1;
            wr16(pc, a, (uint16_t)sum);
            wr8(pc, (uint16_t)(a + 2), (uint8_t)(rd8(pc, (uint16_t)(a + 2)) + (sum >> 16)));
        } else {
            CYC(14);
            bool even = !(rd8(pc, GS_FRAME_COUNTER) & 1);
            JCC(even);
            if (!even) {
                CYC(19 + 19);
                uint16_t w = rd16(pc, a);
                wr16(pc, a, (uint16_t)(w - 1));
                uint8_t h = (uint8_t)(rd8(pc, (uint16_t)(a + 2)) - (w == 0));
                wr8(pc, (uint16_t)(a + 2), h);
                bool hns = !(h & 0x80);
                JCC(hns);
                if (!hns) {
                    CYC(19 + 19 + 17);
                    wr16(pc, a, 0);
                    wr8(pc, (uint16_t)(a + 2), 0);
                }
            }
        }
    } else {
        enemy_step(pc, 0x14, GS_DELTA_ALT + 2, 3);
        enemy_step(pc, 0x13, GS_DELTA_EAST + 2, 0);
        enemy_step(pc, 0x15, GS_DELTA_NORTH + 2, 6);
    }
    enemy_draw(pc);
}

/* ---- 0050:344F war_guns ------------------------------------------------------------ */

/* 0050:344F: Space sets [1E7C] = 3, so three bullets leave on three frames, alternately from
 * the left (41,69) and right (5F,69) gun. A bullet record (6 bytes from 1EE6, FF ends) is a
 * life counter, the aim point (x, y), the current point and the initial life (0F). Each frame
 * a bullet moves a quarter of the way to the aim point; once its life is below 10 it shoots
 * down the first enemy (from the last record) whose in-range flag [+A] is set. Bullets are
 * drawn for 19h frames after the last shot. */
static void war_guns(Pc *pc)
{
    CYC(19);
    bool idle = gs_gun_burst(pc) == 0;
    JCC(idle);
    if (!idle) {
        CYC(19 + 12);
        gs_set_sound_state(pc, 3);
        gs_set_gun_burst(pc, (uint8_t)(rd8(pc, GS_GUN_BURST) - 1));
        call_body(pc, 0x3462, war_ammo_dec);
        CYC(19 + 12 + 6 + 6);
        gs_set_bullet_timer(pc, 0x19);
        REG(AX) = (uint16_t)(gs_bullet_last(pc) + 6);
        bool ne = REG(AX) != 0x1F3A;
        JCC(ne);
        if (!ne) {
            CYC(4);
            REG(AX) = BULLETS;
        }
        CYC(4 + 17 + 18 + 4 + 19);
        REG(BX) = REG(AX);
        SET_LO(AX, rd8(pc, (uint16_t)(REG(BX) + 5)));
        wr8(pc, REG(BX), lo8(REG(AX)));
        REG(AX) = 0x6941;
        uint8_t side = gs_gun_side(pc) ^ 1;
        gs_set_gun_side(pc, side);
        JCC(side == 0);
        if (side) {
            CYC(4);
            REG(AX) = 0x695F;
        }
        CYC(18 + 18);
        wr16(pc, (uint16_t)(REG(BX) + 3), REG(AX));
        gs_set_bullet_last(pc, REG(BX));
    }
    CYC(12);
    uint8_t timer = (uint8_t)(gs_bullet_timer(pc) - 1);
    gs_set_bullet_timer(pc, timer);
    JCC(timer != 0);
    if (timer == 0) {
        CYC(12);
        gs_set_bullet_timer(pc, 1);
        ret(pc);
        return;
    }
    CYC(4 + 25 + 19);
    REG(BX) = BULLETS;
    push(pc, gs_draw_colour(pc));
    gs_set_draw_colour(pc, 0x8008);
    for (;; CYC(6 + 17), REG(BX) = (uint16_t)(REG(BX) + 6)) {
        CYC(4);
        REG(SI) = REG(BX);
        lodsb(pc);
        CYC(5);
        uint8_t al = lo8(REG(AX));
        JCC(al == 0);
        if (al == 0)
            continue;
        CYC(6);
        JCC(al == 0xFF);
        if (al == 0xFF)
            break;
        CYC(5 + 18 + 6);
        al--;
        SET_LO(AX, al);
        wr8(pc, REG(BX), al);
        bool ge = (int8_t)al >= 0x0A;
        JCC(ge);
        if (!ge) {
            CYC(15 + 4);
            push(pc, REG(BX));
            REG(BX) = 0x91;
            for (;;) {
                CYC(19);
                bool hit = rd8(pc, (uint16_t)(REG(BX) + 0x1F45)) != 0;
                JCC(hit);
                if (hit) {
                    CYC(4);
                    REG(AX) = 1;
                    call_body(pc, 0x34D3, war_score_add);
                    CYC(12 + 19 + 19 + 4);
                    gs_set_kills(pc, (uint8_t)(rd8(pc, GS_KILLS) + 1));
                    wr8(pc, (uint16_t)(REG(BX) + 0x1F54), 0);
                    wr8(pc, (uint16_t)(REG(BX) + 0x1F45), 0);
                    SET_LO(CX, 0x64);
                    call_body(pc, 0x34E6, war_explosion);
                    break;
                }
                CYC(6);
                REG(BX) = (uint16_t)(REG(BX) - 0x1D);
                bool ns = !(REG(BX) & 0x8000);
                JCC(ns);
                if (!ns) {
                    CYC(16); /* js 34E6 */
                    break;
                }
            }
            CYC(12);
            REG(BX) = pop(pc);
        }
        lodsw(pc);
        CYC(4);
        REG(DX) = REG(AX);
        lodsw(pc);
        CYC(4 + 5 + 5 + 8 + 8 + 5 + 8 + 8 + 5 + 18 + 4 + 4 + 15 + 19);
        REG(CX) = REG(AX);
        uint8_t dl = (uint8_t)(lo8(REG(DX)) - lo8(REG(CX)));
        uint8_t dh = (uint8_t)(hi8(REG(DX)) - hi8(REG(CX)));
        dl = (uint8_t)((int8_t)dl >> 2);
        dh = (uint8_t)((int8_t)dh >> 2);
        REG(DX) = (uint16_t)(dh << 8 | dl);
        REG(CX) = (uint16_t)((uint8_t)(hi8(REG(CX)) + dh) << 8 | (uint8_t)(lo8(REG(CX)) + dl));
        wr16(pc, (uint16_t)(REG(BX) + 3), REG(CX));
        SET_HI(AX, lo8(REG(CX)));
        SET_LO(CX, hi8(REG(CX)));
        push(pc, REG(BX));
        bool skip = gs_view_not_forward(pc) != 0;
        JCC(skip);
        if (!skip) {
            CYC(19);
            skip = gs_radar_view(pc) == 1;
            JCC(skip);
            if (!skip)
                war_call(pc, PLOT_PIXEL_ES, 0x3516);
        }
        CYC(12);
        REG(BX) = pop(pc);
    }
    CYC(26);
    gs_set_draw_colour(pc, pop(pc));
    ret(pc);
}

/* ---- 0050:3521 war_bomb_fall ------------------------------------------------------- */

/* 0050:3521: a falling bomb counts [0575] down by 32h a frame (X starts it at [0919]). When it
 * lands, a target is hit if the scenery stored its score in [1E53] this frame, i.e. the plane
 * is over the target's box: score += [1E53], war is declared, and a 200-dot explosion is drawn
 * in colour 2002. [1E53] is cleared every frame of the fall. */
static void war_bomb_fall(Pc *pc)
{
    CYC(12 + 5);
    REG(AX) = gs_bomb_state(pc);
    bool none = REG(AX) == 0;
    JCC(none);
    if (none) {
        ret(pc);
        return;
    }
    CYC(6);
    REG(AX) = (uint16_t)(REG(AX) - 0x32);
    bool ns = !(REG(AX) & 0x8000);
    JCC(ns);
    if (!ns) {
        CYC(12 + 5);
        REG(AX) = gs_bomb_score(pc);
        bool miss = REG(AX) == 0;
        JCC(miss);
        if (!miss) {
            call_body(pc, 0x3537, war_score_add);
            CYC(19 + 19 + 4);
            gs_set_war_status_line(pc, rd8(pc, GS_WAR_STATUS_LINE) | 1);
            gs_set_explosion_colour(pc, 0x2002);
            SET_LO(CX, 0xC8);
            call_body(pc, 0x3547, war_explosion);
        }
        CYC(5);
        REG(AX) = 0;
    }
    CYC(19 + 12);
    gs_set_bomb_score(pc, 0);
    gs_set_bomb_state(pc, REG(AX));
    ret(pc);
}

/* ---- 0050:3308 scope clear, 33C2 status line ------------------------------------------ */

/* 0050:3308: clears the panel scope (22 line pairs of 23 bytes from B800:17A8) and draws its
 * fixed centre mark. */
static void war_scope_clear(Pc *pc)
{
    CYC(11 + 5 + 4 + 4);
    SREG(ES) = gs_screen_seg(pc);
    REG(AX) = 0;
    REG(DX) = 0x16;
    REG(SI) = 0x17A8;
    for (;;) {
        CYC(4 + 4);
        REG(DI) = REG(SI);
        REG(CX) = 0x17;
        rep_stosb(pc);
        CYC(4 + 6 + 4);
        REG(DI) = (uint16_t)(REG(SI) + 0x2000);
        REG(CX) = 0x17;
        rep_stosb(pc);
        CYC(6 + 2);
        REG(SI) = (uint16_t)(REG(SI) + 0x50);
        REG(DX)--;
        JCC(REG(DX) != 0);
        if (!REG(DX))
            break;
    }
    CYC(4 + 12 + 19 + 12 + 12 + 19);
    REG(AX) = 0x4001;
    ewr16(pc, 0x1A83, REG(AX));
    ewr16(pc, 0x3A83, 0x7F7F);
    ewr16(pc, 0x1AD3, REG(AX));
    ewr16(pc, 0x3AD3, REG(AX));
    ewr16(pc, 0x1B23, 0x7007);
    ret(pc);
}

/* The "BEGIN WAR!!" pause: two nested DEC/JNZ loops, 3 x 65535 iterations. */
#define BEGIN_WAR_DELAY_CYCLES (4u + 3u * (4u + 65534u * 18u + 6u + 2u) + 2u * 16u + 4u)

/* 0050:33C2: the status line. The left half shows IN RANGE (an enemy in the gun sight this
 * frame, bit 0 of [1E7A]), ENEMY FIRE! ([1E78], once per hit) or blanks, printed only when it
 * changes ([1E82]). The right half cycles every 8 frames between AMMO, SCORE and BOMBS. When
 * war is declared ([1E5A] = 1) it prints BEGIN WAR!!, sets [1E5A] = 3 and pauses. */
static void war_status(Pc *pc)
{
    CYC(15);
    uint8_t v = gs_in_range_msg(pc);
    gs_set_in_range_msg(pc, v >> 1);
    bool cf = v & 1;
    JCC(!cf);
    uint16_t msg = 0;
    if (cf) {
        CYC(19);
        bool same = gs_status_msg(pc) == 1;
        JCC(same);
        if (!same) {
            CYC(19 + 4 + 17);
            gs_set_status_msg(pc, 1);
            msg = 0x1E92; /* IN RANGE */
        }
    } else {
        CYC(19);
        bool fire = gs_enemy_fire(pc) == 1;
        JCC(!fire);
        if (fire) {
            CYC(12 + 19);
            gs_set_enemy_fire(pc, (uint8_t)(rd8(pc, GS_ENEMY_FIRE) - 1));
            bool same = gs_status_msg(pc) == 2;
            JCC(same);
            if (!same) {
                CYC(19 + 4 + 17);
                gs_set_status_msg(pc, 2);
                msg = 0x1EA0; /* ENEMY FIRE! */
            }
        } else {
            CYC(19);
            bool same = gs_status_msg(pc) == 0;
            JCC(same);
            if (!same) {
                CYC(19 + 4);
                gs_set_status_msg(pc, 0);
                msg = 0x1EAE; /* blanks */
            }
        }
    }
    if (msg) {
        REG(SI) = msg;
        war_call(pc, PRINT_STR, 0x3407);
    }
    CYC(12 + 6 + 4 + 26 + 5);
    REG(AX) = gs_frame_counter(pc) & 0x18;
    SET_HI(AX, lo8(REG(AX)));
    uint8_t old = gs_status_phase(pc);
    gs_set_status_phase(pc, hi8(REG(AX)));
    SET_HI(AX, old);
    bool same = lo8(REG(AX)) == old;
    JCC(same);
    if (!same) {
        CYC(4 + 5);
        REG(SI) = 0x1ECA; /* AMMO */
        uint8_t al = lo8(REG(AX));
        JCC(al == 0);
        if (al != 0) {
            CYC(4 + 6);
            REG(SI) = 0x1ED8; /* BOMBS */
            JCC(al == 0x18);
            if (al != 0x18) {
                CYC(4);
                REG(SI) = 0x1EBC; /* SCORE */
            }
        }
        CYC(4 + 5);
        REG(BP) = 0xFFFF;
        SET_LO(DX, 0);
        war_call(pc, PRINT_STR2, 0x3430);
    }
    CYC(19);
    bool declare = gs_war_status_line(pc) == 1;
    JCC(!declare);
    if (declare) {
        CYC(19 + 4);
        gs_set_war_status_line(pc, 3);
        REG(SI) = 0x1E84; /* BEGIN WAR!! */
        war_call(pc, PRINT_STR, 0x3442);
        CYC(BEGIN_WAR_DELAY_CYCLES);
        REG(SI) = 0;
        REG(DI) = 0;
    }
    ret(pc);
}

/* ---- 0050:3020 war_frame ----------------------------------------------------------- */

/* 0050:3020: once per frame from the main loop. Outside war mode it only resets [1E59] (and
 * [208B]) after war mode is switched off. The first war frame sets [1E59] and masks [208B]. */
static void war_frame(Pc *pc)
{
    CYC(19);
    bool war = gs_war_mode(pc) != 0;
    JCC(war);
    if (!war) {
        CYC(19);
        bool was = gs_war_active(pc) != 0;
        JCC(!was);
        if (was) {
            CYC(19 + 19);
            gs_set_war_active(pc, 0);
            wr16(pc, 0x208B, 0xFFFF);
        }
        ret(pc);
        return;
    }
    CYC(19);
    bool active = gs_war_active(pc) != 0;
    JCC(active);
    if (!active) {
        CYC(19 + 19);
        gs_set_war_active(pc, 1);
        wr16(pc, 0x208B, rd16(pc, 0x208B) & 0x70FF);
        ret(pc);
        return;
    }
    call_body(pc, 0x3050, war_scope_clear);
    call_body(pc, 0x3053, war_status);
    call_body(pc, 0x3056, war_guns);
    call_body(pc, 0x3059, war_bomb_fall);
    CYC(19);
    bool peace = gs_war_status_line(pc) == 0;
    JCC(peace);
    if (!peace) {
        for (int i = 0; i < 6; i++) {
            CYC(4);
            REG(BX) = (uint16_t)(ENEMY_FIRST + i * ENEMY_SIZE);
            call_body(pc, (uint16_t)(0x3066 + i * 6), war_enemy_update);
        }
    }
    CYC(19);
    bool skip = gs_view_not_forward(pc) != 0;
    JCC(skip);
    if (!skip) {
        CYC(19);
        skip = gs_radar_view(pc) == 1;
        JCC(skip);
    }
    if (!skip) { /* the gun sight */
        CYC(4 + 19 + 16);
        REG(AX) = 0x1FE9;
        gs_set_draw_colour(pc, 0);
        push(pc, SREG(ES));
        war_call(pc, DRAW_LINE_LIST, 0x309F);
        CYC(14);
        SREG(ES) = pop(pc);
    }
    CYC(19 + 19);
    gs_set_in_range(pc, 0);
    gs_set_in_range_msg(pc, 0);
    ret(pc);
}

/* ---- 0050:1B84 ground service, 1BB0 reset -------------------------------------------- */

/* 0050:1BB0: refuel and repair; every enemy record back to state 1. */
static void war_ground_reset(Pc *pc)
{
    CYC(19 * 9 + 4);
    wr8(pc, GS_MAG_OK, 1);
    wr8(pc, (uint16_t)(GS_MAG_OK + 1), 1);
    wr8(pc, (uint16_t)(GS_FUEL_LEFT + 2), 0x23);
    wr8(pc, (uint16_t)(GS_FUEL_RIGHT + 2), 0x23);
    wr16(pc, 0x19A0, 0xFFFF);
    wr16(pc, 0x041B, 0xFFFF);
    gs_set_hits_taken(pc, 0);
    gs_set_engine_faults(pc, 0);
    gs_set_oil_temp_rate(pc, 3);
    REG(BX) = 0x91;
    for (;;) {
        CYC(19 + 6);
        wr8(pc, (uint16_t)(REG(BX) + 0x1F54), 1);
        REG(BX) = (uint16_t)(REG(BX) - 0x1D);
        bool ns = !(REG(BX) & 0x8000);
        JCC(ns);
        if (!ns)
            break;
    }
    ret(pc);
}

void war_ground_service_body(Pc *pc)
{
    CYC(19);
    bool none = gs_ground_service(pc) == 0;
    JCC(none);
    if (none)
        return;
    CYC(19);
    bool moving = gs_airspeed(pc) != 0;
    JCC(moving);
    if (!moving) {
        call_body(pc, 0x1B95, war_ground_reset);
        CYC(19);
        bool home = gs_ground_service(pc) == 2;
        JCC(!home);
        if (home) {
            CYC(19);
            gs_set_ammo(pc, 0x65);
            call_body(pc, 0x1BA5, war_ammo_dec);
            CYC(19);
            gs_set_bombs_left(pc, '5');
        }
    }
    CYC(19);
    gs_set_ground_service(pc, 0);
}

/* 0050:1B84 (the tail of flight_params). */
static void ground_service(Pc *pc)
{
    war_ground_service_body(pc);
    ret(pc);
}

/* ---- public helpers (war.h) ---------------------------------------------------------- */

void war_call_ground_reset(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, war_ground_reset); }
void war_call_score_add(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, war_score_add); }
void war_call_ammo_dec(Pc *pc, uint16_t ret_ip) { call_body(pc, ret_ip, war_ammo_dec); }

/* ---- natives ------------------------------------------------------------------------ */

/* Each .cycles is the original's cost on its shortest path; the bodies add the full cost. */
#define CYC_WAR_FRAME 78          /* not war mode, nothing to reset */
#define CYC_WAR_ENEMY_UPDATE 61   /* state 0 */
#define CYC_WAR_GUNS 79           /* no burst, timer reaches 0 */
#define CYC_WAR_BOMB_FALL 53      /* no bomb */
#define CYC_WAR_AMMO_DEC 535      /* with fmt_dec4's 449 */
#define CYC_WAR_SCORE_ADD 683
#define CYC_WAR_STATUS 125
#define CYC_WAR_SCOPE_CLEAR 11406
#define CYC_GROUND_SERVICE 55     /* [0406] = 0 */
#define CYC_WAR_GROUND_RESET 429

#define NATIVE(fn, fixed)                                                                                              \
    static void n_##fn(Pc *pc)                                                                                         \
    {                                                                                                                  \
        fn(pc);                                                                                                        \
        pc->cpu.cycles -= (fixed);                                                                                     \
    }
NATIVE(war_frame, CYC_WAR_FRAME)
NATIVE(war_enemy_update, CYC_WAR_ENEMY_UPDATE)
NATIVE(war_guns, CYC_WAR_GUNS)
NATIVE(war_bomb_fall, CYC_WAR_BOMB_FALL)
NATIVE(war_ammo_dec, CYC_WAR_AMMO_DEC)
NATIVE(war_score_add, CYC_WAR_SCORE_ADD)
NATIVE(war_status, CYC_WAR_STATUS)
NATIVE(war_scope_clear, CYC_WAR_SCOPE_CLEAR)
NATIVE(ground_service, CYC_GROUND_SERVICE)
NATIVE(war_ground_reset, CYC_WAR_GROUND_RESET)
#undef NATIVE

NativeEntry native_war[] = {
    { .name = "war_frame", .seg = GAME_CS, .off = 0x3020, .fn = n_war_frame, .enabled = true,
      .cycles = CYC_WAR_FRAME },
    { .name = "war_enemy_update", .seg = GAME_CS, .off = 0x30AB, .fn = n_war_enemy_update, .enabled = true,
      .cycles = CYC_WAR_ENEMY_UPDATE },
    { .name = "war_guns", .seg = GAME_CS, .off = 0x344F, .fn = n_war_guns, .enabled = true,
      .cycles = CYC_WAR_GUNS },
    { .name = "war_bomb_fall", .seg = GAME_CS, .off = 0x3521, .fn = n_war_bomb_fall, .enabled = true,
      .cycles = CYC_WAR_BOMB_FALL },
    { .name = "war_ammo_dec", .seg = GAME_CS, .off = 0x3595, .fn = n_war_ammo_dec, .enabled = true,
      .cycles = CYC_WAR_AMMO_DEC },
    { .name = "war_score_add", .seg = GAME_CS, .off = 0x3575, .fn = n_war_score_add, .enabled = true,
      .cycles = CYC_WAR_SCORE_ADD },
    { .name = "war_status", .seg = GAME_CS, .off = 0x33C2, .fn = n_war_status, .enabled = true,
      .cycles = CYC_WAR_STATUS },
    { .name = "war_scope_clear", .seg = GAME_CS, .off = 0x3308, .fn = n_war_scope_clear, .enabled = true,
      .cycles = CYC_WAR_SCOPE_CLEAR },
    { .name = "ground_service", .seg = GAME_CS, .off = 0x1B84, .fn = n_ground_service, .enabled = true,
      .cycles = CYC_GROUND_SERVICE },
    { .name = "war_ground_reset", .seg = GAME_CS, .off = 0x1BB0, .fn = n_war_ground_reset, .enabled = true,
      .cycles = CYC_WAR_GROUND_RESET },
    { .name = NULL },
};
