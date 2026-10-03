/* Natives for subphase 3.2 number formatting (see docs/PHASE3_PLAN.md and docs/subphases/3.2.md). */
#include "native.h"
#include "game/state.h"

/* ---- small helpers ----------------------------------------------------------- */

static uint8_t rd8(Pc *pc, uint16_t seg, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(seg, off)); }
static void wr8(Pc *pc, uint16_t seg, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(seg, off), v); }

/* String-op step for SI/DI: +n, or -n with DF set. */
static uint16_t str_step(const Cpu8086 *c, uint16_t r, uint16_t n)
{
    return (uint16_t)((c->flags & F_DF) ? r - n : r + n);
}

/* ---- 0050:11A9 fmt_signed_dec ------------------------------------------------
 * AX -> DS:[BX] = ' ' or '-', [BX+1..5] = five decimal digits (leading zeros kept;
 * -32768 is written as "-32768"). Then it does not return but jumps into print_str_dim
 * at 1233: CX = 8888h, ES = [view_buf_seg], SI -> {word offset, ASCII..., byte < 20h}
 * is drawn with the 16x5 font into the back buffer. The font address is self-patched
 * into the five MOV AX,[BX+disp] instructions at 1245..1271, so it is read from there. */
static void n_fmt_signed_dec(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t ds = c->sregs[S_DS], bx = c->regs[R_BX];
    uint16_t ax = c->regs[R_AX];

    if (ax == 0x8000) {
        static const char digits[] = "32768";
        wr8(pc, ds, bx, '-');
        for (int i = 0; i < 5; i++)
            wr8(pc, ds, (uint16_t)(bx + 1 + i), (uint8_t)digits[i]); /* DL is left alone */
    } else {
        int v = (int16_t)ax;
        wr8(pc, ds, bx, v < 0 ? '-' : ' ');
        if (v < 0)
            v = -v;
        static const int pow10[4] = { 10000, 1000, 100, 10 };
        uint8_t dl = 0;
        for (int i = 0; i < 4; i++) {
            dl = (uint8_t)('0' + v / pow10[i]);
            v %= pow10[i];
            wr8(pc, ds, (uint16_t)(bx + 1 + i), dl);
        }
        wr8(pc, ds, (uint16_t)(bx + 5), (uint8_t)('0' + v));
        c->regs[R_DX] = (uint16_t)((c->regs[R_DX] & 0xFF00) | dl);
    }

    /* print_str_dim tail (loc_1233) */
    static const uint16_t disp_at[5] = { 0x1247, 0x1252, 0x125D, 0x1268, 0x1273 };
    static const uint16_t di_add[5] = { 0x1FFE, 0xE04E, 0x1FFE, 0xE04E, 0xFF60 };
    uint16_t disp[5];
    for (int i = 0; i < 5; i++)
        disp[i] = mem_read16(pc, GAME_CS, disp_at[i]);
    uint16_t cx = 0x8888;
    uint16_t es = gs_view_buf_seg(pc); /* view_buf_seg */
    uint16_t si = c->regs[R_SI];
    ax = mem_read16(pc, ds, si);
    si = str_step(c, si, 2);
    uint16_t di = ax;
    for (;;) {
        uint8_t al = rd8(pc, ds, si);
        si = str_step(c, si, 1);
        uint8_t r = (uint8_t)(al - 0x20);
        ax = (uint16_t)((ax & 0xFF00) | r);
        if ((int8_t)al < 0x20)
            break; /* SUB AL,20h / JGE: printable is 20h..7Fh */
        bx = (uint16_t)(r * 10);
        for (int i = 0; i < 5; i++) {
            ax = (uint16_t)(mem_read16(pc, ds, (uint16_t)(bx + disp[i])) & cx);
            mem_write16(pc, es, di, ax);
            di = (uint16_t)(str_step(c, di, 2) + di_add[i]);
        }
    }
    c->regs[R_AX] = ax;
    c->regs[R_BX] = bx;
    c->regs[R_CX] = cx;
    c->regs[R_SI] = si;
    c->regs[R_DI] = di;
    c->sregs[S_ES] = es;
    native_ret(pc);
}

/* ---- 0050:1508 fmt_course_digits ---------------------------------------------
 * AL (0..FFh, course in 2-degree steps) -> DL = '0' + AL/50, then (AL%50)*2 as two
 * ASCII digits: AL = tens, AH = units. The caller stores DL then AX, giving three
 * digits of degrees. DH is kept. */
static void n_fmt_course_digits(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint8_t al = (uint8_t)c->regs[R_AX];
    uint8_t r = (uint8_t)(al % 50 * 2);
    c->regs[R_DX] = (uint16_t)((c->regs[R_DX] & 0xFF00) | (uint8_t)('0' + al / 50));
    c->regs[R_AX] = (uint16_t)(('0' + r % 10) << 8 | ('0' + r / 10));
    native_ret(pc);
}

/* ---- 0050:3555 fmt_dec4 ---------------------------------------------------------
 * AX (0..32767) -> CL = thousands, CH = hundreds, AL = tens, AH = units, all ASCII.
 * Leaves DX = AX mod 100 and BP = 100 (DIV BP). Also reached by falling through from
 * 3553 (XOR AH,AH). A negative AX makes the original's DIV fault (INT 0); no caller
 * passes one (ammo is clamped at 0, the score is 0..10000). */
static void n_fmt_dec4(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t v = c->regs[R_AX];
    uint16_t q1 = v / 1000, r1 = v % 1000;
    uint16_t q2 = r1 / 100, r2 = r1 % 100;
    c->regs[R_CX] = (uint16_t)(((q2 & 0xFF) << 8 | (q1 & 0xFF)) + 0x3030);
    c->regs[R_BP] = 100;
    c->regs[R_DX] = r2;
    c->regs[R_AX] = (uint16_t)(('0' + r2 % 10) << 8 | ('0' + r2 / 10));
    native_ret(pc);
}

/* ---- editor 24-bit values ------------------------------------------------------
 * editor_value (DS:2012..2014) is a 24-bit little-endian number. editor_digits
 * (202C..2031) are six ASCII digits, most significant first. The powers of ten
 * 100000..1 are split into three byte tables: low bytes at 2036, middle at 203C,
 * high at 2042 (index 0 = 100000). */

#define ED_DIGITS 0x202C
#define ED_POW_LO 0x2036
#define ED_POW_MID 0x203C
#define ED_POW_HI 0x2042

static uint32_t ed_pow(Pc *pc, uint16_t i)
{
    return ds_read8(pc, (uint16_t)(ED_POW_LO + i)) | (uint32_t)ds_read8(pc, (uint16_t)(ED_POW_MID + i)) << 8 |
           (uint32_t)ds_read8(pc, (uint16_t)(ED_POW_HI + i)) << 16;
}

/* 0050:3855 editor_digits_to_bin: the six digits -> editor_value. Each digit adds its
 * power of ten ((d & 7Fh) - '0') times, counted in a byte, so a non-digit such as a
 * space (F0h times) is not rejected. Returns CX = 0, DI = 6; AL = last high power byte
 * added (unchanged if every digit is '0'). */
static void n_editor_digits_to_bin(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint32_t v = 0;
    uint16_t ax = c->regs[R_AX];
    for (uint16_t di = 0; di < 6; di++) {
        uint8_t n = (uint8_t)((ds_read8(pc, (uint16_t)(ED_DIGITS + di)) & 0x7F) - '0');
        if (!n)
            continue;
        uint32_t p = ed_pow(pc, di);
        v = (v + p * n) & 0xFFFFFF;
        ax = (uint16_t)((ax & 0xFF00) | (uint8_t)(p >> 16));
    }
    ds_write8(pc, GS_EDITOR_VALUE, (uint8_t)v);
    ds_write8(pc, (uint16_t)(GS_EDITOR_VALUE + 1), (uint8_t)(v >> 8));
    ds_write8(pc, (uint16_t)(GS_EDITOR_VALUE + 2), (uint8_t)(v >> 16));
    c->regs[R_AX] = ax;
    c->regs[R_CX] = 0;
    c->regs[R_DI] = 6;
    native_ret(pc);
}

/* 0050:3890 editor_bin_to_digits: editor_value -> up to six ASCII digits on the
 * text screen at ES:[BX+0Ah..14h] (every other byte), left-aligned with leading zeros
 * dropped, after blanking the six cells. BX = [28E4 + editor_row] (word, row not
 * doubled), also stored at 2020h. Each digit is found by repeated 24-bit subtraction
 * until the result goes negative (bit 23). editor_value is left as the remainder;
 * 2019h = digits written, 201Ah = last digit, 2032h = next cell offset. Returns
 * DI = FFFFh, SI = offset of the units digit, AL = units digit, DX = low word of the
 * last (failed) subtraction. */
static void n_editor_bin_to_digits(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t es = c->sregs[S_ES];
    /* CALL sub_36F4 leaves its return address below the stack pointer */
    mem_write16(pc, c->sregs[S_SS], (uint16_t)(c->regs[R_SP] - 2), 0x3893);
    uint16_t bx = ds_read16(pc, (uint16_t)(0x28E4 + gs_editor_row(pc))); /* sub_36F4 */
    ds_write16(pc, 0x2020, bx);
    ds_write8(pc, 0x2019, 0);
    ds_write16(pc, 0x2032, 0x0A);
    for (uint16_t si = 0x0A; si != 0x16; si += 2)
        wr8(pc, es, (uint16_t)(bx + si), ' ');

    uint32_t v = ds_read8(pc, GS_EDITOR_VALUE) | (uint32_t)ds_read8(pc, (uint16_t)(GS_EDITOR_VALUE + 1)) << 8 |
                 (uint32_t)ds_read8(pc, (uint16_t)(GS_EDITOR_VALUE + 2)) << 16;
    uint8_t written = 0;
    uint16_t pos = 0x0A, si = 0, dx = c->regs[R_DX], ax = c->regs[R_AX];
    for (int di = 5; di >= 0; di--) {
        uint32_t p = ed_pow(pc, (uint16_t)di);
        uint8_t digit = '0';
        for (;;) {
            uint32_t r = (v - p) & 0xFFFFFF;
            dx = (uint16_t)r;
            ax = (uint16_t)((ax & 0xFF00) | (uint8_t)(r >> 16));
            if (r & 0x800000)
                break;
            v = r;
            if (++digit == 0)
                break;
        }
        ax = (uint16_t)((ax & 0xFF00) | digit);
        if (di != 0 && digit == '0') {
            ax = (uint16_t)((ax & 0xFF00) | written);
            if (!written)
                continue;
            ax = (uint16_t)((ax & 0xFF00) | digit);
        }
        written++;
        si = pos;
        wr8(pc, es, (uint16_t)(bx + si), digit);
        pos += 2;
        ds_write8(pc, 0x201A, digit);
    }
    ds_write8(pc, GS_EDITOR_VALUE, (uint8_t)v);
    ds_write8(pc, (uint16_t)(GS_EDITOR_VALUE + 1), (uint8_t)(v >> 8));
    ds_write8(pc, (uint16_t)(GS_EDITOR_VALUE + 2), (uint8_t)(v >> 16));
    ds_write8(pc, 0x2019, written);
    ds_write16(pc, 0x2032, pos);
    c->regs[R_AX] = ax;
    c->regs[R_BX] = bx;
    c->regs[R_DX] = dx;
    c->regs[R_SI] = si;
    c->regs[R_DI] = 0xFFFF;
    native_ret(pc);
}

NativeEntry native_format[] = {
    { .name = "fmt_signed_dec", .seg = GAME_CS, .off = 0x11A9, .fn = n_fmt_signed_dec, .enabled = true,
      .cycles = 3950 },
    { .name = "fmt_course_digits", .seg = GAME_CS, .off = 0x1508, .fn = n_fmt_course_digits, .enabled = true,
      .cycles = 160 },
    { .name = "fmt_dec4", .seg = GAME_CS, .off = 0x3555, .fn = n_fmt_dec4, .enabled = true, .cycles = 449 },
    { .name = "editor_digits_to_bin", .seg = GAME_CS, .off = 0x3855, .fn = n_editor_digits_to_bin,
      .enabled = true, .cycles = 1500 },
    { .name = "editor_bin_to_digits", .seg = GAME_CS, .off = 0x3890, .fn = n_editor_bin_to_digits,
      .enabled = true, .cycles = 4000 },
    { .name = NULL },
};
