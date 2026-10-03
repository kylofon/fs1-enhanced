/* Natives for subphase 3.5 horizon and sky/ground (see docs/PHASE3_PLAN.md and
 * docs/subphases/3.5.md).
 *
 * draw_sky_ground (2AEA) -> horizon_fill (2B0D) -> fill_rows (2C68) / fill_sloped_horizon (2C9E),
 * plus radar_clear (2AB0), which draw_sky_ground jumps to in radar view.
 *
 * Every routine is transliterated so that the registers and memory end up exactly as the
 * original leaves them, including the words its CALLs and PUSHes leave below SP (written
 * through a virtual SP; the real SP only moves for the emulated scenery_interp call and is
 * back at its entry value before native_ret). Each routine adds the emulated cycles the
 * original takes on the path it ran (the cycle model of cpu8086.c), so the timeline stays the
 * same.
 *
 * scenery_interp (3.8, capture step), fill_rows (3.3) and draw_horizon_list (3.4) are called
 * through native_call. When that is unavailable (the native is off), the interpreter is run
 * as original code by single-stepping from 3CC0 with natives off, and fill_rows and the
 * horizon list run from the local C copies below (draw_line's cost computed per path).
 *
 * None of the callers test flags after these routines (main_loop calls draw_view_frame next;
 * horizon_fill's own calls are followed by POP/CMP/RET), so flag_mask is 0 and the flags are
 * not reproduced. */
#include "native.h"
#include "game/state.h"
#include "raster.h"

#define ROW_OFFSETS 0x380C
#define SCENERY_INTERP 0x3CC0

static uint32_t cyc; /* original cycles of the routine being run */

#define REG(r) (pc->cpu.regs[R_##r])
#define SREG(r) (pc->cpu.sregs[S_##r])
#define JCC(taken) (cyc += (taken) ? 16 : 4)

static uint8_t lo8(uint16_t v) { return (uint8_t)v; }
static uint8_t hi8(uint16_t v) { return (uint8_t)(v >> 8); }
static void set_lo(uint16_t *r, uint8_t b) { *r = (uint16_t)((*r & 0xFF00) | b); }
static void set_hi(uint16_t *r, uint8_t b) { *r = (uint16_t)((*r & 0x00FF) | b << 8); }
#define LO(r) lo8(REG(r))
#define HI(r) hi8(REG(r))
#define SET_LO(r, b) set_lo(&REG(r), (uint8_t)(b))
#define SET_HI(r, b) set_hi(&REG(r), (uint8_t)(b))

static uint8_t rd8(Pc *pc, uint16_t seg, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(seg, off)); }
static uint16_t rd16(Pc *pc, uint16_t seg, uint16_t off) { return mem_read16(pc, seg, off); }
static void wr8(Pc *pc, uint16_t seg, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(seg, off), v); }
static void wr16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v) { mem_write16(pc, seg, off, v); }
static uint16_t dsr16(Pc *pc, uint16_t off) { return rd16(pc, SREG(DS), off); }
static void dsw16(Pc *pc, uint16_t off, uint16_t v) { wr16(pc, SREG(DS), off, v); }

/* PUSH / POP through the virtual stack pointer *sp. */
static void vpush(Pc *pc, uint16_t *sp, uint16_t v)
{
    *sp = (uint16_t)(*sp - 2);
    wr16(pc, SREG(SS), *sp, v);
}
static uint16_t vpop(Pc *pc, uint16_t *sp)
{
    uint16_t v = rd16(pc, SREG(SS), *sp);
    *sp = (uint16_t)(*sp + 2);
    return v;
}

/* STOSW / STOSB at ES:DI, honouring DF. */
static void stosw(Pc *pc)
{
    wr16(pc, SREG(ES), REG(DI), REG(AX));
    REG(DI) = (uint16_t)(REG(DI) + ((pc->cpu.flags & F_DF) ? -2 : 2));
}
static void stosb(Pc *pc)
{
    wr8(pc, SREG(ES), REG(DI), LO(AX));
    REG(DI) = (uint16_t)(REG(DI) + ((pc->cpu.flags & F_DF) ? -1 : 1));
}
/* REP STOSW: 2 cycles for the instruction, 10 per word; CX ends 0. */
static void rep_stosw(Pc *pc)
{
    cyc += 2 + 10u * REG(CX);
    for (; REG(CX); REG(CX)--)
        stosw(pc);
}

/* ---- calls into other natives ----------------------------------------------------- */

/* Emulates "CALL off" with SP = sp before the CALL through native_call (the other area's C
 * version). Returns false, with nothing changed but the return address written below sp,
 * when that native is off (or --verify is stepping original code). Its cycles, CALL
 * included, go to cyc. IP, CS and SP are as before. */
static bool call_native(Pc *pc, uint16_t sp, uint16_t ret_ip, uint16_t off)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ip = c->ip, cs = c->sregs[S_CS], sp0 = c->regs[R_SP];
    uint64_t start = c->cycles;
    c->regs[R_SP] = sp;
    cpu_push(c, ret_ip);
    bool ok = native_call(pc, off);
    if (ok)
        cyc += 21 + (uint32_t)(c->cycles - start);
    c->cycles = start;
    c->regs[R_SP] = sp0;
    c->ip = ip;
    c->sregs[S_CS] = cs;
    return ok;
}

/* ---- the scenery interpreter, original code ------------------------------------------ */

/* Emulates "CALL 3CC0" with SP = sp before the CALL: pushes ret_ip, then single-steps the
 * original interpreter (natives off, so a nested --verify cannot start) until it returns to
 * ret_ip. Its cycles, CALL and RET included, go to cyc. On return SP = sp again. */
static void call_scenery_interp(Pc *pc, uint16_t sp, uint16_t ret_ip)
{
    if (call_native(pc, sp, ret_ip, SCENERY_INTERP))
        return;
    Cpu8086 *c = &pc->cpu;
    const uint8_t *map = c->hook_map;
    uint16_t ip = c->ip, cs = c->sregs[S_CS];
    uint64_t start = c->cycles;
    c->regs[R_SP] = sp;
    cpu_push(c, ret_ip);
    c->ip = SCENERY_INTERP;
    c->sregs[S_CS] = GAME_CS;
    c->hook_map = NULL;
    for (long n = 0; n < 5000000L; n++) {
        native_or_cpu_step(pc);
        uint16_t d = (uint16_t)(c->regs[R_SP] - (uint16_t)(sp - 2));
        if (c->ip == ret_ip && c->sregs[S_CS] == GAME_CS && d >= 2 && d < 0x8000)
            break;
    }
    c->hook_map = map;
    cyc += 21 + (uint32_t)(c->cycles - start);
    c->cycles = start;
    c->ip = ip;
    c->sregs[S_CS] = cs;
}

/* ---- fill_rows (0050:2C68, also native in buffer.c) ---------------------------------- */

/* Rows BL..DL of ES:row_offsets with the word pattern BP, 40 words per row: ceil(n/2) rows
 * from row_offsets[BL], then floor(n/2) of the other bank from row_offsets[BL+1].
 * Cycles: 223 + 10 per word, +6 if DL-BL+2 is odd, +10 if the second bank is empty (RET
 * included). */
static void fill_rows_body(Pc *pc)
{
    uint8_t bl = LO(BX);
    uint8_t dl = (uint8_t)(LO(DX) - bl + 2);
    uint16_t bx = (uint16_t)(bl << 1);
    REG(DI) = dsr16(pc, (uint16_t)(bx + ROW_OFFSETS));
    bx = (uint16_t)(bx + 2);
    REG(SI) = dsr16(pc, (uint16_t)(bx + ROW_OFFSETS));
    REG(BX) = bx;
    uint8_t cl = (uint8_t)((int8_t)dl >> 1);
    uint16_t ax = (uint16_t)(0x28 * cl);
    uint16_t dx = ax;
    cyc += 223 - 2 - 2; /* the two REP STOSW are charged by rep_stosw */
    if (dl & 1)
        cyc += 6;
    else
        dx = (uint16_t)(dx - 0x28);
    REG(CX) = ax;
    REG(AX) = REG(BP);
    rep_stosw(pc);
    REG(CX) = dx;
    REG(DI) = REG(SI);
    if (dx)
        rep_stosw(pc);
    else
        cyc += 10 + 2; /* JE taken instead of JE not taken plus an empty REP */
    REG(DX) = dx;
}

/* CALL fill_rows from the virtual stack pointer sp. */
static void call_fill_rows(Pc *pc, uint16_t sp, uint16_t ret_ip)
{
    if (call_native(pc, sp, ret_ip, 0x2C68))
        return;
    vpush(pc, &sp, ret_ip);
    cyc += 21;
    fill_rows_body(pc);
}

/* ---- fill_sloped_horizon (0050:2C9E) -------------------------------------------------- */

/* Rows BL..CL: BH = boundary x (in bytes) at row BL, AH = at row CL. The step per row
 * (8.8 fixed point, IDIV) goes to [1D4D]. Each row is first advanced by one step, then
 * filled with x bytes of pattern BP and 80-x bytes of pattern [1D44] (a word fill plus a
 * byte for odd counts). Ends with AX = [1D44], BP unchanged, CX = 0, SI = FFFF,
 * BX = 2 * (row after the last), DX = the last boundary (8.8), DI past the last row. */
static void fill_sloped_body(Pc *pc)
{
    uint8_t cl = (uint8_t)(LO(CX) - LO(BX) + 1);
    REG(CX) = cl;
    uint8_t ah = (uint8_t)(HI(AX) - HI(BX));
    int32_t num = (int16_t)(ah << 8);
    int32_t q = num / (int16_t)REG(CX);
    /* sub, inc, xor, sub, xor, cwd, idiv, dec: 5+5+5+5+5+5+167+5 */
    cyc += 202;
    REG(AX) = (uint16_t)q;
    REG(DX) = (uint16_t)(num % (int16_t)REG(CX));
    SET_LO(CX, LO(CX) - 1);
    REG(DX) = (uint16_t)(HI(BX) << 8);
    REG(BX) = (uint16_t)((REG(BX) & 0xFF) << 1);
    gs_set_horizon_slope(pc, REG(AX));
    REG(SI) = REG(CX);
    REG(AX) = gs_band_patterns_w(pc, 2);
    SET_HI(CX, 0);
    /* mov dh,bh; xor dl,dl; mov bh,dl; shl bx,1; mov [1D4D],ax; mov si,cx; mov ax,[1D44];
     * xor ch,ch: 4+5+4+8+12+4+12+5 */
    cyc += 54;
    uint16_t t;
    for (;;) {
        REG(DX) = (uint16_t)(REG(DX) + gs_horizon_slope(pc));
        REG(DI) = dsr16(pc, (uint16_t)(REG(BX) + ROW_OFFSETS));
        SET_LO(CX, HI(DX));
        t = REG(BP), REG(BP) = REG(AX), REG(AX) = t;
        REG(CX) >>= 1;
        cyc += 18 + 17 + 4 + 3 + 8 + 5;
        JCC(REG(CX) == 0);
        if (REG(CX))
            rep_stosw(pc);
        cyc += 7;
        JCC(!(HI(DX) & 1));
        if (HI(DX) & 1) {
            stosb(pc);
            cyc += 12;
        }
        SET_HI(DX, (uint8_t)(0x50 - HI(DX)));
        SET_LO(CX, HI(DX));
        t = REG(BP), REG(BP) = REG(AX), REG(AX) = t;
        REG(CX) >>= 1;
        cyc += 5 + 6 + 4 + 3 + 8 + 5;
        JCC(REG(CX) == 0);
        if (REG(CX))
            rep_stosw(pc);
        cyc += 7;
        JCC(!(HI(DX) & 1));
        if (HI(DX) & 1) {
            stosb(pc);
            cyc += 12;
        }
        SET_HI(DX, (uint8_t)(0x50 - HI(DX)));
        REG(BX) = (uint16_t)(REG(BX) + 2);
        REG(SI)--;
        cyc += 6 + 5 + 6 + 2;
        JCC(!(REG(SI) & 0x8000));
        if (REG(SI) & 0x8000)
            break;
    }
    cyc += 20; /* RET */
}

static void call_fill_sloped(Pc *pc, uint16_t sp, uint16_t ret_ip)
{
    vpush(pc, &sp, ret_ip);
    cyc += 21;
    fill_sloped_body(pc);
}

/* ---- horizon_fill (0050:2B0D) ------------------------------------------------------- */

/* The band fill around the sloped rows, shared by the two cases (2BB7 and 2C27). CX, DX:
 * CH/DH = boundary x in bytes, CL/DL = row, CL < DL. top/sloped/bottom are the pattern
 * variables and r1..r3 the return addresses of the three CALLs. sp = SP of horizon_fill. */
static void bands(Pc *pc, uint16_t sp, uint16_t top, uint16_t bottom, uint16_t r1, uint16_t r2, uint16_t r3)
{
    uint16_t s = sp;
    cyc += 6; /* cmp cl,1 */
    JCC(LO(CX) <= 1);
    if (LO(CX) > 1) {
        vpush(pc, &s, REG(CX));
        vpush(pc, &s, REG(DX));
        SET_LO(BX, 0);
        SET_LO(DX, LO(CX) - 1);
        REG(BP) = dsr16(pc, top);
        cyc += 15 + 15 + 5 + 4 + 5 + 17;
        call_fill_rows(pc, s, r1);
        REG(DX) = vpop(pc, &s);
        REG(CX) = vpop(pc, &s);
        cyc += 12 + 12 + 17;
    } else {
        SET_LO(CX, 0);
        cyc += 5;
    }
    vpush(pc, &s, REG(CX));
    vpush(pc, &s, REG(DX));
    SET_LO(BX, LO(CX));
    SET_LO(CX, LO(DX));
    SET_HI(BX, HI(CX));
    SET_HI(AX, HI(DX));
    REG(BP) = gs_band_patterns_w(pc, 1);
    cyc += 15 + 15 + 4 + 4 + 4 + 4 + 17;
    call_fill_sloped(pc, s, r2);
    REG(DX) = vpop(pc, &s);
    REG(CX) = vpop(pc, &s);
    cyc += 12 + 12 + 6; /* pop, pop, cmp dl,6Ah */
    JCC(LO(DX) >= 0x6A);
    if (LO(DX) < 0x6A) {
        SET_LO(BX, LO(DX) + 1);
        SET_LO(DX, 0x6A);
        REG(BP) = dsr16(pc, bottom);
        cyc += 4 + 5 + 4 + 17;
        call_fill_rows(pc, s, r3);
    }
    cyc += 20; /* RET */
}

/* sp = SP at entry (the return address on top). */
static void horizon_fill_body(Pc *pc, uint16_t sp)
{
    SREG(ES) = REG(AX) = gs_view_buf_seg(pc);
    uint8_t bl = (uint8_t)(((uint8_t)(gs_view_angles_b(pc, 5) + 0x10) >> 4) & 0x0E);
    REG(BX) = bl;
    REG(AX) = dsr16(pc, (uint16_t)(bl + 0x1D4F));
    gs_set_scenery_ip(pc, REG(AX));
    gs_set_capture_ptr(pc, GS_HORIZON_LIST);
    gs_set_capture_mode(pc, 0xFF);
    REG(BX) = 0;
    /* mov ax,[3806]; mov es,ax; mov bl,[30FC]; add bl,10h; 4x shr bl,1; and bl,0Eh; xor bh,bh;
     * mov ax,[bx+1D4F]; mov [30C2],ax; mov [30C6],041F; mov [30CA],FF; xor bx,bx */
    cyc += 12 + 4 + 17 + 6 + 32 + 6 + 5 + 17 + 12 + 19 + 19 + 5;
    uint16_t s = sp;
    for (uint16_t v = 0x30EB; v <= 0x30EF; v = (uint16_t)(v + 2)) {
        REG(AX) = dsr16(pc, v);
        dsw16(pc, v, 0);
        vpush(pc, &s, REG(AX));
        cyc += 4 + 26 + 15; /* mov ax,bx; xchg [v],ax; push ax */
    }
    call_scenery_interp(pc, s, 0x2B52);
    for (uint16_t v = 0x30EF; v >= 0x30EB; v = (uint16_t)(v - 2)) {
        dsw16(pc, v, vpop(pc, &s));
        cyc += 26;
    }
    cyc += 19; /* cmp byte [041F],FF */
    bool none = gs_horizon_list_b(pc, 0) == 0xFF;
    JCC(!none);
    if (none) {
        /* off screen: rows 0..69h with one pattern, ground when [30F8] >= 0 */
        SET_LO(BX, 0);
        SET_LO(DX, 0x69);
        REG(BP) = gs_ground_pattern(pc);
        cyc += 4 + 4 + 17 + 14;
        bool neg = gs_view_angles_b(pc, 1) & 0x80;
        JCC(!neg);
        if (neg) {
            REG(BP) = gs_sky_pattern(pc);
            cyc += 17;
        }
        cyc += 17; /* jmp fill_rows: its RET returns to our caller */
        uint16_t ret = rd16(pc, SREG(SS), sp);
        if (!call_native(pc, (uint16_t)(sp + 2), ret, 0x2C68))
            fill_rows_body(pc);
        else
            cyc -= 21; /* a JMP, not a CALL */
        return;
    }
    REG(CX) = gs_horizon_list_w(pc, 0);
    REG(DX) = gs_horizon_list_w(pc, 1);
    REG(CX) = (uint16_t)(REG(CX) << 8 | REG(CX) >> 8);
    REG(DX) = (uint16_t)(REG(DX) << 8 | REG(DX) >> 8);
    SET_HI(CX, HI(CX) >> 1);
    SET_HI(DX, HI(DX) >> 1);
    SET_LO(AX, gs_horizon_x_limit(pc));
    cyc += 17 + 17 + 6 + 6 + 8 + 8 + 12 + 5; /* ... cmp ch,al */
    uint16_t t;
    bool le = HI(CX) <= LO(AX);
    JCC(le);
    if (!le) {
        t = REG(CX), REG(CX) = REG(DX), REG(DX) = t;
        cyc += 6 + 5;
        le = HI(CX) <= LO(AX);
        JCC(le);
    }
    if (!le) {
        /* 2B98: both ends right of [1D4C] */
        SET_LO(AX, gs_view_angles_b(pc, 3));
        REG(SI) = gs_ground_pattern(pc);
        REG(DI) = gs_sky_pattern(pc);
        cyc += 12 + 17 + 17 + 5;
        bool neg = LO(AX) & 0x80;
        JCC(!neg);
        if (neg) {
            t = REG(SI), REG(SI) = REG(DI), REG(DI) = t;
            cyc += 6;
        }
        gs_set_band_patterns_w(pc, 1, REG(SI));
        gs_set_band_patterns_w(pc, 2, REG(DI));
        cyc += 18 + 18 + 5;
        bool below = LO(CX) < LO(DX);
        JCC(below);
        if (!below) {
            t = REG(CX), REG(CX) = REG(DX), REG(DX) = t;
            cyc += 6;
        }
        bands(pc, sp, 0x1D42, 0x1D42, 0x2BCB, 0x2BE3, 0x2BF7);
        return;
    }
    /* 2BF8 */
    SET_LO(AX, gs_view_angles_b(pc, 3));
    REG(DI) = gs_ground_pattern(pc);
    REG(SI) = gs_sky_pattern(pc);
    SET_LO(AX, LO(AX) + 0x40);
    cyc += 12 + 17 + 17 + 6;
    bool neg = LO(AX) & 0x80;
    JCC(!neg);
    if (neg) {
        t = REG(SI), REG(SI) = REG(DI), REG(DI) = t;
        cyc += 6;
    }
    gs_set_band_patterns_w(pc, 0, REG(SI));
    gs_set_band_patterns_w(pc, 1, REG(DI));
    gs_set_band_patterns_w(pc, 2, REG(SI));
    gs_set_band_patterns_w(pc, 3, REG(DI));
    cyc += 4 * 18 + 5;
    bool below = LO(CX) < LO(DX);
    JCC(below);
    if (!below) {
        t = REG(CX), REG(CX) = REG(DX), REG(DX) = t;
        gs_set_band_patterns_w(pc, 1, REG(SI));
        gs_set_band_patterns_w(pc, 2, REG(DI));
        cyc += 6 + 18 + 18;
    }
    bands(pc, sp, 0x1D40, 0x1D46, 0x2C3B, 0x2C53, 0x2C67);
}

/* ---- draw_horizon_list (0050:408C, native in line.c) --------------------------------- */

typedef struct Bus {
    Pc *pc;
    uint16_t buf, dat; /* DS and ES inside draw_line */
    uint16_t ss, call_sp;
} Bus;

static uint8_t bus_buf_rd(void *ctx, uint16_t o)
{
    Bus *b = ctx;
    return rd8(b->pc, b->buf, o);
}
static void bus_buf_wr(void *ctx, uint16_t o, uint8_t v)
{
    Bus *b = ctx;
    wr8(b->pc, b->buf, o, v);
}
static uint8_t bus_dat_rd(void *ctx, uint16_t o)
{
    Bus *b = ctx;
    return rd8(b->pc, b->dat, o);
}
static void bus_dat_wr(void *ctx, uint16_t o, uint8_t v)
{
    Bus *b = ctx;
    wr8(b->pc, b->dat, o, v);
}
static void bus_call(void *ctx, uint16_t ret)
{
    Bus *b = ctx;
    wr16(b->pc, b->ss, b->call_sp, ret);
}

/* Cycles of draw_line's normal path (0050:569B) from the x-major loops: n+1 pixels along x,
 * e = error (starts at SAR(n)), e -= ady per pixel, a y step when e <= 0 (then e += n).
 * The y-up loops (5814..588A) cost the same as the y-down ones (5725..579B). odd_x picks
 * the low-nibble entry, odd_row the 2000h bank. */
static uint32_t xmajor_cycles(int n, int ady, bool odd_x, bool odd_row)
{
    uint32_t c = 0;
    int e = n >> 1, bp = n;
    if (!odd_x) {
        c += 22;
        if (odd_row) {
            c += 16;
            goto L5780;
        }
        c += 4;
        goto L5731;
    }
    c += 39;
    if (odd_row) {
        c += 16;
        goto L5748;
    }
    c += 4;
    goto L576C;
L5731:
    c += 30;
    if (--bp < 0) {
        c += 16;
        goto L5799;
    }
    c += 4 + 5;
    e -= ady;
    if (e > 0) {
        c += 16;
        goto L576C;
    }
    c += 4 + 46;
    e += n;
L5748:
    c += 31;
    if (--bp < 0) {
        c += 16;
        goto L579B;
    }
    c += 4 + 2 + 5;
    e -= ady;
    if (e > 0) {
        c += 16;
        goto L5780;
    }
    c += 4 + 6 + 5 + 17;
    e += n;
    goto L5731;
L576C:
    c += 31;
    if (--bp < 0) {
        c += 16;
        goto L579B;
    }
    c += 4 + 2 + 5;
    e -= ady;
    if (e > 0) {
        c += 16;
        goto L5731;
    }
    c += 4 + 6 + 5;
    e += n;
L5780:
    c += 30;
    if (--bp < 0) {
        c += 16;
        goto L5799;
    }
    c += 4 + 5;
    e -= ady;
    if (e > 0) {
        c += 16;
        goto L5748;
    }
    c += 4 + 18 + 6 + 17 + 5 + 17;
    e += n;
    goto L576C;
L5799:
    c += 18;
L579B:
    return c + 20;
}

/* The y-major loops (579C..5813 down, 588B..5906 up, same costs plus NEG BP going up): n+1
 * pixels along y, e starts at e0, e -= adx per pixel, an x step when e <= 0 (then e += n). */
static uint32_t ymajor_cycles(int n, int e0, int adx, bool odd_x, bool odd_row, uint32_t entry)
{
    uint32_t c = entry;
    int e = e0, bp = n;
    if (!odd_x) {
        if (odd_row) {
            c += 16;
            goto P_FC;
        }
        c += 4;
        goto P_A8;
    }
    if (odd_row) {
        c += 16;
        goto P_BD;
    }
    c += 4 + 16;
    goto P_E6;
P_A8:
    c += 48;
    if (--bp < 0)
        goto done;
    c += 4 + 6 + 5;
    e -= adx;
    if (e > 0) {
        c += 16;
        goto P_FC;
    }
    c += 4 + 5;
    e += n;
P_BD:
    c += 48;
    if (--bp < 0)
        goto done;
    c += 4 + 5;
    e -= adx;
    if (e > 0) {
        c += 16 + 6; /* to 57E2: add bx,E050h, then 57E6 */
        goto P_E6;
    }
    c += 4 + 6 + 5 + 17;
    e += n;
    goto P_A8;
P_E6:
    c += 48;
    if (--bp < 0)
        goto done;
    c += 4 + 6 + 5;
    e -= adx;
    if (e > 0) {
        c += 16;
        goto P_BD;
    }
    c += 4 + 5 + 2;
    e += n;
P_FC:
    c += 48;
    if (--bp < 0)
        goto done;
    c += 4 + 6 + 5;
    e -= adx;
    if (e > 0) {
        c += 16;
        goto P_A8;
    }
    c += 4 + 5 + 17;
    e += n;
    goto P_E6;
done:
    return c + 16 + 20; /* JL taken, RET */
}

/* Cycles of one draw_line call (0050:5691) in the original, RET included, CALL not.
 * BX,BP = x0,y0 and SI,DI = x1,y1; row_odd(y) says whether row y is in the 2000h bank.
 * Only the normal path ([31D0] = 0); the polygon path is not modelled. */
static uint32_t draw_line_cycles(Pc *pc, uint16_t ds, const RasterRegs *in)
{
    int x0 = (int16_t)in->bx, y0 = (int16_t)in->bp, x1 = (int16_t)in->si, y1 = (int16_t)in->di;
    uint32_t c = 14 + 16 + 12 + 4 + 5 + 4 + 11 + 5; /* test, je, DS/ES setup, cmp si,bx */
#define ODD_ROW(y) ((int16_t)(rd16(pc, ds, (uint16_t)(ROW_OFFSETS + 2 * (y))) + (x0 >> 1)) >= 0x2000)
    if (x1 == x0) {
        c += 4 + 4 + 17; /* jl, jg not taken, jmp 5907 */
        c += 5;
        if (y1 >= y0) {
            c += 16;
        } else {
            int t = y0;
            y0 = y1, y1 = t;
            c += 4 + 6;
        }
        int di = y1 - y0;
        c += 68;
        bool odd_x = x0 & 1;
        c += odd_x ? 16 : 4;
        c += 6;
        if (ODD_ROW(y0)) {
            c += 16;
            goto VB;
        }
        c += 4;
    VA:
        c += 48;
        if (--di < 0)
            return c + 16 + 20;
        c += 4 + 6;
    VB:
        c += 54;
        if (--di >= 0) {
            c += 16;
            goto VA;
        }
        return c + 4 + 20;
    }
    if (x1 < x0) {
        int t = x0;
        x0 = x1, x1 = t;
        t = y0, y0 = y1, y1 = t;
        c += 16 + 12;
    } else {
        c += 4 + 16;
    }
    int n = x1 - x0, dy = y1 - y0;
    bool odd_x = x0 & 1;
    c += 22;
    if (dy == 0) {
        c += 4 + 4 + 17 + 51;
        int si = n;
        if (odd_x) {
            c += 16;
            goto H91;
        }
        c += 4;
    H7D:
        c += 30;
        if (--si < 0)
            return c + 16 + 18 + 20;
        c += 4 + 33;
        if (--si >= 0) {
            c += 16;
            goto H7D;
        }
        return c + 4 + 20;
    H91:
        c += 50;
        if (--si >= 0) {
            c += 16;
            goto H7D;
        }
        return c + 4 + 20;
    }
    bool odd_row = ODD_ROW(y0);
#undef ODD_ROW
    if (dy > 0) {
        c += 4 + 16 + 51 + (odd_x ? 16 : 4) + 5;
        if (dy < n)
            return c + 4 + 17 + xmajor_cycles(n, dy, odd_x, odd_row);
        c += 16;
        if (dy == n)
            return c + 16 + 17 + xmajor_cycles(n, dy, odd_x, odd_row);
        c += 4 + 17;
        return c + ymajor_cycles(dy, dy >> 1, n, odd_x, odd_row, 22);
    }
    c += 16 + 51 + (odd_x ? 16 : 4) + 4 + 5;
    if (dy + n > 0)
        return c + 4 + 17 + xmajor_cycles(n, -dy, odd_x, odd_row);
    c += 16;
    if (dy + n == 0)
        return c + 16 + 17 + xmajor_cycles(n, -dy, odd_x, odd_row);
    c += 4 + 17;
    return c + ymajor_cycles(-dy, -(dy >> 1), n, odd_x, odd_row, 27);
}

/* draw_line_list on horizon_list, with sp = SP inside it (its return address on top). The
 * lines go through the C rasteriser (raster.h); draw_line's CALL writes 40B9 below sp. */
static void horizon_list_body(Pc *pc, uint16_t sp)
{
    REG(AX) = GS_HORIZON_LIST;
    gs_set_line_list_ptr(pc, REG(AX));
    cyc += 4 + 12;
    for (;;) {
        uint16_t bx = gs_line_list_ptr(pc);
        uint16_t ax = dsr16(pc, bx);
        REG(AX) = ax;
        REG(BX) = bx;
        cyc += 17 + 17 + 6;
        JCC(lo8(ax) == 0xFF);
        if (lo8(ax) == 0xFF)
            break;
        uint16_t cx = dsr16(pc, (uint16_t)(bx + 2));
        REG(CX) = cx;
        gs_set_line_list_ptr(pc, (uint16_t)(gs_line_list_ptr(pc) + 4));
        RasterRegs r = { hi8(cx), lo8(ax), cx, REG(DX), lo8(cx), hi8(cx), hi8(ax) };
        wr16(pc, SREG(SS), (uint16_t)(sp - 2), 0x40B9); /* call draw_line */
        cyc += 17 + 19 + 5 + 4 + 4 + 5 + 4 + 4 + 4 + 4 + 4 + 21;
        uint16_t ds = SREG(DS);
        cyc += draw_line_cycles(pc, ds, &r);
        Bus bus = { pc, gs_view_buf_seg(pc), rd16(pc, 0, 0x120), SREG(SS), (uint16_t)(sp - 4) };
        RasterBus rb = { &bus, bus_buf_rd, bus_buf_wr, bus_dat_rd, bus_dat_wr, bus_call };
        if (gs_poly_mode(pc))
            raster_poly_edge_regs(&rb, &r);
        else
            raster_line_regs(&rb, &r);
        SREG(ES) = bus.dat;
        REG(BX) = r.bx;
        REG(CX) = r.cx;
        REG(DX) = r.dx;
        REG(SI) = r.si;
        REG(DI) = r.di;
        REG(BP) = r.bp;
        REG(AX) = 0;
        SREG(DS) = rd16(pc, 0, 0x120);
        cyc += 5 + 4 + 11 + 17; /* xor ax,ax; mov ds,ax; mov ds,[120]; jmp */
    }
    gs_set_scenery_ip(pc, (uint16_t)(gs_scenery_ip(pc) + 1));
    cyc += 12 + 20;
}

/* ---- radar_clear (0050:2AB0) --------------------------------------------------------- */

/* Clears both banks (clear_view_buffer, 0848h words of [30C0] each) and draws the radar
 * view's fixed marks. sp = SP at entry. */
static void radar_clear_body(Pc *pc, uint16_t sp)
{
    uint16_t s = sp;
    gs_set_blit_words0(pc, 0x0848);
    gs_set_blit_words1(pc, 0x0848);
    gs_set_clear_words(pc, 0x0848);
    cyc += 3 * 19;
    /* call clear_view_buffer */
    vpush(pc, &s, 0x2AC5);
    s = sp;
    uint16_t words = gs_clear_words(pc);
    SREG(ES) = gs_view_buf_seg(pc);
    REG(AX) = gs_clear_pattern(pc);
    cyc += 21 + 11 + 17 + 5 + 12 + 4 + 17 + 20;
    REG(DI) = 0;
    REG(CX) = words;
    rep_stosw(pc);
    REG(DI) = 0x2000;
    REG(CX) = words;
    rep_stosw(pc);
    /* push ds; mov ds,[3806]; five words; pop ds */
    vpush(pc, &s, SREG(DS));
    uint16_t buf = gs_view_buf_seg(pc);
    wr16(pc, buf, 0x07F7, 0x4001);
    wr16(pc, buf, 0x27F7, 0x7F7F);
    wr16(pc, buf, 0x0847, 0x4001);
    wr16(pc, buf, 0x2847, 0x4001);
    wr16(pc, buf, 0x0897, 0x7007);
    cyc += 16 + 11 + 5 * 19 + 14 + 20;
}

/* ---- natives ------------------------------------------------------------------------ */

#define CYC_RADAR_CLEAR 42724 /* 0848h words per bank */
#define CYC_DRAW_SKY_GROUND 30000   /* typical; the native charges the exact cost */
#define CYC_HORIZON_FILL 30000
#define CYC_FILL_SLOPED 3000

/* Charges what the original took beyond the entry's fixed .cycles. */
static void charge(Pc *pc, uint32_t fixed)
{
    pc->cpu.cycles += cyc;
    pc->cpu.cycles -= fixed;
}

static void n_radar_clear(Pc *pc)
{
    cyc = 0;
    radar_clear_body(pc, REG(SP));
    charge(pc, CYC_RADAR_CLEAR);
    native_ret(pc);
}

static void n_fill_sloped_horizon(Pc *pc)
{
    cyc = 0;
    fill_sloped_body(pc);
    charge(pc, CYC_FILL_SLOPED);
    native_ret(pc);
}

static void n_horizon_fill(Pc *pc)
{
    uint16_t sp = REG(SP);
    cyc = 0;
    horizon_fill_body(pc, sp);
    REG(SP) = sp;
    charge(pc, CYC_HORIZON_FILL);
    native_ret(pc);
}

/* 0050:2AEA draw_sky_ground: radar view -> radar_clear; otherwise horizon_fill and, when
 * [0412] & 2 (dusk) and a horizon was captured, the horizon line in colour 8008h. */
static void n_draw_sky_ground(Pc *pc)
{
    uint16_t sp = REG(SP);
    cyc = 14; /* test byte [0405],FF */
    bool radar = gs_radar_view(pc) != 0;
    JCC(radar);
    if (radar) {
        radar_clear_body(pc, sp);
    } else {
        uint16_t s = sp;
        vpush(pc, &s, 0x2AF4);
        cyc += 21;
        horizon_fill_body(pc, s);
        REG(SP) = sp;
        cyc += 14; /* test word [0412],2 */
        bool line = gs_time_of_day(pc) & 2;
        JCC(!line);
        if (line) {
            cyc += 19; /* cmp byte [041F],FF */
            line = gs_horizon_list_b(pc, 0) != 0xFF;
            JCC(!line);
        }
        if (line) {
            gs_set_draw_colour(pc, 0x8008);
            s = sp;
            vpush(pc, &s, 0x2B0C);
            cyc += 19;
            if (!call_native(pc, sp, 0x2B0C, 0x408C)) {
                cyc += 21;
                horizon_list_body(pc, s);
            }
        }
        cyc += 20; /* RET */
    }
    charge(pc, CYC_DRAW_SKY_GROUND);
    native_ret(pc);
}

NativeEntry native_horizon[] = {
    { .name = "draw_sky_ground", .seg = GAME_CS, .off = 0x2AEA, .fn = n_draw_sky_ground, .enabled = true,
      .cycles = CYC_DRAW_SKY_GROUND },
    { .name = "horizon_fill", .seg = GAME_CS, .off = 0x2B0D, .fn = n_horizon_fill, .enabled = true,
      .cycles = CYC_HORIZON_FILL },
    { .name = "fill_sloped_horizon", .seg = GAME_CS, .off = 0x2C9E, .fn = n_fill_sloped_horizon,
      .enabled = true, .cycles = CYC_FILL_SLOPED },
    { .name = "radar_clear", .seg = GAME_CS, .off = 0x2AB0, .fn = n_radar_clear, .enabled = true,
      .cycles = CYC_RADAR_CLEAR },
    { .name = NULL },
};
