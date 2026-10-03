/* Natives for subphase 3.3 back buffer primitives (see docs/PHASE3_PLAN.md).
 *
 * The back buffer (segment [3806], 0C0C at run time) has the CGA layout: even rows at
 * offset 0, odd rows at 2000h, 80 bytes per row, two 4-bit pixels per byte.
 *
 * Cycles: each entry's .cycles is the original's fixed cost in the emulator's cycle
 * model (the shortest path); the native adds the variable part (REP iterations, branch
 * differences) to cpu.cycles itself, so a native call costs exactly what the original
 * costs. None of the callers test flags after these routines, so flag_mask is 0, but
 * the flags are still left as the original leaves them. */
#include "native.h"
#include "game/state.h"

#define ROW_OFFSETS 0x380C

static bool parity8(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

/* Flags of a logical op (AND/OR/XOR): CF, OF, AF clear; ZF, SF, PF from the result. */
static void logic_flags(Cpu8086 *c, uint16_t r, bool word)
{
    uint16_t f = c->flags & (uint16_t)~(F_CF | F_OF | F_AF | F_ZF | F_SF | F_PF);
    if ((word ? r : (r & 0xFF)) == 0)
        f |= F_ZF;
    if (r & (word ? 0x8000 : 0x80))
        f |= F_SF;
    if (parity8((uint8_t)r))
        f |= F_PF;
    c->flags = f;
}

/* REP STOSW at ES:DI, CX words (CX ends 0), honouring DF. */
static void rep_stosw(Cpu8086 *c)
{
    uint16_t es = c->sregs[S_ES], ax = c->regs[R_AX];
    int16_t delta = (c->flags & F_DF) ? -2 : 2;
    for (uint16_t n = c->regs[R_CX]; n; n--) {
        cpu_write16(c, cpu_linear(es, c->regs[R_DI]), ax);
        c->regs[R_DI] = (uint16_t)(c->regs[R_DI] + delta);
    }
    c->regs[R_CX] = 0;
}

/* REP MOVSW from DS:SI to ES:DI, CX words (CX ends 0), honouring DF. */
static void rep_movsw(Cpu8086 *c)
{
    uint16_t ds = c->sregs[S_DS], es = c->sregs[S_ES];
    int16_t delta = (c->flags & F_DF) ? -2 : 2;
    for (uint16_t n = c->regs[R_CX]; n; n--) {
        cpu_write16(c, cpu_linear(es, c->regs[R_DI]), cpu_read16(c, cpu_linear(ds, c->regs[R_SI])));
        c->regs[R_SI] = (uint16_t)(c->regs[R_SI] + delta);
        c->regs[R_DI] = (uint16_t)(c->regs[R_DI] + delta);
    }
    c->regs[R_CX] = 0;
}

/* 0050:561E clear_view_buffer: [3800] words of [30C0] at ES:0 and ES:2000h, ES = [3806].
 * Ends with CX = 0, DI past the second bank, AX = pattern. Flags from XOR DI,DI. */
#define CLEAR_FIXED 90
static void n_clear_view_buffer(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t words = gs_clear_words(pc);
    c->sregs[S_ES] = gs_view_buf_seg(pc);
    c->regs[R_AX] = gs_clear_pattern(pc);
    logic_flags(c, 0, true);
    c->regs[R_DI] = 0;
    c->regs[R_CX] = words;
    rep_stosw(c);
    c->regs[R_DI] = 0x2000;
    c->regs[R_CX] = words;
    rep_stosw(c);
    c->cycles += 20u * words;
    native_ret(pc);
}

/* 0050:5637 blit_view_to_screen: [3802] words from [3808] and [3804] words from [380A]
 * of segment [3806] to the same offsets in B800. Ends with AX = ES = B800, CX = 0,
 * DX = [3804], BP = [380A], SI = DI past the second block. DS is pushed and popped
 * (the stack write is part of the comparison). Flags unchanged. */
#define BLIT_FIXED 162
static void n_blit_view_to_screen(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ds = c->sregs[S_DS];
    cpu_push(c, ds);
    uint16_t n1 = gs_blit_words0(pc);
    c->regs[R_CX] = n1;
    c->regs[R_SI] = gs_blit_off0(pc);
    c->regs[R_DX] = gs_blit_words1(pc);
    c->regs[R_BP] = gs_blit_off1(pc);
    c->sregs[S_DS] = gs_view_buf_seg(pc);
    c->regs[R_AX] = 0xB800;
    c->sregs[S_ES] = 0xB800;
    c->regs[R_DI] = c->regs[R_SI];
    rep_movsw(c);
    c->regs[R_SI] = c->regs[R_BP];
    c->regs[R_DI] = c->regs[R_SI];
    c->regs[R_CX] = c->regs[R_DX];
    rep_movsw(c);
    c->sregs[S_DS] = cpu_pop(c);
    c->cycles += 17u * ((uint32_t)n1 + c->regs[R_DX]);
    native_ret(pc);
}

/* 0050:5664 plot_pixel: AH = x (0-159), CL = y. Sets one nibble of ES:[row_offsets[y] +
 * x/2]: odd x the low nibble from [30C8], even x the high nibble from [30C9] (the colour
 * bytes are pre-shifted). Ends with BX = DX = byte offset, AL = new byte, AH = x SAR 1.
 * Flags from the final OR AL. */
#define PLOT_FIXED 146 /* odd x; even x takes the JAE (+12) */
static void plot_pixel(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ds = c->sregs[S_DS], es = c->sregs[S_ES];
    uint8_t x = (uint8_t)(c->regs[R_AX] >> 8);
    uint16_t row = (uint16_t)((c->regs[R_CX] & 0xFF) << 1);
    uint16_t off = (uint16_t)((x >> 1) + mem_read16(pc, ds, (uint16_t)(row + ROW_OFFSETS)));
    uint32_t a = cpu_linear(es, off);
    uint8_t al = cpu_read8(c, a);
    if (x & 1) {
        al = (uint8_t)((al & 0xF0) | cpu_read8(c, cpu_linear(ds, GS_DRAW_COLOUR)));
    } else {
        al = (uint8_t)((al & 0x0F) | cpu_read8(c, cpu_linear(ds, (uint16_t)(GS_DRAW_COLOUR + 1))));
        c->cycles += 12;
    }
    cpu_write8(c, a, al);
    logic_flags(c, al, false);
    uint8_t ah = (uint8_t)((int8_t)x >> 1);
    c->regs[R_AX] = (uint16_t)(ah << 8 | al);
    c->regs[R_BX] = off;
    c->regs[R_DX] = off;
    native_ret(pc);
}

static void n_plot_pixel(Pc *pc)
{
    plot_pixel(pc);
}

/* 0050:5660 plot_pixel_es: ES = [3806], then plot_pixel. */
static void n_plot_pixel_es(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    c->sregs[S_ES] = gs_view_buf_seg(pc);
    plot_pixel(pc);
}

/* 0050:2C68 fill_rows: rows BL..DL (inclusive) of ES:row_offsets with the word pattern BP,
 * 40 words per row: ceil(n/2) rows from row_offsets[BL], then floor(n/2) rows of the other
 * bank from row_offsets[BL+1]. The 8-bit arithmetic (n+1 in DL, SAR) is kept as in the
 * original. Ends with AX = BP, BX = BL*2+2, CX = 0,
 * DX = word count of the second bank, SI = row_offsets[BL+1]; flags from OR CX,CX. */
#define FILL_FIXED 223 /* DL-BL+2 even (JB not taken), nonempty second bank */
static void n_fill_rows(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ds = c->sregs[S_DS];
    uint8_t bl = (uint8_t)c->regs[R_BX];
    uint8_t dl = (uint8_t)((uint8_t)c->regs[R_DX] - bl + 2);
    uint16_t bx = (uint16_t)(bl << 1);
    c->regs[R_DI] = mem_read16(pc, ds, (uint16_t)(bx + ROW_OFFSETS));
    bx = (uint16_t)(bx + 2);
    c->regs[R_SI] = mem_read16(pc, ds, (uint16_t)(bx + ROW_OFFSETS));
    c->regs[R_BX] = bx;
    uint8_t cl = (uint8_t)((int8_t)dl >> 1);
    uint16_t ax = (uint16_t)(0x28 * cl);
    bool odd = dl & 1; /* CF of SAR DL,1 */
    uint16_t dx = ax;
    uint32_t cyc = 0;
    if (odd)
        cyc += 6; /* JB taken costs 6 more than JB not taken plus SUB DX,28h */
    else
        dx = (uint16_t)(dx - 0x28);
    c->regs[R_CX] = ax;
    c->regs[R_AX] = c->regs[R_BP];
    rep_stosw(c);
    cyc += 10u * ax;
    c->regs[R_CX] = dx;
    c->regs[R_DI] = c->regs[R_SI];
    logic_flags(c, dx, true);
    if (dx) {
        rep_stosw(c);
        cyc += 10u * dx;
    } else {
        cyc += 10; /* JE taken costs 10 more than JE not taken plus an empty REP */
    }
    c->regs[R_DX] = dx;
    c->cycles += cyc;
    native_ret(pc);
}

NativeEntry native_buffer[] = {
    { .name = "clear_view_buffer", .seg = GAME_CS, .off = 0x561E, .fn = n_clear_view_buffer, .enabled = true,
      .cycles = CLEAR_FIXED },
    { .name = "blit_view_to_screen", .seg = GAME_CS, .off = 0x5637, .fn = n_blit_view_to_screen,
      .enabled = true, .cycles = BLIT_FIXED },
    { .name = "plot_pixel_es", .seg = GAME_CS, .off = 0x5660, .fn = n_plot_pixel_es, .enabled = true,
      .cycles = PLOT_FIXED + 11 },
    { .name = "plot_pixel", .seg = GAME_CS, .off = 0x5664, .fn = n_plot_pixel, .enabled = true,
      .cycles = PLOT_FIXED },
    { .name = "fill_rows", .seg = GAME_CS, .off = 0x2C68, .fn = n_fill_rows, .enabled = true,
      .cycles = FILL_FIXED },
    { .name = NULL },
};
