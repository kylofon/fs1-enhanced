/* Natives for subphase 3.9 text and font (see docs/PHASE3_PLAN.md, docs/subphases/3.9.md). */
#include "native.h"
#include "game/state.h"
#include "natives/textdraw.h"

/* Displacements of the five font loads (disp16 of mov ax,[bx+disp]) in each loop. */
static const uint16_t disp_print_str[5] = { 0x1247, 0x1252, 0x125D, 0x1268, 0x1273 };
static const uint16_t disp_str2_even[5] = { 0x12A1, 0x12B4, 0x12C7, 0x12DA, 0x12ED };
static const uint16_t disp_str2_odd[5] = { 0x1312, 0x1325, 0x1338, 0x134B, 0x135E };

/* Emulated cycles of the original loops, measured with --verify (cpu8086.c model). The
 * entry's .cycles is the fixed part of a call; the natives add the per-glyph part. */
#define CYC_GLYPH 275      /* print_str_mask loop, one glyph */
#define CYC_GLYPH2 375     /* print_str2 loop, one glyph with a zero shift */
#define CYC_GLYPH2_BIT 20  /* print_str2: SHR AX,CL costs 4 per bit, 5 rows */
#define CYC_LIST_STR 171   /* print_str_list: CMP, CALL and print_str's fixed part per string */

static bool even_parity(uint8_t v)
{
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return !(v & 1);
}

/* Flags after SUB AL,20h with AL = a. */
static void flags_sub20(Cpu8086 *c, uint8_t a)
{
    uint8_t r = (uint8_t)(a - 0x20);
    uint16_t f = c->flags & (uint16_t)~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF);
    if (a < 0x20) f |= F_CF;
    if (even_parity(r)) f |= F_PF;
    if ((a ^ 0x20 ^ r) & 0x10) f |= F_AF;
    if (!r) f |= F_ZF;
    if (r & 0x80) f |= F_SF;
    if ((a ^ 0x20) & (a ^ r) & 0x80) f |= F_OF;
    c->flags = f;
}

static int df_dir(const Cpu8086 *c) { return (c->flags & F_DF) ? -1 : 1; }

/* LODSW / LODSB from DS:SI. */
static uint16_t lodsw(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t v = mem_read16(pc, c->sregs[S_DS], c->regs[R_SI]);
    c->regs[R_SI] = (uint16_t)(c->regs[R_SI] + 2 * df_dir(c));
    return v;
}

/* print_str_mask body from 0050:1233 with ES already set: one string at DS:SI, mask CX. */
static void print_str_body(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ax = lodsw(pc);
    TextDraw t = { .ds = c->sregs[S_DS], .si = c->regs[R_SI], .es = c->sregs[S_ES], .di = ax,
                   .mask = c->regs[R_CX], .shift = -1, .ax = ax, .bx = c->regs[R_BX] };
    textdraw_read_disp(pc, disp_print_str, t.disp);
    textdraw_string(pc, &t, df_dir(c));
    c->regs[R_AX] = t.ax;
    c->regs[R_BX] = t.bx;
    c->regs[R_SI] = t.si;
    c->regs[R_DI] = t.di;
    flags_sub20(c, (uint8_t)(t.ax + 0x20));
    c->cycles += (uint64_t)t.glyphs * CYC_GLYPH;
}

/* 0050:122E print_str_mask: ES = B800 (immediate at 122F), CX = AND mask. */
static void n_print_str_mask(Pc *pc)
{
    pc->cpu.sregs[S_ES] = mem_read16(pc, GAME_CS, 0x122F);
    print_str_body(pc);
    native_ret(pc);
}

/* 0050:1225 print_str: CX = FFFF (immediate at 1226). */
static void n_print_str(Pc *pc)
{
    pc->cpu.regs[R_CX] = mem_read16(pc, GAME_CS, 0x1226);
    n_print_str_mask(pc);
}

/* 0050:122B print_str_dim: CX = 8888 (immediate at 122C). */
static void n_print_str_dim(Pc *pc)
{
    pc->cpu.regs[R_CX] = mem_read16(pc, GAME_CS, 0x122C);
    n_print_str_mask(pc);
}

/* 0050:1282 print_str2: ES = B800, mask BP, each glyph row shifted right by DL pixels
 * (in screen byte order). DI < 1F40 (signed) starts on an even line, else an odd one.
 * CL = DL once a glyph is drawn. */
static void n_print_str2(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    c->sregs[S_ES] = mem_read16(pc, GAME_CS, 0x1283);
    uint16_t ax = lodsw(pc);
    uint8_t dl = (uint8_t)c->regs[R_DX];
    bool odd = (int16_t)ax >= 0x1F40;
    TextDraw t = { .ds = c->sregs[S_DS], .si = c->regs[R_SI], .es = c->sregs[S_ES], .di = ax,
                   .mask = c->regs[R_BP], .odd_first = odd, .shift = dl, .ax = ax, .bx = c->regs[R_BX] };
    textdraw_read_disp(pc, odd ? disp_str2_odd : disp_str2_even, t.disp);
    textdraw_string(pc, &t, df_dir(c));
    if (t.glyphs)
        c->regs[R_CX] = (uint16_t)((c->regs[R_CX] & 0xFF00) | dl);
    c->regs[R_AX] = t.ax;
    c->regs[R_BX] = t.bx;
    c->regs[R_SI] = t.si;
    c->regs[R_DI] = t.di;
    flags_sub20(c, (uint8_t)(t.ax + 0x20));
    c->cycles += (uint64_t)t.glyphs * (CYC_GLYPH2 + CYC_GLYPH2_BIT * (uint64_t)dl);
    native_ret(pc);
}

/* 0050:15E1 print_str_list: print_str for each record at DS:SI until a 0 word. Each
 * CALL leaves its return address (15E9) below SP, as the original does. */
static void n_print_str_list(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    while (mem_read16(pc, c->sregs[S_DS], c->regs[R_SI]) != 0) {
        mem_write16(pc, c->sregs[S_SS], (uint16_t)(c->regs[R_SP] - 2), 0x15E9);
        c->regs[R_CX] = mem_read16(pc, GAME_CS, 0x1226);
        c->sregs[S_ES] = mem_read16(pc, GAME_CS, 0x122F);
        print_str_body(pc);
        c->cycles += CYC_LIST_STR;
    }
    /* flags of CMP word,0 with equal operands */
    c->flags = (uint16_t)((c->flags & ~(F_CF | F_AF | F_SF | F_OF)) | F_ZF | F_PF);
    native_ret(pc);
}

/* 0050:15EC clear_screen: zero 848h words at ES:0000 and ES:2000, ES = [DS:03C2]. */
static void n_clear_screen(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t es = gs_screen_seg(pc);
    int step = 2 * df_dir(c);
    uint16_t di = 0;
    c->sregs[S_ES] = es;
    for (int bank = 0; bank < 2; bank++) {
        di = bank ? 0x2000 : 0x0000;
        for (int i = 0; i < 0x848; i++, di = (uint16_t)(di + step))
            mem_write16(pc, es, di, 0);
    }
    c->regs[R_AX] = 0;
    c->regs[R_CX] = 0;
    c->regs[R_DI] = di;
    /* flags of XOR AX,AX */
    c->flags = (uint16_t)((c->flags & ~(F_CF | F_AF | F_SF | F_OF)) | F_ZF | F_PF);
    native_ret(pc);
}

NativeEntry native_text[] = {
    { .name = "print_str", .seg = GAME_CS, .off = 0x1225, .fn = n_print_str, .enabled = true, .cycles = 110 },
    { .name = "print_str_dim", .seg = GAME_CS, .off = 0x122B, .fn = n_print_str_dim, .enabled = true,
      .cycles = 93 },
    { .name = "print_str_mask", .seg = GAME_CS, .off = 0x122E, .fn = n_print_str_mask, .enabled = true,
      .cycles = 89 },
    { .name = "print_str2", .seg = GAME_CS, .off = 0x1282, .fn = n_print_str2, .enabled = true, .cycles = 99 },
    { .name = "print_str_list", .seg = GAME_CS, .off = 0x15E1, .fn = n_print_str_list, .enabled = true,
      .cycles = 55 },
    { .name = "clear_screen", .seg = GAME_CS, .off = 0x15EC, .fn = n_clear_screen, .enabled = true,
      .cycles = 42457 },
    { .name = NULL },
};
