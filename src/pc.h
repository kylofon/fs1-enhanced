#ifndef FS1_PC_H
#define FS1_PC_H

#include <stdbool.h>
#include <stdint.h>

#include "cpu8086.h"
#include "disk.h"

/* Minimal IBM PC/XT for a self-booting game: 8088 @ 4.77 MHz, 640K RAM, CGA,
 * i8259 PIC, i8253 PIT, 8255 port 61h/keyboard, and a high-level BIOS. */

#define PC_CPU_HZ 4772727.0
#define PC_PIT_HZ 1193182.0

#define CGA_W 640 /* rendered width; 320-pixel modes are doubled horizontally */
#define CGA_H 200

typedef struct PitChannel {
    uint16_t reload;
    int32_t count;
    uint8_t mode;
    uint8_t access;
    bool write_hi_next;
    bool read_hi_next;
    bool latched;
    uint16_t latch;
    bool loaded;
} PitChannel;

typedef struct SpeakerEvent {
    uint64_t cycle;
    uint8_t port61;
    uint16_t pit2_reload;
} SpeakerEvent;

#define PC_MAX_SPEAKER_EVENTS 8192
#define PC_KBD_QUEUE 64

typedef struct Pc {
    Cpu8086 cpu;
    uint8_t *mem;
    Disk *disk;

    /* i8259 */
    uint8_t pic_irr, pic_imr, pic_isr, pic_vector;
    int pic_icw_step;
    bool pic_need_icw4;
    bool pic_read_isr;

    /* i8253 */
    PitChannel pit[3];
    uint32_t irq0_backlog; /* timer ticks still to deliver after a long step */
    uint32_t pit_cycle_frac;
    uint64_t pit_synced; /* cpu.cycles up to which the PIT has been advanced (pc_sync_pit) */
    uint64_t run_target; /* pc_run's target: the end of the current front-end frame */

    /* 8255 / keyboard */
    uint8_t port61;
    uint8_t kbd_queue[PC_KBD_QUEUE];
    int kbd_head, kbd_tail;
    uint8_t kbd_latch;
    bool kbd_full;
    uint64_t kbd_next_cycle;

    /* CGA */
    uint8_t cga_mode;   /* 3D8 */
    uint8_t cga_color;  /* 3D9 */
    uint8_t crtc_index; /* 3D4 */
    uint8_t crtc[32];
    bool composite; /* display: composite monitor (artifact colour) vs RGB */
    uint8_t font[256][8]; /* 8x8 text-mode glyphs, MSB = leftmost pixel (see main.c) */
    bool blink_phase;     /* toggled by the front end for blinking text */

    /* speaker log for the current audio slice */
    SpeakerEvent speaker[PC_MAX_SPEAKER_EVENTS];
    int speaker_count;
    uint8_t speaker_port61_at_start;
    uint16_t speaker_reload_at_start;
    uint64_t speaker_slice_start;
    double speaker_sample_frac, speaker_phase, speaker_x1, speaker_y1;

    uint32_t unknown_hle_mask[8];

    /* C scheduler (3.22, sched.c). When csched is set (always in the default build), pc_run
     * calls the natives directly instead of the interpreter: IRQ0 every irq0_period cycles
     * (4 x the channel 0 divisor the game wrote), IRQ1 when a key is queued, both only between
     * two natives. The PIC/PIT fields above then hold just the mask, the in-service and pending
     * bits and the divisors the game wrote; nothing counts down. */
    bool csched;
    uint64_t irq0_next;    /* cpu.cycles at which the next IRQ0 is due */
    uint64_t pit_loaded[3]; /* cpu.cycles at which each channel's divisor was last loaded */
    uint64_t irq0_due;     /* when the pending IRQ0 fell due */
    bool irq0_entered;     /* IRQ0 was just delivered: the next step is its handler */
    int64_t speaker_bias;  /* added to cpu.cycles for speaker events (sched.c) */
    uint64_t boot_until;   /* the C boot's stand-in for the loader's time (sched_boot) */
    bool boot_pending;     /* the load stream is decoded when boot_until is reached */
    uint64_t sched_steps;  /* natives dispatched by the scheduler */
    uint64_t sched_irqs;   /* interrupts it delivered */
    uint64_t sched_fallback; /* original instructions it had to run (FS1_EMULATOR only, else fatal) */
} Pc;

/* csched: run with the C scheduler (3.22). Ignored (always on) without FS1_EMULATOR. */
bool pc_init(Pc *pc, Disk *disk, bool csched);
void pc_free(Pc *pc);
/* Loads the boot sector to 0000:7C00 and jumps to it, like the BIOS would. With the C
 * scheduler: decodes the load stream in C instead and leaves the CPU at start (0050:5C9F). */
void pc_boot(Pc *pc);
/* Runs until cpu.cycles reaches target. */
void pc_run(Pc *pc, uint64_t target_cycles);

/* Advances the PIT to cpu.cycles in the middle of a step (3.21). A native calls this before
 * it reprograms a PIT channel, so the new count starts at the right cycle, as it would after
 * the original's OUT instruction; pc_run then advances only the rest of the step. */
void pc_sync_pit(Pc *pc);

/* For natives that may batch or decline (3.21): the step running now can go on for any
 * n < pc_irq_horizon(pc) more cycles without the timer or the keyboard raising an interrupt,
 * and without crossing the end of the front-end frame (pc_run's target), at an instruction
 * boundary before the step's end. 0 when an interrupt is already pending (IRR or IRQ0
 * backlog). Interrupt-enable and PIC masks are not considered (conservative). */
uint64_t pc_irq_horizon(const Pc *pc);

/* XT set-1 scancode (make code; set bit 7 for break). */
void pc_key_event(Pc *pc, uint8_t scancode);

/* Renders the current CGA screen into CGA_W x CGA_H XRGB8888 pixels. */
void pc_render_cga(const Pc *pc, uint32_t *pixels);

/* Generates mono float samples for cycles [slice_start, cpu.cycles) and starts a new slice.
 * Returns the number of samples written (at most max_samples). */
int pc_speaker_render(Pc *pc, float *out, int max_samples, int sample_rate);

/* ---- C scheduler (sched.c, 3.22) ---- */

/* Port handlers of the scheduler (only the state the game reads back, no countdown). */
void sched_attach(Pc *pc);
/* The boot in C (tools/pc_loadstream.py's decoding of the load stream). */
void sched_boot(Pc *pc);
/* Runs natives until cpu.cycles reaches target, with the interrupts between them. */
void sched_run(Pc *pc, uint64_t target);
uint64_t sched_horizon(const Pc *pc);
#ifdef FS1_EMULATOR
/* --check-boot: compares the C boot with the original loader up to start. */
bool sched_check_boot(Disk *disk);
#endif
/* Power-on memory and device state (pc.c): RAM cleared, IVT, BIOS data area, text mode. */
void pc_power_on(Pc *pc);
/* Appends a speaker event (port 61h or the channel 2 divisor changed) at cpu.cycles. */
void pc_speaker_log(Pc *pc);
/* CGA status register 3DAh (retrace bits) at cpu.cycles. */
uint8_t pc_cga_status(const Pc *pc);

#endif
