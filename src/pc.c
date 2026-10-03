#include "pc.h"

#include <SDL3/SDL.h>
#include <math.h>

#define BIOS_SEG 0xF000
#define BIOS_STUBS 0x0000 /* F000:0000 + n*4 holds "0F nn CF 90" for interrupt n */
#define BDA 0x400         /* BIOS data area, segment 0040 */

#define CGA_FRAME_CYCLES 79648 /* 4.77 MHz / 59.92 Hz */
#define CGA_LINE_CYCLES 304    /* 262 lines per frame */

/* The emulated hardware (PIC, PIT, port I/O, BIOS HLE) and pc_run's interpreter loop are in the
 * FS1_EMULATOR build only (3.22). The default build keeps the machine as data (memory, CGA
 * registers, the speaker log) and runs it with the C scheduler (sched.c). */

/* ---- speaker log, CGA status, key queue (both builds) ------------------------- */

void pc_speaker_log(Pc *pc)
{
    if (pc->speaker_count < PC_MAX_SPEAKER_EVENTS) {
        SpeakerEvent *e = &pc->speaker[pc->speaker_count++];
        uint64_t t = pc->cpu.cycles + (uint64_t)pc->speaker_bias;
        uint64_t floor = pc->speaker_count > 1 ? pc->speaker[pc->speaker_count - 2].cycle : pc->speaker_slice_start;
        e->cycle = t > floor ? t : floor; /* in order, within the current slice */
        e->port61 = pc->port61;
        e->pit2_reload = pc->pit[2].reload;
    }
}

uint8_t pc_cga_status(const Pc *pc)
{
    uint32_t pos = (uint32_t)(pc->cpu.cycles % CGA_FRAME_CYCLES);
    uint32_t line = pos / CGA_LINE_CYCLES;
    uint32_t col = pos % CGA_LINE_CYCLES;
    uint8_t s = 0;
    if (line >= 200 || col >= 240)
        s |= 0x01;
    if (line >= 224 && line < 240)
        s |= 0x08;
    return s;
}

void pc_key_event(Pc *pc, uint8_t scancode)
{
    int next = (pc->kbd_tail + 1) % PC_KBD_QUEUE;
    if (next == pc->kbd_head)
        return;
    pc->kbd_queue[pc->kbd_tail] = scancode;
    pc->kbd_tail = next;
}

#ifdef FS1_EMULATOR
/* ---- i8259 PIC --------------------------------------------------------------- */

static void pic_raise(Pc *pc, int irq)
{
    pc->pic_irr |= (uint8_t)(1 << irq);
}

/* Returns the vector to deliver, or -1. Moves the IRQ from IRR to ISR. */
static int pic_ack(Pc *pc)
{
    uint8_t pending = pc->pic_irr & (uint8_t)~pc->pic_imr;
    if (!pending)
        return -1;
    for (int irq = 0; irq < 8; irq++) {
        uint8_t bit = (uint8_t)(1 << irq);
        if (pc->pic_isr & ((bit << 1) - 1))
            return -1; /* equal or higher priority in service */
        if (pending & bit) {
            pc->pic_irr &= (uint8_t)~bit;
            pc->pic_isr |= bit;
            return pc->pic_vector + irq;
        }
    }
    return -1;
}

static void pic_eoi(Pc *pc)
{
    for (int irq = 0; irq < 8; irq++) {
        if (pc->pic_isr & (1 << irq)) {
            pc->pic_isr &= (uint8_t)~(1 << irq);
            return;
        }
    }
}

static void pic_write(Pc *pc, uint16_t port, uint8_t v)
{
    if (port == 0x20) {
        if (v & 0x10) { /* ICW1 */
            pc->pic_icw_step = 2;
            pc->pic_need_icw4 = v & 1;
            pc->pic_imr = 0;
            pc->pic_isr = 0;
            pc->pic_irr = 0;
        } else if ((v & 0x18) == 0x08) { /* OCW3 */
            if (v & 2)
                pc->pic_read_isr = v & 1;
        } else if ((v & 0xE0) == 0x20) { /* non-specific EOI */
            pic_eoi(pc);
        } else if ((v & 0xE0) == 0x60) { /* specific EOI */
            pc->pic_isr &= (uint8_t)~(1 << (v & 7));
        }
    } else {
        if (pc->pic_icw_step == 2) {
            pc->pic_vector = v & 0xF8;
            pc->pic_icw_step = pc->pic_need_icw4 ? 4 : 0; /* single mode: no ICW3 */
        } else if (pc->pic_icw_step == 4) {
            pc->pic_icw_step = 0;
        } else {
            pc->pic_imr = v;
        }
    }
}

/* ---- i8253 PIT ----------------------------------------------------------------- */

static int32_t pit_period(const PitChannel *ch)
{
    return ch->reload ? ch->reload : 0x10000;
}

static void pit_write(Pc *pc, uint16_t port, uint8_t v)
{
    if (port == 0x43) {
        int n = v >> 6;
        if (n == 3)
            return;
        PitChannel *ch = &pc->pit[n];
        int access = (v >> 4) & 3;
        if (access == 0) {
            ch->latched = true;
            ch->latch = (uint16_t)ch->count;
            ch->read_hi_next = false;
            return;
        }
        ch->access = (uint8_t)access;
        ch->mode = (v >> 1) & 7;
        ch->write_hi_next = false;
        ch->read_hi_next = false;
        return;
    }
    PitChannel *ch = &pc->pit[port - 0x40];
    bool done = true;
    switch (ch->access) {
    case 1: ch->reload = v; break;
    case 2: ch->reload = (uint16_t)(v << 8); break;
    default:
        if (!ch->write_hi_next) {
            ch->reload = (uint16_t)((ch->reload & 0xFF00) | v);
            ch->write_hi_next = true;
            done = false;
        } else {
            ch->reload = (uint16_t)((ch->reload & 0x00FF) | v << 8);
            ch->write_hi_next = false;
        }
        break;
    }
    if (done) {
        ch->count = pit_period(ch);
        ch->loaded = true;
        if (port == 0x42)
            pc_speaker_log(pc);
    }
}

static uint8_t pit_read(Pc *pc, uint16_t port)
{
    PitChannel *ch = &pc->pit[port - 0x40];
    uint16_t value = ch->latched ? ch->latch : (uint16_t)ch->count;
    uint8_t out;
    switch (ch->access) {
    case 1: out = (uint8_t)value; ch->latched = false; break;
    case 2: out = (uint8_t)(value >> 8); ch->latched = false; break;
    default:
        if (!ch->read_hi_next) {
            out = (uint8_t)value;
            ch->read_hi_next = true;
        } else {
            out = (uint8_t)(value >> 8);
            ch->read_hi_next = false;
            ch->latched = false;
        }
        break;
    }
    return out;
}

static void pit_advance(Pc *pc, uint32_t cpu_cycles)
{
    uint32_t total = pc->pit_cycle_frac + cpu_cycles;
    int32_t ticks = (int32_t)(total / 4);
    pc->pit_cycle_frac = total % 4;
    if (!ticks)
        return;
    for (int n = 0; n < 3; n++) {
        PitChannel *ch = &pc->pit[n];
        if (!ch->loaded)
            continue;
        ch->count -= ticks;
        if (ch->count <= 0) {
            int32_t period = pit_period(ch);
            int fired = 0;
            while (ch->count <= 0) {
                ch->count += period;
                fired++;
            }
            if (n == 0 && fired) {
                pic_raise(pc, 0);
                /* Ticks beyond the first fell due inside one long step (a native routine or
                 * a REP string op), where real hardware would have been interrupted. Keep
                 * them and deliver them one at a time so the game's timer-driven clock,
                 * physics and sound keep their rates. */
                if (ch->mode != 0 && fired > 1) {
                    pc->irq0_backlog += (uint32_t)(fired - 1);
                    if (pc->irq0_backlog > 256)
                        pc->irq0_backlog = 256;
                }
            }
            if (ch->mode == 0 && n == 0)
                ch->loaded = false; /* one-shot */
        }
    }
}

/* ---- keyboard ------------------------------------------------------------------- */

static void kbd_poll(Pc *pc)
{
    if (pc->kbd_full || pc->kbd_head == pc->kbd_tail || pc->cpu.cycles < pc->kbd_next_cycle)
        return;
    pc->kbd_latch = pc->kbd_queue[pc->kbd_head];
    pc->kbd_head = (pc->kbd_head + 1) % PC_KBD_QUEUE;
    pc->kbd_full = true;
    pc->kbd_next_cycle = pc->cpu.cycles + 10000;
    pic_raise(pc, 1);
}

/* ---- port I/O ----------------------------------------------------------------- */

static uint8_t port_in(void *ctx, uint16_t port)
{
    Pc *pc = ctx;
    switch (port) {
    case 0x20:
        return pc->pic_read_isr ? pc->pic_isr : pc->pic_irr;
    case 0x21:
        return pc->pic_imr;
    case 0x40: case 0x41: case 0x42:
        return pit_read(pc, port);
    case 0x60:
        pc->kbd_full = false;
        return pc->kbd_latch;
    case 0x61:
        return pc->port61;
    case 0x62: /* 8255 port C: PIT ch2 output in bit 5 */
        return (pc->pit[2].count > pit_period(&pc->pit[2]) / 2) ? 0x20 : 0x00;
    case 0x201: /* game port: no joystick attached */
        return 0xFF;
    case 0x3D5:
        return pc->crtc[pc->crtc_index & 31];
    case 0x3DA:
        return pc_cga_status(pc);
    case 0x3F4: /* FDC main status: ready */
        return 0x80;
    default:
        return 0xFF;
    }
}

static void port_out(void *ctx, uint16_t port, uint8_t v)
{
    Pc *pc = ctx;
    switch (port) {
    case 0x20: case 0x21:
        pic_write(pc, port, v);
        break;
    case 0x40: case 0x41: case 0x42: case 0x43:
        pit_write(pc, port, v);
        break;
    case 0x61:
        if (v != pc->port61) {
            pc->port61 = v;
            pc_speaker_log(pc);
        }
        break;
    case 0x3D4:
        pc->crtc_index = v;
        break;
    case 0x3D5:
        pc->crtc[pc->crtc_index & 31] = v;
        break;
    case 0x3D8:
        pc->cga_mode = v;
        break;
    case 0x3D9:
        pc->cga_color = v;
        break;
    default:
        break;
    }
}

/* ---- high-level BIOS --------------------------------------------------------------- */

static uint8_t bda8(Pc *pc, uint16_t off) { return pc->mem[BDA + off]; }
static uint16_t bda16(Pc *pc, uint16_t off) { return (uint16_t)(pc->mem[BDA + off] | pc->mem[BDA + off + 1] << 8); }
static void bda_set8(Pc *pc, uint16_t off, uint8_t v) { pc->mem[BDA + off] = v; }
static void bda_set16(Pc *pc, uint16_t off, uint16_t v)
{
    pc->mem[BDA + off] = (uint8_t)v;
    pc->mem[BDA + off + 1] = (uint8_t)(v >> 8);
}

#define AX (pc->cpu.regs[R_AX])
#define BX (pc->cpu.regs[R_BX])
#define CX (pc->cpu.regs[R_CX])
#define DX (pc->cpu.regs[R_DX])
#define AH ((uint8_t)(AX >> 8))
#define AL ((uint8_t)AX)

static void set_ah(Pc *pc, uint8_t v) { AX = (uint16_t)((AX & 0x00FF) | v << 8); }
static void set_al(Pc *pc, uint8_t v) { AX = (uint16_t)((AX & 0xFF00) | v); }

/* The HLE stub ends in IRET, so return flags are patched in the stacked FLAGS. */
static void set_return_flag(Pc *pc, uint16_t flag, bool on)
{
    uint32_t a = cpu_linear(pc->cpu.sregs[S_SS], (uint16_t)(pc->cpu.regs[R_SP] + 4));
    uint16_t f = cpu_read16(&pc->cpu, a);
    f = on ? (uint16_t)(f | flag) : (uint16_t)(f & ~flag);
    cpu_write16(&pc->cpu, a, f);
}

/* Re-executes the current HLE stub next step (used to block, e.g. waiting for a key). */
static void hle_retry(Pc *pc)
{
    pc->cpu.ip -= 2;
    pc->cpu.flags |= F_IF;
}

static void cga_set_mode(Pc *pc, uint8_t mode)
{
    static const uint8_t mode_reg[8] = { 0x2C, 0x28, 0x2D, 0x29, 0x2A, 0x2E, 0x1E, 0x29 };
    mode &= 0x7F;
    if (mode > 7)
        return;
    pc->cga_mode = mode_reg[mode];
    pc->cga_color = mode == 6 ? 0x0F : 0x30;
    bda_set8(pc, 0x49, mode);
    bda_set16(pc, 0x4A, (mode <= 1) ? 40 : 80);
    bda_set8(pc, 0x65, pc->cga_mode);
    bda_set8(pc, 0x66, pc->cga_color);
    if (mode >= 4) {
        SDL_memset(pc->mem + 0xB8000, 0, 0x4000);
    } else {
        for (int i = 0; i < 0x4000; i += 2) {
            pc->mem[0xB8000 + i] = ' ';
            pc->mem[0xB8000 + i + 1] = 0x07;
        }
    }
}

static void bios_int10(Pc *pc)
{
    switch (AH) {
    case 0x00:
        cga_set_mode(pc, AL);
        break;
    case 0x0B:
        if ((BX >> 8) == 0)
            pc->cga_color = (uint8_t)((pc->cga_color & 0xF0) | (BX & 0x1F));
        else
            pc->cga_color = (uint8_t)((pc->cga_color & ~0x20) | ((BX & 1) ? 0x20 : 0));
        break;
    case 0x0F:
        set_al(pc, bda8(pc, 0x49));
        set_ah(pc, (uint8_t)bda16(pc, 0x4A));
        BX &= 0x00FF;
        break;
    default:
        break;
    }
}

static void bios_int13(Pc *pc)
{
    Disk *disk = pc->disk;
    uint8_t drive = (uint8_t)DX;
    uint8_t head = (uint8_t)(DX >> 8);
    uint8_t cyl = (uint8_t)(CX >> 8);
    uint8_t sector = (uint8_t)(CX & 0x3F);
    uint8_t status = 0;

    switch (AH) {
    case 0x00: /* reset */
    case 0x04: /* verify */
    case 0x05: /* format track: accepted, nothing to do */
        if (drive != 0)
            status = 0x80;
        break;
    case 0x01:
        status = bda8(pc, 0x41);
        break;
    case 0x02: case 0x03: {
        uint32_t buf = cpu_linear(pc->cpu.sregs[S_ES], BX);
        int count = AL;
        if (drive != 0) {
            status = 0x80;
        } else if (head != 0 || cyl >= disk->tracks || sector == 0 ||
                   sector - 1 + count > disk->sectors_per_track) {
            status = 0x04; /* sector not found */
        } else {
            for (int i = 0; i < count; i++) {
                uint8_t *sec = (uint8_t *)disk_sector(disk, cyl, sector - 1 + i);
                for (int b = 0; b < disk->sector_size; b++) {
                    uint32_t a = (buf + (uint32_t)i * disk->sector_size + b) & CPU_MEM_MASK;
                    if (AH == 0x02)
                        cpu_write8(&pc->cpu, a, sec[b]);
                    else
                        sec[b] = pc->mem[a]; /* writes stay in memory, never saved */
                }
            }
            /* Disk reads are instant: the game never depends on drive timing, and charging
             * cycles here would only delay start-up and scenery loads. */
        }
        if (status)
            set_al(pc, 0);
        break;
    }
    default:
        status = 0x01;
        break;
    }
    bda_set8(pc, 0x41, status);
    set_ah(pc, status);
    set_return_flag(pc, F_CF, status != 0);
}

static void kbd_buffer_push(Pc *pc, uint16_t word)
{
    uint16_t head = bda16(pc, 0x1A), tail = bda16(pc, 0x1C);
    uint16_t next = (uint16_t)(tail + 2);
    if (next >= 0x3E)
        next = 0x1E;
    if (next == head)
        return;
    bda_set16(pc, tail, word);
    bda_set16(pc, 0x1C, next);
}

static uint8_t scancode_to_ascii(uint8_t sc, bool shift)
{
    static const char lower[] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char upper[] = "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    if (sc >= sizeof lower - 1)
        return 0;
    return (uint8_t)(shift ? upper[sc] : lower[sc]);
}

static void bios_int09(Pc *pc)
{
    uint8_t sc = port_in(pc, 0x60);
    uint8_t shift = bda8(pc, 0x17);
    bool up = sc & 0x80;
    uint8_t code = sc & 0x7F;
    if (code == 0x2A || code == 0x36) {
        uint8_t bit = code == 0x2A ? 0x02 : 0x01;
        bda_set8(pc, 0x17, up ? (uint8_t)(shift & ~bit) : (uint8_t)(shift | bit));
    } else if (code == 0x1D) {
        bda_set8(pc, 0x17, up ? (uint8_t)(shift & ~0x04) : (uint8_t)(shift | 0x04));
    } else if (code == 0x38) {
        bda_set8(pc, 0x17, up ? (uint8_t)(shift & ~0x08) : (uint8_t)(shift | 0x08));
    } else if (!up) {
        uint8_t ascii = scancode_to_ascii(code, shift & 0x03);
        kbd_buffer_push(pc, (uint16_t)(code << 8 | ascii));
    }
    pic_eoi(pc);
}

static void bios_int16(Pc *pc)
{
    uint16_t head = bda16(pc, 0x1A), tail = bda16(pc, 0x1C);
    switch (AH) {
    case 0x00:
        if (head == tail) {
            hle_retry(pc);
            return;
        }
        AX = bda16(pc, head);
        head = (uint16_t)(head + 2);
        bda_set16(pc, 0x1A, head >= 0x3E ? 0x1E : head);
        break;
    case 0x01:
        if (head == tail) {
            set_return_flag(pc, F_ZF, true);
        } else {
            AX = bda16(pc, head);
            set_return_flag(pc, F_ZF, false);
        }
        break;
    case 0x02:
        set_al(pc, bda8(pc, 0x17));
        break;
    default:
        break;
    }
}

static void bios_int08(Pc *pc)
{
    uint32_t ticks = (uint32_t)bda16(pc, 0x6C) | (uint32_t)bda16(pc, 0x6E) << 16;
    if (++ticks >= 0x1800B0) {
        ticks = 0;
        bda_set8(pc, 0x70, 1);
    }
    bda_set16(pc, 0x6C, (uint16_t)ticks);
    bda_set16(pc, 0x6E, (uint16_t)(ticks >> 16));
    uint8_t motor = bda8(pc, 0x40);
    if (motor && --motor == 0)
        bda_set8(pc, 0x3F, bda8(pc, 0x3F) & 0xF0);
    bda_set8(pc, 0x40, motor);
    pic_eoi(pc);
}

static void hle(void *ctx, uint8_t n)
{
    Pc *pc = ctx;
    switch (n) {
    case 0x08: bios_int08(pc); break;
    case 0x09: bios_int09(pc); break;
    case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        pic_eoi(pc);
        break;
    case 0x10: bios_int10(pc); break;
    case 0x11: AX = bda16(pc, 0x10); break;
    case 0x12: AX = bda16(pc, 0x13); break;
    case 0x13: bios_int13(pc); break;
    case 0x16: bios_int16(pc); break;
    case 0x1A:
        if (AH == 0) {
            CX = bda16(pc, 0x6E);
            DX = bda16(pc, 0x6C);
            set_al(pc, bda8(pc, 0x70));
            bda_set8(pc, 0x70, 0);
        }
        break;
    case 0x18: case 0x19:
        SDL_Log("Reboot");
        if (pc->cpu.trace)
            for (uint32_t i = 0; i < CPU_MEM_SIZE; i++)
                pc->cpu.trace[i] &= (uint8_t)~T_RUN;
        pc_power_on(pc);
        pc_boot(pc);
        break;
    default:
        if (!(pc->unknown_hle_mask[n >> 5] & (1u << (n & 31)))) {
            pc->unknown_hle_mask[n >> 5] |= 1u << (n & 31);
            SDL_Log("BIOS: unhandled int %02Xh (AX=%04X) from %04X:%04X", n, AX,
                    cpu_read16(&pc->cpu, cpu_linear(pc->cpu.sregs[S_SS], (uint16_t)(pc->cpu.regs[R_SP] + 2))),
                    cpu_read16(&pc->cpu, cpu_linear(pc->cpu.sregs[S_SS], pc->cpu.regs[R_SP])));
        }
        break;
    }
}

#endif /* FS1_EMULATOR */

/* ---- machine ------------------------------------------------------------------- */

static void bda_put8(Pc *pc, uint16_t off, uint8_t v) { pc->mem[BDA + off] = v; }
static void bda_put16(Pc *pc, uint16_t off, uint16_t v)
{
    pc->mem[BDA + off] = (uint8_t)v;
    pc->mem[BDA + off + 1] = (uint8_t)(v >> 8);
}

/* Text mode 3, as the BIOS leaves it at power-on (cga_set_mode(3) of the HLE). */
static void text_mode_3(Pc *pc)
{
    pc->cga_mode = 0x29;
    pc->cga_color = 0x30;
    bda_put8(pc, 0x49, 3);
    bda_put16(pc, 0x4A, 80);
    bda_put8(pc, 0x65, pc->cga_mode);
    bda_put8(pc, 0x66, pc->cga_color);
    for (int i = 0; i < 0x4000; i += 2) {
        pc->mem[0xB8000 + i] = ' ';
        pc->mem[0xB8000 + i + 1] = 0x07;
    }
}

/* Power-on state: clears RAM and all devices but keeps the disk, display settings,
 * font and the running cycle count (so timing and audio stay continuous). */
void pc_power_on(Pc *pc)
{
    bool csched = pc->csched;
    uint8_t *mem = pc->mem;
    Disk *disk = pc->disk;
    bool composite = pc->composite;
    uint64_t cycles = pc->cpu.cycles;
    uint8_t *trace = pc->cpu.trace;
    Cpu8086 hooks = pc->cpu; /* native replacement hooks survive a reboot */
    uint8_t font[256][8];
    SDL_memcpy(font, pc->font, sizeof font);

    SDL_memset(pc, 0, sizeof *pc);
    SDL_memset(mem, 0, CPU_MEM_SIZE);
    pc->mem = mem;
    pc->disk = disk;
    pc->composite = composite;
    pc->csched = csched;
    SDL_memcpy(pc->font, font, sizeof font);

    Cpu8086 *c = &pc->cpu;
    c->mem = pc->mem;
    c->rom_start = 0xF0000;
    cpu_reset(c);

    /* Fake BIOS ROM: one HLE stub per interrupt vector. */
    for (int n = 0; n < 256; n++) {
        uint32_t stub = BIOS_SEG * 16 + BIOS_STUBS + (uint32_t)n * 4;
        pc->mem[stub + 0] = 0x0F;
        pc->mem[stub + 1] = (uint8_t)n;
        pc->mem[stub + 2] = 0xCF;
        pc->mem[stub + 3] = 0x90;
        pc->mem[n * 4 + 0] = (uint8_t)(BIOS_STUBS + n * 4);
        pc->mem[n * 4 + 1] = (uint8_t)((BIOS_STUBS + n * 4) >> 8);
        pc->mem[n * 4 + 2] = (uint8_t)BIOS_SEG;
        pc->mem[n * 4 + 3] = (uint8_t)(BIOS_SEG >> 8);
    }
    /* BIOS reset entry F000:E05B (the game jumps here on Ctrl+Alt+Del): trap to a reboot */
    pc->mem[0xFE05B] = 0x0F;
    pc->mem[0xFE05C] = 0x19;
    /* IBM PC model byte at F000:FFFE */
    pc->mem[0xFFFFE] = 0xFF;

    /* BIOS data area */
    bda_put16(pc, 0x10, 0x0021); /* 1 floppy, 80x25 colour */
    bda_put16(pc, 0x13, 640);
    bda_put16(pc, 0x1A, 0x1E);
    bda_put16(pc, 0x1C, 0x1E);
    bda_put16(pc, 0x63, 0x3D4);

    /* PIC as the BIOS leaves it: vectors 08h-0Fh, IRQ 0, 1, 6 enabled */
    pc->pic_vector = 0x08;
    pc->pic_imr = 0xBC;

    /* PIT ch0 at 18.2 Hz; ch1 DRAM refresh is not modelled */
    pc->pit[0] = (PitChannel){ .reload = 0, .count = 0x10000, .mode = 3, .access = 3, .loaded = true };
    pc->pit[2] = (PitChannel){ .reload = 0x0533, .count = 0x533, .mode = 3, .access = 3, .loaded = true };

    text_mode_3(pc);
    c->cycles = cycles;
    c->trace = trace;
    c->hook_map = hooks.hook_map;
    c->hook_seg = hooks.hook_seg;
    c->pre_exec = hooks.pre_exec;
    c->hook_ctx = hooks.hook_ctx;
    c->write_log = hooks.write_log;
    pc->speaker_slice_start = cycles;
    pc->kbd_next_cycle = cycles;
#ifdef FS1_EMULATOR
    if (!csched)
        c->bus = (CpuBus){ pc, port_in, port_out, hle };
    else
#endif
        sched_attach(pc);
}

bool pc_init(Pc *pc, Disk *disk, bool csched)
{
    SDL_memset(pc, 0, sizeof *pc);
#ifdef FS1_EMULATOR
    pc->csched = csched;
#else
    (void)csched;
    pc->csched = true;
#endif
    pc->mem = SDL_calloc(1, CPU_MEM_SIZE);
    if (!pc->mem)
        return false;
    pc->disk = disk;
    pc->composite = true;
    pc_power_on(pc);
    return true;
}

void pc_free(Pc *pc)
{
    SDL_free(pc->mem);
    pc->mem = NULL;
}

void pc_boot(Pc *pc)
{
    if (pc->csched) {
        sched_boot(pc);
        return;
    }
#ifdef FS1_EMULATOR
    const uint8_t *boot = disk_sector(pc->disk, 0, 0);
    SDL_memcpy(pc->mem + 0x7C00, boot, (size_t)pc->disk->sector_size);
    Cpu8086 *c = &pc->cpu;
    c->sregs[S_CS] = 0;
    c->sregs[S_DS] = 0;
    c->sregs[S_ES] = 0;
    c->sregs[S_SS] = 0;
    c->regs[R_SP] = 0x7C00;
    c->regs[R_DX] = 0; /* boot drive A: */
    c->ip = 0x7C00;
    c->flags = 0xF202;
    c->halted = false;
#endif
}

void pc_sync_pit(Pc *pc)
{
#ifdef FS1_EMULATOR
    if (pc->csched)
        return; /* nothing counts down */
    uint64_t now = pc->cpu.cycles;
    if (now > pc->pit_synced)
        pit_advance(pc, (uint32_t)(now - pc->pit_synced));
    pc->pit_synced = now;
#else
    (void)pc;
#endif
}

uint64_t pc_irq_horizon(const Pc *pc)
{
    if (pc->csched)
        return sched_horizon(pc);
#ifdef FS1_EMULATOR
    uint64_t now = pc->cpu.cycles;
    if (pc->pic_irr || pc->irq0_backlog)
        return 0;
    uint64_t h = pc->run_target > now ? pc->run_target - now : 0;
    const PitChannel *t = &pc->pit[0];
    if (t->loaded) {
        /* it fires when pit_cycle_frac + (now - pit_synced) + n >= 4 * count */
        int64_t left = 4 * (int64_t)t->count - pc->pit_cycle_frac - (int64_t)(now - pc->pit_synced);
        uint64_t l = left > 0 ? (uint64_t)left : 0;
        if (l < h)
            h = l;
    }
    if (!pc->kbd_full && pc->kbd_head != pc->kbd_tail) {
        uint64_t l = pc->kbd_next_cycle > now ? pc->kbd_next_cycle - now : 0;
        if (l < h)
            h = l;
    }
    return h;
#else
    return 0;
#endif
}

void pc_run(Pc *pc, uint64_t target)
{
    if (pc->csched) {
        sched_run(pc, target);
        return;
    }
#ifdef FS1_EMULATOR
    Cpu8086 *c = &pc->cpu;
    pc->run_target = target;
    while (c->cycles < target) {
        if (pc->irq0_backlog && !(pc->pic_irr & 1) && !(pc->pic_isr & 1)) {
            pc->pic_irr |= 1;
            pc->irq0_backlog--;
        }
        if ((c->flags & F_IF) && !c->int_inhibit) {
            int vector = pic_ack(pc);
            if (vector >= 0)
                cpu_interrupt(c, (uint8_t)vector);
        }
        pc->pit_synced = c->cycles;
        cpu_step(c);
        pc_sync_pit(pc);
        kbd_poll(pc);
    }
#endif
}

/* ---- CGA rendering ---------------------------------------------------------------- */

static const uint32_t cga_rgbi[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* NTSC decode of the 640-pixel mode: each pixel is one sample at 4x the colour
 * subcarrier. Colour is demodulated from a 4-sample window around the pixel. */
#define COMPOSITE_HUE_DEG 340.0
#define COMPOSITE_CHROMA 0.75

static uint32_t composite_lut[4][16];
static bool composite_lut_ready;

static void build_composite_lut(void)
{
    double hue = COMPOSITE_HUE_DEG * SDL_PI_D / 180.0;
    for (int phase = 0; phase < 4; phase++) {
        for (int pattern = 0; pattern < 16; pattern++) {
            double y = 0, i = 0, q = 0;
            for (int k = 0; k < 4; k++) {
                int bit = pattern >> (3 - k) & 1; /* window covers pixels x-1 .. x+2 */
                double a = ((phase + 3 + k) & 3) * SDL_PI_D / 2 + hue;
                y += bit / 4.0;
                i += bit * cos(a) / 2 * COMPOSITE_CHROMA;
                q += bit * sin(a) / 2 * COMPOSITE_CHROMA;
            }
            double rgb[3] = { y + 0.956 * i + 0.621 * q, y - 0.272 * i - 0.647 * q, y - 1.106 * i + 1.703 * q };
            uint32_t c = 0;
            for (int n = 0; n < 3; n++) {
                double v = rgb[n] < 0 ? 0 : rgb[n] > 1 ? 1 : rgb[n];
                c = c << 8 | (uint32_t)(v * 255 + 0.5);
            }
            composite_lut[phase][pattern] = c;
        }
    }
    composite_lut_ready = true;
}

static int hires_pixel(const uint8_t *row, int x)
{
    if (x < 0 || x >= 640)
        return 0;
    return row[x >> 3] >> (7 - (x & 7)) & 1;
}

void pc_render_cga(const Pc *pc, uint32_t *px)
{
    const uint8_t *vram = pc->mem + 0xB8000;
    uint8_t mode = pc->cga_mode, color = pc->cga_color;

    if (!(mode & 0x08)) { /* video disabled */
        SDL_memset(px, 0, CGA_W * CGA_H * sizeof *px);
        return;
    }

    if (mode & 0x02) {
        if ((mode & 0x10) && pc->composite && !(mode & 0x04)) { /* 640x200 artifact colour */
            if (!composite_lut_ready)
                build_composite_lut();
            for (int y = 0; y < CGA_H; y++) {
                const uint8_t *row = vram + (y & 1) * 0x2000 + (y >> 1) * 80;
                for (int x = 0; x < 640; x++) {
                    int pattern = hires_pixel(row, x - 1) << 3 | hires_pixel(row, x) << 2 |
                                  hires_pixel(row, x + 1) << 1 | hires_pixel(row, x + 2);
                    px[y * CGA_W + x] = composite_lut[x & 3][pattern];
                }
            }
        } else if (mode & 0x10) { /* 640x200 mono */
            uint32_t fg = cga_rgbi[color & 0x0F];
            for (int y = 0; y < CGA_H; y++) {
                const uint8_t *row = vram + (y & 1) * 0x2000 + (y >> 1) * 80;
                for (int x = 0; x < 640; x++)
                    px[y * CGA_W + x] = (row[x >> 3] >> (7 - (x & 7)) & 1) ? fg : 0;
            }
        } else { /* 320x200 4-colour */
            uint32_t pal[4];
            int intense = (color & 0x10) ? 8 : 0;
            pal[0] = cga_rgbi[color & 0x0F];
            if (mode & 0x04) { /* "mono" bit: cyan/red/white */
                pal[1] = cga_rgbi[3 + intense];
                pal[2] = cga_rgbi[4 + intense];
                pal[3] = cga_rgbi[7 + intense];
            } else if (color & 0x20) {
                pal[1] = cga_rgbi[3 + intense];
                pal[2] = cga_rgbi[5 + intense];
                pal[3] = cga_rgbi[7 + intense];
            } else {
                pal[1] = cga_rgbi[2 + intense];
                pal[2] = cga_rgbi[4 + intense];
                pal[3] = cga_rgbi[6 + intense];
            }
            for (int y = 0; y < CGA_H; y++) {
                const uint8_t *row = vram + (y & 1) * 0x2000 + (y >> 1) * 80;
                for (int x = 0; x < 320; x++) {
                    uint32_t c = pal[row[x >> 2] >> (6 - 2 * (x & 3)) & 3];
                    px[y * CGA_W + 2 * x] = c;
                    px[y * CGA_W + 2 * x + 1] = c;
                }
            }
        }
        return;
    }

    /* Text mode (40 or 80 columns, 8-line cells). Glyphs come from pc->font. */
    int cols = (mode & 0x01) ? 80 : 40;
    int cell_w = CGA_W / cols;
    int lines = (pc->crtc[9] & 0x1F) + 1;
    if (lines > 8)
        lines = 8;
    uint32_t start = (uint32_t)((pc->crtc[12] << 8 | pc->crtc[13]) * 2) & 0x3FFF;
    bool blink_enabled = mode & 0x20;
    for (int y = 0; y < CGA_H; y++) {
        int row = y / lines, cy = y % lines;
        for (int x = 0; x < CGA_W; x++) {
            int col = x / cell_w, cx = (x % cell_w) * 8 / cell_w;
            const uint8_t *cell = vram + ((start + (uint32_t)(row * cols + col) * 2) & 0x3FFF);
            uint8_t ch = cell[0], attr = cell[1];
            int bg = blink_enabled ? (attr >> 4) & 0x07 : attr >> 4;
            bool on = pc->font[ch][cy] >> (7 - cx) & 1;
            if (blink_enabled && (attr & 0x80) && pc->blink_phase)
                on = false;
            px[y * CGA_W + x] = cga_rgbi[on ? (attr & 0x0F) : bg];
        }
    }
}

/* ---- speaker -------------------------------------------------------------------- */

int pc_speaker_render(Pc *pc, float *out, int max_samples, int rate)
{
    uint64_t start = pc->speaker_slice_start, end = pc->cpu.cycles;
    double exact = (double)(end - start) * rate / PC_CPU_HZ + pc->speaker_sample_frac;
    int n = (int)exact;
    pc->speaker_sample_frac = exact - n;
    if (n > max_samples)
        n = max_samples;

    uint8_t p61 = pc->speaker_port61_at_start;
    uint16_t reload = pc->speaker_reload_at_start;
    int e = 0;
    double cycles_per_sample = PC_CPU_HZ / rate;
    for (int i = 0; i < n; i++) {
        uint64_t t = start + (uint64_t)((i + 0.5) * cycles_per_sample);
        while (e < pc->speaker_count && pc->speaker[e].cycle <= t) {
            p61 = pc->speaker[e].port61;
            reload = pc->speaker[e].pit2_reload;
            e++;
        }
        double x;
        if ((p61 & 3) == 3) {
            double freq = PC_PIT_HZ / (reload ? reload : 65536);
            if (freq > rate / 2.0) {
                x = 0;
            } else {
                pc->speaker_phase += freq / rate;
                pc->speaker_phase -= floor(pc->speaker_phase);
                x = pc->speaker_phase < 0.5 ? 1.0 : -1.0;
            }
        } else {
            x = (p61 & 2) ? 1.0 : -1.0;
        }
        double y = x - pc->speaker_x1 + 0.995 * pc->speaker_y1;
        pc->speaker_x1 = x;
        pc->speaker_y1 = y;
        out[i] = (float)(y * 0.2);
    }

    pc->speaker_slice_start = end;
    pc->speaker_port61_at_start = pc->port61;
    pc->speaker_reload_at_start = pc->pit[2].reload;
    pc->speaker_count = 0;
    return n;
}
