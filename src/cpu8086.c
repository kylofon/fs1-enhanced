/* The 8086 interpreter (FS1_EMULATOR build only; the memory accessors are in cpu_core.c). */
#include "cpu8086.h"

#include <string.h>

/* Cycle costs are approximate 8088 timings; good enough for pacing. */
#define EA_CYCLES 7

static uint8_t parity_table[256];
static bool tables_ready;

static void trace_target(Cpu8086 *c, uint8_t flag)
{
    if (c->trace)
        c->trace[cpu_linear(c->sregs[S_CS], c->ip)] |= flag;
}

static void init_tables(void)
{
    for (int i = 0; i < 256; i++) {
        int bits = 0;
        for (int b = 0; b < 8; b++)
            bits += (i >> b) & 1;
        parity_table[i] = (bits & 1) == 0;
    }
    tables_ready = true;
}

static uint8_t fetch8(Cpu8086 *c)
{
    return c->mem[cpu_linear(c->sregs[S_CS], c->ip++)];
}

static uint16_t fetch16(Cpu8086 *c)
{
    uint16_t lo = fetch8(c);
    return (uint16_t)(lo | fetch8(c) << 8);
}

/* ---- registers --------------------------------------------------------- */

static uint8_t get_r8(Cpu8086 *c, int i)
{
    return i < 4 ? (uint8_t)c->regs[i] : (uint8_t)(c->regs[i - 4] >> 8);
}

static void set_r8(Cpu8086 *c, int i, uint8_t v)
{
    if (i < 4)
        c->regs[i] = (c->regs[i] & 0xFF00) | v;
    else
        c->regs[i - 4] = (uint16_t)((c->regs[i - 4] & 0x00FF) | v << 8);
}

/* ---- flags ------------------------------------------------------------- */

static void set_flag(Cpu8086 *c, uint16_t f, bool on)
{
    if (on)
        c->flags |= f;
    else
        c->flags &= (uint16_t)~f;
}

static void set_szp(Cpu8086 *c, uint32_t v, int w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF;
    uint32_t sign = w ? 0x8000 : 0x80;
    set_flag(c, F_ZF, (v & mask) == 0);
    set_flag(c, F_SF, (v & sign) != 0);
    set_flag(c, F_PF, parity_table[v & 0xFF]);
}

static uint32_t do_add(Cpu8086 *c, uint32_t a, uint32_t b, uint32_t cin, int w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint32_t r = a + b + cin;
    set_flag(c, F_CF, r > mask);
    set_flag(c, F_OF, ((r ^ a) & (r ^ b) & sign) != 0);
    set_flag(c, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    set_szp(c, r, w);
    return r & mask;
}

static uint32_t do_sub(Cpu8086 *c, uint32_t a, uint32_t b, uint32_t cin, int w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    uint32_t r = (a - b - cin) & mask;
    set_flag(c, F_CF, a < b + cin);
    set_flag(c, F_OF, ((a ^ b) & (a ^ r) & sign) != 0);
    set_flag(c, F_AF, ((a ^ b ^ r) & 0x10) != 0);
    set_szp(c, r, w);
    return r;
}

static uint32_t do_logic(Cpu8086 *c, uint32_t r, int w)
{
    c->flags &= (uint16_t)~(F_CF | F_OF | F_AF);
    set_szp(c, r, w);
    return r & (w ? 0xFFFF : 0xFF);
}

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static uint32_t alu(Cpu8086 *c, int op, uint32_t a, uint32_t b, int w)
{
    uint32_t cf = c->flags & F_CF;
    switch (op) {
    case 0: return do_add(c, a, b, 0, w);
    case 1: return do_logic(c, a | b, w);
    case 2: return do_add(c, a, b, cf, w);
    case 3: return do_sub(c, a, b, cf, w);
    case 4: return do_logic(c, a & b, w);
    case 5: return do_sub(c, a, b, 0, w);
    case 6: return do_logic(c, a ^ b, w);
    default: do_sub(c, a, b, 0, w); return a;
    }
}

static uint32_t do_inc_dec(Cpu8086 *c, uint32_t v, bool dec, int w)
{
    uint16_t cf = c->flags & F_CF;
    uint32_t r = dec ? do_sub(c, v, 1, 0, w) : do_add(c, v, 1, 0, w);
    c->flags = (uint16_t)((c->flags & ~F_CF) | cf);
    return r;
}

static bool condition(Cpu8086 *c, int cc)
{
    uint16_t f = c->flags;
    bool r;
    switch (cc >> 1) {
    case 0: r = f & F_OF; break;
    case 1: r = f & F_CF; break;
    case 2: r = f & F_ZF; break;
    case 3: r = (f & F_CF) || (f & F_ZF); break;
    case 4: r = f & F_SF; break;
    case 5: r = f & F_PF; break;
    case 6: r = !(f & F_SF) != !(f & F_OF); break;
    default: r = (f & F_ZF) || (!(f & F_SF) != !(f & F_OF)); break;
    }
    return (cc & 1) ? !r : r;
}

/* ---- ModRM ------------------------------------------------------------- */

typedef struct ModRM {
    int mod, reg, rm;
    uint16_t off;
    uint32_t addr;
} ModRM;

typedef struct Decode {
    int seg_override; /* -1 = none */
    int rep;          /* 0, 0xF2, 0xF3 */
} Decode;

static void decode_modrm(Cpu8086 *c, Decode *d, ModRM *m)
{
    uint8_t b = fetch8(c);
    m->mod = b >> 6;
    m->reg = (b >> 3) & 7;
    m->rm = b & 7;
    if (m->mod == 3)
        return;

    uint16_t off;
    int seg = S_DS;
    switch (m->rm) {
    case 0: off = c->regs[R_BX] + c->regs[R_SI]; break;
    case 1: off = c->regs[R_BX] + c->regs[R_DI]; break;
    case 2: off = c->regs[R_BP] + c->regs[R_SI]; seg = S_SS; break;
    case 3: off = c->regs[R_BP] + c->regs[R_DI]; seg = S_SS; break;
    case 4: off = c->regs[R_SI]; break;
    case 5: off = c->regs[R_DI]; break;
    case 6:
        if (m->mod == 0) {
            off = 0;
        } else {
            off = c->regs[R_BP];
            seg = S_SS;
        }
        break;
    default: off = c->regs[R_BX]; break;
    }
    if (m->mod == 0 && m->rm == 6)
        off = fetch16(c);
    else if (m->mod == 1)
        off = (uint16_t)(off + (int8_t)fetch8(c));
    else if (m->mod == 2)
        off = (uint16_t)(off + fetch16(c));

    if (d->seg_override >= 0)
        seg = d->seg_override;
    m->off = off;
    m->addr = cpu_linear(c->sregs[seg], off);
    c->cycles += EA_CYCLES;
}

static uint32_t get_e(Cpu8086 *c, ModRM *m, int w)
{
    if (m->mod == 3)
        return w ? c->regs[m->rm] : get_r8(c, m->rm);
    return w ? cpu_read16(c, m->addr) : cpu_read8(c, m->addr);
}

static void set_e(Cpu8086 *c, ModRM *m, int w, uint32_t v)
{
    if (m->mod == 3) {
        if (w)
            c->regs[m->rm] = (uint16_t)v;
        else
            set_r8(c, m->rm, (uint8_t)v);
    } else if (w) {
        cpu_write16(c, m->addr, (uint16_t)v);
    } else {
        cpu_write8(c, m->addr, (uint8_t)v);
    }
}

static uint32_t get_g(Cpu8086 *c, ModRM *m, int w)
{
    return w ? c->regs[m->reg] : get_r8(c, m->reg);
}

static void set_g(Cpu8086 *c, ModRM *m, int w, uint32_t v)
{
    if (w)
        c->regs[m->reg] = (uint16_t)v;
    else
        set_r8(c, m->reg, (uint8_t)v);
}

/* ---- group 2: shifts and rotates ----------------------------------------- */

static uint32_t shift_rotate(Cpu8086 *c, int op, uint32_t v, int count, int w)
{
    uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
    int bits = w ? 16 : 8;
    if (count == 0)
        return v;
    c->cycles += 4 * count;
    bool cf = c->flags & F_CF;
    for (int i = 0; i < count; i++) {
        bool msb = v & sign;
        switch (op) {
        case 0: /* ROL */
            cf = msb;
            v = ((v << 1) | cf) & mask;
            break;
        case 1: /* ROR */
            cf = v & 1;
            v = (v >> 1) | (cf ? sign : 0);
            break;
        case 2: { /* RCL */
            bool ncf = msb;
            v = ((v << 1) | cf) & mask;
            cf = ncf;
            break;
        }
        case 3: { /* RCR */
            bool ncf = v & 1;
            v = (v >> 1) | (cf ? sign : 0);
            cf = ncf;
            break;
        }
        case 4: case 6: /* SHL/SAL */
            cf = msb;
            v = (v << 1) & mask;
            break;
        case 5: /* SHR */
            cf = v & 1;
            v >>= 1;
            break;
        default: /* SAR */
            cf = v & 1;
            v = (v >> 1) | (v & sign);
            break;
        }
    }
    set_flag(c, F_CF, cf);
    bool msb = v & sign;
    switch (op) {
    case 0: case 2: case 4: case 6:
        set_flag(c, F_OF, msb != cf);
        break;
    case 1: case 3:
        set_flag(c, F_OF, msb != (bool)(v & (sign >> 1)));
        break;
    case 5:
        set_flag(c, F_OF, count == 1 && (v & (sign >> 1)));
        break;
    default:
        set_flag(c, F_OF, false);
        break;
    }
    if (op >= 4)
        set_szp(c, v, w);
    (void)bits;
    return v;
}

/* ---- group 3: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV --------------------------- */

static void divide_error(Cpu8086 *c)
{
    cpu_interrupt(c, 0);
}

static void group3(Cpu8086 *c, Decode *d, int w)
{
    ModRM m;
    decode_modrm(c, d, &m);
    uint32_t v = get_e(c, &m, w);
    switch (m.reg) {
    case 0: case 1:
        do_logic(c, v & (w ? fetch16(c) : fetch8(c)), w);
        c->cycles += 5;
        break;
    case 2:
        set_e(c, &m, w, ~v & (w ? 0xFFFF : 0xFF));
        c->cycles += 3;
        break;
    case 3:
        set_e(c, &m, w, do_sub(c, 0, v, 0, w));
        set_flag(c, F_CF, v != 0);
        c->cycles += 3;
        break;
    case 4: /* MUL */
        if (w) {
            uint32_t r = (uint32_t)c->regs[R_AX] * v;
            c->regs[R_AX] = (uint16_t)r;
            c->regs[R_DX] = (uint16_t)(r >> 16);
            set_flag(c, F_CF, c->regs[R_DX] != 0);
            set_flag(c, F_OF, c->regs[R_DX] != 0);
            c->cycles += 118;
        } else {
            uint16_t r = (uint16_t)((c->regs[R_AX] & 0xFF) * v);
            c->regs[R_AX] = r;
            set_flag(c, F_CF, (r >> 8) != 0);
            set_flag(c, F_OF, (r >> 8) != 0);
            c->cycles += 70;
        }
        set_flag(c, F_ZF, false);
        break;
    case 5: /* IMUL */
        if (w) {
            int32_t r = (int32_t)(int16_t)c->regs[R_AX] * (int16_t)v;
            c->regs[R_AX] = (uint16_t)r;
            c->regs[R_DX] = (uint16_t)((uint32_t)r >> 16);
            bool ext = r != (int16_t)r;
            set_flag(c, F_CF, ext);
            set_flag(c, F_OF, ext);
            c->cycles += 128;
        } else {
            int16_t r = (int16_t)((int8_t)c->regs[R_AX] * (int8_t)v);
            c->regs[R_AX] = (uint16_t)r;
            bool ext = r != (int8_t)r;
            set_flag(c, F_CF, ext);
            set_flag(c, F_OF, ext);
            c->cycles += 80;
        }
        set_flag(c, F_ZF, false);
        break;
    case 6: /* DIV */
        if (v == 0) {
            divide_error(c);
            return;
        }
        if (w) {
            uint32_t n = (uint32_t)c->regs[R_DX] << 16 | c->regs[R_AX];
            uint32_t q = n / v;
            if (q > 0xFFFF) {
                divide_error(c);
                return;
            }
            c->regs[R_AX] = (uint16_t)q;
            c->regs[R_DX] = (uint16_t)(n % v);
            c->cycles += 144;
        } else {
            uint16_t n = c->regs[R_AX];
            uint16_t q = (uint16_t)(n / v);
            if (q > 0xFF) {
                divide_error(c);
                return;
            }
            c->regs[R_AX] = (uint16_t)((n % v) << 8 | q);
            c->cycles += 80;
        }
        break;
    default: /* IDIV */
        if (v == 0) {
            divide_error(c);
            return;
        }
        if (w) {
            int32_t n = (int32_t)((uint32_t)c->regs[R_DX] << 16 | c->regs[R_AX]);
            int32_t dv = (int16_t)v;
            int32_t q = n / dv;
            if (q > 32767 || q < -32768) {
                divide_error(c);
                return;
            }
            c->regs[R_AX] = (uint16_t)q;
            c->regs[R_DX] = (uint16_t)(n % dv);
            c->cycles += 165;
        } else {
            int32_t n = (int16_t)c->regs[R_AX];
            int32_t dv = (int8_t)v;
            int32_t q = n / dv;
            if (q > 127 || q < -128) {
                divide_error(c);
                return;
            }
            c->regs[R_AX] = (uint16_t)(((uint8_t)(n % dv)) << 8 | (uint8_t)q);
            c->cycles += 101;
        }
        break;
    }
}

/* ---- string instructions --------------------------------------------------- */

static void string_op(Cpu8086 *c, Decode *d, uint8_t op)
{
    int w = op & 1;
    int size = w ? 2 : 1;
    int src_seg = d->seg_override >= 0 ? d->seg_override : S_DS;
    int16_t delta = (int16_t)((c->flags & F_DF) ? -size : size);
    bool is_cmp = (op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE;

    for (;;) {
        if (d->rep && c->regs[R_CX] == 0)
            break;
        uint32_t src = cpu_linear(c->sregs[src_seg], c->regs[R_SI]);
        uint32_t dst = cpu_linear(c->sregs[S_ES], c->regs[R_DI]);
        switch (op & 0xFE) {
        case 0xA4: /* MOVS */
            if (w)
                cpu_write16(c, dst, cpu_read16(c, src));
            else
                cpu_write8(c, dst, cpu_read8(c, src));
            c->regs[R_SI] += delta;
            c->regs[R_DI] += delta;
            c->cycles += 17;
            break;
        case 0xA6: /* CMPS */
            if (w)
                do_sub(c, cpu_read16(c, src), cpu_read16(c, dst), 0, 1);
            else
                do_sub(c, cpu_read8(c, src), cpu_read8(c, dst), 0, 0);
            c->regs[R_SI] += delta;
            c->regs[R_DI] += delta;
            c->cycles += 22;
            break;
        case 0xAA: /* STOS */
            if (w)
                cpu_write16(c, dst, c->regs[R_AX]);
            else
                cpu_write8(c, dst, (uint8_t)c->regs[R_AX]);
            c->regs[R_DI] += delta;
            c->cycles += 10;
            break;
        case 0xAC: /* LODS */
            if (w)
                c->regs[R_AX] = cpu_read16(c, src);
            else
                set_r8(c, 0, cpu_read8(c, src));
            c->regs[R_SI] += delta;
            c->cycles += 13;
            break;
        default: /* SCAS */
            if (w)
                do_sub(c, c->regs[R_AX], cpu_read16(c, dst), 0, 1);
            else
                do_sub(c, c->regs[R_AX] & 0xFF, cpu_read8(c, dst), 0, 0);
            c->regs[R_DI] += delta;
            c->cycles += 15;
            break;
        }
        if (!d->rep)
            break;
        c->regs[R_CX]--;
        if (is_cmp) {
            bool zf = c->flags & F_ZF;
            if ((d->rep == 0xF3 && !zf) || (d->rep == 0xF2 && zf))
                break;
        }
    }
}

/* ---- main dispatch --------------------------------------------------------- */

int cpu_step(Cpu8086 *c)
{
    uint64_t start = c->cycles;
    Decode d = { -1, 0 };
    ModRM m;
    uint8_t op;

    if (!tables_ready)
        init_tables();
    c->int_inhibit = false;
    if (c->trace)
        c->trace[cpu_linear(c->sregs[S_CS], c->ip)] |= T_EXEC | T_RUN;
    if (c->halted) {
        c->cycles += 4;
        return 4;
    }
    if (c->hook_map && c->sregs[S_CS] == c->hook_seg && c->hook_map[c->ip] && c->pre_exec(c->hook_ctx, c))
        return (int)(c->cycles - start);
    if (c->exec_count)
        c->exec_count[cpu_linear(c->sregs[S_CS], c->ip)]++;

    for (;;) {
        op = fetch8(c);
        switch (op) {
        case 0x26: d.seg_override = S_ES; continue;
        case 0x2E: d.seg_override = S_CS; continue;
        case 0x36: d.seg_override = S_SS; continue;
        case 0x3E: d.seg_override = S_DS; continue;
        case 0xF0: case 0xF1: continue; /* LOCK */
        case 0xF2: case 0xF3: d.rep = op; continue;
        default: break;
        }
        break;
    }
    c->cycles += 2;

    /* ALU block 00-3F */
    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3;
        int w = op & 1;
        switch (op & 7) {
        case 0: case 1:
            decode_modrm(c, &d, &m);
            {
                uint32_t r = alu(c, aop, get_e(c, &m, w), get_g(c, &m, w), w);
                if (aop != 7)
                    set_e(c, &m, w, r);
            }
            c->cycles += m.mod == 3 ? 3 : 9;
            break;
        case 2: case 3:
            decode_modrm(c, &d, &m);
            {
                uint32_t r = alu(c, aop, get_g(c, &m, w), get_e(c, &m, w), w);
                if (aop != 7)
                    set_g(c, &m, w, r);
            }
            c->cycles += m.mod == 3 ? 3 : 9;
            break;
        case 4: {
            uint32_t r = alu(c, aop, c->regs[R_AX] & 0xFF, fetch8(c), 0);
            if (aop != 7)
                set_r8(c, 0, (uint8_t)r);
            c->cycles += 4;
            break;
        }
        default: {
            uint32_t r = alu(c, aop, c->regs[R_AX], fetch16(c), 1);
            if (aop != 7)
                c->regs[R_AX] = (uint16_t)r;
            c->cycles += 4;
            break;
        }
        }
        return (int)(c->cycles - start);
    }

    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        cpu_push(c, c->sregs[op >> 3]);
        c->cycles += 14;
        break;
    case 0x07: case 0x17: case 0x1F:
        c->sregs[op >> 3] = cpu_pop(c);
        if (op == 0x17)
            c->int_inhibit = true;
        c->cycles += 12;
        break;
    case 0x0F: /* HLE trap */
        if (c->bus.hle)
            c->bus.hle(c->bus.ctx, fetch8(c));
        break;
    case 0x27: { /* DAA */
        uint8_t al = (uint8_t)c->regs[R_AX], old = al;
        bool cf = c->flags & F_CF;
        if ((al & 0x0F) > 9 || (c->flags & F_AF)) {
            al += 6;
            set_flag(c, F_AF, true);
        } else {
            set_flag(c, F_AF, false);
        }
        if (old > 0x99 || cf) {
            al += 0x60;
            cf = true;
        } else {
            cf = false;
        }
        set_flag(c, F_CF, cf);
        set_r8(c, 0, al);
        set_szp(c, al, 0);
        c->cycles += 4;
        break;
    }
    case 0x2F: { /* DAS */
        uint8_t al = (uint8_t)c->regs[R_AX], old = al;
        bool cf = c->flags & F_CF;
        if ((al & 0x0F) > 9 || (c->flags & F_AF)) {
            al -= 6;
            set_flag(c, F_AF, true);
        } else {
            set_flag(c, F_AF, false);
        }
        if (old > 0x99 || cf) {
            al -= 0x60;
            cf = true;
        } else {
            cf = false;
        }
        set_flag(c, F_CF, cf);
        set_r8(c, 0, al);
        set_szp(c, al, 0);
        c->cycles += 4;
        break;
    }
    case 0x37: case 0x3F: { /* AAA / AAS */
        if ((c->regs[R_AX] & 0x0F) > 9 || (c->flags & F_AF)) {
            if (op == 0x37) {
                c->regs[R_AX] += 0x106;
            } else {
                set_r8(c, 0, (uint8_t)(c->regs[R_AX] - 6));
                set_r8(c, 4, (uint8_t)((c->regs[R_AX] >> 8) - 1));
            }
            set_flag(c, F_AF, true);
            set_flag(c, F_CF, true);
        } else {
            set_flag(c, F_AF, false);
            set_flag(c, F_CF, false);
        }
        set_r8(c, 0, c->regs[R_AX] & 0x0F);
        c->cycles += 8;
        break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        c->regs[op & 7] = (uint16_t)do_inc_dec(c, c->regs[op & 7], false, 1);
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        c->regs[op & 7] = (uint16_t)do_inc_dec(c, c->regs[op & 7], true, 1);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if (op == 0x54) {
            cpu_push(c, (uint16_t)(c->regs[R_SP] - 2)); /* 8086 pushes the decremented SP */
        } else {
            cpu_push(c, c->regs[op & 7]);
        }
        c->cycles += 13;
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        c->regs[op & 7] = cpu_pop(c);
        c->cycles += 10;
        break;

    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(c);
        if (condition(c, op & 0x0F)) {
            c->ip = (uint16_t)(c->ip + rel);
            trace_target(c, T_JUMP);
            c->cycles += 14;
        } else {
            c->cycles += 2;
        }
        break;
    }

    case 0x80: case 0x81: case 0x82: case 0x83: {
        int w = op & 1;
        decode_modrm(c, &d, &m);
        uint32_t a = get_e(c, &m, w);
        uint32_t b;
        if (op == 0x81)
            b = fetch16(c);
        else if (op == 0x83)
            b = (uint16_t)(int8_t)fetch8(c);
        else
            b = fetch8(c);
        uint32_t r = alu(c, m.reg, a, b, w);
        if (m.reg != 7)
            set_e(c, &m, w, r);
        c->cycles += m.mod == 3 ? 4 : 10;
        break;
    }
    case 0x84: case 0x85: {
        int w = op & 1;
        decode_modrm(c, &d, &m);
        do_logic(c, get_e(c, &m, w) & get_g(c, &m, w), w);
        c->cycles += 3;
        break;
    }
    case 0x86: case 0x87: {
        int w = op & 1;
        decode_modrm(c, &d, &m);
        uint32_t a = get_e(c, &m, w);
        set_e(c, &m, w, get_g(c, &m, w));
        set_g(c, &m, w, a);
        c->cycles += m.mod == 3 ? 4 : 17;
        break;
    }
    case 0x88: case 0x89:
        decode_modrm(c, &d, &m);
        set_e(c, &m, op & 1, get_g(c, &m, op & 1));
        c->cycles += m.mod == 3 ? 2 : 9;
        break;
    case 0x8A: case 0x8B:
        decode_modrm(c, &d, &m);
        set_g(c, &m, op & 1, get_e(c, &m, op & 1));
        c->cycles += m.mod == 3 ? 2 : 8;
        break;
    case 0x8C:
        decode_modrm(c, &d, &m);
        set_e(c, &m, 1, c->sregs[m.reg & 3]);
        c->cycles += 2;
        break;
    case 0x8D:
        decode_modrm(c, &d, &m);
        c->regs[m.reg] = m.off;
        c->cycles += 2;
        break;
    case 0x8E:
        decode_modrm(c, &d, &m);
        c->sregs[m.reg & 3] = (uint16_t)get_e(c, &m, 1);
        if ((m.reg & 3) == S_SS)
            c->int_inhibit = true;
        c->cycles += 2;
        break;
    case 0x8F:
        decode_modrm(c, &d, &m);
        set_e(c, &m, 1, cpu_pop(c));
        c->cycles += 17;
        break;

    case 0x90:
        c->cycles += 1;
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint16_t t = c->regs[op & 7];
        c->regs[op & 7] = c->regs[R_AX];
        c->regs[R_AX] = t;
        c->cycles += 1;
        break;
    }
    case 0x98:
        c->regs[R_AX] = (uint16_t)(int8_t)c->regs[R_AX];
        break;
    case 0x99:
        c->regs[R_DX] = (c->regs[R_AX] & 0x8000) ? 0xFFFF : 0;
        c->cycles += 3;
        break;
    case 0x9A: {
        uint16_t ip = fetch16(c);
        uint16_t cs = fetch16(c);
        cpu_push(c, c->sregs[S_CS]);
        cpu_push(c, c->ip);
        c->ip = ip;
        trace_target(c, T_CALL);
        c->sregs[S_CS] = cs;
        c->cycles += 36;
        break;
    }
    case 0x9B:
        break;
    case 0x9C:
        cpu_push(c, c->flags);
        c->cycles += 10;
        break;
    case 0x9D:
        c->flags = (uint16_t)((cpu_pop(c) & 0x0FD5) | 0xF002);
        c->cycles += 8;
        break;
    case 0x9E:
        c->flags = (uint16_t)((c->flags & 0xFF00) | ((c->regs[R_AX] >> 8) & 0xD5) | 0x02);
        break;
    case 0x9F:
        set_r8(c, 4, (uint8_t)c->flags);
        break;

    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        uint16_t off = fetch16(c);
        int seg = d.seg_override >= 0 ? d.seg_override : S_DS;
        uint32_t a = cpu_linear(c->sregs[seg], off);
        if (op == 0xA0)
            set_r8(c, 0, cpu_read8(c, a));
        else if (op == 0xA1)
            c->regs[R_AX] = cpu_read16(c, a);
        else if (op == 0xA2)
            cpu_write8(c, a, (uint8_t)c->regs[R_AX]);
        else
            cpu_write16(c, a, c->regs[R_AX]);
        c->cycles += 10;
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, &d, op);
        break;
    case 0xA8:
        do_logic(c, c->regs[R_AX] & fetch8(c), 0);
        c->cycles += 2;
        break;
    case 0xA9:
        do_logic(c, c->regs[R_AX] & fetch16(c), 1);
        c->cycles += 2;
        break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        set_r8(c, op & 7, fetch8(c));
        c->cycles += 2;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        c->regs[op & 7] = fetch16(c);
        c->cycles += 2;
        break;

    case 0xC0: case 0xC2: {
        uint16_t n = fetch16(c);
        c->ip = cpu_pop(c);
        c->regs[R_SP] += n;
        c->cycles += 18;
        break;
    }
    case 0xC1: case 0xC3:
        c->ip = cpu_pop(c);
        c->cycles += 18;
        break;
    case 0xC4: case 0xC5:
        decode_modrm(c, &d, &m);
        c->regs[m.reg] = cpu_read16(c, m.addr);
        c->sregs[op == 0xC4 ? S_ES : S_DS] = cpu_read16(c, m.addr + 2);
        c->cycles += 16;
        break;
    case 0xC6: case 0xC7:
        decode_modrm(c, &d, &m);
        set_e(c, &m, op & 1, (op & 1) ? fetch16(c) : fetch8(c));
        c->cycles += 10;
        break;
    case 0xC8: case 0xCA: {
        uint16_t n = fetch16(c);
        c->ip = cpu_pop(c);
        c->sregs[S_CS] = cpu_pop(c);
        c->regs[R_SP] += n;
        c->cycles += 25;
        break;
    }
    case 0xC9: case 0xCB:
        c->ip = cpu_pop(c);
        c->sregs[S_CS] = cpu_pop(c);
        c->cycles += 26;
        break;
    case 0xCC:
        cpu_interrupt(c, 3);
        break;
    case 0xCD:
        cpu_interrupt(c, fetch8(c));
        break;
    case 0xCE:
        if (c->flags & F_OF)
            cpu_interrupt(c, 4);
        break;
    case 0xCF:
        c->ip = cpu_pop(c);
        c->sregs[S_CS] = cpu_pop(c);
        c->flags = (uint16_t)((cpu_pop(c) & 0x0FD5) | 0xF002);
        c->cycles += 32;
        break;

    case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int w = op & 1;
        decode_modrm(c, &d, &m);
        int count = (op & 2) ? (c->regs[R_CX] & 0xFF) : 1;
        set_e(c, &m, w, shift_rotate(c, m.reg, get_e(c, &m, w), count, w));
        c->cycles += 2;
        break;
    }
    case 0xD4: { /* AAM */
        uint8_t base = fetch8(c);
        if (base == 0) {
            divide_error(c);
            break;
        }
        uint8_t al = (uint8_t)c->regs[R_AX];
        set_r8(c, 4, al / base);
        set_r8(c, 0, al % base);
        set_szp(c, c->regs[R_AX] & 0xFF, 0);
        c->cycles += 83;
        break;
    }
    case 0xD5: { /* AAD */
        uint8_t base = fetch8(c);
        uint8_t al = (uint8_t)((c->regs[R_AX] & 0xFF) + (c->regs[R_AX] >> 8) * base);
        c->regs[R_AX] = al;
        set_szp(c, al, 0);
        c->cycles += 60;
        break;
    }
    case 0xD6: /* SALC */
        set_r8(c, 0, (c->flags & F_CF) ? 0xFF : 0);
        break;
    case 0xD7: {
        int seg = d.seg_override >= 0 ? d.seg_override : S_DS;
        set_r8(c, 0, cpu_read8(c, cpu_linear(c->sregs[seg], (uint16_t)(c->regs[R_BX] + (c->regs[R_AX] & 0xFF)))));
        c->cycles += 11;
        break;
    }
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        decode_modrm(c, &d, &m); /* FPU escape: no coprocessor */
        break;

    case 0xE0: case 0xE1: case 0xE2: {
        int8_t rel = (int8_t)fetch8(c);
        c->regs[R_CX]--;
        bool jump = c->regs[R_CX] != 0;
        if (op == 0xE0)
            jump = jump && !(c->flags & F_ZF);
        else if (op == 0xE1)
            jump = jump && (c->flags & F_ZF);
        if (jump) {
            c->ip = (uint16_t)(c->ip + rel);
            trace_target(c, T_JUMP);
            c->cycles += 15;
        } else {
            c->cycles += 3;
        }
        break;
    }
    case 0xE3: {
        int8_t rel = (int8_t)fetch8(c);
        if (c->regs[R_CX] == 0) {
            c->ip = (uint16_t)(c->ip + rel);
            trace_target(c, T_JUMP);
            c->cycles += 16;
        } else {
            c->cycles += 4;
        }
        break;
    }
    case 0xE4: case 0xE5: case 0xEC: case 0xED: {
        uint16_t port = (op & 8) ? c->regs[R_DX] : fetch8(c);
        uint8_t lo = c->bus.in8(c->bus.ctx, port);
        if (op & 1)
            c->regs[R_AX] = (uint16_t)(lo | c->bus.in8(c->bus.ctx, (uint16_t)(port + 1)) << 8);
        else
            set_r8(c, 0, lo);
        c->cycles += 10;
        break;
    }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: {
        uint16_t port = (op & 8) ? c->regs[R_DX] : fetch8(c);
        c->bus.out8(c->bus.ctx, port, (uint8_t)c->regs[R_AX]);
        if (op & 1)
            c->bus.out8(c->bus.ctx, (uint16_t)(port + 1), (uint8_t)(c->regs[R_AX] >> 8));
        c->cycles += 10;
        break;
    }
    case 0xE8: {
        uint16_t rel = fetch16(c);
        cpu_push(c, c->ip);
        c->ip = (uint16_t)(c->ip + rel);
        trace_target(c, T_CALL);
        c->cycles += 19;
        break;
    }
    case 0xE9: {
        uint16_t rel = fetch16(c);
        c->ip = (uint16_t)(c->ip + rel);
        trace_target(c, T_JUMP);
        c->cycles += 15;
        break;
    }
    case 0xEA: {
        uint16_t ip = fetch16(c);
        uint16_t cs = fetch16(c);
        c->ip = ip;
        trace_target(c, T_JUMP);
        c->sregs[S_CS] = cs;
        c->cycles += 15;
        break;
    }
    case 0xEB: {
        int8_t rel = (int8_t)fetch8(c);
        c->ip = (uint16_t)(c->ip + rel);
        trace_target(c, T_JUMP);
        c->cycles += 15;
        break;
    }

    case 0xF4:
        c->halted = true;
        break;
    case 0xF5:
        c->flags ^= F_CF;
        break;
    case 0xF6: case 0xF7:
        group3(c, &d, op & 1);
        break;
    case 0xF8: set_flag(c, F_CF, false); break;
    case 0xF9: set_flag(c, F_CF, true); break;
    case 0xFA: set_flag(c, F_IF, false); break;
    case 0xFB:
        set_flag(c, F_IF, true);
        c->int_inhibit = true;
        break;
    case 0xFC: set_flag(c, F_DF, false); break;
    case 0xFD: set_flag(c, F_DF, true); break;

    case 0xFE: case 0xFF: {
        int w = op & 1;
        decode_modrm(c, &d, &m);
        switch (m.reg) {
        case 0:
            set_e(c, &m, w, do_inc_dec(c, get_e(c, &m, w), false, w));
            c->cycles += 3;
            break;
        case 1:
            set_e(c, &m, w, do_inc_dec(c, get_e(c, &m, w), true, w));
            c->cycles += 3;
            break;
        case 2: { /* CALL near indirect */
            uint16_t target = (uint16_t)get_e(c, &m, 1);
            cpu_push(c, c->ip);
            c->ip = target;
            trace_target(c, T_CALL);
            c->cycles += 21;
            break;
        }
        case 3: { /* CALL far indirect */
            uint16_t ip = cpu_read16(c, m.addr);
            uint16_t cs = cpu_read16(c, m.addr + 2);
            cpu_push(c, c->sregs[S_CS]);
            cpu_push(c, c->ip);
            c->ip = ip;
            trace_target(c, T_CALL);
            c->sregs[S_CS] = cs;
            c->cycles += 37;
            break;
        }
        case 4:
            c->ip = (uint16_t)get_e(c, &m, 1);
            trace_target(c, T_JUMP);
            c->cycles += 11;
            break;
        case 5:
            c->ip = cpu_read16(c, m.addr);
            c->sregs[S_CS] = cpu_read16(c, m.addr + 2);
            trace_target(c, T_JUMP);
            c->cycles += 24;
            break;
        default:
            cpu_push(c, (uint16_t)get_e(c, &m, 1));
            c->cycles += 16;
            break;
        }
        break;
    }

    default:
        break; /* remaining opcodes are prefixes handled above */
    }

    return (int)(c->cycles - start);
}
