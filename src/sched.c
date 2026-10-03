/* Subphase 3.22: the C scheduler and the boot in C.
 *
 * With every routine of the game replaced by a native (phases 3.1-3.22), the machine is run
 * without the 8086 interpreter. Cpu8086's register file and the 1 MB memory array stay: they
 * are the natives' calling convention and data model, and the CGA renderer reads B800.
 *
 * Scheduling (sched_run). The CPU's CS:IP always points at the address of a native: the
 * program body (start, then the main-loop blocks of 3.21) and the interrupt handlers. Each
 * step looks the native up and runs it; it charges the original's cycles to cpu.cycles, which
 * stays the game's clock. Between two steps the scheduler raises:
 *   - IRQ0 every 4 x divisor cycles, the divisor being what the game last wrote to PIT
 *     channel 0 (sound_update's 0922, 3.19); the channel's countdown is not emulated;
 *   - IRQ1 when a scancode is queued (pc_key_event, from SDL), at most one per 10,000 cycles,
 *     as the emulated keyboard did.
 * and delivers one when IF is set and no interrupt of the same or a higher priority is in
 * service (the handler's EOI to port 20h ends it). Delivery pushes FLAGS, CS and IP and jumps
 * through the IVT, as the CPU does, so the handler natives (int8_timer, int9_keyboard) and
 * their IRET are unchanged. Interrupts never fall inside a native, so the 3.11-3.21
 * hand-back and decline paths are not taken (pc->csched; they stay for the emulator build).
 *
 * Ports. The natives still do their port I/O through cpu.bus. Here it only keeps what the
 * game reads back or what the front end needs: the PIC mask and in-service bits, the
 * divisors of channels 0 and 2 (the counter value is computed from the cycle clock when the
 * game reads it), port 61h, the keyboard latch, and the CGA registers. Port 61h and channel 2
 * changes go to the speaker log, from which pc_speaker_render makes the audio (3.19 model).
 *
 * BIOS. The game calls no BIOS service after the loader. The scheduler handles the BIOS
 * timer and keyboard interrupts in C while the game has not hooked them yet, and the reboot
 * entry F000:E05B (Ctrl+Alt+Del). Any other F000 address is fatal, as is a CS:IP with no
 * native (the default build has no interpreter; the FS1_EMULATOR build runs one original
 * instruction instead and counts it, see --stats). */
#include <SDL3/SDL.h>
#include <stdlib.h>

#include "native.h"
#include "pc.h"

#define BDA 0x400
#define KBD_GAP_CYCLES 10000 /* as pc.c's emulated keyboard */
#define IRQ0_MAX_LAG 256     /* timer ticks kept when a step runs late (pc.c's backlog cap) */

/* Cycles the original loader takes from the boot sector to start (0050:5C9F) in the
 * emulator build (--check-boot prints it). The C boot charges them, so the start-up menus
 * appear at the same time and scripted sessions keep their timing. */
#define BOOT_CYCLES 8510338ull

/* ---- PIT (divisors only) ---------------------------------------------------------------- */

static uint32_t period(const PitChannel *ch) { return ch->reload ? ch->reload : 0x10000u; }

static uint16_t pit_count(const Pc *pc, int n)
{
    uint64_t ticks = (pc->cpu.cycles - pc->pit_loaded[n]) / 4;
    uint32_t p = period(&pc->pit[n]);
    return (uint16_t)(p - ticks % p);
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
            ch->latch = pit_count(pc, n);
            ch->read_hi_next = false;
            return;
        }
        ch->access = (uint8_t)access;
        ch->mode = (v >> 1) & 7;
        ch->write_hi_next = false;
        ch->read_hi_next = false;
        return;
    }
    int n = port - 0x40;
    PitChannel *ch = &pc->pit[n];
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
    if (!done)
        return;
    pc->pit_loaded[n] = pc->cpu.cycles;
    if (n == 0)
        pc->irq0_next = pc->cpu.cycles + 4ull * period(ch); /* the count restarts, as in pc.c */
    if (n == 2)
        pc_speaker_log(pc);
}

static uint8_t pit_read(Pc *pc, uint16_t port)
{
    int n = port - 0x40;
    PitChannel *ch = &pc->pit[n];
    uint16_t value = ch->latched ? ch->latch : pit_count(pc, n);
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

/* ---- ports ---------------------------------------------------------------------------- */

static void eoi(Pc *pc)
{
    for (int irq = 0; irq < 8; irq++) {
        if (pc->pic_isr & (1 << irq)) {
            pc->pic_isr &= (uint8_t)~(1 << irq);
            return;
        }
    }
}

static uint8_t port_in(void *ctx, uint16_t port)
{
    Pc *pc = ctx;
    switch (port) {
    case 0x20: return pc->pic_irr;
    case 0x21: return pc->pic_imr;
    case 0x40: case 0x41: case 0x42: return pit_read(pc, port);
    case 0x60:
        pc->kbd_full = false;
        return pc->kbd_latch;
    case 0x61: return pc->port61;
    case 0x62: return pit_count(pc, 2) > period(&pc->pit[2]) / 2 ? 0x20 : 0x00;
    case 0x3D5: return pc->crtc[pc->crtc_index & 31];
    case 0x3DA: return pc_cga_status(pc);
    case 0x3F4: return 0x80;
    default: return 0xFF; /* includes the game port 201h: no joystick */
    }
}

static void port_out(void *ctx, uint16_t port, uint8_t v)
{
    Pc *pc = ctx;
    switch (port) {
    case 0x20:
        if ((v & 0xE0) == 0x20)
            eoi(pc);
        else if ((v & 0xE0) == 0x60)
            pc->pic_isr &= (uint8_t)~(1 << (v & 7));
        break;
    case 0x21: pc->pic_imr = v; break;
    case 0x40: case 0x41: case 0x42: case 0x43: pit_write(pc, port, v); break;
    case 0x61:
        if (v != pc->port61) {
            pc->port61 = v;
            pc_speaker_log(pc);
        }
        break;
    case 0x3D4: pc->crtc_index = v; break;
    case 0x3D5: pc->crtc[pc->crtc_index & 31] = v; break;
    case 0x3D8: pc->cga_mode = v; break;
    case 0x3D9: pc->cga_color = v; break;
    default: break;
    }
}

static void no_bios(void *ctx, uint8_t n)
{
    Pc *pc = ctx;
    SDL_Log("sched: BIOS service int %02Xh reached (AX=%04X); the C scheduler has no BIOS", n, pc->cpu.regs[R_AX]);
}

void sched_attach(Pc *pc)
{
    pc->cpu.bus = (CpuBus){ pc, port_in, port_out, no_bios };
    pc->irq0_next = pc->cpu.cycles + 4ull * period(&pc->pit[0]);
    for (int n = 0; n < 3; n++)
        pc->pit_loaded[n] = pc->cpu.cycles;
}

/* ---- interrupts ---------------------------------------------------------------------- */

static bool vector_is_bios(const Pc *pc, int vector)
{
    return (pc->mem[vector * 4 + 2] | pc->mem[vector * 4 + 3] << 8) == 0xF000;
}

/* The BIOS's own IRQ handlers, for the time before the game hooks the vector. */
static void bios_irq(Pc *pc, int irq)
{
    if (irq == 0) {
        uint32_t t = (uint32_t)(pc->mem[BDA + 0x6C] | pc->mem[BDA + 0x6D] << 8 | pc->mem[BDA + 0x6E] << 16 |
                                (uint32_t)pc->mem[BDA + 0x6F] << 24);
        if (++t >= 0x1800B0) {
            t = 0;
            pc->mem[BDA + 0x70] = 1;
        }
        for (int i = 0; i < 4; i++)
            pc->mem[BDA + 0x6C + i] = (uint8_t)(t >> (8 * i));
        uint8_t motor = pc->mem[BDA + 0x40];
        if (motor && --motor == 0)
            pc->mem[BDA + 0x3F] &= 0xF0;
        pc->mem[BDA + 0x40] = motor;
    } else {
        pc->kbd_full = false; /* the scancode is dropped: the game has not hooked IRQ1 yet */
    }
}

/* Raises the timer and keyboard, then delivers one interrupt if the CPU takes it now. A timer
 * tick can be delivered late (a native is never interrupted): its handler's speaker writes are
 * then stamped as if it had run on time (speaker_bias), so the engine note keeps the IRQ0 rate
 * that the game programmed, which is the 3.19 sound model. */
static void irqs(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    if (!pc->kbd_full && pc->kbd_head != pc->kbd_tail && c->cycles >= pc->kbd_next_cycle) {
        pc->kbd_latch = pc->kbd_queue[pc->kbd_head];
        pc->kbd_head = (pc->kbd_head + 1) % PC_KBD_QUEUE;
        pc->kbd_full = true;
        pc->kbd_next_cycle = c->cycles + KBD_GAP_CYCLES;
        pc->pic_irr |= 2;
    }
    if (!(pc->pic_irr & 1) && c->cycles >= pc->irq0_next) {
        uint64_t p = 4ull * period(&pc->pit[0]);
        pc->pic_irr |= 1;
        pc->irq0_due = pc->irq0_next;
        pc->irq0_next += p;
        if (c->cycles > pc->irq0_next + IRQ0_MAX_LAG * p)
            pc->irq0_next = c->cycles; /* a very long stall: drop the excess ticks */
    }
    if (!(c->flags & F_IF))
        return;
    uint8_t pending = pc->pic_irr & (uint8_t)~pc->pic_imr;
    for (int irq = 0; irq < 8 && pending; irq++) {
        uint8_t bit = (uint8_t)(1 << irq);
        if (pc->pic_isr & ((bit << 1) - 1))
            return;
        if (pending & bit) {
            pc->pic_irr &= (uint8_t)~bit;
            pc->sched_irqs++;
            if (vector_is_bios(pc, 8 + irq)) {
                bios_irq(pc, irq);
                return;
            }
            pc->pic_isr |= bit;
            cpu_interrupt(c, (uint8_t)(8 + irq));
            pc->irq0_entered = irq == 0;
            return;
        }
    }
}

uint64_t sched_horizon(const Pc *pc)
{
    uint64_t now = pc->cpu.cycles, h = pc->run_target > now ? pc->run_target - now : 0;
    if (pc->pic_irr & ~pc->pic_imr)
        return 0;
    uint64_t t = pc->irq0_next > now ? pc->irq0_next - now : 0;
    if (t < h)
        h = t;
    if (!pc->kbd_full && pc->kbd_head != pc->kbd_tail) {
        uint64_t k = pc->kbd_next_cycle > now ? pc->kbd_next_cycle - now : 0;
        if (k < h)
            h = k;
    }
    return h;
}

/* ---- dispatch -------------------------------------------------------------------------- */

static void no_native(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
#ifdef FS1_EMULATOR
    /* the verification build: one original instruction, counted (--stats) */
    const uint8_t *map = c->hook_map;
    c->hook_map = NULL;
    cpu_step(c);
    c->hook_map = map;
    pc->sched_fallback++;
#else
    SDL_Log("sched: no native at %04X:%04X (this build has no 8086 interpreter); stopping", c->sregs[S_CS], c->ip);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Flight Simulator 1",
                             "Internal error: the game reached code that has no C implementation.", NULL);
    exit(3);
#endif
}

#ifndef FS1_EMULATOR
/* The natives' own fallbacks (an original callee where no native is enabled) end up here in
 * the default build: there is no interpreter to run them. */
int cpu_step(Cpu8086 *c)
{
    SDL_Log("sched: original code at %04X:%04X reached from a native (this build has no 8086 interpreter); "
            "stopping",
            c->sregs[S_CS], c->ip);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Flight Simulator 1",
                             "Internal error: the game reached code that has no C implementation.", NULL);
    exit(3);
}
#endif

static void step(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    if (pc->irq0_entered) {
        /* the timer handler's speaker writes are stamped from the tick's due time (see irqs) */
        pc->irq0_entered = false;
        pc->speaker_bias = (int64_t)pc->irq0_due - (int64_t)c->cycles;
    }
    bool ran = native_step(pc);
    pc->speaker_bias = 0;
    if (ran) {
        pc->sched_steps++;
        return;
    }
    if (c->sregs[S_CS] == 0xF000 && c->ip == 0xE05B) {
        SDL_Log("Reboot");
        pc_power_on(pc);
        sched_boot(pc);
        return;
    }
    no_native(pc);
}

static void load_stream(Pc *pc);

/* The load at boot_until; BOOT_CYCLES already holds its time (cga_program_regs included). */
static void load(Pc *pc)
{
    uint64_t t = pc->cpu.cycles;
    pc->boot_pending = false;
    load_stream(pc);
    pc->cpu.cycles = t;
}

void sched_run(Pc *pc, uint64_t target)
{
    Cpu8086 *c = &pc->cpu;
    pc->run_target = target;
    while (c->cycles < target) {
        irqs(pc);
        if (c->cycles < pc->boot_until) {
            /* the loader's time (sched_boot): only the BIOS timer runs */
            uint64_t t = pc->boot_until < target ? pc->boot_until : target;
            if (pc->irq0_next > c->cycles && pc->irq0_next < t)
                t = pc->irq0_next;
            c->cycles = t > c->cycles ? t : c->cycles + 1;
            if (c->cycles >= pc->boot_until && pc->boot_pending) {
                pc->boot_pending = false;
                load(pc);
            }
            continue;
        }
        if (pc->boot_pending) {
            pc->boot_pending = false;
            load(pc);
        }
        step(pc);
    }
    /* The last step may have been long (a scenery load or draw_scenery takes 0.1-0.3 s as one
     * native): the timer ticks that fell due inside it are delivered now, each one's handler
     * running as its own step, so that the engine note (and the slow work they do) keep their
     * rate in this frame's audio instead of bunching up in a later one. */
    uint64_t end = c->cycles;
    for (int n = 0; n < 512; n++) {
        bool due = (pc->pic_irr & 1) || pc->irq0_next <= end;
        if (!due || !(c->flags & F_IF) || (pc->pic_imr & 1) || (pc->pic_isr & 1) || vector_is_bios(pc, 8))
            break;
        irqs(pc);
        if (!(pc->pic_isr & 1))
            break;
        step(pc);
    }
}

/* ---- boot ----------------------------------------------------------------------------------
 * The boot sector (relocated to 0050:0000) reads the disk track by track into B800:0000 and
 * interprets it as a command stream (tools/pc_loadstream.py):
 *   01 LL LL data           copy LL-3 bytes to 0000:DI (DI carries on)
 *   02 LL LL SS SS data     copy LL-5 bytes to SSSS:0000
 *   03 w w(SS) w(SP) w(CX) w(AX)   [0:120] = DS = AX, [0:122] = view_buf_seg = CX, SS:SP,
 *                                  call cga_program_regs
 *   04 w w(II VV)           area_tracks[II] = VV
 *   other                   jmp start (0050:5C9F)
 * This is the same in C, with the registers, the stack words and the loader's variables
 * (in the segment at 0000:0120) left as the original leaves them. */

typedef struct Loader {
    Pc *pc;
    Cpu8086 *c;
    bool cf; /* CF after the last boot_stream_byte (its CMP BH,10h, or 0 after a track read) */
} Loader;

#define LR(r) (L->c->regs[R_##r])
#define LS(r) (L->c->sregs[S_##r])

static uint16_t vseg(Loader *L) { return (uint16_t)(L->pc->mem[0x120] | L->pc->mem[0x121] << 8); }
static uint16_t v16(Loader *L, uint16_t off) { return mem_read16(L->pc, vseg(L), off); }
static void v16w(Loader *L, uint16_t off, uint16_t v) { mem_write16(L->pc, vseg(L), off, v); }
static uint8_t v8(Loader *L, uint16_t off) { return cpu_read8(L->c, cpu_linear(vseg(L), off)); }
static void v8w(Loader *L, uint16_t off, uint8_t v) { cpu_write8(L->c, cpu_linear(vseg(L), off), v); }

#define STREAM_POS 0x03B0
#define TRACK_VERIFY 0x03B2
#define DISK_TRACK 0x03B3
#define AREA_TRACKS 0x03B4
#define DISK_DX 0x03C0
#define SCREEN_SEG 0x03C2
#define VIEW_BUF_SEG 0x3806

/* boot_read_track (0104) as called from boot_stream_byte: int 13h AH=02, 8 sectors of track
 * [disk_track] into B800:0000. */
static void read_track(Loader *L)
{
    Pc *pc = L->pc;
    Cpu8086 *c = L->c;
    cpu_push(c, 0x0107); /* call sub_00F8 */
    LS(ES) = LS(DS) = vseg(L);
    cpu_pop(c);
    v8w(L, TRACK_VERIFY, 0);
    v16w(L, SCREEN_SEG, 0xB800);
    v16w(L, DISK_DX, 0);
    LR(DX) = 0;
    uint8_t track = v8(L, DISK_TRACK);
    LR(CX) = (uint16_t)(track << 8 | 1);
    LS(ES) = 0xB800;
    LR(BX) = 0;
    /* INT 13h: FLAGS, CS, IP on the stack; the BIOS clears CF in the stacked FLAGS */
    cpu_push(c, (uint16_t)(c->flags & ~F_CF));
    cpu_push(c, GAME_CS);
    cpu_push(c, 0x0137);
    const Disk *d = pc->disk;
    for (int s = 0; s < 8; s++) {
        const uint8_t *sec = disk_sector(d, track, s);
        for (int b = 0; b < d->sector_size; b++)
            cpu_write8(c, 0xB8000u + (uint32_t)(s * d->sector_size + b), sec[b]);
    }
    pc->mem[BDA + 0x41] = 0;
    LR(AX) = 0x0008; /* AH = status 0, AL = sectors read */
    c->regs[R_SP] += 6; /* IRET */
    v8w(L, TRACK_VERIFY, 0); /* SHR [track_verify],1 */
    v8w(L, DISK_TRACK, (uint8_t)(track + 1));
}

/* boot_stream_byte (017C) called from ret-3. */
static uint8_t stream_byte(Loader *L, uint16_t ret)
{
    Cpu8086 *c = L->c;
    cpu_push(c, ret);
    cpu_push(c, LS(ES));
    uint16_t bx = (uint16_t)(v16(L, STREAM_POS) + 1);
    L->cf = (bx >> 8) < 0x10;
    if ((bx >> 8) == 0x10) {
        cpu_push(c, LR(CX));
        cpu_push(c, LR(DI));
        cpu_push(c, 0x019A);
        read_track(L);
        cpu_pop(c);
        bx = 0;
        L->cf = false;
        LR(DI) = cpu_pop(c);
        LR(CX) = cpu_pop(c);
    }
    LS(ES) = 0xB800;
    uint8_t al = L->pc->mem[0xB8000u + bx];
    LR(AX) = (uint16_t)(0xB800 | al);
    v16w(L, STREAM_POS, bx);
    LR(BX) = bx;
    LS(ES) = cpu_pop(c);
    cpu_pop(c);
    return al;
}

/* boot_stream_word (01A0) called from ret-3. */
static uint16_t stream_word(Loader *L, uint16_t ret)
{
    Cpu8086 *c = L->c;
    cpu_push(c, ret);
    uint8_t lo = stream_byte(L, 0x01A3);
    cpu_push(c, LR(AX));
    uint8_t hi = stream_byte(L, 0x01A7);
    LR(BX) = cpu_pop(c);
    LR(AX) = (uint16_t)(hi << 8 | lo);
    cpu_pop(c);
    return LR(AX);
}

/* Runs the native at 0050:target as a near call returning to ret (an enabled native, or the
 * fallback of no_native). */
static void call_native(Pc *pc, uint16_t ret, uint16_t target)
{
    Cpu8086 *c = &pc->cpu;
    uint16_t sp = c->regs[R_SP];
    cpu_push(c, ret);
    c->ip = target;
    while (c->regs[R_SP] != sp || c->ip != ret)
        step(pc);
}

static void load_stream(Pc *pc)
{
    Cpu8086 *c = &pc->cpu;
    Loader L0 = { pc, c, false }, *L = &L0;
    const uint8_t *boot = disk_sector(pc->disk, 0, 0);

    /* The BIOS loads the boot sector to 0000:7C00; it copies itself to 0000:0500. */
    for (uint32_t i = 0; i < 512; i++) {
        cpu_write8(c, 0x7C00 + i, boot[i]);
        cpu_write8(c, 0x0500 + i, boot[i]);
    }
    SDL_memset(c->regs, 0, sizeof c->regs);
    LS(DS) = LS(ES) = LS(SS) = 0;
    LR(SP) = 0xC0B0;
    LR(SI) = 0x7E00;
    LR(DI) = 0x0700;
    LR(AX) = 0x0C0C;
    mem_write16(pc, 0, 0x0120, 0x0C0C);
    LS(DS) = 0x0C0C;
    LS(CS) = GAME_CS;
    c->flags = 0xF202;
    v8w(L, DISK_TRACK, 1);
    v16w(L, STREAM_POS, 0x0FFF);
    LR(DI) = 0x0700;

    uint8_t op;
    for (;;) {
        op = stream_byte(L, 0x0036);
        if (op == 1) {
            uint16_t n = (uint16_t)(stream_word(L, 0x003D) - 3);
            LS(ES) = 0;
            for (LR(CX) = n; LR(CX); LR(CX)--) {
                uint8_t b = stream_byte(L, 0x0049);
                cpu_write8(c, cpu_linear(LS(ES), LR(DI)), b);
                LR(DI)++;
            }
        } else if (op == 2) {
            uint16_t n = (uint16_t)(stream_word(L, 0x0055) - 5);
            LR(DI) = 0;
            uint16_t seg = stream_word(L, 0x005F);
            LS(ES) = seg;
            for (LR(CX) = n; LR(CX); LR(CX)--) {
                uint8_t b = stream_byte(L, 0x0049);
                cpu_write8(c, cpu_linear(LS(ES), LR(DI)), b);
                LR(DI)++;
            }
        } else if (op == 3) {
            stream_word(L, 0x0068);
            uint16_t ss = stream_word(L, 0x006B);
            cpu_push(c, ss);
            LR(DI) = stream_word(L, 0x006F);
            LR(CX) = stream_word(L, 0x0074);
            uint16_t ds = stream_word(L, 0x0079);
            LR(SI) = v16(L, STREAM_POS);
            LR(DX) = (uint16_t)((LR(DX) & 0xFF00) | v8(L, DISK_TRACK));
            LR(BX) = 0;
            LS(DS) = 0;
            mem_write16(pc, 0, 0x0120, ds);
            mem_write16(pc, 0, 0x0122, LR(CX));
            LR(BX) = cpu_pop(c);
            LR(SP) = LR(DI);
            LS(SS) = LR(BX);
            LS(DS) = ds;
            v16w(L, STREAM_POS, LR(SI));
            v8w(L, DISK_TRACK, (uint8_t)LR(DX));
            v16w(L, VIEW_BUF_SEG, LR(CX));
            call_native(pc, 0x00A4, 0x5600); /* cga_program_regs */
        } else if (op == 4) {
            stream_word(L, 0x00AD);
            uint16_t w = stream_word(L, 0x00B0);
            LR(BX) = (uint8_t)w;
            v8w(L, (uint16_t)(AREA_TRACKS + LR(BX)), (uint8_t)(w >> 8));
        } else {
            break;
        }
    }

    /* jmp start: AL = op - 4 (the DECs of the dispatch), flags of the last DEC */
    uint8_t al = (uint8_t)(op - 4);
    LR(AX) = (uint16_t)((LR(AX) & 0xFF00) | al);
    uint16_t f = (uint16_t)(c->flags & ~(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF));
    if (L->cf)
        f |= F_CF;
    uint8_t p = al;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    if (!(p & 1))
        f |= F_PF;
    if ((al & 0x0F) == 0x0F)
        f |= F_AF;
    if (!al)
        f |= F_ZF;
    if (al & 0x80)
        f |= F_SF;
    if (al == 0x7F)
        f |= F_OF;
    c->flags = f;
    LS(CS) = GAME_CS;
    c->ip = 0x5C9F;
}

/* The original loader takes BOOT_CYCLES. The scheduler lets that time pass first, with only
 * the BIOS timer ticking and the power-on text screen shown, then loads (load) and starts the
 * game at the time the original reaches start; so the menus come up at the same frame. */
void sched_boot(Pc *pc)
{
    pc->boot_until = pc->cpu.cycles + BOOT_CYCLES;
    pc->boot_pending = true;
    pc->cpu.flags |= F_IF; /* the boot sector's STI: the BIOS timer ticks during the load */
}

#ifdef FS1_EMULATOR
/* --check-boot: the original loader (emulated, natives on) against the C boot, both up to
 * start. Prints the registers, flags and memory bytes that differ. */
bool sched_check_boot(Disk *disk)
{
    Pc *a = SDL_malloc(sizeof(Pc)), *b = SDL_malloc(sizeof(Pc));
    if (!a || !b || !pc_init(a, disk, false) || !pc_init(b, disk, true))
        return false;
    native_init(a);
    pc_boot(a);
    while (!(a->cpu.sregs[S_CS] == GAME_CS && a->cpu.ip == 0x5C9F))
        pc_run(a, a->cpu.cycles + 1);
    native_init(b);
    pc_boot(b);
    pc_run(b, b->boot_until);
    SDL_Log("check-boot: original loader %llu cycles, C boot %llu",
            (unsigned long long)a->cpu.cycles, (unsigned long long)b->cpu.cycles);
    static const char *rn[8] = { "AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI" };
    static const char *sn[4] = { "ES", "CS", "SS", "DS" };
    for (int r = 0; r < 8; r++)
        if (a->cpu.regs[r] != b->cpu.regs[r])
            SDL_Log("check-boot: %s original %04X C %04X", rn[r], a->cpu.regs[r], b->cpu.regs[r]);
    for (int r = 0; r < 4; r++)
        if (a->cpu.sregs[r] != b->cpu.sregs[r])
            SDL_Log("check-boot: %s original %04X C %04X", sn[r], a->cpu.sregs[r], b->cpu.sregs[r]);
    if (a->cpu.flags != b->cpu.flags)
        SDL_Log("check-boot: FLAGS original %04X C %04X", a->cpu.flags, b->cpu.flags);
    uint32_t diffs = 0;
    for (uint32_t i = 0; i < 0xF0000; i++) {
        if (a->mem[i] == b->mem[i])
            continue;
        uint32_t j = i;
        while (j < 0xF0000 && a->mem[j] != b->mem[j])
            j++;
        if (diffs++ < 40)
            SDL_Log("check-boot: memory %05X-%05X differs (%u bytes)", i, j - 1, j - i);
        i = j;
    }
    SDL_Log("check-boot: %u differing memory ranges", diffs);
    native_shutdown();
    pc_free(a);
    pc_free(b);
    SDL_free(a);
    SDL_free(b);
    return true;
}
#endif
