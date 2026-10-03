/* Natives for subphase 3.11 gauge updaters (see docs/PHASE3_PLAN.md and
 * docs/subphases/3.11.md).
 *
 * The per-instrument update routines that the main loop runs each frame or on one phase of
 * its 4-frame cycle. Each routine is transliterated so that registers, segment registers
 * and memory end up exactly as the original leaves them, including the return addresses
 * and PUSHes it leaves below SP (written through a virtual SP, as in indicator.c).
 *
 * Calls:
 * - draw_indicator, draw_needle(2) and blit_sprite go to the C versions from 3.10
 *   (panel.h).
 * - Original code with no C version reachable from here (print_str and its variants, the
 *   rectangle routines at 5AD7/5AF3, the pixel XOR at 5B88, and clear_view_buffer /
 *   fmt_signed_dec, whose natives are static in other areas) is run on the CPU core with
 *   the natives switched off (call_orig). Once 3.9's print_str is merged it can be called
 *   directly instead.
 *
 * Cycles: every routine adds the emulated cycles the original takes on the path it ran
 * (the cpu8086.c model: 2 per instruction plus the opcode's cost, 7 more for a memory
 * operand, 4 per bit shifted). Code run through call_orig adds its own cycles as it
 * executes. So the emulated timeline is the same with the natives on. */
#include "panel.h"
#include "game/state.h"

static uint32_t cyc; /* original cycles of the routine being run (own code only) */

/* ---- memory and register helpers ------------------------------------------------ */

static uint8_t rd8(Pc *pc, uint16_t seg, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(seg, off)); }
static uint16_t rd16(Pc *pc, uint16_t seg, uint16_t off) { return mem_read16(pc, seg, off); }
static void wr8(Pc *pc, uint16_t seg, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(seg, off), v); }
static void wr16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v) { mem_write16(pc, seg, off, v); }

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])
#define DS SREG(DS)

static uint8_t lo8(uint16_t v) { return (uint8_t)v; }
static uint16_t with_lo(uint16_t v, uint8_t b) { return (uint16_t)((v & 0xFF00) | b); }
static uint16_t with_hi(uint16_t v, uint8_t b) { return (uint16_t)((v & 0x00FF) | b << 8); }
#define SET_LO(r, b) (REG(r) = with_lo(REG(r), (uint8_t)(b)))
#define SET_HI(r, b) (REG(r) = with_hi(REG(r), (uint8_t)(b)))

/* Conditional jump: taken 16, not taken 4. */
#define JCC(taken) (cyc += (taken) ? 16 : 4)

#define CYC_CALL 21
#define CYC_RET 20

/* PUSH through the virtual stack pointer *sp. */
static void vpush(Pc *pc, uint16_t *sp, uint16_t v)
{
    *sp = (uint16_t)(*sp - 2);
    wr16(pc, SREG(SS), *sp, v);
}

/* ---- calls out of a native ---------------------------------------------------------- */

/* Runs original code from CS:IP on the CPU core with the natives off, until SP rises to
 * stop_sp with IP = stop_ip. The cycles go to the CPU's counter as they run. Returns false
 * if the step cap was hit. */
static bool step_until(Pc *pc, uint16_t stop_ip, uint16_t stop_sp)
{
    Cpu8086 *c = &pc->cpu;
    const uint8_t *map = c->hook_map;
    bool ok = false;
    c->hook_map = NULL;
    for (long n = 0; n < 5000000L; n++) {
        native_or_cpu_step(pc);
        if (c->ip == stop_ip && c->sregs[S_CS] == GAME_CS && c->regs[R_SP] == stop_sp) {
            ok = true;
            break;
        }
    }
    c->hook_map = map;
    return ok;
}

/* Runs the original routine at target as if CALLed with SP = sp and return address
 * ret_ip, on the CPU core. The real SP and IP are left as they were. */
static void run_orig_at(Pc *pc, uint16_t sp, uint16_t target, uint16_t ret_ip)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t real_sp = c->regs[R_SP], ip = c->ip;
    c->regs[R_SP] = sp;
    cpu_push(c, ret_ip);
    c->ip = target;
    step_until(pc, ret_ip, sp);
    c->regs[R_SP] = real_sp;
    c->ip = ip;
}

/* Emulates "CALL target" made with SP = sp; adds the CALL's cycles. */
static void call_orig(Pc *pc, uint16_t sp, uint16_t target, uint16_t ret_ip)
{
    cyc += CYC_CALL;
    run_orig_at(pc, sp, target, ret_ip);
}

/* Emulates the native's tail "JMP target" into original code, which returns to the
 * native's caller with its RET. The native must not return again afterwards. */
static void jmp_orig(Pc *pc, uint16_t target)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP];
    uint16_t ret_ip = rd16(pc, SREG(SS), sp);
    c->ip = target;
    cyc += 17;
    step_until(pc, ret_ip, (uint16_t)(sp + 2));
}

/* Runs the whole original routine at CS:IP instead of the native (cases the native does
 * not reproduce, such as a divide error). Returns its cycles; the CPU counter is left as it
 * was, so the caller charges them. */
static uint32_t run_original(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP];
    uint16_t ret_ip = rd16(pc, SREG(SS), sp);
    uint64_t start = c->cycles;
    step_until(pc, ret_ip, (uint16_t)(sp + 2));
    uint32_t used = (uint32_t)(c->cycles - start);
    c->cycles = start;
    return used;
}

/* The 3.10 panel helpers, called with SP = sp. Adds the CALL and the routine's cycles. */
static void call_panel(Pc *pc, uint16_t sp, void (*fn)(Pc *, uint16_t), uint16_t ret_ip)
{
    uint16_t real_sp = REG(SP);
    REG(SP) = sp;
    fn(pc, ret_ip);
    REG(SP) = real_sp;
    cyc += CYC_CALL + panel_cycles();
}

/* Original entry points run through call_orig. */
#define PRINT_STR 0x1225
#define PRINT_STR_DIM 0x122B
#define PRINT_STR_MASK 0x122E
#define CLEAR_VIEW_BUFFER 0x561E
#define FMT_SIGNED_DEC 0x11A9
#define RECT_CLEAR 0x5AD7
#define RECT_RESTORE 0x5AF3
#define XOR_DOT 0x5B88

/* panel_mask (DS:19A3) bits */
#define MASK_AIRSPEED 0x0001
#define MASK_TURN 0x0002
#define MASK_HEADING 0x0004
#define MASK_ALTIMETER 0x0008
#define MASK_TACH 0x0040
#define MASK_VSI 0x0080
#define MASK_IND_A 0x2000
#define MASK_IND_B 0x4000
#define MASK_TEXT 0x8000

static uint16_t panel_mask(Pc *pc) { return rd16(pc, DS, 0x19A3); }

/* ---- panel_text_gate 0050:156D ------------------------------------------------------
 * CALLed first thing by the text routines. With panel_mask bit 15 clear it pops its own
 * return address into AX and returns from the caller. Returns true if the caller goes on.
 * sp points at the caller's return address; the gate's return address goes below it. */
static bool text_gate(Pc *pc, uint16_t sp, uint16_t ret_ip)
{
    vpush(pc, &sp, ret_ip);
    bool on = panel_mask(pc) & MASK_TEXT;
    cyc += CYC_CALL + 14;
    JCC(on);
    if (!on) {
        REG(AX) = ret_ip;
        cyc += 12;
    }
    cyc += CYC_RET;
    return on;
}

/* ---- gauge_upd_1401/140D/1419/1425 0050:02CB/02E2/02F9/0310 -------------------------
 * If the panel_mask bit is set: frame = var + 2 -> descriptor +6, draw_indicator. */
static void gauge_upd_simple(Pc *pc, uint16_t mask, uint16_t var, uint16_t desc, uint16_t ret_ip)
{
    bool on = panel_mask(pc) & mask;
    cyc += 14;
    JCC(!on);
    if (on) {
        uint8_t al = (uint8_t)(rd8(pc, DS, var) + 2);
        SET_LO(AX, al);
        wr8(pc, DS, (uint16_t)(desc + 6), al);
        REG(BP) = desc;
        cyc += 12 + 6 + 12 + 4;
        call_panel(pc, REG(SP), panel_call_draw_indicator, ret_ip);
    }
    cyc += CYC_RET;
}

/* ---- gauge_upd_1431 0050:0327 (indicator 1431, value [08F7]) --------------------------
 * In the air: [084B] * 2 (saturated) * 30h >> 8; on the ground: rudder * -30h >> 8 while
 * rolling, else 0. Frame = value + 1Fh. */
static void gauge_upd_1431(Pc *pc)
{
    uint16_t ds = DS;
    bool on = panel_mask(pc) & MASK_TURN;
    cyc += 14;
    JCC(!on);
    if (on) {
        bool ground = gs_on_ground(pc) != 0;
        cyc += 14;
        JCC(ground);
        if (!ground) {
            uint8_t a = rd8(pc, ds, (uint16_t)(GS_CROSS_CONTROL + 1));
            int r = (int8_t)a * 2;
            uint8_t al = (uint8_t)r;
            bool of = r != (int8_t)al;
            cyc += 12 + 5;
            JCC(!of);
            if (of) {
                bool sf = al & 0x80;
                al = 0x7F;
                cyc += 4;
                JCC(sf);
                if (!sf) {
                    al = 0x81;
                    cyc += 4;
                }
            }
            int16_t p = (int16_t)((int8_t)al * 0x30);
            REG(DX) = with_lo(REG(DX), 0x30);
            REG(AX) = (uint16_t)(int16_t)(int8_t)((uint16_t)p >> 8);
            gs_set_ball_value(pc, REG(AX));
            cyc += 4 + 82 + 4 + 2 + 12 + 17;
        } else {
            bool moving = gs_airspeed(pc) != 0;
            cyc += 14;
            JCC(!moving);
            if (moving) {
                uint8_t a = rd8(pc, ds, (uint16_t)(GS_RUDDER + 1));
                int16_t p = (int16_t)((int8_t)a * (int8_t)0xD0);
                REG(DX) = with_lo(REG(DX), 0xD0);
                REG(AX) = (uint16_t)(int16_t)(int8_t)((uint16_t)p >> 8);
                gs_set_ball_value(pc, REG(AX));
                cyc += 12 + 4 + 82 + 4 + 2 + 12 + 17;
            } else {
                gs_set_ball_value(pc, 0);
                cyc += 19;
            }
        }
        uint16_t dx = gs_ball_value(pc);
        dx = with_lo(dx, (uint8_t)(dx + 0x1F));
        REG(DX) = dx;
        wr8(pc, ds, 0x1437, lo8(dx));
        REG(BP) = 0x1431;
        cyc += 17 + 6 + 18 + 4;
        call_panel(pc, REG(SP), panel_call_draw_indicator, 0x037D);
    }
    cyc += CYC_RET;
}

/* ---- turn_needle 0050:037E (sprite set at DS:16DD, state [0403]) ----------------------
 * turn_rate * 190h >> 16, clamped to -6..6; on a change the 6-word area is cleared
 * (sub_04A5) and the sprite record for the new value is blitted (blit_sprite). */

/* 0050:04A5: ES = B800, clears 3 words on 13 rows from B800:1AE4. */
static void turn_clear(Pc *pc)
{
    static const uint16_t rows[13] = { 0x1AE4, 0x3AE4, 0x1B34, 0x3B34, 0x1B84, 0x3B84, 0x1BD4,
                                       0x3BD4, 0x1C24, 0x3C24, 0x1C74, 0x3C74, 0x1CC4 };
    int16_t d = (pc->cpu.flags & F_DF) ? -2 : 2;
    uint16_t di = 0;
    REG(AX) = 0;
    SREG(ES) = 0xB800;
    for (int i = 0; i < 13; i++) {
        di = rows[i];
        for (int k = 0; k < 3; k++) {
            wr16(pc, 0xB800, di, 0);
            di = (uint16_t)(di + d);
        }
    }
    REG(DI) = di;
    cyc += 4 + 4 + 5 + 13 * (4 + 3 * 12) + CYC_RET;
}

static void turn_needle(Pc *pc)
{
    uint16_t ds = DS;
    bool on = panel_mask(pc) & MASK_TURN;
    cyc += 14;
    JCC(!on);
    if (!on) {
        cyc += CYC_RET;
        return;
    }
    int32_t p = (int16_t)(uint16_t)gs_turn_rate(pc) * 0x190;
    uint16_t dx = (uint16_t)((uint32_t)p >> 16);
    REG(AX) = (uint16_t)p;
    REG(DX) = dx;
    uint16_t bx = dx;
    cyc += 12 + 4 + 130 + 4 + 6;
    JCC((int16_t)dx > 6);
    if ((int16_t)dx > 6) {
        bx = with_lo(bx, 6);
        cyc += 4 + 17;
    } else {
        cyc += 6;
        JCC((int16_t)dx < -6);
        if ((int16_t)dx < -6) {
            bx = with_lo(bx, 0xFA);
            cyc += 4;
        } else {
            cyc += 17;
        }
    }
    REG(BX) = bx;
    uint8_t bl = lo8(bx);
    bool same = bl == rd8(pc, ds, 0x403);
    cyc += 18;
    JCC(same);
    if (!same) {
        wr8(pc, ds, 0x403, bl);
        bl = (uint8_t)(bl + 6);
        REG(BX) = with_lo(bx, bl);
        cyc += 18 + 6 + CYC_CALL;
        uint16_t sp = REG(SP);
        vpush(pc, &sp, 0x03B2);
        turn_clear(pc);
        REG(BX) = rd16(pc, ds, (uint16_t)(0x16DD + (uint8_t)(bl << 1)));
        cyc += 8 + 5 + 17;
        call_panel(pc, REG(SP), panel_call_blit_sprite, 0x03BD);
    }
    cyc += CYC_RET;
}

/* ---- alt_setting 0050:0742 ---------------------------------------------------------
 * While airspeed <= A00h the altimeter offset [0906] follows the setting [090A]. In flight
 * a difference moves [0905] and the altitude by 100h per call (never reached in traces). */

/* Flags of SUB/SBB a,b,borrow (16-bit): returns JG and sets *sf. */
static bool sub16_jg(uint16_t a, uint16_t b, int borrow, bool *sf)
{
    uint16_t r = (uint16_t)(a - b - borrow);
    bool zf = r == 0;
    bool s = r & 0x8000;
    bool of = ((a ^ b) & (a ^ r)) & 0x8000;
    if (sf)
        *sf = s;
    return !zf && s == of;
}

static void alt_setting(Pc *pc)
{
    uint16_t ds = DS;
    uint16_t ax = gs_field_elevation(pc);
    bool fast = (int16_t)gs_airspeed(pc) > 0x0A00;
    cyc += 12 + 19;
    JCC(fast);
    if (!fast) {
        gs_set_ground_elev(pc, ax);
        cyc += 12;
    }
    uint16_t off = gs_ground_elev(pc);
    bool jg = sub16_jg(ax, off, 0, NULL);
    ax = (uint16_t)(ax - off);
    cyc += 18;
    JCC(ax == 0);
    if (ax != 0) {
        ax = 0x100;
        cyc += 4;
        JCC(jg);
        if (!jg) {
            ax = 0xFF00;
            cyc += 5;
        }
        uint16_t dx = (ax & 0x8000) ? 0xFFFF : 0;
        uint16_t a = rd16(pc, ds, 0x905);
        uint32_t s = (uint32_t)a + ax;
        wr16(pc, ds, 0x905, (uint16_t)s);
        uint16_t b = rd16(pc, ds, (uint16_t)(GS_GROUND_ELEV + 1));
        wr16(pc, ds, (uint16_t)(GS_GROUND_ELEV + 1), (uint16_t)(b + dx + (s >> 16)));
        uint16_t lo = rd16(pc, ds, GS_ALTITUDE);
        int borrow = lo < ax;
        wr16(pc, ds, GS_ALTITUDE, (uint16_t)(lo - ax));
        uint16_t hi = rd16(pc, ds, (uint16_t)(GS_ALTITUDE + 2));
        bool sf;
        bool hjg = sub16_jg(hi, dx, borrow, &sf);
        wr16(pc, ds, (uint16_t)(GS_ALTITUDE + 2), (uint16_t)(hi - dx - borrow));
        REG(DX) = dx;
        cyc += 5 + 18 + 18 + 18 + 18;
        JCC(hjg);
        if (!hjg) {
            JCC(sf);
            bool clamp = true;
            if (!sf) {
                bool above = (int16_t)rd16(pc, ds, GS_ALTITUDE) > 0x300;
                cyc += 19;
                JCC(above);
                clamp = !above;
            }
            if (clamp) {
                wr16(pc, ds, GS_ALTITUDE, 0x300);
                wr16(pc, ds, (uint16_t)(GS_ALTITUDE + 2), 0);
                cyc += 19 + 19;
            }
        }
    }
    REG(AX) = ax;
    cyc += CYC_RET;
}

/* ---- upd_altimeter 0050:1FE0 --------------------------------------------------------
 * [0919] = altitude word, [0900] = it + [0906] (offset); (that + [1E20]) * 6B8Fh gives the
 * large hand (angle, low byte) and, mod 9F6h / 10, the small hand angle (+9 of 0943).
 * draw_needle runs with [0914] raised (altitude band marker) and AH = 1 (second needle). */

/* Small-hand value, or -1 if the original's DIV would overflow. */
static int alt_small_hand(uint16_t ax, uint32_t *loops)
{
    *loops = 0;
    while ((int16_t)ax > 0x9F6) {
        ax = (uint16_t)(ax - 0x9F6);
        (*loops)++;
    }
    if (ax & 0x8000)
        ax = (uint16_t)(ax + 0x9F6);
    if (ax / 10 > 0xFF)
        return -1;
    return ax;
}

static void upd_altimeter(Pc *pc)
{
    uint16_t ds = DS;
    bool on = panel_mask(pc) & MASK_ALTIMETER;
    cyc += 14;
    JCC(!on);
    if (!on) {
        cyc += CYC_RET;
        return;
    }
    uint16_t ax = rd16(pc, ds, (uint16_t)(GS_ALTITUDE + 1));
    gs_set_alt_m(pc, ax);
    ax = (uint16_t)(ax + gs_ground_elev(pc));
    gs_set_alt_display(pc, ax);
    ax = (uint16_t)(ax + rd16(pc, ds, 0x1E20));
    int32_t p = (int16_t)ax * 0x6B8F;
    uint16_t plo = (uint16_t)p, dx = (uint16_t)((uint32_t)p >> 16);
    ax = (uint16_t)(dx << 1 | plo >> 15);
    uint8_t cl = lo8(ax);
    cyc += 12 + 12 + 18 + 12 + 18 + 4 + 130 + 8 + 4 + 8 + 4;
    uint32_t loops;
    int v = alt_small_hand(ax, &loops); /* >= 0: the native is not run otherwise */
    cyc += loops * (6 + 4 + 6 + 17) + 6 + 16 + 5;
    bool neg = (int16_t)(uint16_t)(ax - loops * 0x9F6) < 0;
    JCC(!neg);
    if (neg)
        cyc += 6;
    wr8(pc, ds, 0x943 + 9, (uint8_t)(v / 10));
    gs_set_second_needle_desc(pc, 0x943);
    gs_set_needle2_force(pc, 1);
    REG(AX) = (uint16_t)(0x0100 | cl);
    REG(BX) = 0x943;
    REG(CX) = 0x939;
    REG(DX) = dx;
    gs_set_alt_band_on(pc, (uint8_t)(gs_alt_band_on(pc) + 1));
    cyc += 4 + 82 + 4 + 18 + 18 + 19 + 4 + 4 + 4 + 12;
    call_panel(pc, REG(SP), panel_call_draw_needle, 0x2038);
    ds = DS;
    gs_set_alt_band_on(pc, (uint8_t)(gs_alt_band_on(pc) - 1));
    cyc += 12 + CYC_RET;
}

/* True if upd_altimeter would raise a divide error (small hand value out of range). */
static bool altimeter_div_overflow(Pc *pc)
{
    uint16_t ds = DS;
    if (!(panel_mask(pc) & MASK_ALTIMETER))
        return false;
    uint16_t ax = (uint16_t)(rd16(pc, ds, (uint16_t)(GS_ALTITUDE + 1)) + gs_ground_elev(pc) + rd16(pc, ds, 0x1E20));
    int32_t p = (int16_t)ax * 0x6B8F;
    ax = (uint16_t)((uint16_t)((uint32_t)p >> 16) << 1 | (uint16_t)p >> 15);
    uint32_t loops;
    return alt_small_hand(ax, &loops) < 0;
}

/* ---- upd_airspeed 0050:2043 ---------------------------------------------------------
 * airspeed * 28Fh >> 16 (-> [0902]), clamped to 33h..100h, minus 19h: the needle angle. */
static void upd_airspeed(Pc *pc)
{
    bool on = panel_mask(pc) & MASK_AIRSPEED;
    cyc += 14;
    JCC(!on);
    if (on) {
        uint32_t p = (uint32_t)gs_airspeed(pc) * 0x28F;
        uint16_t dx = (uint16_t)(p >> 16);
        gs_set_ias_knots(pc, dx);
        cyc += 12 + 4 + 120 + 18 + 6;
        JCC((int16_t)dx >= 0x33);
        if ((int16_t)dx < 0x33) {
            dx = 0x33;
            cyc += 4;
        }
        cyc += 6;
        JCC((int16_t)dx <= 0x100);
        if ((int16_t)dx > 0x100) {
            dx = 0x100;
            cyc += 4;
        }
        dx = (uint16_t)((dx - 0x19) & 0xFF);
        REG(DX) = dx;
        REG(AX) = dx;
        REG(CX) = 0x92F;
        cyc += 6 + 5 + 4 + 4;
        call_panel(pc, REG(SP), panel_call_draw_needle, 0x2075);
    }
    cyc += CYC_RET;
}

/* ---- upd_vsi 0050:207C --------------------------------------------------------------
 * Pushes vertical_speed into the 10-word history at [091B] and shows the oldest value:
 * v * 7D0h >> 15, clamped to -7Dh..7Dh, + C0h -> draw_needle2 (descriptor 094D). */
static void upd_vsi(Pc *pc)
{
    uint16_t ds = DS;
    bool on = panel_mask(pc) & MASK_VSI;
    cyc += 14;
    JCC(!on);
    if (on) {
        uint16_t ax = (uint16_t)gs_vertical_speed(pc);
        cyc += 12 + 4;
        for (int bx = 0x12; bx >= 0; bx -= 2) {
            uint16_t a = (uint16_t)(0x91B + bx);
            uint16_t old = rd16(pc, ds, a);
            wr16(pc, ds, a, ax);
            ax = old;
            cyc += 26 + 6;
            JCC(bx != 0);
        }
        int32_t p = (int16_t)ax * 0x7D0;
        uint16_t dx = (uint16_t)((uint32_t)p >> 16);
        ax = (uint16_t)(dx << 1 | (uint16_t)p >> 15);
        cyc += 4 + 130 + 8 + 4 + 8 + 6;
        JCC((int16_t)ax >= -0x7D);
        if ((int16_t)ax < -0x7D) {
            ax = 0xFF83;
            cyc += 4;
        }
        cyc += 6;
        JCC((int16_t)ax <= 0x7D);
        if ((int16_t)ax > 0x7D) {
            ax = 0x7D;
            cyc += 4;
        }
        ax = (uint16_t)((ax + 0xC0) & 0xFF);
        REG(AX) = ax;
        REG(BX) = 0xFFFE;
        REG(CX) = 0x94D;
        REG(DX) = dx;
        cyc += 6 + 4 + 4;
        call_panel(pc, REG(SP), panel_call_draw_needle2, 0x20B9);
    }
    cyc += CYC_RET;
}

/* ---- upd_tach 0050:20C0 (reached by JMP from rpm_lag 2EEC) ---------------------------
 * engine_rpm * 3B6h >> 8 (low byte) + 8Ah -> draw_needle2 (descriptor 0957). */
static void upd_tach(Pc *pc)
{
    bool on = panel_mask(pc) & MASK_TACH;
    cyc += 14;
    JCC(!on);
    if (on) {
        uint32_t p = (uint32_t)gs_engine_rpm(pc) * 0x3B6;
        uint16_t dx = (uint16_t)(p >> 16);
        uint16_t ax = (uint16_t)(((uint8_t)(p >> 8) + 0x8A) & 0xFF);
        REG(AX) = ax;
        REG(CX) = 0x957;
        REG(DX) = dx;
        cyc += 12 + 5 + 4 + 120 + 4 + 6 + 5 + 4;
        call_panel(pc, REG(SP), panel_call_draw_needle2, 0x20DF);
    }
    cyc += CYC_RET;
}

/* ---- rpm_lag 0050:2EEC --------------------------------------------------------------
 * The shown RPM [1DC0] moves one step towards the engine's [1DC1]; then the tachometer. */
static void rpm_lag(Pc *pc)
{
    uint8_t al = gs_engine_rpm(pc), target = gs_rpm_target(pc);
    SET_LO(AX, al);
    cyc += 12 + 18;
    JCC(al == target);
    if (al == target) {
        cyc += CYC_RET;
        return;
    }
    bool gt = (int8_t)al > (int8_t)target;
    JCC(gt);
    if (!gt) {
        al = (uint8_t)(al + 2);
        cyc += 6;
    }
    al = (uint8_t)(al - 1);
    SET_LO(AX, al);
    gs_set_engine_rpm(pc, al);
    cyc += 6 + 12 + 17;
    upd_tach(pc);
}

/* ---- heading_readout 0050:1375 ------------------------------------------------------
 * ([03F9] + view_heading - [0917] - [0915]) * 168h >> 16: three ASCII digits at [0799],
 * printed with mask 5555h (record DS:0797). */
static void heading_readout(Pc *pc)
{
    uint16_t ds = DS;
    bool on = panel_mask(pc) & MASK_HEADING;
    cyc += 14;
    JCC(!on);
    if (on) {
        uint16_t ax = (uint16_t)(rd16(pc, ds, 0x3F9) + (uint16_t)gs_view_heading(pc) - gs_wind_gust_offset(pc) -
                                 gs_wind_dir_offset(pc));
        ax = (uint16_t)(((uint32_t)ax * 0x168) >> 16);
        uint16_t dx = ax;
        cyc += 12 + 18 + 18 + 18 + 4 + 120 + 4 + 4 + 4 + 4;
        uint8_t dl = 0x30;
        for (;;) {
            bool lt = (int16_t)ax < 100;
            cyc += 6;
            JCC(lt);
            if (lt)
                break;
            ax -= 100;
            dl++;
            cyc += 6 + 5 + 17;
        }
        wr8(pc, ds, 0x799, dl);
        dl = 0x30;
        cyc += 18 + 4;
        for (;;) {
            bool lt = (int16_t)ax < 10;
            cyc += 6;
            JCC(lt);
            if (lt)
                break;
            ax -= 10;
            dl++;
            cyc += 6 + 5 + 17;
        }
        wr8(pc, ds, 0x79A, dl);
        ax = with_lo(ax, (uint8_t)(ax + 0x30));
        wr8(pc, ds, 0x79B, lo8(ax));
        REG(AX) = ax;
        REG(BX) = 0x799;
        REG(CX) = 0x5555;
        REG(DX) = with_lo(dx, dl);
        REG(SI) = 0x797;
        cyc += 18 + 6 + 18 + 4;
        call_orig(pc, REG(SP), PRINT_STR_MASK, 0x13C5);
    }
    cyc += CYC_RET;
}

/* ---- slew_readout 0050:1150 ---------------------------------------------------------
 * In slew mode: clears the view buffer (A0h words) and formats the north/east readouts
 * ([2050]/[2052], and the position bytes when [0418] is set) with fmt_signed_dec. */
static void slew_readout(Pc *pc)
{
    uint16_t ds = DS;
    uint16_t sp = REG(SP);
    bool slew = gs_slew_mode(pc) != 0;
    cyc += 14;
    JCC(slew);
    if (!slew) {
        cyc += 17 + CYC_RET;
        return;
    }
    gs_set_clear_words(pc, 0xA0);
    cyc += 19;
    call_orig(pc, sp, CLEAR_VIEW_BUFFER, 0x1163);
    REG(AX) = gs_editor_north(pc);
    REG(SI) = 0x6DA;
    REG(BX) = 0x6E1;
    cyc += 12 + 4 + 4;
    call_orig(pc, sp, FMT_SIGNED_DEC, 0x116F);
    REG(AX) = gs_editor_east(pc);
    REG(SI) = 0x6E8;
    REG(BX) = 0x6EE;
    cyc += 12 + 4 + 4;
    call_orig(pc, sp, FMT_SIGNED_DEC, 0x117B);
    ds = DS;
    bool pos = rd8(pc, ds, 0x418) != 0;
    cyc += 19;
    JCC(!pos);
    if (pos) {
        uint16_t n = rd8(pc, ds, (uint16_t)(GS_POS_NORTH + 1));
        wr16(pc, ds, 0x6AD, n);
        uint16_t e = rd8(pc, ds, (uint16_t)(GS_POS_EAST + 1));
        wr16(pc, ds, 0x6AF, e);
        REG(AX) = rd16(pc, ds, 0x6AD);
        REG(SI) = 0x6F5;
        REG(BX) = 0x6F7;
        cyc += 5 + 12 + 12 + 12 + 12 + 12 + 4 + 4;
        call_orig(pc, sp, FMT_SIGNED_DEC, 0x119C);
        REG(AX) = rd16(pc, DS, 0x6AF);
        REG(SI) = 0x6FE;
        REG(BX) = 0x700;
        cyc += 12 + 4 + 4;
        call_orig(pc, sp, FMT_SIGNED_DEC, 0x11A8);
    }
    cyc += CYC_RET;
}

/* ---- clock text 0050:151C / 1540 ----------------------------------------------------
 * 151C: if [0550], prints the seconds (record 07D9); at second 0 it goes on into 1540.
 * 1540: prints minutes (07D4) and hours (07CF), then sets the light level [0412] from the
 * time of day (sub_2FD0). Both go through panel_text_gate. */

/* Two ASCII digits of AL (AAM 10), tens in the low byte. */
static uint16_t two_digits(uint8_t al) { return (uint16_t)((0x30 + al / 10) | (0x30 + al % 10) << 8); }

/* 0050:2FD0: [0412] = 4 / 2 / 1 / 2 / 4 by clock (hour:min) against the 4 times of the
 * season row [206A] & 3 of the table at DS:1E2C. */
static void light_level(Pc *pc)
{
    uint16_t ds = DS;
    uint16_t bx = (uint16_t)(((gs_season(pc) & 3) << 3) + 0x1E2C);
    uint16_t ax = (uint16_t)(gs_clock_min(pc) | gs_clock_hour(pc) << 8);
    REG(AX) = ax;
    REG(BX) = bx;
    REG(CX) = 0x1E2C;
    cyc += 17 + 6 + 8 + 8 + 8 + 4 + 5 + 12 + 17;
    uint16_t level = 4;
    for (int i = 0; i < 4; i++) {
        bool lt = (int16_t)ax < (int16_t)rd16(pc, ds, (uint16_t)(bx + 2 * i));
        cyc += 18;
        JCC(lt);
        if (lt) {
            static const uint16_t lv[4] = { 4, 2, 1, 2 };
            level = lv[i];
            break;
        }
    }
    gs_set_time_of_day(pc, level);
    cyc += 19 + CYC_RET;
}

static void clock_hm(Pc *pc, uint16_t sp)
{
    if (!text_gate(pc, sp, 0x1543))
        return;
    uint16_t ax = two_digits(gs_clock_min(pc));
    REG(AX) = ax;
    wr16(pc, DS, 0x7D6, ax);
    REG(SI) = 0x7D4;
    cyc += 12 + 85 + 6 + 6 + 12 + 4;
    call_orig(pc, sp, PRINT_STR_DIM, 0x1556);
    ax = two_digits(gs_clock_hour(pc));
    REG(AX) = ax;
    wr16(pc, DS, 0x7D1, ax);
    REG(SI) = 0x7CF;
    cyc += 12 + 85 + 6 + 6 + 12 + 4;
    call_orig(pc, sp, PRINT_STR_DIM, 0x1569);
    cyc += CYC_CALL;
    uint16_t s = sp;
    vpush(pc, &s, 0x156C);
    light_level(pc);
    cyc += CYC_RET;
}

static void clock_text(Pc *pc)
{
    uint16_t sp = REG(SP);
    if (!text_gate(pc, sp, 0x151F))
        return;
    bool on = rd8(pc, DS, 0x550) != 0;
    cyc += 14;
    JCC(!on);
    if (!on) {
        cyc += CYC_RET;
        return;
    }
    uint16_t ax = two_digits(gs_clock_sec(pc));
    REG(AX) = ax;
    wr16(pc, DS, 0x7DB, ax);
    REG(SI) = 0x7D9;
    cyc += 12 + 85 + 6 + 6 + 12 + 4;
    call_orig(pc, sp, PRINT_STR_DIM, 0x1539);
    bool sec = gs_clock_sec(pc) != 0;
    cyc += 14;
    JCC(sec);
    if (sec) {
        cyc += CYC_RET;
        return;
    }
    clock_hm(pc, sp);
}

/* ---- 0050:1577: prints record 07C5 dimmed (behind panel_text_gate) ------------------ */
static void text_07c5(Pc *pc)
{
    uint16_t sp = REG(SP);
    if (!text_gate(pc, sp, 0x157A))
        return;
    REG(SI) = 0x7C5;
    cyc += 4;
    call_orig(pc, sp, PRINT_STR_DIM, 0x1580);
    cyc += CYC_RET;
}

/* ---- switch_text 0050:1582 ----------------------------------------------------------
 * The switch labels: gear (07DE / 07E3), lights (07ED / 07E8), magnetos (07F2.. by
 * [0583]) and carburettor heat (0815 / 0810). Ends with JMP print_str. */
static bool switch_text(Pc *pc)
{
    uint16_t sp = REG(SP);
    if (!text_gate(pc, sp, 0x1585))
        return false;
    bool gear = gs_gear_down(pc) != 0;
    REG(SI) = gear ? 0x7E3 : 0x7DE;
    cyc += 4 + 19;
    JCC(!gear);
    if (gear)
        cyc += 4;
    call_orig(pc, sp, PRINT_STR, 0x1595);
    bool lights = gs_lights(pc) != 0;
    REG(SI) = lights ? 0x7E8 : 0x7ED;
    cyc += 4 + 19;
    JCC(lights);
    if (!lights)
        cyc += 4;
    call_orig(pc, sp, PRINT_STR, 0x15A5);
    static const uint16_t mag[6] = { 0x7F2, 0x7F7, 0x7FC, 0x801, 0x806, 0x80B };
    uint8_t al = gs_magnetos(pc);
    uint16_t si = mag[0];
    cyc += 12 + 4;
    int i = 0;
    for (;;) {
        al--;
        cyc += 5;
        JCC(al & 0x80);
        if (al & 0x80)
            break;
        si = mag[++i];
        cyc += 4;
        if (i == 5)
            break;
    }
    SET_LO(AX, al);
    REG(SI) = si;
    call_orig(pc, sp, PRINT_STR, 0x15D1);
    bool carb = gs_carb_heat(pc) != 0;
    REG(SI) = carb ? 0x810 : 0x815;
    cyc += 4 + 19;
    JCC(!carb);
    if (carb)
        cyc += 4;
    jmp_orig(pc, PRINT_STR);
    return true;
}

/* ---- panel_update_mask 0050:21F0 ----------------------------------------------------
 * panel_mask = [208B] & [19A0] & [19A6], and the byte [19A5] likewise from [208D],
 * [19A2], [19A8]. On a change, sub_2231 walks the 16 bits: an instrument whose bit went
 * off is cleared (5AD7 with the 6-byte record at CX); one that came on is restored from
 * the view buffer (5AF3) and redrawn by the routine at record +4 with [1400] = 4. */

/* 0050:2231. sp points at its return address. */
static void mask_apply(Pc *pc, uint16_t sp)
{
    uint16_t ax = REG(AX), bx = REG(BX), cx = REG(CX), dx = 1;
    cyc += 4;
    for (;;) {
        uint16_t si = (uint16_t)((bx ^ ax) & dx);
        REG(SI) = si;
        cyc += 4 + 5 + 5;
        JCC(si == 0);
        if (si) {
            uint16_t s = sp;
            vpush(pc, &s, ax);
            vpush(pc, &s, dx);
            vpush(pc, &s, bx);
            vpush(pc, &s, cx);
            REG(BX) = cx;
            REG(AX) = ax & dx;
            cyc += 15 * 4 + 4 + 5;
            JCC((ax & dx) == 0);
            if ((ax & dx) == 0) {
                call_orig(pc, s, RECT_CLEAR, 0x225E);
            } else {
                call_orig(pc, s, RECT_RESTORE, 0x2249);
                uint16_t rec = cx;
                REG(BX) = rec;
                uint16_t s2 = s;
                vpush(pc, &s2, SREG(ES));
                vpush(pc, &s2, DS);
                uint16_t es = SREG(ES), ds = DS;
                REG(CX) = rd16(pc, ds, (uint16_t)(rec + 4));
                gs_set_indicator_force(pc, 4);
                cyc += 12 + 15 + 16 + 16 + 17 + 19 + 2;
                call_orig(pc, s2, REG(CX), 0x2257);
                SREG(DS) = ds;
                SREG(ES) = es;
                cyc += 14 + 14 + 17;
            }
            REG(CX) = cx;
            REG(BX) = bx;
            REG(DX) = dx;
            REG(AX) = ax;
            cyc += 12 * 4;
        }
        cx = (uint16_t)(cx + 6);
        bool carry = dx & 0x8000;
        dx = (uint16_t)(dx << 1);
        cyc += 6 + 8;
        JCC(!carry);
        if (carry)
            break;
    }
    REG(CX) = cx;
    REG(DX) = dx;
    cyc += CYC_RET;
}

static void panel_update_mask(Pc *pc)
{
    uint16_t sp = REG(SP);
    uint16_t ds = DS;
    uint16_t ax = (uint16_t)(rd16(pc, ds, 0x208B) & rd16(pc, ds, 0x19A0) & rd16(pc, ds, 0x19A6));
    REG(AX) = ax;
    cyc += 12 + 18 + 18 + 18;
    bool same = ax == gs_panel_mask(pc);
    JCC(same);
    if (!same) {
        REG(BX) = gs_panel_mask(pc);
        REG(CX) = 0x19A9;
        gs_set_panel_mask(pc, ax);
        cyc += 17 + 4 + 12 + CYC_CALL;
        uint16_t s = sp;
        vpush(pc, &s, 0x220E);
        mask_apply(pc, s);
        ds = DS;
    }
    uint8_t al = (uint8_t)(rd8(pc, ds, 0x208D) & rd8(pc, ds, 0x19A2) & rd8(pc, ds, 0x19A8));
    SET_LO(AX, al);
    cyc += 12 + 18 + 18 + 18;
    same = al == rd8(pc, ds, 0x19A5);
    JCC(same);
    if (!same) {
        REG(BX) = rd8(pc, ds, 0x19A5);
        REG(CX) = 0x1A09;
        SET_HI(AX, 0);
        wr8(pc, ds, 0x19A5, al);
        cyc += 17 + 4 + 5 + 4 + 12 + CYC_CALL;
        uint16_t s = sp;
        vpush(pc, &s, 0x2230);
        mask_apply(pc, s);
    }
    cyc += CYC_RET;
}

/* ---- OBI display 0050:240E / 2419 ---------------------------------------------------
 * Animates the OBI course needle [1A10] one step towards [1A11] (computed by the radio
 * code, sub_2290) and the glide-slope needle [1A12] towards [1A13], shows the TO/FROM
 * flag [1A14] (text records 0825 / 081A / 081F) and the marker lights (sub_25D4).
 * 240E does nothing while the course needle is in place; 2419 always runs. The area is
 * first restored from the view buffer (13 rows of 5 words at 133A in both banks). */

/* 0050:24F4: copies the OBI area from the view buffer [3806] to the screen [03C2]. */
static void obi_restore(Pc *pc, uint16_t sp)
{
    uint16_t ds0 = DS;
    vpush(pc, &sp, ds0);
    uint16_t es = rd16(pc, ds0, 0x3C2), src = rd16(pc, ds0, 0x3806);
    int16_t d = (pc->cpu.flags & F_DF) ? -2 : 2;
    uint16_t si = 0x133A, di = 0x133A;
    SREG(ES) = es;
    cyc += 16 + 11 + 11 + 4 + 4 + 4;
    for (int row = 13; row > 0; row--) {
        for (int half = 0; half < 2; half++) {
            for (int k = 0; k < 5; k++) {
                wr16(pc, es, di, rd16(pc, src, si));
                si = (uint16_t)(si + d);
                di = (uint16_t)(di + d);
            }
            uint16_t step = half ? 0xE046 : 0x1FF6;
            si = (uint16_t)(si + step);
            di = (uint16_t)(di + step);
            cyc += 4 + 2 + 5 * 17 + 6 + 6;
        }
        cyc += 2;
        JCC(row != 1);
    }
    REG(CX) = 0;
    REG(SI) = si;
    REG(DI) = di;
    REG(BP) = 0;
    cyc += 14 + CYC_RET;
}

/* 0050:24D6: the TO/FROM text by [1A14] (0 -> 0825, 1 -> 081A, else 081F), JMP print_str. */
static void obi_flag_text(Pc *pc, uint16_t sp)
{
    uint8_t al = gs_nav_flag(pc);
    uint16_t si;
    cyc += 12;
    al--;
    cyc += 5;
    JCC(!(al & 0x80));
    if (al & 0x80) {
        si = 0x825;
    } else {
        al--;
        cyc += 5;
        JCC(!(al & 0x80));
        si = (al & 0x80) ? 0x81A : 0x81F;
    }
    SET_LO(AX, al);
    REG(SI) = si;
    cyc += 4 + 17;
    run_orig_at(pc, sp, PRINT_STR, 0x2440);
}

/* 0050:2474: ORs the needle column for position BL: record DS:1A25 + 3*BL (+0 screen
 * offset, +2 bits 0-2 pattern index into DS:1B04, bits 3-7 rows). */
static void obi_needle(Pc *pc)
{
    uint16_t ds = DS;
    uint16_t es = gs_screen_seg(pc);
    uint8_t bl = lo8(REG(BX));
    uint16_t rec = (uint16_t)((uint8_t)(bl * 3) + 0x1A25);
    uint8_t cl = rd8(pc, ds, (uint16_t)(rec + 2));
    uint16_t si = (uint16_t)((cl & 7) << 1);
    uint16_t ax = rd16(pc, ds, (uint16_t)(si + 0x1B04));
    uint16_t bx = rd16(pc, ds, rec);
    cl >>= 3;
    SREG(ES) = es;
    cyc += 11 + 4 + 8 + 5 + 5 + 4 + 5 + 17 + 4 + 6 + 8 + 17 + 17 + 8 + 8 + 8 + 6;
    bool odd = (int16_t)bx > 0x1FFE;
    JCC(odd);
    for (;;) {
        if (!odd) {
            wr16(pc, es, bx, rd16(pc, es, bx) | ax);
            cl--;
            cyc += 18 + 5;
            JCC(cl == 0);
            if (cl == 0)
                break;
            bx = (uint16_t)(bx + 0x1FB0);
            cyc += 6;
        }
        odd = false;
        wr16(pc, es, bx, rd16(pc, es, bx) | ax);
        bx = (uint16_t)(bx + 0xE000);
        cl--;
        cyc += 18 + 6 + 5;
        JCC(cl != 0);
        if (cl == 0)
            break;
    }
    REG(AX) = ax;
    REG(BX) = bx;
    REG(CX) = (uint16_t)(0x1A00 | cl);
    REG(SI) = si;
    cyc += CYC_RET;
}

/* 0050:24B9: the glide-slope mark for position BL: 5555h in 2 words at
 * row_offsets[BL + 7Ah] + 2Ch. */
static void obi_glideslope(Pc *pc)
{
    uint16_t ds = DS;
    uint16_t bx = (uint16_t)((uint8_t)(lo8(REG(BX)) + 0x7A) << 1);
    bx = (uint16_t)(rd16(pc, ds, (uint16_t)(bx + 0x380C)) + 0x2C);
    uint16_t es = gs_screen_seg(pc);
    wr16(pc, es, bx, 0x5555);
    wr16(pc, es, (uint16_t)(bx + 2), 0x5555);
    REG(AX) = 0x5555;
    REG(BX) = bx;
    SREG(ES) = es;
    cyc += 5 + 6 + 8 + 17 + 6 + 4 + 11 + 18 + 18 + CYC_RET;
}

/* 0050:25D4: marker lights. Shifts bit 0 out of [1A17], [1A1B], [1A19] into a 3-bit
 * state; when it differs from [1A16], record 082A is printed with one character per light
 * (79h off, 7Ah / 7Bh / 7Ch on). */
static void marker_lights(Pc *pc, uint16_t sp)
{
    uint16_t ds = DS;
    static const uint16_t bits[3] = { 0x1A17, 0x1A1B, 0x1A19 };
    uint8_t al = 0;
    cyc += 5;
    for (int i = 0; i < 3; i++) {
        uint8_t v = rd8(pc, ds, bits[i]);
        wr8(pc, ds, bits[i], (uint8_t)(v >> 1));
        al = (uint8_t)(al << 1 | (v & 1));
        cyc += 15 + 8;
    }
    uint8_t dl = al;
    uint8_t old = gs_marker_state(pc);
    gs_set_marker_state(pc, al);
    al = old;
    cyc += 4 + 26 + 5;
    JCC(al == dl);
    if (al != dl) {
        for (int i = 0; i < 3; i++) {
            al = 0x79;
            bool cf = dl & 1;
            dl >>= 1;
            cyc += 4 + 8;
            JCC(!cf);
            if (cf) {
                al = (uint8_t)(al + i + 1);
                cyc += i ? 6 : 5; /* inc al / add al,n */
            }
            wr8(pc, ds, (uint16_t)(0x82C + i), al);
            cyc += 12;
        }
        SET_LO(AX, al);
        SET_LO(DX, dl);
        REG(SI) = 0x82A;
        cyc += 4;
        call_orig(pc, sp, PRINT_STR, 0x2619);
    } else {
        SET_LO(AX, al);
        SET_LO(DX, dl);
    }
    cyc += CYC_RET;
}

/* One step of a needle position towards its target (2440 / 2458). */
static uint8_t obi_step(Pc *pc, uint16_t pos, uint16_t target)
{
    uint8_t bl = rd8(pc, DS, pos), t = rd8(pc, DS, target);
    cyc += 17 + 18;
    JCC(bl == t);
    if (bl != t) {
        bool sf = (uint8_t)(bl - t) & 0x80;
        JCC(!sf);
        if (sf) {
            bl = (uint8_t)(bl + 2);
            cyc += 6;
        }
        bl--;
        cyc += 5;
    }
    wr8(pc, DS, pos, bl);
    cyc += 18;
    return bl;
}

static void obi_display(Pc *pc)
{
    uint16_t sp = REG(SP);
    uint8_t ah = (uint8_t)(gs_loc_target(pc) - gs_loc_needle(pc));
    uint8_t al = gs_nav_flag(pc);
    uint8_t bh = al;
    uint8_t old = gs_nav_flag_drawn(pc);
    gs_set_nav_flag_drawn(pc, al);
    al = old;
    bh = (uint8_t)(bh - al);
    uint8_t ch = (uint8_t)(gs_gs_target(pc) - gs_gs_needle(pc));
    ah |= bh;
    ah |= ch;
    REG(AX) = (uint16_t)(al | ah << 8);
    SET_HI(BX, bh);
    SET_HI(CX, ch);
    cyc += 17 + 18 + 12 + 4 + 26 + 5 + 17 + 18 + 5 + 5;
    JCC(ah == 0);
    if (ah) {
        uint16_t s = sp;
        cyc += CYC_CALL;
        vpush(pc, &s, 0x243D);
        obi_restore(pc, s);
        cyc += CYC_CALL;
        obi_flag_text(pc, sp);
        SET_LO(BX, obi_step(pc, 0x1A10, 0x1A11));
        cyc += CYC_CALL;
        s = sp;
        vpush(pc, &s, 0x2458);
        obi_needle(pc);
        SET_LO(BX, obi_step(pc, 0x1A12, 0x1A13));
        cyc += CYC_CALL;
        s = sp;
        vpush(pc, &s, 0x2470);
        obi_glideslope(pc);
    }
    cyc += CYC_CALL;
    uint16_t s = sp;
    vpush(pc, &s, 0x2473);
    marker_lights(pc, s);
    cyc += CYC_RET;
}

static void obi_check(Pc *pc)
{
    uint8_t ah = (uint8_t)(gs_loc_target(pc) - gs_loc_needle(pc));
    SET_HI(AX, ah);
    cyc += 17 + 18;
    JCC(ah != 0);
    if (!ah) {
        cyc += CYC_RET;
        return;
    }
    obi_display(pc);
}

/* ---- attitude indicator 0050:2620 / 262C --------------------------------------------
 * The artificial horizon on the panel: rows 76h..98h of the screen, one 8-byte record per
 * row at DS:1BAB + (row - 76h) * 8:
 *   +1 / +2  left edge column, right edge column of the instrument on this row
 *   +3 / +4  drawn / new colour of the part left of the horizon (0 or 2, an index into the
 *            two-word colour pairs at DS:1CEB)
 *   +5 / +6  drawn / new horizon column on this row
 * 262C computes the horizon line from view_bank and view_pitch (end points from the table
 * DS:1B29, pitch through DS:1CCB), updates the records, redraws only the spans of each row
 * that changed (sub_293F), puts back the fixed aircraft symbol, and moves the bank pointer
 * dots (XOR, table DS:1D31 of angles, dot positions DS:1CEF) when view_bank changed.
 * 2620 runs it on the even phases only while |roll_rate| > 500h. */

/* Registers of the routine, kept as locals while it runs. */
typedef struct AiRegs {
    uint16_t ax, bx, cx, dx, si, di, bp;
} AiRegs;

#define L8(r) ((uint8_t)(r))
#define H8(r) ((uint8_t)((r) >> 8))
#define SETL(r, v) ((r) = (uint16_t)(((r) & 0xFF00) | (uint8_t)(v)))
#define SETH(r, v) ((r) = (uint16_t)(((r) & 0x00FF) | (uint16_t)((uint8_t)(v) << 8)))

/* Row record address of the row in BL (sub bl,76h / xor bh,bh / shl bx,1 x3 / add). */
static uint16_t ai_rec(uint8_t row)
{
    cyc += 6 + 5 + 8 + 8 + 8 + 6;
    return (uint16_t)(0x1BAB + (uint8_t)(row - 0x76) * 8);
}

/* 0050:5B88: XORs pixel pattern [3A00 + 2*(CX & 7)] into ES:row_offsets[BL] + CX/8. */
static void xor_dot(Pc *pc, AiRegs *r)
{
    uint16_t ds = DS;
    uint16_t bx = (uint16_t)(L8(r->bx) << 1);
    uint16_t dx = rd16(pc, ds, (uint16_t)(bx + 0x380C));
    uint8_t cl = L8(r->cx);
    r->cx >>= 3;
    dx = (uint16_t)(dx + r->cx);
    bx = (uint16_t)((cl & 7) << 1);
    r->ax = rd16(pc, ds, (uint16_t)(bx + 0x3A00));
    r->bx = dx;
    r->dx = dx;
    wr16(pc, SREG(ES), dx, rd16(pc, SREG(ES), dx) ^ r->ax);
    cyc += 136;
}

/* 0050:2A9C: CX = [BX + 1CEF]; BL = CH; CH = 0. */
static void bank_dot_pos(Pc *pc, AiRegs *r)
{
    r->cx = rd16(pc, DS, (uint16_t)(r->bx + 0x1CEF));
    SETL(r->bx, H8(r->cx));
    SETH(r->cx, 0);
    cyc += 17 + 4 + 5 + CYC_RET;
}

/* 0050:2A56: the dot for angle AL (0..7Fh, 4 quadrants mirrored from the table). */
static void bank_dot(Pc *pc, AiRegs *r, uint16_t sp)
{
    uint8_t al = L8(r->ax);
    uint16_t s = sp;
    r->bx = (uint16_t)(al << 1);
    cyc += 4 + 5 + 8 + 6;
    JCC(al < 0x61);
    if (al >= 0x61) {
        r->bx = (uint16_t)(0x100 - r->bx);
        cyc += 5 + 6 + CYC_CALL;
        vpush(pc, &s, 0x2A69);
        bank_dot_pos(pc, r);
        r->bx = (uint16_t)-r->bx;
        r->cx = (uint16_t)-r->cx;
        cyc += 5 + 17 + 5 + 17;
    } else {
        cyc += 6;
        JCC(al < 0x41);
        if (al >= 0x41) {
            r->bx = (uint16_t)(r->bx - 0x80);
            cyc += 6 + CYC_CALL;
            vpush(pc, &s, 0x2A78);
            bank_dot_pos(pc, r);
            r->cx = (uint16_t)-r->cx;
            cyc += 5 + 17;
        } else {
            cyc += 6;
            JCC(al < 0x21);
            if (al >= 0x21) {
                r->bx = (uint16_t)(0x80 - r->bx);
                cyc += 5 + 6 + CYC_CALL;
                vpush(pc, &s, 0x2A89);
                bank_dot_pos(pc, r);
                cyc += 17;
            } else {
                cyc += CYC_CALL;
                vpush(pc, &s, 0x2A8E);
                bank_dot_pos(pc, r);
                r->bx = (uint16_t)-r->bx;
                cyc += 5;
            }
        }
    }
    r->cx = (uint16_t)(r->cx + 0xA0);
    r->bx = (uint16_t)(r->bx + 0x87);
    cyc += 6 + 6 + CYC_CALL;
    s = sp;
    vpush(pc, &s, 0x2A9B);
    xor_dot(pc, r);
    cyc += CYC_RET;
}

/* 0050:2A3E: XORs the bank pointer dots for bank position AL: one per angle offset in the
 * list at DS:1D31 (ends with a negative byte). */
static void bank_pointer(Pc *pc, AiRegs *r, uint16_t sp)
{
    r->bx = 0;
    cyc += 5;
    for (;;) {
        uint8_t ah = rd8(pc, DS, (uint16_t)(r->bx + 0x1D31));
        SETH(r->ax, ah);
        cyc += 17 + 5;
        JCC(ah & 0x80);
        if (ah & 0x80)
            break;
        uint16_t s = sp;
        vpush(pc, &s, r->bx);
        vpush(pc, &s, r->ax);
        uint16_t bx0 = r->bx, ax0 = r->ax;
        SETL(r->ax, (L8(r->ax) + ah) & 0x7F);
        cyc += 15 + 15 + 5 + 6 + CYC_CALL;
        vpush(pc, &s, 0x2A51);
        bank_dot(pc, r, s);
        r->ax = ax0;
        r->bx = (uint16_t)(bx0 + 1);
        cyc += 12 + 12 + 2 + 17;
    }
    cyc += CYC_RET;
}

/* 0050:293F: fills the span DH..DL (pixel columns, two per byte) of row CL with the colour
 * pair BL (DS:1CEB + BL: the word is saved at [1B24]; its F00F parts colour the partial
 * bytes at the ends). sp points at its return address. */
static void ai_span(Pc *pc, AiRegs *r, uint16_t sp)
{
    uint16_t ds = DS, es = SREG(ES);
    uint8_t dh = H8(r->dx), dl = L8(r->dx);
    cyc += 5;
    JCC(dh > dl);
    if (dh > dl) {
        cyc += CYC_RET;
        return;
    }
    SETH(r->bx, 0);
    r->ax = rd16(pc, ds, (uint16_t)(r->bx + 0x1CEB));
    wr16(pc, ds, 0x1B24, r->ax);
    r->ax &= 0xF00F;
    r->bx = (uint16_t)(dh >> 1);
    r->di = r->bx;
    r->bx = (uint16_t)(L8(r->cx) << 1);
    r->di = (uint16_t)(r->di + rd16(pc, ds, (uint16_t)(r->bx + 0x380C)));
    dl = (uint8_t)(dl - dh);
    bool odd = dh & 1;
    dh >>= 1;
    cyc += 5 + 17 + 12 + 6 + 4 + 8 + 4 + 4 + 8 + 18 + 5 + 8;
    JCC(odd);
    bool fill = true;
    if (odd) {
        uint8_t b = (uint8_t)((rd8(pc, es, r->di) & 0xF0) | L8(r->ax));
        SETL(r->bx, b);
        wr8(pc, es, r->di, b);
        r->di++;
        dl--;
        cyc += 17 + 6 + 5 + 18 + 2 + 5;
    } else {
        cyc += 5;
        JCC((int8_t)dl > 0);
        if ((int8_t)dl <= 0) {
            dl++;
            fill = false;
            cyc += 5 + 17;
        }
    }
    if (fill) {
        uint16_t s = sp;
        vpush(pc, &s, r->ax);
        uint16_t ax0 = r->ax;
        r->ax = rd16(pc, ds, 0x1B24);
        dl++;
        r->cx = (uint16_t)(dl >> 2);
        int16_t d = (pc->cpu.flags & F_DF) ? -1 : 1;
        cyc += 15 + 12 + 5 + 5 + 4 + 8 + 8 + 5;
        JCC(r->cx == 0);
        if (r->cx) {
            cyc += 2 + 10 * r->cx;
            for (; r->cx; r->cx--) {
                wr16(pc, es, r->di, r->ax);
                r->di = (uint16_t)(r->di + 2 * d);
            }
        }
        r->cx = (uint16_t)((dl >> 1) & 1);
        cyc += 4 + 8 + 6;
        JCC(r->cx == 0);
        if (r->cx) {
            cyc += 2 + 10;
            wr8(pc, es, r->di, L8(r->ax));
            r->di = (uint16_t)(r->di + d);
            r->cx = 0;
        }
        r->ax = ax0;
        cyc += 12;
    }
    bool cf = dl & 1;
    dl = (uint8_t)((int8_t)dl >> 1);
    cyc += 8;
    JCC(!cf);
    if (cf) {
        uint8_t b = (uint8_t)((rd8(pc, es, r->di) & 0x0F) | H8(r->ax));
        SETL(r->bx, b);
        wr8(pc, es, r->di, b);
        cyc += 17 + 6 + 5 + 18;
    }
    r->dx = (uint16_t)(dh << 8 | dl);
    cyc += CYC_RET;
}

/* Moves the left-colour flags (+3 = old +4, +4 = DH) of AL... rows: the loops at
 * 2755/2767/27A7/27B9/27D2/27FB/2833/2849. */
static void ai_flag_row(Pc *pc, AiRegs *r)
{
    uint16_t ds = DS;
    uint8_t old = rd8(pc, ds, (uint16_t)(r->bx + 4));
    wr8(pc, ds, (uint16_t)(r->bx + 4), H8(r->dx));
    SETL(r->dx, old);
    wr8(pc, ds, (uint16_t)(r->bx + 3), old);
    r->bx = (uint16_t)(r->bx + 8);
    cyc += 4 + 26 + 18 + 6;
}

/* Moves the horizon column of a row (+5 = old +6, +6 = v). */
static uint8_t ai_col_row(Pc *pc, uint16_t bx, uint8_t v)
{
    uint16_t ds = DS;
    uint8_t old = rd8(pc, ds, (uint16_t)(bx + 6));
    wr8(pc, ds, (uint16_t)(bx + 6), v);
    wr8(pc, ds, (uint16_t)(bx + 5), old);
    cyc += 26 + 18;
    return old;
}

/* Horizon end point from the 2-byte table DS:1B29 at index (v & FE) >> 1 (odd halves are
 * the average of two neighbours, low byte only, with RCR). sar/shl costs included. */
static uint16_t ai_endpoint(Pc *pc, uint16_t v, uint16_t *ax)
{
    uint16_t ds = DS;
    v &= 0xFE;
    bool half = v & 2;
    uint16_t i = (uint16_t)((v >> 2) << 1);
    cyc += 6 + 8 + 8;
    JCC(!half);
    if (half) {
        uint16_t w = rd16(pc, ds, (uint16_t)(i + 0x1B29));
        uint16_t sum = (uint16_t)(L8(w) + rd8(pc, ds, (uint16_t)(i + 0x1B2B)));
        w = (uint16_t)((w & 0xFF00) | (uint8_t)(sum >> 1));
        *ax = w;
        cyc += 8 + 17 + 18 + 8 + 4 + 17;
        return w;
    }
    cyc += 8 + 17;
    return rd16(pc, ds, (uint16_t)(i + 0x1B29));
}

static void attitude(Pc *pc)
{
    uint16_t ds = DS, sp = REG(SP);
    AiRegs r = { REG(AX), REG(BX), REG(CX), REG(DX), REG(SI), REG(DI), REG(BP) };

    /* 262C: end points of the horizon line */
    SETL(r.bx, (uint8_t)-rd8(pc, ds, (uint16_t)(GS_VIEW_BANK + 1)));
    r.si = r.bx;
    r.ax = (uint16_t)((uint16_t)gs_view_pitch(pc) << 1);
    SETL(r.ax, H8(r.ax));
    SETL(r.ax, (uint8_t)-(uint8_t)(L8(r.ax) + 0xFA));
    r.ax = (uint16_t)(int16_t)(int8_t)L8(r.ax);
    r.bx = r.ax;
    cyc += 17 + 5 + 4 + 12 + 8 + 4 + 6 + 5 + 2 + 4 + 5;
    bool neg = r.bx & 0x8000;
    JCC(neg);
    if (!neg) {
        cyc += 6;
        JCC((int8_t)L8(r.bx) <= 0x1F);
        if ((int8_t)L8(r.bx) > 0x1F) {
            SETL(r.bx, 0x1F);
            cyc += 4;
        }
        SETL(r.bx, rd8(pc, ds, (uint16_t)(r.bx + 0x1CCB)));
        cyc += 17 + 17;
    } else {
        cyc += 6;
        JCC((int8_t)L8(r.bx) >= -0x1F);
        if ((int8_t)L8(r.bx) < -0x1F) {
            SETL(r.bx, 0xE1);
            cyc += 4;
        }
        r.bx = (uint16_t)-r.bx;
        SETL(r.bx, (uint8_t)-rd8(pc, ds, (uint16_t)(r.bx + 0x1CCB)));
        cyc += 5 + 17 + 5;
    }
    r.si = (uint16_t)(r.si - r.bx);
    r.bx = (uint16_t)((r.bx << 1) + r.si);
    cyc += 5 + 8 + 5;
    r.si = ai_endpoint(pc, r.si, &r.ax);
    r.bx = (uint16_t)(r.bx + 0x81);
    cyc += 6;
    r.di = ai_endpoint(pc, r.bx, &r.ax);
    r.bx = (uint16_t)(((r.bx & 0xFE) >> 2) << 1);

    /* 26AD: rows outside the line's row range get their column moved */
    r.ax = 0x9876;
    gs_set_ai_rows(pc, r.ax);
    r.dx = r.si;
    r.cx = r.di;
    cyc += 4 + 12 + 4 + 4 + 5;
    JCC(L8(r.dx) < L8(r.cx));
    if (L8(r.dx) >= L8(r.cx)) {
        uint8_t t = L8(r.dx);
        SETL(r.dx, L8(r.cx));
        SETL(r.cx, t);
        cyc += 6;
    }
    cyc += 5;
    JCC(L8(r.ax) == L8(r.dx));
    if (L8(r.ax) != L8(r.dx)) {
        SETL(r.bx, L8(r.ax));
        r.bx = ai_rec(L8(r.ax)) /* + 4 for mov bl,al */;
        cyc += 4;
        do {
            uint8_t ch = rd8(pc, ds, (uint16_t)(r.bx + 2));
            SETH(r.cx, ai_col_row(pc, r.bx, ch));
            r.bx = (uint16_t)(r.bx + 8);
            SETL(r.ax, L8(r.ax) + 1);
            cyc += 17 + 6 + 5 + 5;
            JCC(L8(r.ax) != L8(r.dx));
        } while (L8(r.ax) != L8(r.dx));
    }
    cyc += 5;
    JCC(H8(r.ax) == L8(r.cx));
    if (H8(r.ax) != L8(r.cx)) {
        r.bx = ai_rec(H8(r.ax));
        cyc += 4;
        do {
            uint8_t ch = rd8(pc, ds, (uint16_t)(r.bx + 2));
            SETH(r.cx, ai_col_row(pc, r.bx, ch));
            r.bx = (uint16_t)(r.bx - 8);
            SETH(r.ax, H8(r.ax) - 1);
            cyc += 17 + 6 + 5 + 5;
            JCC(H8(r.ax) != L8(r.cx));
        } while (H8(r.ax) != L8(r.cx));
    }

    /* 270B: the left-colour flags (sky / ground) of every row, by bank quadrant */
    SETH(r.dx, 0);
    SETL(r.dx, rd8(pc, ds, (uint16_t)(GS_VIEW_BANK + 1)));
    cyc += 5 + 17 + 5;
    JCC(!(L8(r.dx) & 0x80));
    if (L8(r.dx) & 0x80) {
        SETH(r.dx, 2);
        SETL(r.dx, L8(r.dx) - 0x80);
        cyc += 4 + 6;
    }
    r.ax = r.si;
    r.bx = r.di;
    SETH(r.ax, H8(r.ax) - 0x28);
    SETH(r.bx, H8(r.bx) - 0x28);
    SETL(r.dx, L8(r.dx) << 1);
    cyc += 4 + 4 + 6 + 6 + 8;
    bool sf = L8(r.dx) & 0x80;
    JCC(!sf);
    int path; /* 0: 272B, 1: 2779, 2: 2814 */
    if (sf) {
        cyc += 17;
        SETH(r.ax, H8(r.ax) ^ H8(r.bx));
        cyc += 5;
        JCC(H8(r.ax) & 0x80);
        if (H8(r.ax) & 0x80) {
            path = 2;
        } else {
            cyc += 17;
            path = 1;
        }
    } else {
        SETH(r.ax, H8(r.ax) ^ H8(r.bx));
        cyc += 5;
        JCC(!(H8(r.ax) & 0x80));
        path = (H8(r.ax) & 0x80) ? 0 : 1;
    }
    if (path == 0) {
        cyc += 5;
        JCC(L8(r.bx) >= L8(r.ax));
        if (L8(r.bx) < L8(r.ax)) {
            SETL(r.ax, L8(r.bx));
            cyc += 4;
        }
        r.bx = gs_ai_rows(pc);
        SETH(r.ax, H8(r.bx) - L8(r.ax));
        SETL(r.cx, L8(r.bx));
        cyc += 17 + 4 + 5 + 4;
        r.bx = ai_rec(L8(r.bx));
        SETL(r.ax, L8(r.ax) - L8(r.cx));
        cyc += 5;
        JCC(L8(r.ax) == 0);
        if (L8(r.ax)) {
            SETH(r.dx, H8(r.dx) ^ 2);
            cyc += 6;
            do {
                ai_flag_row(pc, &r);
                SETL(r.ax, L8(r.ax) - 1);
                cyc += 5;
                JCC(L8(r.ax) != 0);
            } while (L8(r.ax));
            SETH(r.dx, H8(r.dx) ^ 2);
            cyc += 6;
        }
        do {
            ai_flag_row(pc, &r);
            SETH(r.ax, H8(r.ax) - 1);
            cyc += 5;
            JCC(!(H8(r.ax) & 0x80));
        } while (!(H8(r.ax) & 0x80));
        cyc += 17;
    } else if (path == 1) {
        cyc += 5;
        JCC(!(H8(r.bx) & 0x80));
        if (H8(r.bx) & 0x80) {
            cyc += 5;
            JCC(L8(r.bx) >= L8(r.ax));
            if (L8(r.bx) < L8(r.ax)) {
                uint8_t t = L8(r.bx);
                SETL(r.bx, L8(r.ax));
                SETL(r.ax, t);
                cyc += 6;
            }
            SETH(r.cx, L8(r.bx));
            r.bx = gs_ai_rows(pc);
            SETH(r.ax, H8(r.bx) - H8(r.cx));
            SETH(r.cx, H8(r.cx) - L8(r.ax));
            SETL(r.ax, L8(r.ax) - L8(r.bx));
            cyc += 4 + 17 + 4 + 5 + 5 + 5;
            r.bx = ai_rec(L8(r.bx));
            cyc += 5;
            JCC(L8(r.ax) == 0);
            if (L8(r.ax)) {
                SETH(r.dx, H8(r.dx) ^ 2);
                cyc += 6;
                do {
                    ai_flag_row(pc, &r);
                    SETL(r.ax, L8(r.ax) - 1);
                    cyc += 5;
                    JCC(L8(r.ax) != 0);
                } while (L8(r.ax));
                SETH(r.dx, H8(r.dx) ^ 2);
                cyc += 6;
            }
            do {
                ai_flag_row(pc, &r);
                SETH(r.cx, H8(r.cx) - 1);
                cyc += 5;
                JCC(!(H8(r.cx) & 0x80));
            } while (!(H8(r.cx) & 0x80));
            SETH(r.dx, H8(r.dx) ^ 2);
            cyc += 6 + 5;
            JCC(H8(r.ax) != 0);
            if (H8(r.ax)) {
                do {
                    ai_flag_row(pc, &r);
                    SETH(r.ax, H8(r.ax) - 1);
                    cyc += 5;
                    JCC(H8(r.ax) != 0);
                } while (H8(r.ax));
            }
            cyc += 17;
        } else {
            r.bx = gs_ai_rows(pc);
            SETH(r.ax, H8(r.bx) - L8(r.bx));
            cyc += 17 + 4 + 5;
            r.bx = ai_rec(L8(r.bx));
            do {
                ai_flag_row(pc, &r);
                SETH(r.ax, H8(r.ax) - 1);
                cyc += 5;
                JCC(!(H8(r.ax) & 0x80));
            } while (!(H8(r.ax) & 0x80));
            cyc += 17;
        }
    } else {
        cyc += 5;
        JCC(L8(r.bx) < L8(r.ax));
        if (L8(r.bx) >= L8(r.ax)) {
            SETL(r.ax, L8(r.bx));
            cyc += 4;
        }
        r.bx = gs_ai_rows(pc);
        SETH(r.ax, H8(r.bx) - L8(r.ax));
        SETL(r.ax, L8(r.ax) - L8(r.bx));
        cyc += 17 + 4 + 5 + 5;
        r.bx = ai_rec(L8(r.bx));
        do {
            ai_flag_row(pc, &r);
            SETL(r.ax, L8(r.ax) - 1);
            cyc += 5;
            JCC(!(L8(r.ax) & 0x80));
        } while (!(L8(r.ax) & 0x80));
        SETH(r.dx, H8(r.dx) ^ 2);
        cyc += 6 + 6;
        JCC(L8(r.ax) == 0);
        if (L8(r.ax)) {
            do {
                ai_flag_row(pc, &r);
                SETH(r.ax, H8(r.ax) - 1);
                cyc += 5;
                JCC(!(H8(r.ax) & 0x80));
            } while (!(H8(r.ax) & 0x80));
        }
    }

    /* 2858: the horizon column of each row between the end points (8.8 steps) */
    r.ax = (uint16_t)L8(r.si);
    r.bx = (uint16_t)L8(r.di);
    cyc += 4 + 5 + 4 + 4 + 5;
    JCC(r.bx >= r.ax);
    if (r.bx < r.ax) {
        uint16_t t = r.bx;
        r.bx = r.ax;
        r.ax = t;
        t = r.di;
        r.di = r.si;
        r.si = t;
        cyc += 3 + 6;
    }
    r.bx = (uint16_t)(r.bx - r.ax);
    cyc += 5;
    JCC(r.bx == 0);
    if (r.bx) {
        r.bx++;
        r.dx = r.si;
        r.ax = r.di;
        SETL(r.ax, H8(r.ax));
        SETL(r.dx, H8(r.dx));
        SETH(r.ax, 0);
        SETH(r.dx, 0);
        r.ax = (uint16_t)(r.ax - r.dx);
        r.dx = (r.ax & 0x8000) ? 0xFFFF : 0;
        r.ax = (uint16_t)(L8(r.ax) << 8);
        int32_t n = (int32_t)((uint32_t)r.dx << 16 | r.ax);
        int32_t q = n / (int16_t)r.bx, m = n % (int16_t)r.bx;
        r.ax = (uint16_t)q;
        r.dx = (uint16_t)m;
        r.bx--;
        cyc += 2 + 4 + 4 + 4 + 4 + 5 + 4 + 5 + 5 + 4 + 5 + 167 + 2 + 17;
        /* 28A2 */
        SETL(r.dx, L8(r.bx));
        r.cx = (uint16_t)(r.si & 0xFF00);
        cyc += 4 + 4 + 5 + 4;
        r.bx = ai_rec(L8(r.si));
        do {
            r.cx = (uint16_t)(r.cx + r.ax);
            SETH(r.dx, ai_col_row(pc, r.bx, H8(r.cx)));
            r.bx = (uint16_t)(r.bx + 8);
            SETL(r.dx, L8(r.dx) - 1);
            cyc += 5 + 4 + 6 + 5;
            JCC(!(L8(r.dx) & 0x80));
        } while (!(L8(r.dx) & 0x80));
    } else {
        r.bx = r.ax;
        cyc += 4;
        r.bx = ai_rec(L8(r.bx));
        uint8_t v = rd8(pc, ds, (uint16_t)(r.bx + 2));
        SETL(r.ax, ai_col_row(pc, r.bx, v));
        cyc += 17 + 17;
    }

    /* 28CA: redraw what changed, row by row */
    r.ax = 0xB800;
    SREG(ES) = 0xB800;
    r.cx = gs_ai_rows(pc);
    cyc += 4 + 4 + 17 + 4;
    r.bx = ai_rec(L8(r.cx));
    SETH(r.cx, H8(r.cx) - L8(r.cx));
    cyc += 5;
    for (;;) {
        r.si = r.bx;
        r.bp = r.cx;
        r.dx = rd16(pc, ds, (uint16_t)(r.bx + 5));
        SETL(r.ax, rd8(pc, ds, (uint16_t)(r.bx + 4)));
        bool same = L8(r.ax) == rd8(pc, ds, (uint16_t)(r.bx + 3));
        cyc += 4 + 4 + 17 + 17 + 18;
        JCC(!same);
        if (same) {
            uint8_t dh = H8(r.dx), dl = L8(r.dx);
            cyc += 5;
            JCC(dh == dl);
            if (dh != dl) {
                JCC(dh >= dl);
                if (dh < dl) {
                    SETL(r.ax, L8(r.ax) ^ 2);
                    dh++;
                    uint8_t t = dh;
                    dh = dl;
                    dl = t;
                    cyc += 6 + 5 + 6;
                }
                SETL(r.bx, L8(r.ax));
                r.dx = (uint16_t)(dl << 8 | dh); /* xchg dl,dh */
                cyc += 4 + 6 + CYC_CALL;
                uint16_t s = sp;
                vpush(pc, &s, 0x2908);
                ai_span(pc, &r, s);
            }
        } else {
            uint8_t dh = H8(r.dx), dl = L8(r.dx);
            cyc += 5;
            JCC(dh >= dl);
            if (dh < dl) {
                uint8_t t = dh;
                dh = dl;
                dl = t;
                cyc += 6;
            }
            r.ax = rd16(pc, ds, (uint16_t)(r.bx + 1));
            SETL(r.bx, rd8(pc, ds, (uint16_t)(r.bx + 4)));
            uint8_t t = dh; /* xchg dh,al */
            dh = L8(r.ax);
            SETL(r.ax, t);
            r.dx = (uint16_t)(dh << 8 | dl);
            cyc += 17 + 17 + 6;
            uint16_t s = sp;
            vpush(pc, &s, r.ax);
            vpush(pc, &s, r.bx);
            vpush(pc, &s, r.cx);
            uint16_t ax0 = r.ax, bx0 = r.bx, cx0 = r.cx;
            cyc += 15 + 15 + 15 + CYC_CALL;
            uint16_t s2 = s;
            vpush(pc, &s2, 0x292C);
            ai_span(pc, &r, s2);
            r.cx = cx0;
            r.bx = bx0;
            r.dx = ax0;
            SETL(r.bx, L8(r.bx) ^ 2);
            r.dx = (uint16_t)(L8(r.dx) << 8 | H8(r.dx)); /* xchg dl,dh */
            cyc += 12 + 12 + 12 + 6 + 6 + 5;
            JCC(H8(r.dx) == L8(r.dx));
            if (H8(r.dx) != L8(r.dx)) {
                SETH(r.dx, H8(r.dx) + 1);
                cyc += 5 + CYC_CALL;
                s2 = sp;
                vpush(pc, &s2, 0x293D);
                ai_span(pc, &r, s2);
                cyc += 17;
            }
        }
        r.bx = (uint16_t)(r.si + 8);
        r.cx = r.bp;
        SETL(r.cx, L8(r.cx) + 1);
        SETH(r.cx, H8(r.cx) - 1);
        cyc += 4 + 4 + 6 + 5 + 5;
        JCC(!(H8(r.cx) & 0x80));
        if (H8(r.cx) & 0x80)
            break;
    }
    cyc += 17;

    /* 29AA: the fixed aircraft symbol, ORed / ANDed back into the screen [03C2] */
    {
        uint16_t s = sp;
        vpush(pc, &s, ds);
        uint16_t seg = gs_screen_seg(pc);
        r.ax = 0xA80A;
        static const uint16_t or_ax[4] = { 0x3413, 0x14B3, 0x15A3, 0x35F3 };
        for (int i = 0; i < 4; i++)
            wr16(pc, seg, or_ax[i], rd16(pc, seg, or_ax[i]) | r.ax);
        wr16(pc, seg, 0x3501, rd16(pc, seg, 0x3501) | 0xFFFF);
        wr16(pc, seg, 0x3503, rd16(pc, seg, 0x3503) | 0xC081);
        wr16(pc, seg, 0x3505, rd16(pc, seg, 0x3505) | 0xFFFF);
        wr16(pc, seg, 0x3733, rd16(pc, seg, 0x3733) | 0xF807);
        wr16(pc, seg, 0x1783, 0x5735);
        wr16(pc, seg, 0x3783, 0x57D5);
        wr8(pc, seg, 0x3785, rd8(pc, seg, 0x3785) | 0x80);
        wr8(pc, seg, 0x17D2, rd8(pc, seg, 0x17D2) | 0x01);
        wr16(pc, seg, 0x17D3, 0x55D5);
        wr8(pc, seg, 0x17D5, 0x60);
        wr16(pc, seg, 0x1283, rd16(pc, seg, 0x1283) & 0x3FFE);
        wr16(pc, seg, 0x3283, rd16(pc, seg, 0x3283) & 0x1FFC);
        wr16(pc, seg, 0x12D3, rd16(pc, seg, 0x12D3) & 0x0FF8);
        wr16(pc, seg, 0x32D3, rd16(pc, seg, 0x32D3) & 0x07F0);
        cyc += 383;
    }

    /* 2A14: the bank pointer, when view_bank moved */
    SETL(r.ax, (uint8_t)(rd8(pc, ds, (uint16_t)(GS_VIEW_BANK + 1)) - 0x40) >> 1);
    bool drawn = rd8(pc, ds, 0x1B28) != 0;
    cyc += 12 + 6 + 8 + 19;
    JCC(!drawn);
    bool draw = true;
    if (drawn) {
        SETH(r.ax, L8(r.ax));
        SETL(r.ax, gs_bank_ptr_pos(pc));
        cyc += 4 + 12 + 5;
        JCC(L8(r.ax) == H8(r.ax));
        if (L8(r.ax) == H8(r.ax)) {
            draw = false;
        } else {
            uint16_t s = sp;
            vpush(pc, &s, r.ax);
            uint16_t ax0 = r.ax;
            cyc += 15 + CYC_CALL;
            vpush(pc, &s, 0x2A2F);
            bank_pointer(pc, &r, s); /* erase at the old position */
            r.ax = ax0;
            SETL(r.ax, H8(r.ax));
            cyc += 12 + 4;
        }
    }
    if (draw) {
        gs_set_bank_ptr_pos(pc, L8(r.ax));
        wr8(pc, ds, 0x1B28, 1);
        cyc += 12 + 19 + CYC_CALL;
        uint16_t s = sp;
        vpush(pc, &s, 0x2A3D);
        bank_pointer(pc, &r, s);
    }
    cyc += CYC_RET;

    REG(AX) = r.ax;
    REG(BX) = r.bx;
    REG(CX) = r.cx;
    REG(DX) = r.dx;
    REG(SI) = r.si;
    REG(DI) = r.di;
    REG(BP) = r.bp;
}

static void attitude_fast(Pc *pc)
{
    uint16_t ax = (uint16_t)((uint16_t)gs_roll_rate(pc) + 0x500);
    REG(AX) = ax;
    cyc += 12 + 6 + 6;
    JCC(ax > 0xA00);
    if (ax <= 0xA00) {
        cyc += CYC_RET;
        return;
    }
    attitude(pc);
}

/* ---- natives ------------------------------------------------------------------------ */

/* Charges what the original took beyond the entry's fixed .cycles. */
static void charge(Pc *pc, uint32_t fixed)
{
    pc->cpu.cycles += cyc;
    pc->cpu.cycles -= fixed;
}

/* .cycles of each entry: the original's cost on its most common path. */
#define CYC_GATE 50
#define CYC_SIMPLE_OFF 50 /* mask bit clear */
#define CYC_1431 365
#define CYC_TURN 207       /* unchanged */
#define CYC_FIELD_ELEV 85  /* slow, offset already equal */
#define CYC_SLEW_OFF 55
#define CYC_HEADING 600
#define CYC_CLOCK 1000
#define CYC_SWITCHES 2000
#define CYC_ALTIMETER 700
#define CYC_AIRSPEED 400
#define CYC_VSI 800
#define CYC_TACH 400
#define CYC_RPM_LAG 66     /* unchanged */
#define CYC_MASK 254       /* unchanged */
#define CYC_OBI_CHECK 59   /* needle in place */
#define CYC_OBI 400
#define CYC_ATTITUDE 3000
#define CYC_ATTITUDE_FAST 48 /* not rolling fast */

#define NATIVE(fname, body, fixed) \
    static void fname(Pc *pc)      \
    {                              \
        cyc = 0;                   \
        body;                      \
        charge(pc, fixed);         \
        native_ret(pc);            \
    }

NATIVE(n_gauge_upd_1401, gauge_upd_simple(pc, MASK_IND_A, 0x1DCE, 0x1401, 0x02E1), CYC_SIMPLE_OFF)
NATIVE(n_gauge_upd_140D, gauge_upd_simple(pc, MASK_IND_A, 0x1DD1, 0x140D, 0x02F8), CYC_SIMPLE_OFF)
NATIVE(n_gauge_upd_1419, gauge_upd_simple(pc, MASK_IND_B, 0x1DDA, 0x1419, 0x030F), CYC_SIMPLE_OFF)
NATIVE(n_gauge_upd_1425, gauge_upd_simple(pc, MASK_IND_B, 0x1DDC, 0x1425, 0x0326), CYC_SIMPLE_OFF)
NATIVE(n_gauge_upd_1431, gauge_upd_1431(pc), CYC_1431)
NATIVE(n_upd_turn, turn_needle(pc), CYC_TURN)
NATIVE(n_field_elev_follow, alt_setting(pc), CYC_FIELD_ELEV)
NATIVE(n_slew_readout, slew_readout(pc), CYC_SLEW_OFF)
NATIVE(n_heading_readout, heading_readout(pc), CYC_HEADING)
NATIVE(n_upd_airspeed, upd_airspeed(pc), CYC_AIRSPEED)
NATIVE(n_upd_vsi, upd_vsi(pc), CYC_VSI)
NATIVE(n_upd_tach, upd_tach(pc), CYC_TACH)
NATIVE(n_rpm_lag, rpm_lag(pc), CYC_RPM_LAG)
NATIVE(n_panel_update_mask, panel_update_mask(pc), CYC_MASK)
NATIVE(n_obi_check, obi_check(pc), CYC_OBI_CHECK)
NATIVE(n_obi_display, obi_display(pc), CYC_OBI)
NATIVE(n_clock_text, clock_text(pc), CYC_CLOCK)
NATIVE(n_clock_hm, clock_hm(pc, REG(SP)), CYC_CLOCK)
NATIVE(n_text_07c5, text_07c5(pc), CYC_CLOCK)
NATIVE(n_attitude, attitude(pc), CYC_ATTITUDE)
NATIVE(n_attitude_fast, attitude_fast(pc), CYC_ATTITUDE_FAST)

static void n_switch_text(Pc *pc)
{
    cyc = 0;
    bool jumped = switch_text(pc); /* JMP print_str has returned to our caller */
    charge(pc, CYC_SWITCHES);
    if (!jumped)
        native_ret(pc);
}

static void n_upd_altimeter(Pc *pc)
{
    if (altimeter_div_overflow(pc)) { /* the original raises INT 0; let it */
        cyc = run_original(pc);
        charge(pc, CYC_ALTIMETER);
        return;
    }
    cyc = 0;
    upd_altimeter(pc);
    charge(pc, CYC_ALTIMETER);
    native_ret(pc);
}

/* 0050:156D: with panel_mask bit 15 clear, POP AX drops our return address, so the RET
 * returns from the caller. Both paths cost 50 cycles. */
static void n_panel_text_gate(Pc *pc)
{
    if (!(panel_mask(pc) & MASK_TEXT))
        REG(AX) = cpu_pop(&pc->cpu);
    native_ret(pc);
}

NativeEntry native_gauges[] = {
    { .name = "gauge_upd_1401", .seg = GAME_CS, .off = 0x02CB, .fn = n_gauge_upd_1401, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_SIMPLE_OFF },
    { .name = "gauge_upd_140D", .seg = GAME_CS, .off = 0x02E2, .fn = n_gauge_upd_140D, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_SIMPLE_OFF },
    { .name = "gauge_upd_1419", .seg = GAME_CS, .off = 0x02F9, .fn = n_gauge_upd_1419, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_SIMPLE_OFF },
    { .name = "gauge_upd_1425", .seg = GAME_CS, .off = 0x0310, .fn = n_gauge_upd_1425, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_SIMPLE_OFF },
    { .name = "gauge_upd_1431", .seg = GAME_CS, .off = 0x0327, .fn = n_gauge_upd_1431, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_1431 },
    { .name = "upd_turn", .seg = GAME_CS, .off = 0x037E, .fn = n_upd_turn, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_TURN },
    { .name = "field_elev_follow", .seg = GAME_CS, .off = 0x0742, .fn = n_field_elev_follow, .enabled = true,
      .cycles = CYC_FIELD_ELEV },
    { .name = "slew_readout", .seg = GAME_CS, .off = 0x1150, .fn = n_slew_readout, .enabled = true,
      .cycles = CYC_SLEW_OFF },
    { .name = "heading_readout", .seg = GAME_CS, .off = 0x1375, .fn = n_heading_readout, .enabled = true,
      .cycles = CYC_HEADING },
    { .name = "clock_text", .seg = GAME_CS, .off = 0x151C, .fn = n_clock_text, .enabled = true,
      .cycles = CYC_CLOCK },
    { .name = "clock_hm", .seg = GAME_CS, .off = 0x1540, .fn = n_clock_hm, .enabled = true,
      .cycles = CYC_CLOCK },
    { .name = "panel_text_gate", .seg = GAME_CS, .off = 0x156D, .fn = n_panel_text_gate, .enabled = true,
      .cycles = CYC_GATE },
    { .name = "text_07c5", .seg = GAME_CS, .off = 0x1577, .fn = n_text_07c5, .enabled = true,
      .cycles = CYC_CLOCK },
    { .name = "switch_text", .seg = GAME_CS, .off = 0x1582, .fn = n_switch_text, .enabled = true,
      .cycles = CYC_SWITCHES },
    { .name = "upd_altimeter", .seg = GAME_CS, .off = 0x1FE0, .fn = n_upd_altimeter, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_ALTIMETER },
    { .name = "upd_airspeed", .seg = GAME_CS, .off = 0x2043, .fn = n_upd_airspeed, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_AIRSPEED },
    { .name = "upd_vsi", .seg = GAME_CS, .off = 0x207C, .fn = n_upd_vsi, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_VSI },
    { .name = "upd_tach", .seg = GAME_CS, .off = 0x20C0, .fn = n_upd_tach, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_TACH },
    { .name = "panel_update_mask", .seg = GAME_CS, .off = 0x21F0, .fn = n_panel_update_mask, .enabled = true,
      .cycles = CYC_MASK },
    { .name = "obi_check", .seg = GAME_CS, .off = 0x240E, .fn = n_obi_check, .enabled = true,
      .cycles = CYC_OBI_CHECK },
    { .name = "obi_display", .seg = GAME_CS, .off = 0x2419, .fn = n_obi_display, .enabled = true,
      .cycles = CYC_OBI },
    { .name = "attitude_fast", .seg = GAME_CS, .off = 0x2620, .fn = n_attitude_fast, .enabled = true,
      .cycles = CYC_ATTITUDE_FAST },
    { .name = "attitude", .seg = GAME_CS, .off = 0x262C, .fn = n_attitude, .enabled = true,
      .cycles = CYC_ATTITUDE },
    { .name = "rpm_lag", .seg = GAME_CS, .off = 0x2EEC, .fn = n_rpm_lag, .enabled = true,
      .flag_mask = F_IF, .cycles = CYC_RPM_LAG },
    { .name = NULL },
};
