/* Subphase 3.22: the last original routines, so that the C scheduler (sched.c) never has to run
 * original code. These are the start-up code after the loader, the editor's outer loop, and
 * a few routines of the frame, the timer and war mode that had no native:
 *
 *   start            5C9F  (blocks) cga_program_regs, hooks, menus, user mode, jmp 01F0
 *   start_tail       01F0  (blocks) store_ds_for_isr, select_scenery_area, then main_loop
 *   startup_menus    5CE9  (blocks) display menu, disk check, mode menu (key waits: 3.21)
 *   set_reloc_args   5CCD  store_ds_for_isr  0732  hook_int9 5DA6  hook_int8 5DC3
 *   sub_6254 6254 (INT 0 vector), sub_6265 6265 (IRQ2-7 vectors), panel_text_all 0790
 *   cga_program_regs 5600, cga_program_regs2 5619 (the editor's register table)
 *   editor_main      3640  (blocks) the editor loop
 *   editor_get_input 3703  (blocks) one field: the key dispatch after key_wait (3726)
 *   demo_step 110C, upd_obi 13C6, sub_04FB 04FB (+ sub_05D4, the reliability failures),
 *   clip_isect_p1 4A82, clip_isect_p2 4AAB, int0_divide_error 4DC6 (their IDIV overflow),
 *   war_span_clear 5AD7, war_span_setup 5B1D, war_next_line 5B76,
 *   slew_to_editor_pos 1CEC, engine_stop 2EDB,
 *   crash_body       0629  (blocks) crash_handler's message and reset (the delay is 3.21's)
 *   hang             0211, 631A  the original's JMP $ (timer not hooked; no disk)
 *
 * Routines that return are one native call each, with their callees run nested (an enabled
 * native, as the CPU would with the natives on). Code that waits for a key or a delay, or that
 * never returns, is split into blocks as main_loop is (3.21): one function bound at the entry
 * and at every return address; a block runs to the next CALL, which it performs (push the
 * return address, IP = callee), so the callee is the scheduler's next step and interrupts come
 * between the steps. Under --verify a block is checked against the original up to the same
 * point (stop ranges).
 *
 * Every register, the stack words and memory end as with the original; the flags are set where
 * a later step reads them. Cycles: the cpu8086.c cost of every original instruction executed
 * (startup_cycles.h, tools/gen_startup_cycles.py). */
#include <SDL3/SDL.h>

#include "native.h"
#include "startup_cycles.h"

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])
#define SET_LO(r, b) (REG(r) = (uint16_t)((REG(r) & 0xFF00) | (uint8_t)(b)))
#define SET_HI(r, b) (REG(r) = (uint16_t)((REG(r) & 0x00FF) | (uint16_t)((uint8_t)(b)) << 8))
#define LO(r) ((uint8_t)REG(r))
#define HI(r) ((uint8_t)(REG(r) >> 8))
#define ENTRY_CYCLES 1
#define STEP_CAP 5000000L

/* ---- cycles --------------------------------------------------------------------------- */

static const SuInsCost *ins_at(uint16_t addr)
{
    size_t lo = 0, hi = SDL_arraysize(su_ins_cost);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (su_ins_cost[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    SDL_assert(lo < SDL_arraysize(su_ins_cost) && su_ins_cost[lo].addr == addr);
    return &su_ins_cost[lo];
}

/* The original executes every instruction in [from, to) in order. */
static void run(Pc *pc, uint16_t from, uint16_t to)
{
    for (const SuInsCost *i = ins_at(from); i < su_ins_cost + SDL_arraysize(su_ins_cost) && i->addr < to; i++)
        pc->cpu.cycles += i->cost;
}

/* The conditional jump at addr, already counted by run(), was taken. */
static void taken(Pc *pc, uint16_t addr)
{
    const SuInsCost *i = ins_at(addr);
    pc->cpu.cycles += (uint64_t)i->taken - i->cost;
}

/* REP string instruction at addr (counted by run() with CX = 2) done with n elements. */
static void rep_elems(Pc *pc, uint16_t addr, uint32_t per, uint32_t n)
{
    (void)addr;
    pc->cpu.cycles += (uint64_t)per * n - 2ull * per;
}

/* ---- memory, flags ---------------------------------------------------------------------- */

static uint8_t rd8(Pc *pc, uint16_t seg, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(seg, off)); }
static uint16_t rd16(Pc *pc, uint16_t seg, uint16_t off) { return mem_read16(pc, seg, off); }
static void wr8(Pc *pc, uint16_t seg, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(seg, off), v); }
static void wr16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v) { mem_write16(pc, seg, off, v); }
#define D8(o) rd8(pc, SREG(DS), (uint16_t)(o))
#define D16(o) rd16(pc, SREG(DS), (uint16_t)(o))
#define W8(o, v) wr8(pc, SREG(DS), (uint16_t)(o), (uint8_t)(v))
#define W16(o, v) wr16(pc, SREG(DS), (uint16_t)(o), (uint16_t)(v))

static bool flag(Pc *pc, uint16_t f) { return (pc->cpu.flags & f) != 0; }
static void set_flag(Pc *pc, uint16_t f, bool on)
{
    pc->cpu.flags = (uint16_t)(on ? pc->cpu.flags | f : pc->cpu.flags & ~f);
}

static void szp(Pc *pc, uint32_t r, int w)
{
    uint32_t m = w ? 0xFFFF : 0xFF;
    uint8_t p = (uint8_t)r;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    set_flag(pc, F_ZF, (r & m) == 0);
    set_flag(pc, F_SF, (r & (w ? 0x8000 : 0x80)) != 0);
    set_flag(pc, F_PF, !(p & 1));
}

static uint32_t f_logic(Pc *pc, uint32_t r, int w)
{
    set_flag(pc, F_CF, false);
    set_flag(pc, F_OF, false);
    set_flag(pc, F_AF, false);
    szp(pc, r, w);
    return r & (w ? 0xFFFF : 0xFF);
}

static uint32_t f_add(Pc *pc, uint32_t a, uint32_t b, int w)
{
    uint32_t m = w ? 0xFFFF : 0xFF, s = w ? 0x8000 : 0x80;
    uint32_t r = (a & m) + (b & m);
    set_flag(pc, F_CF, r > m);
    set_flag(pc, F_OF, ((a ^ r) & (b ^ r) & s) != 0);
    set_flag(pc, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    szp(pc, r, w);
    return r & m;
}

static uint32_t f_sub(Pc *pc, uint32_t a, uint32_t b, int w)
{
    uint32_t m = w ? 0xFFFF : 0xFF, s = w ? 0x8000 : 0x80;
    uint32_t r = ((a & m) - (b & m)) & m;
    set_flag(pc, F_CF, (a & m) < (b & m));
    set_flag(pc, F_OF, ((a ^ b) & (a ^ r) & s) != 0);
    set_flag(pc, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    szp(pc, r, w);
    return r;
}

static uint32_t f_inc(Pc *pc, uint32_t a, int w)
{
    bool cf = flag(pc, F_CF);
    uint32_t r = f_add(pc, a, 1, w);
    set_flag(pc, F_CF, cf);
    return r;
}

static uint32_t f_dec(Pc *pc, uint32_t a, int w)
{
    bool cf = flag(pc, F_CF);
    uint32_t r = f_sub(pc, a, 1, w);
    set_flag(pc, F_CF, cf);
    return r;
}

static bool jg(Pc *pc) { return !flag(pc, F_ZF) && flag(pc, F_SF) == flag(pc, F_OF); }
static bool jl(Pc *pc) { return flag(pc, F_SF) != flag(pc, F_OF); }

/* OUT port, AL at 0050:at (the clock at the instruction's start): as sound.c, the write is 2
 * cycles into the instruction, after the PIT has been brought up to date. */
static void out_at(Pc *pc, uint16_t at, uint16_t port, uint8_t v)
{
    if (port >= 0x40 && port <= 0x43)
        pc_sync_pit(pc);
    pc->cpu.cycles += 2;
    pc->cpu.bus.out8(pc->cpu.bus.ctx, port, v);
    pc->cpu.cycles -= 2;
    run(pc, at, (uint16_t)(at + 1));
}

/* ---- calls ----------------------------------------------------------------------------- */

/* Runs from 0050:target, as the CPU would with the natives on, until the routine returns past
 * the word on top of the stack. */
static void run_from(Pc *pc, uint16_t target)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp0 = REG(SP);
    const uint8_t *map = c->hook_map;
    SREG(CS) = GAME_CS;
    c->ip = target;
    for (long n = 0; n < STEP_CAP; n++) {
        bool done = false;
        if (map && SREG(CS) == GAME_CS && map[c->ip])
            done = native_call(pc, c->ip);
        if (!done) {
            c->hook_map = NULL; /* stepped here, so that no nested --verify starts */
            native_or_cpu_step(pc);
            c->hook_map = map;
        }
        uint16_t d = (uint16_t)(REG(SP) - sp0);
        if (d >= 2 && d < 0x8000)
            break;
    }
}

static uint16_t call_target(Pc *pc, uint16_t at)
{
    return (uint16_t)(at + 3 + rd16(pc, GAME_CS, (uint16_t)(at + 1)));
}

/* The near CALL at 0050:at, the callee run to its return (nested). */
static void call(Pc *pc, uint16_t at)
{
    uint16_t ret_ip = (uint16_t)(at + 3);
    run(pc, at, ret_ip);
    cpu_push(&pc->cpu, ret_ip);
    run_from(pc, call_target(pc, at));
}

/* The near CALL at 0050:at as the end of a block: the callee is the scheduler's next step. */
static void call_out(Pc *pc, uint16_t at)
{
    uint16_t ret_ip = (uint16_t)(at + 3);
    run(pc, at, ret_ip);
    cpu_push(&pc->cpu, ret_ip);
    pc->cpu.ip = call_target(pc, at);
}

/* RET at 0050:at. */
static void ret(Pc *pc, uint16_t at)
{
    run(pc, at, (uint16_t)(at + 1));
    native_ret(pc);
}

/* INT n performed by the instruction ending at next (a divide error): FLAGS, CS, IP pushed,
 * through the vector, run until the handler's IRET is back at next. */
static void soft_int(Pc *pc, uint8_t n, uint16_t next)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp0 = REG(SP);
    c->ip = next;
    cpu_interrupt(c, n);
    const uint8_t *map = c->hook_map;
    for (long k = 0; k < STEP_CAP; k++) {
        bool done = false;
        if (map && SREG(CS) == GAME_CS && map[c->ip])
            done = native_call(pc, c->ip);
        if (!done) {
            c->hook_map = NULL;
            native_or_cpu_step(pc);
            c->hook_map = map;
        }
        if (REG(SP) == sp0 && SREG(CS) == GAME_CS && c->ip == next)
            break;
    }
}

/* ---- 0050:5600 cga_program_regs, 5619 ---------------------------------------------------- */

/* (port - 3D0h, value) byte pairs from DS:BX up to a 0 port byte. */
static void cga_regs_loop(Pc *pc)
{
    bool any = false;
    for (;;) {
        run(pc, 0x5603, 0x5609);
        uint8_t dl = D8(REG(BX));
        SET_LO(DX, dl);
        f_logic(pc, dl, 0);
        if (!dl) {
            taken(pc, 0x5607);
            break;
        }
        run(pc, 0x5609, 0x5615);
        REG(BX)++;
        REG(DX) = (uint16_t)(dl + 0x3D0);
        SET_LO(AX, D8(REG(BX)));
        SET_HI(AX, HI(DX));
        REG(BX)++;
        out_at(pc, 0x5615, REG(DX), LO(AX));
        run(pc, 0x5616, 0x5618);
        any = true;
    }
    (void)any;
    ret(pc, 0x5618);
}

static void b_cga_program_regs(Pc *pc)
{
    run(pc, 0x5600, 0x5603);
    REG(BX) = 0x399C;
    cga_regs_loop(pc);
}

static void b_cga_program_regs2(Pc *pc)
{
    run(pc, 0x5619, 0x561E);
    REG(BX) = 0x39DB;
    cga_regs_loop(pc);
}

/* ---- start-up helpers -------------------------------------------------------------------- */

/* 5CCD: ES = 0618 (the data segment after relocation), CX = 3A84, SI = DI = BP = 0. */
static void b_set_reloc_args(Pc *pc)
{
    run(pc, 0x5CCD, 0x5CE8);
    REG(AX) = (uint16_t)((0x5C70 >> 4) + 1);
    REG(BX) = SREG(CS);
    REG(AX) = (uint16_t)f_add(pc, REG(AX), REG(BX), 1);
    SREG(ES) = REG(AX);
    REG(CX) = 0x3A84;
    REG(SI) = 0;
    REG(DI) = 0;
    REG(BP) = (uint16_t)f_logic(pc, 0, 1);
    ret(pc, 0x5CE8);
}

/* 0732: copies CX bytes DS:SI -> ES:DI (the data segment to 0618), [0:0120] = DS = AX. */
static void b_store_ds_for_isr(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    run(pc, 0x0732, 0x0739);
    set_flag(pc, F_IF, false);
    W16(0x03F0, REG(CX));
    uint16_t n = REG(CX);
    int d = flag(pc, F_DF) ? -1 : 1;
    rep_elems(pc, 0x0737, 17, n);
    for (; REG(CX); REG(CX)--) {
        wr8(pc, SREG(ES), REG(DI), rd8(pc, SREG(DS), REG(SI)));
        REG(SI) = (uint16_t)(REG(SI) + d);
        REG(DI) = (uint16_t)(REG(DI) + d);
    }
    run(pc, 0x0739, 0x0741);
    SREG(DS) = REG(BP);
    W16(0x0120, REG(AX));
    SREG(DS) = REG(AX);
    set_flag(pc, F_IF, true);
    (void)c;
    ret(pc, 0x0741);
}

/* 5DA6: IRQ1 vector = 0050:0970 (int9_keyboard), the old one at [03FB]. */
static void b_hook_int9(Pc *pc)
{
    run(pc, 0x5DA6, 0x5DC2);
    set_flag(pc, F_IF, false);
    REG(AX) = (uint16_t)f_logic(pc, 0, 1);
    SREG(ES) = 0;
    REG(BX) = SREG(CS);
    uint16_t old_ip = rd16(pc, 0, 0x24), old_cs = rd16(pc, 0, 0x26);
    wr16(pc, 0, 0x24, 0x0970);
    wr16(pc, 0, 0x26, REG(BX));
    REG(AX) = old_ip;
    REG(BX) = old_cs;
    W16(0x03FB, REG(AX));
    W16(0x03FD, REG(BX));
    set_flag(pc, F_IF, true);
    ret(pc, 0x5DC2);
}

/* 5DC3: IRQ0 vector = 0050:07A0 (int8_timer), the old one at [040E]; PIT channel 2 = 8080h,
 * timer_hooked = 1; on a re-entry from the menus ([0557]) the text screen is copied to the
 * panel buffer; the panel and radio texts are drawn. */
static void b_hook_int8(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    run(pc, 0x5DC3, 0x5DDE);
    set_flag(pc, F_IF, false);
    REG(AX) = (uint16_t)f_logic(pc, 0, 1);
    SREG(ES) = 0;
    REG(BX) = SREG(CS);
    uint16_t old_ip = rd16(pc, 0, 0x20), old_cs = rd16(pc, 0, 0x22);
    wr16(pc, 0, 0x20, 0x07A0);
    wr16(pc, 0, 0x22, REG(BX));
    REG(AX) = old_ip;
    REG(BX) = old_cs;
    W16(0x040E, REG(AX));
    W16(0x0410, REG(BX));
    run(pc, 0x5DDE, 0x5DE0);
    SET_LO(AX, 0x80);
    out_at(pc, 0x5DE0, 0x42, 0x80);
    out_at(pc, 0x5DE2, 0x42, 0x80);
    run(pc, 0x5DE4, 0x5DF2);
    W16(0x040A, 1);
    set_flag(pc, F_IF, true);
    uint8_t v = D8(0x0557);
    f_logic(pc, v, 0);
    if (!v) {
        taken(pc, 0x5DF0);
    } else {
        run(pc, 0x5DF2, 0x5E1B);
        cpu_push(c, SREG(DS));
        W8(0x0557, 0);
        REG(AX) = (uint16_t)f_logic(pc, 0, 1);
        SREG(ES) = 0;
        SREG(ES) = rd16(pc, 0, 0x0122);
        REG(AX) = 0xB800;
        SREG(DS) = 0xB800;
        for (int k = 0; k < 2; k++) {
            uint16_t at = k ? 0x5E18 : 0x5E0E;
            REG(SI) = k ? 0x2FA0 : 0x0FA0;
            REG(DI) = REG(SI);
            REG(CX) = 0x07D0;
            rep_elems(pc, at, 17, REG(CX));
            int d = flag(pc, F_DF) ? -2 : 2;
            for (; REG(CX); REG(CX)--) {
                wr16(pc, SREG(ES), REG(DI), rd16(pc, SREG(DS), REG(SI)));
                REG(SI) = (uint16_t)(REG(SI) + d);
                REG(DI) = (uint16_t)(REG(DI) + d);
            }
        }
        SREG(DS) = cpu_pop(c);
    }
    run(pc, 0x5E1B, 0x5E33);
    W16(0x3800, 0x0848);
    W16(0x30C8, 0xF00F);
    W16(0x3802, 0x0848);
    W16(0x3804, 0x0848);
    call(pc, 0x5E33); /* obi_readout */
    call(pc, 0x5E36); /* controls_redraw */
    call(pc, 0x5E39); /* sub_0790 */
    ret(pc, 0x5E3C);
}

/* 6254: INT 0 (divide error) vector = 0050:4DC6. */
static void b_set_int0(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    run(pc, 0x6254, 0x6264);
    cpu_push(c, SREG(DS));
    REG(AX) = (uint16_t)f_logic(pc, 0, 1);
    wr16(pc, 0, 0x0002, SREG(CS));
    wr16(pc, 0, 0x0000, 0x4DC6);
    SREG(DS) = cpu_pop(c);
    ret(pc, 0x6264);
}

/* 6265: IRQ2-5 and IRQ7 vectors (ES:28h-3Eh) = 0050:07F0. */
static void b_set_irq_vectors(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    run(pc, 0x6265, 0x629B);
    cpu_push(c, SREG(DS));
    REG(AX) = (uint16_t)f_logic(pc, 0, 1);
    REG(AX) = 0x07F0;
    static const uint16_t vec[] = { 0x28, 0x2C, 0x30, 0x34, 0x3C };
    for (size_t i = 0; i < SDL_arraysize(vec); i++) {
        wr16(pc, SREG(ES), vec[i], REG(AX));
        wr16(pc, SREG(ES), (uint16_t)(vec[i] + 2), SREG(CS));
    }
    SREG(DS) = 0;
    SREG(DS) = cpu_pop(c);
    ret(pc, 0x629B);
}

/* 0790: NAV, COM, 07C5, clock and switch texts. */
static void b_panel_texts(Pc *pc)
{
    call(pc, 0x0790);
    call(pc, 0x0793);
    call(pc, 0x0796);
    call(pc, 0x0799);
    call(pc, 0x079C);
    ret(pc, 0x079F);
}

/* ---- 0050:5C9F start, 01F0 (blocks) --------------------------------------------------------- */

static bool n_start(Pc *pc)
{
    switch (pc->cpu.ip) {
    case 0x5C9F:
        run(pc, 0x5C9F, 0x5CA7);
        REG(AX) = (uint16_t)f_logic(pc, 0, 1);
        SREG(DS) = 0;
        SREG(DS) = rd16(pc, 0, 0x0120);
        call_out(pc, 0x5CA7); /* cga_program_regs */
        break;
    case 0x5CAA: call_out(pc, 0x5CAA); break; /* hook_int9 */
    case 0x5CAD: call_out(pc, 0x5CAD); break; /* sub_6254 */
    case 0x5CB0: call_out(pc, 0x5CB0); break; /* sub_6265 */
    case 0x5CB3: call_out(pc, 0x5CB3); break; /* startup_menus */
    case 0x5CB6: call_out(pc, 0x5CB6); break; /* cga_program_regs */
    case 0x5CB9: call_out(pc, 0x5CB9); break; /* hook_int8 */
    case 0x5CBC: call_out(pc, 0x5CBC); break; /* usermode_recall */
    case 0x5CBF: call_out(pc, 0x5CBF); break; /* editor_apply_state */
    case 0x5CC2: call_out(pc, 0x5CC2); break; /* set_reloc_args */
    case 0x5CC5:
        run(pc, 0x5CC5, 0x5CCD);
        f_sub(pc, D8(0x0418), 1, 0); /* enter_unhooks */
        pc->cpu.ip = 0x01F0;
        break;
    default:
        return false;
    }
    pc->cpu.cycles -= ENTRY_CYCLES;
    return true;
}

/* 01F0: (ZF from start's CMP) store_ds_for_isr unless enter_unhooks == 1, then
 * select_scenery_area, then the main loop. */
static bool n_start_tail(Pc *pc)
{
    switch (pc->cpu.ip) {
    case 0x01F0:
        run(pc, 0x01F0, 0x01F2);
        if (flag(pc, F_ZF)) {
            taken(pc, 0x01F0);
            call_out(pc, 0x01F5);
        } else {
            call_out(pc, 0x01F2);
        }
        break;
    case 0x01F5: call_out(pc, 0x01F5); break;
    default:
        return false;
    }
    pc->cpu.cycles -= ENTRY_CYCLES;
    return true;
}

/* ---- 0050:5CE9 startup_menus (blocks) ------------------------------------------------------- */

static bool n_startup_menus(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    switch (c->ip) {
    case 0x5CE9:
        run(pc, 0x5CE9, 0x5CF3);
        W8(0x39D7, 0x1A); /* cga_mode_ctrl */
        W8(0x03F5, 1);    /* editor_active */
        call_out(pc, 0x5CF3); /* clear_screen */
        break;
    case 0x5CF6:
        run(pc, 0x5CF6, 0x5CF9);
        REG(SI) = 0x3A24;
        call_out(pc, 0x5CF9); /* print_str_list, then the key wait at 5CFC */
        break;
    case 0x5D03: { /* after the display menu's key wait */
        run(pc, 0x5D03, 0x5D0E);
        W8(0x057F, 0);
        SET_LO(AX, f_logic(pc, LO(AX) & 0x7F, 0));
        uint8_t al = LO(AX);
        f_sub(pc, al, 0x61, 0);
        uint16_t si;
        if (al == 0x61) {
            taken(pc, 0x5D0C);
            run(pc, 0x5D18, 0x5D1D);
            si = 0x4075;
        } else {
            run(pc, 0x5D0E, 0x5D12);
            f_sub(pc, al, 0x62, 0);
            if (al == 0x62) {
                taken(pc, 0x5D10);
                run(pc, 0x5D1D, 0x5D27);
                si = 0x408B;
                W8(0x39D7, 0x1E);
            } else {
                run(pc, 0x5D12, 0x5D16);
                f_sub(pc, al, 0x63, 0);
                if (al != 0x63) {
                    run(pc, 0x5D16, 0x5D18);
                    taken(pc, 0x5D16);
                    c->ip = 0x5CFC;
                    break;
                }
                taken(pc, 0x5D14);
                run(pc, 0x5D27, 0x5D2A);
                si = 0x408B;
            }
        }
        REG(SI) = si;
        run(pc, 0x5D2A, 0x5D2C);
        REG(BX) = (uint16_t)f_logic(pc, 0, 1);
        for (;;) {
            run(pc, 0x5D2C, 0x5D3A);
            REG(AX) = D16(REG(BX) + REG(SI));
            W16(REG(BX) + 0x31EA, REG(AX));
            REG(BX) = (uint16_t)f_add(pc, REG(BX), 2, 1);
            f_sub(pc, REG(BX), 0x16, 1);
            if (REG(BX) == 0x16)
                break;
            taken(pc, 0x5D38);
        }
        run(pc, 0x5D3A, 0x5D41);
        uint8_t backup = D8(0x3A21);
        f_sub(pc, backup, 1, 0);
        if (backup == 1)
            call_out(pc, 0x5D41); /* check_master_disk */
        else {
            taken(pc, 0x5D3F);
            call_out(pc, 0x5D57); /* check_master_disk */
        }
        break;
    }
    case 0x5D44: /* master disk check, backup offered */
        run(pc, 0x5D44, 0x5D46);
        if (!flag(pc, F_CF)) {
            taken(pc, 0x5D44);
            call_out(pc, 0x5D5F); /* clear_screen */
            break;
        }
        run(pc, 0x5D46, 0x5D4B);
        W8(0x3A22, 1);
        call_out(pc, 0x5D4B); /* clear_screen */
        break;
    case 0x5D4E:
        run(pc, 0x5D4E, 0x5D51);
        REG(SI) = 0x3B14;
        call_out(pc, 0x5D51); /* print_str_list */
        break;
    case 0x5D54:
        run(pc, 0x5D54, 0x5D56);
        c->ip = 0x5D68;
        break;
    case 0x5D5A: /* disk check without the backup option */
        run(pc, 0x5D5A, 0x5D5C);
        if (!flag(pc, F_CF)) {
            taken(pc, 0x5D5A);
            call_out(pc, 0x5D5F); /* clear_screen */
            break;
        }
        run(pc, 0x5D5C, 0x5D5F);
        c->ip = 0x6311; /* not a flight disk: message and hang */
        break;
    case 0x5D62:
        run(pc, 0x5D62, 0x5D65);
        REG(SI) = 0x3B7F;
        call_out(pc, 0x5D65); /* print_str_list, then the key wait at 5D68 */
        break;
    case 0x5D6F: { /* after the mode menu's key wait */
        run(pc, 0x5D6F, 0x5D7A);
        W8(0x057F, 0);
        SET_LO(AX, f_logic(pc, LO(AX) & 0x7F, 0));
        uint8_t al = LO(AX);
        f_sub(pc, al, 0x61, 0);
        if (al == 0x61) {
            taken(pc, 0x5D78);
            run(pc, 0x5D92, 0x5D98);
            W8(0x2035, f_inc(pc, D8(0x2035), 0)); /* demo_request */
        } else {
            run(pc, 0x5D7A, 0x5D7E);
            f_sub(pc, al, 0x62, 0);
            if (al == 0x62) {
                taken(pc, 0x5D7C);
            } else {
                run(pc, 0x5D7E, 0x5D85);
                uint8_t m = D8(0x3A22);
                f_sub(pc, m, 1, 0);
                if (m != 1) {
                    taken(pc, 0x5D83);
                    c->ip = 0x5D68;
                    break;
                }
                run(pc, 0x5D85, 0x5D8C);
                uint8_t b = D8(0x3A21);
                f_sub(pc, b, 1, 0);
                if (b != 1) {
                    taken(pc, 0x5D8A);
                    c->ip = 0x5D68;
                    break;
                }
                run(pc, 0x5D8C, 0x5D90);
                f_sub(pc, al, 0x63, 0);
                if (al != 0x63) {
                    run(pc, 0x5D90, 0x5D92);
                    taken(pc, 0x5D90);
                    c->ip = 0x5D68;
                    break;
                }
                taken(pc, 0x5D8E);
                /* disk_backup: needs a second (blank) disk; see docs/subphases/3.22.md */
                run(pc, 0x5D9D, 0x5DA0);
                cpu_push(c, 0x5DA0);
                c->ip = 0x5F54;
                break;
            }
        }
        run(pc, 0x5DA0, 0x5DA5);
        W8(0x03F5, 0); /* editor_active */
        ret(pc, 0x5DA5);
        break;
    }
    case 0x5DA0:
        run(pc, 0x5DA0, 0x5DA5);
        W8(0x03F5, 0);
        ret(pc, 0x5DA5);
        break;
    default:
        return false;
    }
    c->cycles -= ENTRY_CYCLES;
    return true;
}

/* 6311: "not a flight disk" message, then the original hangs (631A, JMP $). */
static bool n_no_disk(Pc *pc)
{
    switch (pc->cpu.ip) {
    case 0x6311: call_out(pc, 0x6311); break; /* clear_screen */
    case 0x6314:
        run(pc, 0x6314, 0x6317);
        REG(SI) = 0x3DFE;
        call_out(pc, 0x6317); /* print_str_list, returns to 631A */
        break;
    default:
        return false;
    }
    pc->cpu.cycles -= ENTRY_CYCLES;
    return true;
}

/* JMP $ (0211: the main loop with the timer not hooked; 631A): as many passes as fit before
 * the next interrupt (one under --verify, at least one with the C scheduler). */
static bool n_hang(Pc *pc)
{
    uint32_t pass = ins_at(pc->cpu.ip)->cost;
    uint64_t h = native_logged(pc) ? (uint64_t)pass + 1 : pc_irq_horizon(pc);
    if (pc->csched && h <= pass)
        h = (uint64_t)pass + 1;
    if (pass >= h)
        return false;
    pc->cpu.cycles += (uint64_t)pass * ((h - 1) / pass) - ENTRY_CYCLES;
    return true;
}

/* 5F54: disk_backup - "insert master/copy" backup loop. Requires a second blank disk.
 * This is a stub that returns to the menu without performing the backup, since the modern
 * port has no physical disk drives. The original hangs at 602B; we return immediately. */
static void b_disk_backup(Pc *pc)
{
    ret(pc, 0x5DA0);
}

/* ---- 0050:3640 editor_main, 3703 editor_get_input (blocks) ------------------------------- */

static void editor_input_call(Pc *pc)
{
    call_out(pc, 0x364D); /* editor_get_input */
}

static bool n_editor_main(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    switch (c->ip) {
    case 0x3640: call_out(pc, 0x3640); break; /* editor_capture_state */
    case 0x3643:
        run(pc, 0x3643, 0x3647);
        SREG(ES) = D16(0x03C2);
        call_out(pc, 0x3647); /* cga_program_regs2 */
        break;
    case 0x364A: call_out(pc, 0x364A); break; /* editor_first_page */
    case 0x364D: editor_input_call(pc); break;
    case 0x3650: {
        run(pc, 0x3650, 0x3657);
        SET_LO(AX, f_logic(pc, D8(0x2018), 0)); /* editor_typed */
        if (!LO(AX)) {
            taken(pc, 0x3655);
            goto l366A;
        }
        call_out(pc, 0x3657); /* editor_commit */
        break;
    }
    case 0x365A:
        run(pc, 0x365A, 0x3661);
        SET_LO(AX, f_logic(pc, D8(0x2017), 0)); /* editor_page */
        if (LO(AX)) {
            taken(pc, 0x365F);
            goto l366A;
        }
        run(pc, 0x3661, 0x3668);
        SET_LO(AX, D8(0x2015)); /* editor_row */
        f_sub(pc, LO(AX), 2, 0);
        if (LO(AX) != 2) {
            taken(pc, 0x3666);
            goto l366A;
        }
        run(pc, 0x3668, 0x366A);
        editor_input_call(pc);
        break;
    case 0x3683:
        run(pc, 0x3683, 0x3685);
        editor_input_call(pc);
        break;
    case 0x3688:
        run(pc, 0x3688, 0x368A);
        editor_input_call(pc);
        break;
    default:
        return false;
    }
    c->cycles -= ENTRY_CYCLES;
    return true;

l366A:
    run(pc, 0x366A, 0x3671);
    SET_LO(AX, f_logic(pc, D8(0x2011), 0)); /* editor_exit */
    if (LO(AX)) {
        run(pc, 0x3671, 0x3674);
        c->ip = 0x3B36; /* jmp editor_apply_state */
    } else {
        taken(pc, 0x366F);
        run(pc, 0x3674, 0x367B);
        SET_LO(AX, f_logic(pc, D8(0x2010), 0)); /* editor_up */
        if (LO(AX)) {
            run(pc, 0x367B, 0x3680);
            W8(0x2010, 0);
            call_out(pc, 0x3680); /* editor_prev_row */
        } else {
            taken(pc, 0x3679);
            call_out(pc, 0x3685); /* editor_next_row */
        }
    }
    c->cycles -= ENTRY_CYCLES;
    return true;
}

static bool n_editor_get_input(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    switch (c->ip) {
    case 0x3703:
        run(pc, 0x3703, 0x3708);
        W8(0x2018, 0); /* editor_typed */
        call_out(pc, 0x3708); /* editor_row_ptr */
        break;
    case 0x370B:
        run(pc, 0x370B, 0x371E);
        W16(0x2032, 5); /* editor_cell */
        W8(0x202B, 0);  /* editor_ndigits */
        W8(0x2011, 0);  /* editor_exit */
        REG(DI) = 5;
        for (;;) {
            run(pc, 0x371E, 0x3726);
            W8(REG(DI) + 0x202C, 0x30);
            REG(DI) = (uint16_t)f_dec(pc, REG(DI), 1);
            if (REG(DI) & 0x8000)
                break;
            taken(pc, 0x3724);
        }
        c->ip = 0x3726; /* key_wait */
        break;
    case 0x372D: {
        run(pc, 0x372D, 0x3738);
        W8(0x057F, 0);
        SET_LO(AX, f_logic(pc, LO(AX) & 0x7F, 0));
        uint8_t al = LO(AX);
        f_sub(pc, al, 0x0D, 0);
        if (al == 0x0D) {
            taken(pc, 0x3736);
            c->ip = 0x374E;
            break;
        }
        run(pc, 0x3738, 0x373C);
        f_sub(pc, al, 0x3D, 0);
        if (al == 0x3D) {
            taken(pc, 0x373A);
            c->ip = 0x374E;
            break;
        }
        run(pc, 0x373C, 0x3740);
        f_sub(pc, al, 0x2D, 0);
        if (al == 0x2D) {
            c->ip = 0x3740;
            break;
        }
        taken(pc, 0x373E);
        run(pc, 0x3746, 0x374A);
        f_sub(pc, al, 0x1B, 0);
        if (al == 0x1B) {
            run(pc, 0x374A, 0x374E);
            W8(0x2011, f_inc(pc, D8(0x2011), 0)); /* editor_exit */
            c->ip = 0x374E;
            break;
        }
        taken(pc, 0x3748);
        run(pc, 0x3752, 0x3756);
        f_sub(pc, al, 0x08, 0);
        if (al == 0x08) {
            c->ip = 0x3756; /* editor_key_backspace */
            break;
        }
        taken(pc, 0x3754);
        run(pc, 0x3789, 0x3798);
        REG(CX) = 0x30B4;
        REG(SI) = 0x2916;
        REG(CX) = (uint16_t)f_sub(pc, REG(CX), REG(SI), 1);
        W8(0x03B3, 0x26); /* disk_track */
        f_sub(pc, al, 0x73, 0);
        if (al == 0x73) {
            c->ip = 0x379A; /* preset_save */
            break;
        }
        taken(pc, 0x3798);
        run(pc, 0x37B1, 0x37B5);
        f_sub(pc, al, 0x6C, 0);
        if (al == 0x6C) {
            c->ip = 0x37B5; /* preset_load */
            break;
        }
        taken(pc, 0x37B3);
        run(pc, 0x37D0, 0x37D4);
        f_sub(pc, al, 0x2A, 0);
        if (al == 0x2A) {
            c->ip = 0x37D4; /* editor_key_star */
            break;
        }
        taken(pc, 0x37D2);
        run(pc, 0x37D9, 0x37DD);
        f_sub(pc, al, 0x11, 0);
        if (al == 0x11) {
            c->ip = 0x37DD; /* editor_key_ins */
            break;
        }
        taken(pc, 0x37DB);
        run(pc, 0x37FC, 0x3800);
        f_sub(pc, al, 0x30, 0);
        if (al < 0x30) {
            run(pc, 0x3800, 0x3803);
            c->ip = 0x3726;
            break;
        }
        taken(pc, 0x37FE);
        run(pc, 0x3803, 0x3807);
        f_sub(pc, al, 0x3A, 0);
        if (al >= 0x3A) {
            taken(pc, 0x3805);
            run(pc, 0x3800, 0x3803);
            c->ip = 0x3726;
            break;
        }
        run(pc, 0x3807, 0x3810);
        REG(SI) = D16(0x2032);
        f_sub(pc, REG(SI), 0x0B, 1);
        if (REG(SI) == 0x0B) {
            taken(pc, 0x380E);
            run(pc, 0x3800, 0x3803);
            c->ip = 0x3726;
            break;
        }
        /* a digit */
        run(pc, 0x3810, 0x381E);
        W8(0x2018, f_inc(pc, D8(0x2018), 0));
        W8(0x2024, al); /* editor_digit */
        SET_LO(AX, f_logic(pc, D8(0x202B), 0));
        if (LO(AX)) {
            taken(pc, 0x381C);
        } else {
            run(pc, 0x381E, 0x3823);
            REG(SI) = 0x18;
            SET_LO(AX, 0x20);
            for (;;) {
                run(pc, 0x3823, 0x382E);
                wr8(pc, SREG(ES), (uint16_t)(REG(BX) + REG(SI)), LO(AX));
                REG(SI) = (uint16_t)f_sub(pc, REG(SI), 2, 1);
                f_sub(pc, REG(SI), 8, 1);
                if (REG(SI) == 8)
                    break;
                taken(pc, 0x382C);
            }
        }
        run(pc, 0x382E, 0x3831);
        REG(DI) = 4;
        for (;;) {
            run(pc, 0x3831, 0x383C);
            SET_LO(AX, D8(REG(DI) + 0x202C));
            W8(REG(DI) + 0x202D, LO(AX));
            REG(DI) = (uint16_t)f_dec(pc, REG(DI), 1);
            if (REG(DI) & 0x8000)
                break;
            taken(pc, 0x383A);
        }
        run(pc, 0x383C, 0x3853);
        SET_LO(AX, D8(0x2024));
        W8(0x202C, LO(AX));
        W8(0x202B, f_inc(pc, D8(0x202B), 0));
        REG(SI) = D16(0x2032);
        REG(SI) = (uint16_t)(REG(SI) << 1);
        wr8(pc, SREG(ES), (uint16_t)(REG(BX) + REG(SI)), LO(AX));
        W16(0x2032, f_inc(pc, D16(0x2032), 1));
        run(pc, 0x3853, 0x3855);
        run(pc, 0x3800, 0x3803);
        c->ip = 0x3726;
        break;
    }
    default:
        return false;
    }
    c->cycles -= ENTRY_CYCLES;
    return true;
}

/* ---- 0050:110C demo_step ------------------------------------------------------------------ */

/* The next byte of the demo script: a key code to key_dispatch, 7Fh restarts the script, 80h+n
 * waits n calls. */
static void b_demo_step(Pc *pc)
{
    for (;;) {
        run(pc, 0x110C, 0x1114);
        REG(BX) = (uint16_t)f_logic(pc, D16(0x0571), 1); /* demo_ptr */
        if (!REG(BX)) {
        restart:
            run(pc, 0x1114, 0x1120);
            REG(AX) = D16(0x31E8); /* demo_script */
            REG(BX) = REG(AX);
            W16(0x0571, REG(AX));
            f_logic(pc, REG(AX), 1);
            if (!REG(AX)) {
                taken(pc, 0x111E);
                ret(pc, 0x1131);
                return;
            }
        } else {
            taken(pc, 0x1112);
        }
        run(pc, 0x1120, 0x1126);
        SET_LO(AX, f_logic(pc, D8(REG(BX)), 0));
        if (LO(AX) & 0x80) {
            taken(pc, 0x1124);
            run(pc, 0x1132, 0x1139);
            uint8_t w = D8(0x0570);
            f_sub(pc, w, 0, 0);
            if (!w) {
                taken(pc, 0x1137);
                run(pc, 0x1145, 0x114C);
                SET_LO(AX, f_sub(pc, LO(AX), 0x80, 0));
                W8(0x0570, LO(AX));
            }
            run(pc, 0x1139, 0x113F);
            W8(0x0570, f_dec(pc, D8(0x0570), 0));
            if (D8(0x0570)) {
                taken(pc, 0x113D);
                ret(pc, 0x1131);
                return;
            }
            run(pc, 0x113F, 0x1145);
            W16(0x0571, f_inc(pc, D16(0x0571), 1));
            continue; /* jmp demo_step */
        }
        run(pc, 0x1126, 0x112A);
        f_sub(pc, LO(AX), 0x7F, 0);
        if (LO(AX) == 0x7F) {
            taken(pc, 0x1128);
            goto restart;
        }
        call(pc, 0x112A); /* key_dispatch */
        run(pc, 0x112D, 0x1131);
        W16(0x0571, f_inc(pc, D16(0x0571), 1));
        ret(pc, 0x1131);
        return;
    }
}

/* ---- 0050:13C6 upd_obi (the heading readout) ---------------------------------------------- */

static void b_upd_obi(Pc *pc)
{
    run(pc, 0x13C6, 0x13CE);
    uint16_t mask = D16(0x19A3);
    f_logic(pc, mask & 0x1000, 1);
    if (!(mask & 0x1000)) {
        taken(pc, 0x13CC);
        ret(pc, 0x13F9);
        return;
    }
    run(pc, 0x13CE, 0x13E5);
    REG(AX) = D16(0x06A0);
    REG(DX) = REG(AX);
    REG(CX) = D16(0x06A2);
    REG(AX) = (uint16_t)f_sub(pc, REG(AX), D16(0x30F5), 1);
    REG(AX) = (uint16_t)f_add(pc, REG(AX), D16(0x0917), 1);
    REG(AX) = (uint16_t)f_add(pc, REG(AX), D16(0x0915), 1);
    if (!REG(AX)) {
        taken(pc, 0x13E3);
        ret(pc, 0x1446);
        return;
    }
    if (REG(AX) & 0x8000) {
        run(pc, 0x13E5, 0x13E7);
        taken(pc, 0x13E5);
        run(pc, 0x13FA, 0x13FF);
        f_sub(pc, REG(AX), 0xF600, 1);
        if (jl(pc)) {
            taken(pc, 0x13FD);
        } else {
            run(pc, 0x13FF, 0x1407);
            REG(CX) = (uint16_t)((int16_t)REG(CX) >> 4);
        }
        run(pc, 0x1407, 0x1409);
        REG(DX) = (uint16_t)f_add(pc, REG(DX), REG(CX), 1);
    } else {
        run(pc, 0x13E5, 0x13EC);
        f_sub(pc, REG(AX), 0x0A00, 1);
        if (jg(pc)) {
            taken(pc, 0x13EA);
        } else {
            run(pc, 0x13EC, 0x13F4);
            REG(CX) = (uint16_t)((int16_t)REG(CX) >> 4);
        }
        run(pc, 0x13F4, 0x13F8);
        REG(DX) = (uint16_t)f_sub(pc, REG(DX), REG(CX), 1);
    }
    run(pc, 0x1409, 0x141C);
    W16(0x06A0, REG(DX));
    uint32_t prod = 0x168u * REG(DX);
    REG(AX) = (uint16_t)(prod >> 16);
    REG(DX) = (uint16_t)(prod >> 16);
    set_flag(pc, F_CF, REG(DX) != 0);
    set_flag(pc, F_OF, REG(DX) != 0);
    REG(SI) = 0x079D;
    REG(BX) = 0x079F;
    SET_LO(DX, 0x30);
    for (;;) {
        run(pc, 0x141C, 0x1421);
        f_sub(pc, REG(AX), 0x64, 1);
        if (jl(pc)) {
            taken(pc, 0x141F);
            break;
        }
        run(pc, 0x1421, 0x1428);
        REG(AX) = (uint16_t)f_sub(pc, REG(AX), 0x64, 1);
        SET_LO(DX, f_inc(pc, LO(DX), 0));
    }
    run(pc, 0x1428, 0x142C);
    W8(REG(BX), LO(DX));
    SET_LO(DX, 0x30);
    for (;;) {
        run(pc, 0x142C, 0x1431);
        f_sub(pc, REG(AX), 0x0A, 1);
        if (jl(pc)) {
            taken(pc, 0x142F);
            break;
        }
        run(pc, 0x1431, 0x1438);
        REG(AX) = (uint16_t)f_sub(pc, REG(AX), 0x0A, 1);
        SET_LO(DX, f_inc(pc, LO(DX), 0));
    }
    run(pc, 0x1438, 0x1443);
    W8(REG(BX) + 1, LO(DX));
    SET_LO(AX, f_add(pc, LO(AX), 0x30, 0));
    W8(REG(BX) + 2, LO(AX));
    REG(CX) = 0x5555;
    call(pc, 0x1443); /* print_str_mask */
    ret(pc, 0x1446);
}

/* ---- 0050:04FB sub_04FB (reality mode: trim, reliability, altimeter, DG drift, lights) ------- */

/* 05D4: a reliability failure: message, then one of the small handlers at 05EF-0624 by
 * (frame_counter + altitude) & 0Eh. */
static void b_failure(Pc *pc)
{
    run(pc, 0x05D4, 0x05D7);
    REG(SI) = 0x04E7;
    call(pc, 0x05D7); /* clear_screen */
    call(pc, 0x05DA); /* print_str */
    run(pc, 0x05DD, 0x05EF);
    SET_LO(AX, D8(0x03F6));
    SET_LO(AX, f_add(pc, LO(AX), D8(0x30D6), 0));
    REG(AX) = (uint16_t)f_logic(pc, REG(AX) & 0x0E, 1);
    REG(BX) = REG(AX);
    REG(CX) = D16(REG(BX) + 0x0473);
    /* the handlers: AND word [19A0], mask / OR byte [1DC8], bits; RET */
    uint16_t at = REG(CX);
    uint8_t op = rd8(pc, GAME_CS, at);
    if (op == 0x81) {
        uint16_t m = rd16(pc, GAME_CS, (uint16_t)(at + 4));
        W16(0x19A0, f_logic(pc, D16(0x19A0) & m, 1));
        pc->cpu.cycles += 24; /* AND m16,imm16 */
        at = (uint16_t)(at + 6);
    } else if (op == 0x80) {
        uint8_t m = rd8(pc, GAME_CS, (uint16_t)(at + 4));
        W8(0x1DC8, f_logic(pc, D8(0x1DC8) | m, 0));
        pc->cpu.cycles += 24; /* OR m8,imm8 */
        at = (uint16_t)(at + 5);
    } else {
        SDL_Log("sub_05D4: unexpected handler at 0050:%04X", at);
    }
    pc->cpu.cycles += 20; /* RET */
    (void)at;
    native_ret(pc);
}

static void b_sub_04FB(Pc *pc)
{
    run(pc, 0x04FB, 0x0502);
    uint8_t rm = D8(0x204C);
    f_logic(pc, rm, 0);
    if (!rm) {
        run(pc, 0x0502, 0x0505);
        run(pc, 0x05C7, 0x05D3);
        W16(0x1E20, 0);
        W16(0x03F9, 0);
        ret(pc, 0x05D3);
        return;
    }
    taken(pc, 0x0500);
    run(pc, 0x0505, 0x050E);
    SET_LO(AX, D8(0x0579));
    SET_LO(AX, f_sub(pc, LO(AX), D8(0x057B), 0));
    if (LO(AX)) {
        run(pc, 0x050E, 0x0510);
        if (jl(pc)) {
            taken(pc, 0x050E);
            run(pc, 0x0520, 0x0526);
            W16(0x0578, f_add(pc, D16(0x0578), 0x100, 1));
            run(pc, 0x0526, 0x0528);
            if (!flag(pc, F_OF)) {
                taken(pc, 0x0526);
            } else {
                run(pc, 0x0528, 0x052E);
                W16(0x0578, 0x7FFF);
            }
        } else {
            run(pc, 0x0510, 0x0516);
            W16(0x0578, f_sub(pc, D16(0x0578), 0x100, 1));
            run(pc, 0x0516, 0x0518);
            if (!flag(pc, F_OF)) {
                taken(pc, 0x0516);
            } else {
                run(pc, 0x0518, 0x0520);
                taken(pc, 0x051E);
                W16(0x0578, 0x8001);
            }
        }
    } else {
        taken(pc, 0x050C);
    }
    call(pc, 0x052E); /* elevator_update */
    run(pc, 0x0531, 0x0537);
    W16(0x0408, f_dec(pc, D16(0x0408), 1));
    if (!(D16(0x0408) & 0x8000)) {
        taken(pc, 0x0535);
    } else {
        run(pc, 0x0537, 0x0546);
        W16(0x0408, 0x3C);
        SET_LO(AX, D8(0x30DA));
        SET_LO(AX, f_add(pc, LO(AX), D8(0x30D2), 0));
        f_sub(pc, LO(AX), 0x64, 0);
        if (LO(AX) >= 0x64) {
            run(pc, 0x0546, 0x0548);
            taken(pc, 0x0546);
        } else {
            run(pc, 0x0546, 0x054E);
            f_sub(pc, LO(AX), D8(0x2089), 0);
            if (LO(AX) < D8(0x2089))
                taken(pc, 0x054C);
            else {
                run(pc, 0x054E, 0x0551);
                cpu_push(&pc->cpu, 0x0551);
                b_failure(pc);
            }
        }
    }
    run(pc, 0x0551, 0x055A);
    REG(AX) = D16(0x1E20);
    uint16_t spot = D16(0x1E22);
    f_sub(pc, REG(AX), spot, 1);
    if (REG(AX) == spot) {
        taken(pc, 0x0558);
    } else {
        run(pc, 0x055A, 0x055C);
        if (jl(pc)) {
            taken(pc, 0x055A);
        } else {
            run(pc, 0x055C, 0x055F);
            REG(AX) = (uint16_t)f_sub(pc, REG(AX), 2, 1);
        }
        run(pc, 0x055F, 0x0563);
        REG(AX) = (uint16_t)f_inc(pc, REG(AX), 1);
        W16(0x1E20, REG(AX));
    }
    run(pc, 0x0563, 0x0575);
    uint8_t b = D8(0x03F8);
    W8(0x03F8, f_add(pc, b, 0xFF, 0));
    bool cf = flag(pc, F_CF);
    W16(0x03F9, f_add(pc, D16(0x03F9), cf, 1));
    uint16_t tod = D16(0x0412);
    f_logic(pc, tod & 1, 1);
    if (tod & 1) {
        taken(pc, 0x0573);
    } else {
        run(pc, 0x0575, 0x057B);
        uint8_t v = D8(0x041A);
        W8(0x041A, f_logic(pc, v >> 1, 0));
        set_flag(pc, F_CF, v & 1);
        if (v & 1) {
            taken(pc, 0x0579);
            run(pc, 0x059D, 0x05B2);
            REG(CX) = (uint16_t)((REG(CX) & 0xFF00) | D8(0x03F7));
            REG(AX) = 0xFF7F;
            uint8_t cl = LO(CX), al = LO(AX);
            for (uint8_t k = 0; k < cl; k++)
                al = (uint8_t)(al << 1 | al >> 7);
            SET_LO(AX, al);
            pc->cpu.cycles += 4u * cl;
            W16(0x041B, f_logic(pc, D16(0x041B) & REG(AX), 1));
            REG(AX) = (uint16_t)f_logic(pc, D16(0x03F6) & 0x3FFC, 1);
            if (REG(AX)) {
                taken(pc, 0x05B0);
                ret(pc, 0x05C6);
                return;
            }
            run(pc, 0x05B2, 0x05BA);
            f_sub(pc, D16(0x0597), 0x4000, 1);
            if (!jl(pc)) {
                taken(pc, 0x05B8);
                ret(pc, 0x05C6);
                return;
            }
            run(pc, 0x05BA, 0x05C1);
            uint8_t season = D8(0x206A);
            f_sub(pc, season, 2, 0);
            if (season != 2) {
                taken(pc, 0x05BF);
                ret(pc, 0x05C6);
                return;
            }
            run(pc, 0x05C1, 0x05C6);
            W8(0x1DCA, 0x14);
            ret(pc, 0x05C6);
            return;
        }
        run(pc, 0x057B, 0x0588);
        W16(0x19A6, 0x8010);
        uint8_t lights = D8(0x0582);
        f_sub(pc, lights, 0, 0);
        if (!lights) {
            ret(pc, 0x0588);
            return;
        }
        taken(pc, 0x0586);
    }
    run(pc, 0x0589, 0x0591);
    uint16_t fc = D16(0x03F6);
    f_logic(pc, fc & 0x0FFC, 1);
    if (fc & 0x0FFC) {
        taken(pc, 0x058F);
    } else {
        run(pc, 0x0591, 0x0596);
        W8(0x041A, 0);
    }
    run(pc, 0x0596, 0x059C);
    REG(AX) = D16(0x041B);
    W16(0x19A6, REG(AX));
    ret(pc, 0x059C);
}

/* ---- 0050:4A82 clip_isect_p1, 4AAB clip_isect_p2, 4DC6 int0_divide_error ---------------- */

/* IDIV BX at 0050:at (DX:AX); a quotient out of range is a divide error (INT 0). */
static void idiv_bx(Pc *pc, uint16_t at)
{
    int32_t n = (int32_t)((uint32_t)REG(DX) << 16 | REG(AX));
    int32_t d = (int16_t)REG(BX);
    int32_t q = d ? n / d : 0;
    if (!d || q > 32767 || q < -32768) {
        pc->cpu.cycles += ins_at(at)->cost - 165u; /* the instruction up to the trap */
        soft_int(pc, 0, (uint16_t)(at + 2));
        return;
    }
    run(pc, at, (uint16_t)(at + 2));
    REG(AX) = (uint16_t)q;
    REG(DX) = (uint16_t)(n % d);
}

/* IMUL r16 (DX:AX = AX * v), then SHL AX,1 / RCL DX,1. */
static void imul_shl(Pc *pc, uint16_t v)
{
    int32_t r = (int32_t)(int16_t)REG(AX) * (int16_t)v;
    uint32_t u = (uint32_t)r << 1;
    REG(AX) = (uint16_t)u;
    REG(DX) = (uint16_t)(u >> 16);
    set_flag(pc, F_CF, ((uint32_t)r >> 31) & 1);
}

static void clip_isect(Pc *pc, uint16_t base, uint16_t pa, uint16_t pb)
{
    /* base: 4A82 (p1) or 4AAB (p2); the two halves are the same code 29h apart */
    run(pc, base, (uint16_t)(base + 6));
    REG(AX) = 0;
    bool lsb = REG(DX) & 1;
    REG(DX) = (uint16_t)f_logic(pc, (uint16_t)((int16_t)REG(DX) >> 1), 1); /* SAR: SZP, OF = AF = 0 */
    REG(AX) = (uint16_t)(lsb ? 0x8000 : 0);
    set_flag(pc, F_CF, false); /* RCR AX,1: CF = old AX bit 0, OF = new bit 15 ^ bit 14 */
    set_flag(pc, F_OF, lsb);
    /* (the flags matter only to a divide error: INT 0 pushes them) */
    idiv_bx(pc, (uint16_t)(base + 6));
    run(pc, (uint16_t)(base + 8), (uint16_t)(base + 0x16));
    REG(BP) = REG(AX);
    REG(CX) = (uint16_t)(REG(CX) - REG(SI));
    imul_shl(pc, REG(CX));
    REG(SI) = (uint16_t)(REG(SI) + REG(DX));
    REG(CX) = REG(AX);
    REG(AX) = (uint16_t)(D16(pa) - D16(pb));
    imul_shl(pc, REG(BP));
    REG(DX) = (uint16_t)(REG(DX) + D16(pb));
    run(pc, (uint16_t)(base + 0x16), (uint16_t)(base + 0x27));
    if (base == 0x4A82)
        run(pc, 0x4AA9, 0x4AAB); /* jmp 4AD2 */
    /* 4AD2: shift DX:DI and SI:CX left until either high byte leaves -10h..0Fh, at most 16 times */
    run(pc, 0x4AD2, 0x4AD7);
    REG(DI) = REG(AX);
    REG(BP) = 0x10;
    for (;;) {
        run(pc, 0x4AD7, 0x4AE1);
        REG(AX) = REG(DX);
        SET_HI(AX, f_add(pc, HI(AX), 0x10, 0));
        f_sub(pc, HI(AX), 0x1F, 0);
        if (HI(AX) > 0x1F) {
            taken(pc, 0x4ADF);
            break;
        }
        run(pc, 0x4AE1, 0x4AEB);
        REG(AX) = REG(SI);
        SET_HI(AX, f_add(pc, HI(AX), 0x10, 0));
        f_sub(pc, HI(AX), 0x1F, 0);
        if (HI(AX) > 0x1F) {
            taken(pc, 0x4AE9);
            break;
        }
        run(pc, 0x4AEB, 0x4AF6);
        uint32_t dxdi = ((uint32_t)REG(DX) << 16 | REG(DI)) << 1;
        REG(DI) = (uint16_t)dxdi;
        REG(DX) = (uint16_t)(dxdi >> 16);
        uint32_t sicx = ((uint32_t)REG(SI) << 16 | REG(CX)) << 1;
        REG(CX) = (uint16_t)sicx;
        REG(SI) = (uint16_t)(sicx >> 16);
        REG(BP) = (uint16_t)f_dec(pc, REG(BP), 1);
        if (!REG(BP))
            break;
        taken(pc, 0x4AF4);
    }
    ret(pc, 0x4AF6);
}

static void b_clip_isect_p1(Pc *pc) { clip_isect(pc, 0x4A82, 0x311F, 0x3113); }
static void b_clip_isect_p2(Pc *pc) { clip_isect(pc, 0x4AAB, 0x3113, 0x311F); }

/* INT 0 handler: AX = 7FFFh, or 8001h when DX and BX differ in sign; STI, IRET. */
static void b_int0_divide_error(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    run(pc, 0x4DC6, 0x4DCF);
    bool neg = ((REG(DX) ^ REG(BX)) & 0x8000) != 0;
    REG(AX) = 0x7FFF;
    if (neg) {
        run(pc, 0x4DCF, 0x4DD1);
        REG(AX) = 0x8001;
    } else {
        taken(pc, 0x4DCD);
    }
    run(pc, 0x4DD1, 0x4DD3);
    c->ip = cpu_pop(c);
    SREG(CS) = cpu_pop(c);
    c->flags = (uint16_t)((cpu_pop(c) & 0x0FD5) | 0xF002);
}

/* ---- war mode: 0050:5AD7, 5B1D, 5B76 ------------------------------------------------------ */

/* 5B1D: decodes the span record at DS:BX (x, width; row, rows) into BP (back-buffer offset of
 * the first row), DL/DH (edge masks), AH (whole bytes), BX (rows), ES = screen. */
static void span_setup(Pc *pc)
{
    run(pc, 0x5B1D, 0x5B58);
    REG(AX) = D16(REG(BX));
    REG(CX) = (uint16_t)(HI(AX) >> 2);
    REG(DI) = REG(CX);
    REG(AX) &= 0x03FF;
    REG(CX) = (uint16_t)(REG(AX) & 7);
    REG(AX) >>= 3;
    REG(BP) = REG(AX);
    REG(BX) = D16(REG(BX) + 2);
    REG(SI) = REG(BX);
    REG(BX) = (uint16_t)((REG(BX) & 0xFF) << 1);
    REG(BP) = (uint16_t)(REG(BP) + D16(REG(BX) + 0x380C));
    REG(AX) = REG(SI);
    REG(BX) = REG(CX);
    SET_LO(DX, D8(REG(BX) + 0x39F8));
    SET_LO(BX, (uint8_t)(8 - LO(BX)));
    SET_HI(AX, f_sub(pc, HI(AX), LO(BX), 0));
    SET_LO(AX, 0);
    for (;;) {
        run(pc, 0x5B58, 0x5B5D);
        bool borrow = HI(AX) < 8;
        SET_HI(AX, f_sub(pc, HI(AX), 8, 0));
        if (borrow) {
            taken(pc, 0x5B5B);
            break;
        }
        run(pc, 0x5B5D, 0x5B61);
        SET_LO(AX, f_inc(pc, LO(AX), 0));
    }
    run(pc, 0x5B61, 0x5B75);
    REG(AX) = (uint16_t)(REG(AX) << 8 | REG(AX) >> 8);
    SET_LO(AX, f_add(pc, LO(AX), 8, 0));
    REG(BX) = LO(AX);
    SET_HI(DX, (uint8_t)~D8(REG(BX) + 0x39F8));
    SREG(ES) = D16(0x03C2);
    REG(BX) = REG(DI);
    run(pc, 0x5B75, 0x5B76); /* its RET (the callers pop) */
}

/* 5B76: BP to the next line of the interleaved buffer, BX-- (ZF for the caller's loop). */
static void next_line(Pc *pc)
{
    run(pc, 0x5B76, 0x5B7C);
    f_sub(pc, REG(BP), 0x2000, 1);
    if (!jl(pc)) {
        taken(pc, 0x5B7A);
        run(pc, 0x5B82, 0x5B86);
        REG(BP) = (uint16_t)(REG(BP) + 0xE050);
    } else {
        run(pc, 0x5B7C, 0x5B82);
        REG(BP) = (uint16_t)(REG(BP) + 0x2000);
    }
    run(pc, 0x5B86, 0x5B88);
    REG(BX) = (uint16_t)f_dec(pc, REG(BX), 1);
}

static void b_span_setup(Pc *pc)
{
    span_setup(pc);
    native_ret(pc);
}

static void b_next_line(Pc *pc)
{
    next_line(pc);
    native_ret(pc);
}

/* 5AD7: clears a span on the screen, rows of (edge, AH zero bytes, edge). */
static void b_span_clear(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    run(pc, 0x5AD7, 0x5AD8);
    cpu_push(c, SREG(ES));
    run(pc, 0x5AD8, 0x5ADB);
    cpu_push(c, 0x5ADB);
    span_setup(pc);
    native_ret(pc);
    run(pc, 0x5ADB, 0x5ADD);
    SET_LO(AX, f_logic(pc, 0, 0));
    for (;;) {
        run(pc, 0x5ADD, 0x5AEC);
        REG(DI) = REG(BP);
        wr8(pc, SREG(ES), REG(DI), (uint8_t)(rd8(pc, SREG(ES), REG(DI)) & LO(DX)));
        REG(DI)++;
        REG(CX) = HI(AX);
        rep_elems(pc, 0x5AE7, 10, REG(CX));
        int d = flag(pc, F_DF) ? -1 : 1;
        for (; REG(CX); REG(CX)--) {
            wr8(pc, SREG(ES), REG(DI), LO(AX));
            REG(DI) = (uint16_t)(REG(DI) + d);
        }
        wr8(pc, SREG(ES), REG(DI), (uint8_t)(rd8(pc, SREG(ES), REG(DI)) & HI(DX)));
        run(pc, 0x5AEC, 0x5AEF);
        cpu_push(c, 0x5AEF);
        next_line(pc);
        native_ret(pc);
        run(pc, 0x5AEF, 0x5AF1);
        if (!REG(BX))
            break;
        taken(pc, 0x5AEF);
    }
    run(pc, 0x5AF1, 0x5AF2);
    SREG(ES) = cpu_pop(c);
    ret(pc, 0x5AF2);
}

/* ---- small ones ------------------------------------------------------------------------------ */

/* 1CEC: the editor's north/east from the position (+4000h). */
static void b_slew_to_editor_pos(Pc *pc)
{
    run(pc, 0x1CEC, 0x1CFE);
    REG(AX) = (uint16_t)f_add(pc, D16(0x30DB), 0x4000, 1);
    W16(0x2050, REG(AX));
    REG(AX) = (uint16_t)f_add(pc, D16(0x30D3), 0x4000, 1);
    W16(0x2052, REG(AX));
    ret(pc, 0x1CFE);
}

/* 2EDB: engine_sound = 0, rpm_deficit = 0, engine_running = 0. */
static void b_engine_stop(Pc *pc)
{
    run(pc, 0x2EDB, 0x2EEB);
    W16(0x1DD6, 0);
    W8(0x1DCB, 0);
    W8(0x1DD8, 0);
    ret(pc, 0x2EEB);
}

/* ---- 0050:0629 crash_handler after its first instruction (blocks) ------------------------
 * crash_handler (3.14) hands a crash over here: the message, the delay (crash_delay, 3.21),
 * then the user mode is recalled and the gear lowered. */
static bool n_crash_body(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    switch (c->ip) {
    case 0x0629:
        run(pc, 0x0629, 0x062D);
        f_logic(pc, LO(BX), 0);
        if (!LO(BX)) {
            taken(pc, 0x062B);
            ret(pc, 0x0657);
            break;
        }
        run(pc, 0x062D, 0x0633);
        SET_HI(BX, 0);
        REG(SI) = D16(REG(BX) + 0x0481);
        call_out(pc, 0x0633); /* clear_screen */
        break;
    case 0x0636: call_out(pc, 0x0636); break; /* print_str */
    case 0x0639:
        run(pc, 0x0639, 0x063C);
        REG(CX) = 0xFFFF;
        c->ip = 0x063C; /* crash_delay */
        break;
    case 0x0644: call_out(pc, 0x0644); break; /* usermode_recall */
    case 0x0647: call_out(pc, 0x0647); break; /* editor_apply_state */
    case 0x064A:
        run(pc, 0x064A, 0x0654);
        W8(0x03F4, 0);    /* crash_code */
        W8(0x057E, 0xFF); /* gear_down */
        call_out(pc, 0x0654); /* gear_drag_update */
        break;
    case 0x0657: ret(pc, 0x0657); break;
    default:
        return false;
    }
    c->cycles -= ENTRY_CYCLES;
    return true;
}

/* ---- entries ------------------------------------------------------------------------------- */

#define ENTRY_FN(nm, body)                                                                                      \
    static void n_##nm(Pc *pc)                                                                                  \
    {                                                                                                           \
        body(pc);                                                                                               \
        pc->cpu.cycles -= ENTRY_CYCLES;                                                                         \
    }
ENTRY_FN(cga_program_regs, b_cga_program_regs)
ENTRY_FN(cga_program_regs2, b_cga_program_regs2)
ENTRY_FN(set_reloc_args, b_set_reloc_args)
ENTRY_FN(store_ds_for_isr, b_store_ds_for_isr)
ENTRY_FN(hook_int9, b_hook_int9)
ENTRY_FN(hook_int8, b_hook_int8)
ENTRY_FN(set_int0, b_set_int0)
ENTRY_FN(set_irq_vectors, b_set_irq_vectors)
ENTRY_FN(panel_texts, b_panel_texts)
ENTRY_FN(demo_step, b_demo_step)
ENTRY_FN(upd_obi, b_upd_obi)
ENTRY_FN(sub_04FB, b_sub_04FB)
ENTRY_FN(failure, b_failure)
ENTRY_FN(clip_isect_p1, b_clip_isect_p1)
ENTRY_FN(clip_isect_p2, b_clip_isect_p2)
ENTRY_FN(int0_divide_error, b_int0_divide_error)
ENTRY_FN(span_setup, b_span_setup)
ENTRY_FN(next_line, b_next_line)
ENTRY_FN(span_clear, b_span_clear)
ENTRY_FN(slew_to_editor_pos, b_slew_to_editor_pos)
ENTRY_FN(engine_stop, b_engine_stop)
ENTRY_FN(disk_backup, b_disk_backup)

#define E(nm, o, f) { .name = nm, .seg = GAME_CS, .off = o, .fn = n_##f, .enabled = true, .cycles = ENTRY_CYCLES }
#define B(nm, o, f, lo, hi, st)                                                                                 \
    { .name = nm, .seg = GAME_CS, .off = o, .try_fn = f, .enabled = true, .cycles = ENTRY_CYCLES, .stop_lo = lo,  \
      .stop_hi = hi, .stops = st }

static const uint16_t no_stops[] = { 0 };
static const uint16_t menu_stops[] = { 0x5CFC, 0x5D68, 0 };
static const uint16_t input_stops[] = { 0x3726, 0x374E, 0x3740, 0x3756, 0x379A, 0x37B5, 0x37D4, 0x37DD, 0 };
static const uint16_t crash_stops[] = { 0x063C, 0 };
static const uint16_t hang_0211[] = { 0x0211, 0 };
static const uint16_t hang_631A[] = { 0x631A, 0 };

NativeEntry native_startup[] = {
    E("cga_program_regs", 0x5600, cga_program_regs),
    E("cga_program_regs2", 0x5619, cga_program_regs2),
    E("set_reloc_args", 0x5CCD, set_reloc_args),
    E("store_ds_for_isr", 0x0732, store_ds_for_isr),
    E("hook_int9", 0x5DA6, hook_int9),
    E("hook_int8", 0x5DC3, hook_int8),
    E("set_int0_vector", 0x6254, set_int0),
    E("set_irq_vectors", 0x6265, set_irq_vectors),
    E("panel_texts", 0x0790, panel_texts),
    E("demo_step", 0x110C, demo_step),
    E("upd_obi", 0x13C6, upd_obi),
    E("sub_04FB", 0x04FB, sub_04FB),
    E("reliability_failure", 0x05D4, failure),
    E("clip_isect_p1", 0x4A82, clip_isect_p1),
    E("clip_isect_p2", 0x4AAB, clip_isect_p2),
    { .name = "int0_divide_error", .seg = GAME_CS, .off = 0x4DC6, .fn = n_int0_divide_error, .enabled = true,
      .far = true, .cycles = ENTRY_CYCLES },
    E("war_span_setup", 0x5B1D, span_setup),
    E("war_next_line", 0x5B76, next_line),
    E("war_span_clear", 0x5AD7, span_clear),
    E("slew_to_editor_pos", 0x1CEC, slew_to_editor_pos),
    E("engine_stop", 0x2EDB, engine_stop),
    E("disk_backup", 0x5F54, disk_backup),
    B("start", 0x5C9F, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CAA", 0x5CAA, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CAD", 0x5CAD, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CB0", 0x5CB0, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CB3", 0x5CB3, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CB6", 0x5CB6, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CB9", 0x5CB9, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CBC", 0x5CBC, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CBF", 0x5CBF, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CC2", 0x5CC2, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start@5CC5", 0x5CC5, n_start, 0x5C9F, 0x5CCD, no_stops),
    B("start_tail", 0x01F0, n_start_tail, 0x01F0, 0x01F8, no_stops),
    B("start_tail@01F5", 0x01F5, n_start_tail, 0x01F0, 0x01F8, no_stops),
    B("startup_menus", 0x5CE9, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5CF6", 0x5CF6, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D03", 0x5D03, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D44", 0x5D44, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D4E", 0x5D4E, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D54", 0x5D54, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D5A", 0x5D5A, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D62", 0x5D62, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5D6F", 0x5D6F, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("startup_menus@5DA0", 0x5DA0, n_startup_menus, 0x5CE9, 0x5DA6, menu_stops),
    B("no_disk", 0x6311, n_no_disk, 0x6311, 0x631A, no_stops),
    B("no_disk@6314", 0x6314, n_no_disk, 0x6311, 0x631A, no_stops),
    B("hang", 0x0211, n_hang, 0x0211, 0x0213, hang_0211),
    B("hang@631A", 0x631A, n_hang, 0x631A, 0x631C, hang_631A),
    B("editor_main", 0x3640, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@3643", 0x3643, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@364A", 0x364A, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@364D", 0x364D, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@3650", 0x3650, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@365A", 0x365A, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@3683", 0x3683, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_main@3688", 0x3688, n_editor_main, 0x3640, 0x368A, no_stops),
    B("editor_get_input", 0x3703, n_editor_get_input, 0x3703, 0x3855, input_stops),
    B("editor_get_input@370B", 0x370B, n_editor_get_input, 0x3703, 0x3855, input_stops),
    B("editor_get_input@372D", 0x372D, n_editor_get_input, 0x3703, 0x3855, input_stops),
    B("crash_body", 0x0629, n_crash_body, 0x0629, 0x0658, crash_stops),
    B("crash_body@0636", 0x0636, n_crash_body, 0x0629, 0x0658, crash_stops),
    B("crash_body@0639", 0x0639, n_crash_body, 0x0629, 0x0658, crash_stops),
    B("crash_body@0644", 0x0644, n_crash_body, 0x0629, 0x0658, crash_stops),
    B("crash_body@0647", 0x0647, n_crash_body, 0x0629, 0x0658, crash_stops),
    B("crash_body@064A", 0x064A, n_crash_body, 0x0629, 0x0658, crash_stops),
    B("crash_body@0657", 0x0657, n_crash_body, 0x0629, 0x0658, crash_stops),
    { .name = NULL },
};
