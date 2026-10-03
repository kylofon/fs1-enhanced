/* Natives for subphase 3.12 radio navigation (see docs/PHASE3_PLAN.md and
 * docs/subphases/3.12.md).
 *
 * Every routine is transliterated so that registers, segment registers and memory end up as
 * the original leaves them, including the return addresses its CALLs leave below SP (the
 * stack is moved as the original moves it; native_ret pops the final return address).
 * Each routine adds the emulated cycles the original takes on the path it ran (the cycle
 * model of cpu8086.c), so the emulated timeline stays the same with the natives on.
 *
 * Calls to the print_str family (0050:1225, 122B, 1282) push the return address and
 * single-step the original until it returns, with native hooks off, charging its cycles.
 * (3.9 exports no callable C print_str; stepping with hooks on would run 3.9's natives but
 * nests a --verify of print_str inside this native's, which corrupts the verify logs.) */
#include "native.h"
#include "game/state.h"

static uint32_t cyc; /* original cycles of the routine being run */

/* ---- registers and memory --------------------------------------------------------- */

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])
#define LO(r) ((uint8_t)REG(r))
#define HI(r) ((uint8_t)(REG(r) >> 8))
#define SET_LO(r, v) (REG(r) = (uint16_t)((REG(r) & 0xFF00) | (uint8_t)(v)))
#define SET_HI(r, v) (REG(r) = (uint16_t)((REG(r) & 0x00FF) | (uint16_t)((uint8_t)(v) << 8)))

/* Conditional jump: taken 16, not taken 4. */
#define JCC(taken) (cyc += (taken) ? 16 : 4)

static uint8_t rd8(Pc *pc, uint16_t seg, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(seg, off)); }
static uint16_t rd16(Pc *pc, uint16_t seg, uint16_t off) { return mem_read16(pc, seg, off); }
static void wr8(Pc *pc, uint16_t seg, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(seg, off), v); }
static void wr16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v) { mem_write16(pc, seg, off, v); }

/* [off] in the current DS, as the original addresses its variables. */
static uint8_t m8(Pc *pc, uint16_t off) { return rd8(pc, SREG(DS), off); }
static uint16_t m16(Pc *pc, uint16_t off) { return rd16(pc, SREG(DS), off); }
static void w8(Pc *pc, uint16_t off, uint8_t v) { wr8(pc, SREG(DS), off, v); }
static void w16(Pc *pc, uint16_t off, uint16_t v) { wr16(pc, SREG(DS), off, v); }

/* CALL to a routine done in C: pushes the return address. */
static void call_push(Pc *pc, uint16_t ret_ip)
{
    cpu_push(&pc->cpu, ret_ip);
    cyc += 21;
}

/* RET of a routine done in C. */
static void ret_pop(Pc *pc)
{
    REG(SP) = (uint16_t)(REG(SP) + 2);
    cyc += 20;
}

/* ---- original code ------------------------------------------------------------------ */

/* Single-steps the original routine at CS:IP until it returns past the word at SS:SP.
 * Natives are off meanwhile. Returns the cycles it took (including its RET). */
static uint64_t run_steps(Pc *pc, bool hooks)
{
    Cpu8086 *c = &pc->cpu;
    const uint8_t *map = c->hook_map;
    uint16_t sp0 = c->regs[R_SP];
    uint16_t ret_ip = rd16(pc, c->sregs[S_SS], sp0), ret_cs = c->sregs[S_CS];
    uint64_t start = c->cycles;
    if (!hooks)
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

static uint64_t run_original(Pc *pc) { return run_steps(pc, false); }

/* JMP to original code whose return address is already on the stack. */
static void jump_orig(Pc *pc, uint16_t entry)
{
    uint16_t ip = pc->cpu.ip, cs = SREG(CS);
    pc->cpu.ip = entry;
    SREG(CS) = GAME_CS;
    cyc += (uint32_t)run_steps(pc, false);
    pc->cpu.ip = ip;
    SREG(CS) = cs;
}

/* CALL to original code. */
static void call_orig(Pc *pc, uint16_t ret_ip, uint16_t entry)
{
    call_push(pc, ret_ip);
    jump_orig(pc, entry);
}

#define PRINT_STR 0x1225     /* SI = {screen offset, text, byte < 20h}, mask FFFF */
#define PRINT_STR_DIM 0x122B /* the same with the 8888h stipple mask */
#define PRINT_STR2 0x1282    /* SI = string, BP = colour mask, DL = width */

/* Charges what the original took beyond the entry's fixed .cycles. */
static void charge(Pc *pc, uint32_t fixed)
{
    pc->cpu.cycles += cyc;
    pc->cpu.cycles -= fixed;
}

/* The routine is run as the original instead (divide overflow cases). */
static void run_instead(Pc *pc, uint32_t fixed)
{
    cyc = (uint32_t)run_original(pc);
    charge(pc, fixed);
}

/* ---- tuning: nav_tune 1447, nav_search_start 147D, com_tune 1488 --------------------- */

/* CALL panel_text_gate (156D) from the routine; ret_ip is the address after the CALL.
 * With panel_mask bit 15 clear the gate pops the return address into AX and returns to
 * the routine's caller instead: then this returns false and the native only has to RET. */
static bool text_gate(Pc *pc, uint16_t ret_ip)
{
    call_push(pc, ret_ip);
    cyc += 14; /* test word ptr [panel_mask], 8000h */
    bool on = (gs_panel_mask(pc) & 0x8000) != 0;
    JCC(on);
    if (on) {
        ret_pop(pc);
        return true;
    }
    REG(AX) = cpu_pop(&pc->cpu);
    cyc += 12; /* pop ax; the RET is the native's */
    return false;
}

/* ASCII "ddd" at [freq] and "dd" at [freq+5] -> the BCD word of digits 2-3 (high byte)
 * and 5-6 (low byte); 108.15 -> 0815h. Leaves AX = that word, DX = the low pair minus
 * 3030h with DL = the BCD byte. */
static void freq_bcd(Pc *pc, uint16_t freq)
{
    REG(AX) = (uint16_t)(m16(pc, freq) - 0x3030);
    SET_LO(AX, (uint8_t)(LO(AX) << 4));
    SET_HI(AX, (uint8_t)(HI(AX) + LO(AX)));
    REG(DX) = (uint16_t)(m16(pc, (uint16_t)(freq + 5)) - 0x3030);
    SET_LO(DX, (uint8_t)(LO(DX) << 4));
    SET_LO(DX, (uint8_t)(LO(DX) + HI(DX)));
    SET_LO(AX, LO(DX));
    /* mov ax 12 (17 for DX), sub 6, 4 x shl 8, add 5, mov al,dl 4 */
    cyc += 12 + 6 + 32 + 5 + 17 + 6 + 32 + 5 + 4;
}

#define CYC_NAV_SEARCH 58
#define CYC_NAV_TUNE 71   /* panel text off */
#define CYC_COM_TUNE 71

static void nav_search_body(Pc *pc)
{
    gs_set_nav_found(pc, 0);
    gs_set_nav_search(pc, 1);
    cyc += 19 + 19 + 20;
}

/* 0050:147D nav_search_start: [31D5] = 0 (no station), [31D3] = 1 (search next pass). */
static void n_nav_search_start(Pc *pc)
{
    cyc = 0;
    nav_search_body(pc);
    charge(pc, CYC_NAV_SEARCH);
    native_ret(pc);
}

/* 0050:1447 nav_tune: redraws the NAV digits, [03FF] = BCD of nav_freq, then falls into
 * nav_search_start. */
static void n_nav_tune(Pc *pc)
{
    cyc = 0;
    if (text_gate(pc, 0x144A)) {
        REG(SI) = 0x07AE;
        cyc += 4;
        call_orig(pc, 0x1450, PRINT_STR_DIM);
        REG(SI) = 0x07B4;
        cyc += 4;
        call_orig(pc, 0x1456, PRINT_STR_DIM);
        freq_bcd(pc, 0x07B1);
        gs_set_nav_bcd(pc, REG(AX));
        cyc += 12;
        nav_search_body(pc);
    } else {
        cyc += 20;
    }
    charge(pc, CYC_NAV_TUNE);
    native_ret(pc);
}

/* 0050:1488 com_tune: redraws the COM digits, [03F2] = BCD of com_freq, [3628] = 1 (no
 * ATIS message running), [31DE] = 1 (search the COM stations next pass). */
static void n_com_tune(Pc *pc)
{
    cyc = 0;
    if (text_gate(pc, 0x148B)) {
        REG(SI) = 0x07A3;
        cyc += 4;
        call_orig(pc, 0x1491, PRINT_STR_DIM);
        REG(SI) = 0x07A9;
        cyc += 4;
        call_orig(pc, 0x1497, PRINT_STR_DIM);
        freq_bcd(pc, 0x07A6);
        gs_set_com_bcd(pc, REG(AX));
        gs_set_atis_idle(pc, 1);
        gs_set_com_search(pc, 1);
        cyc += 12 + 19 + 19;
    }
    cyc += 20;
    charge(pc, CYC_COM_TUNE);
    native_ret(pc);
}

/* ---- obi_readout 14C9: the OBI course digits ----------------------------------------- */

/* sub_1508: AL = course in 2-degree steps (0..B3h) -> DL = hundreds digit, AX = the tens
 * and units digits as two ASCII bytes in memory order. */
static void course_digits(Pc *pc, uint16_t ret_ip)
{
    call_push(pc, ret_ip);
    SET_LO(DX, 0x2F);
    cyc += 4;
    uint8_t al = LO(AX);
    for (;;) {
        SET_LO(DX, (uint8_t)(LO(DX) + 1));
        bool carry = al < 0x32;
        al = (uint8_t)(al - 0x32);
        cyc += 5 + 6;
        JCC(!carry);
        if (carry)
            break;
    }
    al = (uint8_t)((uint8_t)(al + 0x32) << 1);
    REG(AX) = (uint16_t)((al / 10) << 8 | (al % 10)); /* aam 0Ah */
    REG(AX) = (uint16_t)(REG(AX) + 0x3030);
    REG(AX) = (uint16_t)(REG(AX) << 8 | REG(AX) >> 8);
    cyc += 6 + 8 + 85 + 6 + 6;
    ret_pop(pc);
}

#define CYC_OBI_READOUT 38 /* OBI not shown */

/* 0050:14C9 obi_readout: prints obi_course*2 and its reciprocal on the panel. */
static void n_obi_readout(Pc *pc)
{
    cyc = 14;
    bool off = (gs_panel_mask(pc) & 0x0020) == 0;
    JCC(off);
    if (!off) {
        SET_LO(AX, m8(pc, GS_OBI_COURSE));
        cyc += 12;
        course_digits(pc, 0x14D7);
        gs_set_obi_readout_rec_b(pc, 2, LO(DX));
        w16(pc, (uint16_t)(GS_OBI_READOUT_REC + 3), REG(AX));
        SET_LO(AX, m8(pc, GS_OBI_COURSE));
        cyc += 18 + 12 + 12;
        bool borrow = LO(AX) < 0x5A;
        SET_LO(AX, (uint8_t)(LO(AX) - 0x5A));
        cyc += 6;
        JCC(!borrow);
        if (borrow) {
            SET_LO(AX, (uint8_t)(LO(AX) + 0xB4));
            cyc += 6;
        }
        course_digits(pc, 0x14EA);
        gs_set_obi_readout_rec_b(pc, 8, LO(DX));
        w16(pc, (uint16_t)(GS_OBI_READOUT_REC + 9), REG(AX));
        REG(SI) = 0x07B9;
        REG(BP) = 0x3333;
        SET_LO(DX, 4);
        cyc += 18 + 12 + 4 + 4 + 4;
        call_orig(pc, 0x14FC, PRINT_STR2);
        SET_LO(DX, 2);
        REG(BP) = 0x1111;
        REG(SI) = 0x07BF;
        cyc += 4 + 4 + 4;
        call_orig(pc, 0x1507, PRINT_STR2);
    }
    cyc += 20;
    charge(pc, CYC_OBI_READOUT);
    native_ret(pc);
}

/* ---- nav_compute 2290: VOR / localizer / glide slope -------------------------------- */

/* Result of sub_2525 and its helpers. */
typedef struct {
    uint16_t ax, bx, cx, dx, si, di;
    uint16_t inner_ret; /* return address of the 2598/259F call, 0 = none */
} Bearing;

/* 2598 (cot: CX/BX) or 259F (tan: BX/CX), then the table interpolation at 25A6.
 * Returns false on a divide overflow. */
static bool bearing_divide(Pc *pc, Bearing *b, bool cot, uint16_t ret_ip)
{
    uint16_t num = cot ? b->cx : b->bx, den = cot ? b->bx : b->cx;
    uint32_t n = (uint32_t)num << 16 | b->ax;
    if (den == 0 || n / den > 0xFFFF)
        return false;
    b->inner_ret = ret_ip;
    uint16_t ax = (uint16_t)(n / den);
    cyc += 21 + 4 + 146 + 17; /* call, mov dx, div, jmp */
    ax >>= 1;
    uint8_t bl = (uint8_t)(ax >> 8);
    bl = (uint8_t)((int8_t)bl >> 1);
    bl = (uint8_t)((int8_t)bl >> 1);
    bl &= 0xFE;
    b->bx = bl;
    ax = (uint16_t)(ax << 5);
    b->si = m16(pc, (uint16_t)(b->bx + 0x1AE2));
    b->di = (uint16_t)(m16(pc, (uint16_t)(b->bx + 0x1AE4)) - b->si);
    uint32_t p = (uint32_t)ax * b->di;
    b->ax = (uint16_t)p;
    b->dx = (uint16_t)((uint16_t)(p >> 16) + b->si);
    b->dx = (uint16_t)((int16_t)b->dx >> 4);
    cyc += 8 + 5 + 4 + 16 + 6 + 40 + 17 + 17 + 5 + 120 + 5 + 32 + 20;
    return true;
}

/* sub_2525: BX = east, CX = north -> DX = bearing of (east, north) from north, clockwise,
 * 0..437h (3 units per degree). Returns false on a divide overflow. */
static bool bearing(Pc *pc, Bearing *b)
{
    b->ax = 0;
    b->dx = 0x87;
    b->inner_ret = 0;
    cyc += 4 + 5 + 5; /* mov dx, xor ax, or bx */
    int16_t bx, cx;
    if ((int16_t)b->bx < 0) {
        cyc += 16;
        b->bx = (uint16_t)-b->bx;
        cyc += 5 + 5; /* neg bx, or cx */
        if ((int16_t)b->cx < 0) {
            cyc += 16;
            b->cx = (uint16_t)-b->cx;
            cyc += 5 + 5;
            bx = (int16_t)b->bx, cx = (int16_t)b->cx;
            if (bx < cx) {
                cyc += 16;
                if (!bearing_divide(pc, b, false, 0x2593))
                    return false;
                b->dx = (uint16_t)(b->dx + 0x21C);
                cyc += 6;
            } else {
                cyc += 4;
                JCC(bx == cx);
                if (bx != cx && !bearing_divide(pc, b, true, 0x2589))
                    return false;
                b->dx = (uint16_t)(0x32A - b->dx);
                cyc += 5 + 6;
            }
        } else {
            cyc += 4 + 5;
            bx = (int16_t)b->bx, cx = (int16_t)b->cx;
            if (bx < cx) {
                cyc += 16;
                if (!bearing_divide(pc, b, false, 0x2577))
                    return false;
                b->dx = (uint16_t)(0x438 - b->dx);
                cyc += 5 + 6;
            } else {
                cyc += 4;
                JCC(bx == cx);
                if (bx != cx && !bearing_divide(pc, b, true, 0x256F))
                    return false;
                b->dx = (uint16_t)(b->dx + 0x32A);
                cyc += 6;
            }
        }
    } else {
        cyc += 4 + 5;
        if ((int16_t)b->cx < 0) {
            cyc += 16;
            b->cx = (uint16_t)-b->cx;
            cyc += 5 + 5;
            bx = (int16_t)b->bx, cx = (int16_t)b->cx;
            if (bx < cx) {
                cyc += 16;
                if (!bearing_divide(pc, b, false, 0x2559))
                    return false;
                b->dx = (uint16_t)(0x21C - b->dx);
                cyc += 5 + 6;
            } else {
                cyc += 4;
                JCC(bx == cx);
                if (bx != cx && !bearing_divide(pc, b, true, 0x2551))
                    return false;
                b->dx = (uint16_t)(b->dx + 0x10E);
                cyc += 6;
            }
        } else {
            cyc += 4 + 5;
            bx = (int16_t)b->bx, cx = (int16_t)b->cx;
            if (bx < cx) {
                cyc += 16;
                if (!bearing_divide(pc, b, false, 0x2545))
                    return false;
            } else {
                cyc += 4;
                JCC(bx == cx);
                if (bx != cx && !bearing_divide(pc, b, true, 0x253B))
                    return false;
                b->dx = (uint16_t)(0x10E - b->dx);
                cyc += 5 + 6;
            }
        }
    }
    cyc += 20;
    return true;
}

#define CYC_NAV_COMPUTE 115 /* no station, no glide slope */

/* 0050:2290 nav_compute (see the notes for the maths). Outputs: [1A11] needle target
 * 0..3Ch (1Eh centred), [1A14] TO/FROM flag 0 off / 1 / 2, [1A13] glide slope target
 * 0..18h, [1A21]/[1A23] north/east offsets from the station (16 m units). Consumes the
 * scenery's [1A1F] (localizer course) and [1A1D] (glide slope angle). */
static void n_nav_compute(Pc *pc)
{
    uint16_t ax = REG(AX), bx = REG(BX), cx = REG(CX), dx = REG(DX), si = REG(SI), di = REG(DI);
    uint16_t ss = SREG(SS), sp = REG(SP);
    cyc = 14;
    bool found = gs_nav_found(pc) != 0;
    JCC(found);
    bool off = true; /* jump to 2394 */
    if (found) {
        /* (pos - station) << 4: high words in 16 m units */
        uint32_t dn = ((uint32_t)m16(pc, (uint16_t)(GS_POS_NORTH + 2)) << 16 | m16(pc, GS_POS_NORTH)) -
                      ((uint32_t)gs_nav_station_w(pc, 3) << 16 | gs_nav_station_w(pc, 2));
        uint32_t de = ((uint32_t)m16(pc, (uint16_t)(GS_POS_EAST + 2)) << 16 | m16(pc, GS_POS_EAST)) -
                      ((uint32_t)gs_nav_station_w(pc, 1) << 16 | gs_nav_station_w(pc, 0));
        dn <<= 4;
        de <<= 4;
        ax = (uint16_t)dn, cx = (uint16_t)(dn >> 16);
        dx = (uint16_t)de, bx = (uint16_t)(de >> 16);
        uint16_t n21 = cx, n23 = bx;
        cyc += 12 + 17 + 18 + 18 + 64 + 18 + 17 + 17 + 18 + 18 + 64 + 18;
        dx = (uint16_t)((dx & 0xFF00) | 0x55);
        cyc += 4;
        /* scale up while both high bytes are within -10h..10h */
        for (;;) {
            ax = (uint16_t)((ax & 0xFF00) | (uint8_t)((cx >> 8) + 0x10));
            cyc += 4 + 6 + 6;
            bool done = (uint8_t)ax > 0x20;
            JCC(done);
            if (done)
                break;
            ax = (uint16_t)((ax & 0xFF00) | (uint8_t)((bx >> 8) + 0x10));
            cyc += 4 + 6 + 6;
            done = (uint8_t)ax > 0x20;
            JCC(done);
            if (done)
                break;
            uint32_t v = ((uint32_t)cx << 16 | ax) << 1;
            ax = (uint16_t)v, cx = (uint16_t)(v >> 16);
            v = ((uint32_t)bx << 16 | dx) << 1;
            dx = (uint16_t)v, bx = (uint16_t)(v >> 16);
            cyc += 32 + 17;
        }
        Bearing b = { .ax = ax, .bx = bx, .cx = cx, .dx = dx, .si = si, .di = di };
        cyc += 21; /* call sub_2525 */
        if (!bearing(pc, &b)) {
            /* nothing written yet: run the original, which takes the INT 0 */
            run_instead(pc, CYC_NAV_COMPUTE);
            return;
        }
        gs_set_nav_dn(pc, n21);
        gs_set_nav_de(pc, n23);
        wr16(pc, ss, (uint16_t)(sp - 2), 0x2300);
        if (b.inner_ret)
            wr16(pc, ss, (uint16_t)(sp - 4), b.inner_ret);
        ax = b.ax, bx = b.bx, cx = b.cx, dx = b.dx, si = b.si, di = b.di;

        /* magnetic: subtract the area's variation, wrap to 0..437h */
        bx = dx;
        ax = (uint16_t)(gs_wind_gust_offset(pc) + gs_wind_dir_offset(pc));
        uint32_t p = (uint32_t)ax * 0x438;
        ax = (uint16_t)p, dx = (uint16_t)(p >> 16);
        bx = (uint16_t)(bx - dx);
        cyc += 4 + 12 + 18 + 4 + 120 + 5;
        bool neg = (int16_t)bx < 0;
        JCC(neg);
        if (!neg) {
            cyc += 6;
            bool lt = (int16_t)bx < 0x438;
            JCC(lt);
            if (!lt) {
                bx = (uint16_t)(bx - 0x870);
                cyc += 6;
                neg = true;
            }
        }
        if (neg) {
            bx = (uint16_t)(bx + 0x438);
            cyc += 6;
        }

        /* course: the localizer's from the scenery for x.x5 frequencies, else the OBI */
        cyc += 19;
        bool ils = gs_nav_freq_ch(pc, 6) == 0x35;
        JCC(!ils);
        if (ils) {
            ax = gs_ils_course(pc);
            gs_set_ils_course(pc, 0);
            cyc += 5 + 26 + 17;
        } else {
            ax = (uint16_t)(m8(pc, GS_OBI_COURSE) * 6);
            cx = (uint16_t)((cx & 0xFF00) | 6);
            cyc += 12 + 4 + 72;
        }

        /* deviation, wrapped to -21Ch..21Bh */
        bx = (uint16_t)(bx - ax);
        cyc += 5;
        neg = (int16_t)bx < 0;
        JCC(neg);
        if (!neg) {
            cyc += 6;
            bool le = (int16_t)bx <= 0x21B;
            JCC(le);
            if (!le) {
                bx = (uint16_t)(bx - 0x438);
                cyc += 6 + 17;
            }
        } else {
            cyc += 6;
            bool ge = (int16_t)bx >= -0x21C;
            JCC(ge);
            if (!ge) {
                bx = (uint16_t)(bx + 0x438);
                cyc += 6;
            }
        }

        /* within 80 deg: flag 2; 80..100 deg: off; beyond: flag 1 against the reciprocal */
        cyc += 5;
        int16_t d = (int16_t)bx;
        JCC(d < 0);
        uint8_t flag;
        if (d >= 0) {
            cyc += 6;
            JCC(d <= 0xF0);
            if (d <= 0xF0) {
                flag = 2;
            } else {
                cyc += 6;
                JCC(d <= 0x12C);
                flag = d <= 0x12C ? 0 : 1;
                if (flag) {
                    bx = (uint16_t)-(uint16_t)(bx - 0x21C);
                    cyc += 17 + 6 + 5;
                }
            }
        } else {
            cyc += 6;
            JCC(d >= -0xF0);
            if (d >= -0xF0) {
                flag = 2;
            } else {
                cyc += 6;
                JCC(d >= -0x12C);
                flag = d >= -0x12C ? 0 : 1;
                if (flag) {
                    bx = (uint16_t)-(uint16_t)(bx + 0x21C);
                    cyc += 17 + 6 + 5;
                }
            }
        }
        if (flag != 0) {
            gs_set_nav_flag(pc, flag);
            cyc += 19 + 17;
            off = false;
        }
    } else {
        cyc += 17; /* jmp 2394 */
    }
    if (off) {
        gs_set_nav_flag(pc, 0);
        bx = 0;
        cyc += 19 + 5;
    }

    /* needle: deviation clamped to +-1Eh (10 deg), 1Eh - deviation */
    ax = bx;
    cyc += 4 + 5;
    if ((int16_t)ax >= 0) {
        cyc += 4 + 6;
        bool le = (int16_t)ax <= 0x1E;
        JCC(le);
        if (!le) {
            ax = 0x1E;
            cyc += 4 + 17;
        }
    } else {
        cyc += 16 + 6;
        bool ge = (int16_t)ax >= -0x1E;
        JCC(ge);
        if (!ge) {
            ax = (uint16_t)-0x1E;
            cyc += 4;
        }
    }
    ax = (uint16_t)(0x1E - ax);
    gs_set_loc_target(pc, (uint8_t)ax);
    cyc += 5 + 6 + 12;

    /* glide slope, when the scenery stored its angle this pass */
    ax = gs_ils_param(pc);
    gs_set_ils_param(pc, 0);
    cyc += 5 + 26 + 5;
    JCC(ax == 0);
    if (ax != 0) {
        di = ax;
        bx = (uint16_t)gs_nav_dn(pc);
        cyc += 4 + 17 + 5;
        JCC((int16_t)bx >= 0);
        if ((int16_t)bx < 0) {
            bx = (uint16_t)-bx;
            cyc += 5;
        }
        ax = (uint16_t)gs_nav_de(pc);
        cyc += 12 + 5;
        JCC((int16_t)ax >= 0);
        if ((int16_t)ax < 0) {
            ax = (uint16_t)-ax;
            cyc += 5;
        }
        cyc += 5;
        JCC((int16_t)bx >= (int16_t)ax);
        if ((int16_t)bx < (int16_t)ax) {
            bx = ax;
            cyc += 4;
        }
        dx = (uint16_t)(gs_alt_m(pc) >> 1);
        cyc += 17 + 8 + 5;
        bool below = (int16_t)dx < (int16_t)bx;
        JCC(below);
        uint8_t bl;
        if (!below) {
            bl = 0x18;
            cyc += 4 + 17;
        } else {
            uint32_t n = (uint32_t)dx << 16;
            ax = (uint16_t)(n / bx); /* dx < bx: no overflow */
            dx = (uint16_t)(n % bx);
            ax = (uint16_t)(ax - di + 0x1800);
            cyc += 5 + 146 + 5 + 6 + 6;
            bool in = ax < 0x3000;
            JCC(in);
            if (in) {
                bl = (uint8_t)(ax >> 8);
                cyc += 4;
            } else {
                bl = 0x30;
                cyc += 4 + 5;
                bool pos = (int16_t)ax >= 0;
                JCC(pos);
                if (!pos) {
                    ax = 0;
                    bl = 0;
                    cyc += 5 + 4;
                }
            }
            bl >>= 1;
            cyc += 8;
        }
        bx = (uint16_t)((bx & 0xFF00) | bl);
        gs_set_gs_target(pc, bl);
        cyc += 18;
    }
    cyc += 20;

    REG(AX) = ax, REG(BX) = bx, REG(CX) = cx, REG(DX) = dx, REG(SI) = si, REG(DI) = di;
    charge(pc, CYC_NAV_COMPUTE);
    native_ret(pc);
}

/* ---- OBI display: obi_check 240E, obi_update 2419 ------------------------------------ */

#define CYC_OBI_UPDATE 400
#define CYC_OBI_CHECK 55

/* 0050:2419 / 240E (obi_update / obi_check) are bound by gauges.c (3.11). */

/* ---- ATIS: atis_tick 4E53, atis_start 4FBF ------------------------------------------- */

/* sub_4F68: copies the 50h-byte line buffer at SI to ES:BX rotated by [3622]/[3624]. */
static void atis_copy_line(Pc *pc, uint16_t ret_ip)
{
    call_push(pc, ret_ip);
    int16_t step = (pc->cpu.flags & F_DF) ? -1 : 1;
    uint16_t es = SREG(ES), ds = SREG(DS);
    for (int part = 0; part < 2; part++) {
        REG(DI) = part ? REG(BX) : (uint16_t)(REG(BX) + gs_atis_ring_a(pc));
        REG(CX) = m16(pc, part ? 0x3622 : 0x3624);
        cyc += part ? 4 + 17 : 4 + 18 + 17;
        cyc += 2 + 17u * REG(CX);
        while (REG(CX)) {
            wr8(pc, es, REG(DI), rd8(pc, ds, REG(SI)));
            REG(SI) = (uint16_t)(REG(SI) + step);
            REG(DI) = (uint16_t)(REG(DI) + step);
            REG(CX)--;
        }
    }
    ret_pop(pc);
}

/* rep stosw of CX = 28h zero words at ES:DI. */
static void atis_clear_line(Pc *pc, uint16_t di)
{
    REG(DI) = di;
    REG(CX) = 0x28;
    REG(AX) = 0;
    cyc += 4 + 4 + 5 + 2 + 10 * 0x28;
    int16_t step = (pc->cpu.flags & F_DF) ? -2 : 2;
    for (; REG(CX); REG(CX)--) {
        wr16(pc, SREG(ES), REG(DI), 0);
        REG(DI) = (uint16_t)(REG(DI) + step);
    }
}

/* sub_4F7D: AL = the next ATIS byte at [3626] -> AL = the character to draw. Bytes 80h+k
 * start fragment k (pointer table 362D, then [362A] = 1 while it runs). A 00 at the top
 * level ends the message: [3628] = 1 and it returns from atis_tick (returns false). */
static bool atis_next_char(Pc *pc)
{
    call_push(pc, 0x4F21);
    cyc += 19;
    bool frag = gs_atis_in_fragment(pc) != 0;
    JCC(!frag);
    if (frag) {
        uint16_t p = gs_atis_frag_ptr(pc);
        REG(SI) = p;
        gs_set_atis_frag_ptr(pc, (uint16_t)(p + 1));
        SET_LO(AX, m8(pc, p));
        cyc += 17 + 12 + 17 + 5;
        JCC(LO(AX) != 0);
        if (LO(AX) == 0) {
            gs_set_atis_in_fragment(pc, (uint8_t)(gs_atis_in_fragment(pc) - 1));
            SET_LO(AX, 0x20);
            cyc += 12 + 4;
        }
        ret_pop(pc);
        return true;
    }
    gs_set_atis_ptr(pc, (uint16_t)(gs_atis_ptr(pc) + 1));
    cyc += 12 + 5;
    uint8_t al = LO(AX);
    JCC(al & 0x80);
    if (al & 0x80) {
        REG(AX) = (uint16_t)((REG(AX) & 0x7F) << 1);
        REG(SI) = REG(AX);
        REG(AX) = m16(pc, (uint16_t)(REG(SI) + 0x362D));
        gs_set_atis_frag_ptr(pc, REG(AX));
        gs_set_atis_in_fragment(pc, (uint8_t)(gs_atis_in_fragment(pc) + 1));
        SET_LO(AX, 0x20);
        cyc += 6 + 8 + 4 + 17 + 12 + 12 + 4;
        ret_pop(pc);
        return true;
    }
    JCC(al != 0);
    if (al != 0) {
        ret_pop(pc);
        return true;
    }
    gs_set_atis_idle(pc, 1);
    REG(AX) = cpu_pop(&pc->cpu);
    cyc += 19 + 12 + 20;
    return false; /* its RET returns from atis_tick (the native's native_ret) */
}

#define CYC_ATIS_TICK 54 /* no message running, 3629 still counting */

static void atis_tick_body(Pc *pc)
{
    cyc = 14;
    bool editor = gs_editor_active(pc) != 0;
    JCC(editor);
    if (editor) {
        cyc += 20;
        return;
    }
    SREG(ES) = gs_screen_seg(pc);
    cyc += 11 + 14;
    bool idle = gs_atis_idle(pc) != 0;
    JCC(!idle);
    if (idle) {
        uint8_t n = (uint8_t)(gs_atis_idle_count(pc) - 1);
        gs_set_atis_idle_count(pc, n);
        cyc += 12;
        JCC(n != 0);
        if (n == 0) {
            /* message over: hold the counter at 1 and restore the full view window */
            gs_set_atis_idle_count(pc, 1);
            gs_set_blit_words0(pc, 0x0848);
            gs_set_blit_words1(pc, 0x0848);
            gs_set_blit_off0(pc, 0x0000);
            gs_set_blit_off1(pc, 0x2000);
            gs_set_atis_in_fragment(pc, 0);
            cyc += 12 + 19 * 5 + 20;
            return;
        }
    } else {
        gs_set_atis_idle_count(pc, 0x4E);
        cyc += 19;
    }

    /* scroll: redraw the 5 line buffers rotated, then advance the ring by one byte */
    SREG(ES) = gs_atis_seg(pc);
    cyc += 11;
    atis_clear_line(pc, 0);
    static const uint16_t dst[5] = { 0x2000, 0x0050, 0x2050, 0x00A0, 0x20A0 };
    static const uint16_t rets[5] = { 0x4EA9, 0x4EB2, 0x4EBB, 0x4EC4, 0x4ECD };
    for (int i = 0; i < 5; i++) {
        REG(BX) = dst[i];
        REG(SI) = (uint16_t)(0x3492 + 0x50 * i);
        cyc += 4 + 4;
        atis_copy_line(pc, rets[i]);
    }
    atis_clear_line(pc, 0x00F0);
    gs_set_atis_ring_b(pc, (uint16_t)(gs_atis_ring_b(pc) + 1));
    uint16_t start = (uint16_t)(gs_atis_ring_a(pc) - 1);
    gs_set_atis_ring_a(pc, start);
    cyc += 12 + 12;
    JCC((int16_t)start >= 0);
    if ((int16_t)start < 0) {
        gs_set_atis_ring_b(pc, 0);
        gs_set_atis_ring_a(pc, 0x50);
        cyc += 19 + 19;
    }
    uint16_t bx = gs_atis_ring_b(pc);
    REG(BX) = bx;
    for (int i = 0; i < 5; i++)
        w8(pc, (uint16_t)(bx + 0x3492 + 0x50 * i), 0);
    cyc += 17 + 5 * 19 + 7;
    JCC(bx & 1);
    if (!(bx & 1)) {
        cyc += 20;
        return;
    }

    /* every second step: the next character into the right end of the line buffers */
    cyc += 14;
    bool done = gs_atis_idle(pc) != 0;
    JCC(done);
    if (done) {
        cyc += 20;
        return;
    }
    REG(SI) = gs_atis_ptr(pc);
    SET_LO(AX, m8(pc, REG(SI)));
    cyc += 17 + 17;
    if (!atis_next_char(pc))
        return;
    gs_set_blit_words0(pc, 0x07A8);
    gs_set_blit_words1(pc, 0x07D0);
    gs_set_blit_off0(pc, 0x0140);
    gs_set_blit_off1(pc, 0x20F0);
    bx = (uint16_t)(bx - 1);
    REG(BX) = bx;
    REG(AX) = (uint16_t)(uint8_t)(LO(AX) - 0x20);
    REG(CX) = REG(AX);
    REG(AX) = (uint16_t)(((REG(AX) << 2) + REG(CX)) << 1);
    REG(SI) = REG(AX);
    cyc += 19 * 4 + 2 + 5 + 6 + 4 + 16 + 5 + 8 + 4 + 6;
    /* the font: 5 words per character from DS:0000, one per line buffer */
    int16_t step = (pc->cpu.flags & F_DF) ? -2 : 2;
    for (int i = 0; i < 5; i++) {
        REG(AX) = m16(pc, REG(SI));
        REG(SI) = (uint16_t)(REG(SI) + step);
        w16(pc, (uint16_t)(bx + 0x3492 + 0x50 * i), REG(AX));
        cyc += 15 + 18;
    }
    cyc += 20;
}

/* 0050:4E53 atis_tick (from int8_timer): scrolls the ATIS message across the top of the
 * view, one pixel pair per call and a new character every second call. */
static void n_atis_tick(Pc *pc)
{
    atis_tick_body(pc);
    charge(pc, CYC_ATIS_TICK);
    native_ret(pc);
}

/* sub_3553 / sub_3555: AX (3553: AL) -> CL = thousands, CH = hundreds, AX = tens and units
 * as ASCII in memory order. BP = 64h, DX = the last remainder. The CWD + DIV would overflow
 * for AX >= 8000h; atis_start passes at most 865h. */
static void num_digits(Pc *pc, bool byte_arg, uint16_t ret_ip)
{
    call_push(pc, ret_ip);
    if (byte_arg) {
        SET_HI(AX, 0);
        cyc += 5;
    }
    uint16_t v = REG(AX);
    SET_LO(CX, (uint8_t)(v / 1000));
    v %= 1000;
    SET_HI(CX, (uint8_t)(v / 100));
    v %= 100;
    REG(BP) = 0x64;
    REG(DX) = v;
    REG(AX) = (uint16_t)((v / 10) << 8 | (v % 10));
    REG(AX) = (uint16_t)(REG(AX) + 0x3030);
    REG(CX) = (uint16_t)(REG(CX) + 0x3030);
    REG(AX) = (uint16_t)(REG(AX) << 8 | REG(AX) >> 8);
    cyc += 5 + 4 + 146 + 4 + 4 + 5 + 4 + 146 + 4 + 4 + 85 + 6 + 6 + 6;
    ret_pop(pc);
}

#define CYC_ATIS_START 1700

static void atis_start_body(Pc *pc)
{
    /* temperature: by season [206A]-1, plus [202A] */
    uint8_t al = (uint8_t)((gs_season(pc) - 1) & 3);
    REG(BX) = al;
    SET_LO(AX, (uint8_t)(m8(pc, (uint16_t)(REG(BX) + 0x31E4)) + m8(pc, 0x202A)));
    cyc += 12 + 5 + 6 + 4 + 5 + 17 + 18;
    num_digits(pc, true, 0x4FD5);
    w16(pc, 0x367A, REG(AX));
    cyc += 12;

    /* wind direction in degrees */
    uint32_t p = (uint32_t)0x168 * gs_wind_layers_w(pc, 7);
    REG(AX) = (uint16_t)(p >> 16);
    REG(DX) = (uint16_t)(p >> 16);
    cyc += 4 + 127 + 4;
    num_digits(pc, false, 0x4FE4);
    w16(pc, 0x36EA, REG(AX));
    cyc += 12 + 6;
    JCC(HI(CX) != 0x30);
    if (HI(CX) == 0x30) {
        SET_HI(CX, 0x20);
        cyc += 4;
    }
    w8(pc, 0x36E9, HI(CX));
    cyc += 18;

    /* wind speed */
    SET_LO(AX, gs_wind_layers_b(pc, 12));
    cyc += 12;
    num_digits(pc, true, 0x4FF8);
    cyc += 6;
    JCC(LO(AX) != 0x30);
    if (LO(AX) == 0x30) {
        SET_LO(AX, 0x20);
        cyc += 4;
    }
    w16(pc, 0x36F0, REG(AX));
    cyc += 12;

    /* time in Zulu: clock_hour + utc_offset */
    SET_LO(AX, gs_clock_hour(pc));
    REG(AX) = (uint16_t)(REG(AX) + gs_utc_offset(pc));
    cyc += 12 + 18 + 6;
    bool lt = (int8_t)LO(AX) < 0x18;
    JCC(lt);
    if (!lt) {
        SET_LO(AX, (uint8_t)(LO(AX) - 0x18));
        cyc += 6;
    }
    num_digits(pc, true, 0x5011);
    w16(pc, 0x3662, REG(AX));
    cyc += 12;

    /* ceiling: [2071] * 866h / 10000h hundreds of feet, blank when 0 */
    p = (uint32_t)0x866 * m16(pc, 0x2071);
    REG(AX) = (uint16_t)(p >> 16);
    REG(DX) = (uint16_t)(p >> 16);
    cyc += 4 + 127 + 4 + 5;
    JCC(REG(AX) != 0);
    if (REG(AX) == 0) {
        w16(pc, 0x36F6, 0x0020);
        cyc += 19 + 17;
    } else {
        w16(pc, 0x36F6, 0x2020);
        cyc += 19;
        num_digits(pc, false, 0x5032);
        cyc += 6;
        JCC(HI(CX) != 0x30);
        if (HI(CX) == 0x30) {
            SET_HI(CX, 0x20);
            cyc += 4 + 6;
            JCC(LO(AX) != 0x30);
            if (LO(AX) == 0x30) {
                SET_LO(AX, 0x20);
                cyc += 4;
            }
        }
        w8(pc, 0x3709, HI(CX));
        w16(pc, 0x370A, REG(AX));
        cyc += 18 + 12;
    }

    /* runway in use: the station's runway for the wind quadrant [2087] >> 14 */
    REG(AX) = (uint16_t)((gs_wind_layers_w(pc, 7) >> 8) << 2);
    REG(BX) = (uint16_t)(REG(AX) >> 8);
    SET_LO(AX, m8(pc, (uint16_t)(REG(BX) + 0x31E0)));
    cyc += 12 + 4 + 5 + 16 + 4 + 5 + 17;
    num_digits(pc, true, 0x505C);
    w16(pc, 0x36A9, REG(AX));
    cyc += 12 + 20;
}

/* 0050:4FBF atis_start (from op_com_station): formats the variable ATIS fields. */
static void n_atis_start(Pc *pc)
{
    cyc = 0;
    atis_start_body(pc);
    charge(pc, CYC_ATIS_START);
    native_ret(pc);
}

NativeEntry native_radio[] = {
    { .name = "nav_tune", .seg = GAME_CS, .off = 0x1447, .fn = n_nav_tune, .enabled = true,
      .cycles = CYC_NAV_TUNE },
    { .name = "nav_search_start", .seg = GAME_CS, .off = 0x147D, .fn = n_nav_search_start, .enabled = true,
      .cycles = CYC_NAV_SEARCH },
    { .name = "com_tune", .seg = GAME_CS, .off = 0x1488, .fn = n_com_tune, .enabled = true,
      .cycles = CYC_COM_TUNE },
    { .name = "obi_readout", .seg = GAME_CS, .off = 0x14C9, .fn = n_obi_readout, .enabled = true,
      .cycles = CYC_OBI_READOUT },
    { .name = "nav_compute", .seg = GAME_CS, .off = 0x2290, .fn = n_nav_compute, .enabled = true,
      .cycles = CYC_NAV_COMPUTE },
    /* 240E/2419 (OBI check/display) are bound by gauges.c (3.11). */
    { .name = "atis_tick", .seg = GAME_CS, .off = 0x4E53, .fn = n_atis_tick, .enabled = true,
      .cycles = CYC_ATIS_TICK },
    { .name = "atis_start", .seg = GAME_CS, .off = 0x4FBF, .fn = n_atis_start, .enabled = true,
      .cycles = CYC_ATIS_START },
    { .name = NULL },
};
