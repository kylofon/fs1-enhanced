#ifndef FS1_NATIVE_H
#define FS1_NATIVE_H

#include <stdbool.h>
#include <stdint.h>

#include "pc.h"

/* Native replacements of original routines (phase 3).
 *
 * Each entry binds a C function to a code address (normally 0050:xxxx). When the CPU is
 * about to execute an instruction at an enabled entry's address, the C function runs
 * instead. It sees the full CPU state and must leave it exactly as the original routine
 * would on return, including popping the return address (native_ret / native_retf).
 * A native call costs the entry's `cycles` (NATIVE_CALL_CYCLES if 0) emulated cycles.
 * Charging what the original takes keeps the emulated timeline, and so the screen,
 * identical; --verify reports the original's measured cycle range for that purpose.
 */

#define GAME_CS 0x0050
#define GAME_DS 0x0618
#define NATIVE_CALL_CYCLES 100

typedef void (*NativeFn)(Pc *pc);
/* A native that may decline a call (3.21): returns false without changing any state, and the
 * CPU then executes the original instruction at the entry's address instead. */
typedef bool (*NativeTryFn)(Pc *pc);

/* Linear address range [lo, hi) excluded from the --verify memory comparison. */
typedef struct NativeRange {
    uint32_t lo, hi;
} NativeRange;

typedef struct NativeEntry {
    const char *name;
    uint16_t seg, off;
    NativeFn fn;
    bool enabled;
    bool far;                  /* returns with RETF instead of RET */
    uint16_t flag_mask;        /* flags the native guarantees to match (F_*); default none */
    const NativeRange *ignore; /* optional write-only scratch areas, ignore_count entries */
    int ignore_count;
    uint32_t cycles; /* emulated cycles charged per native call; 0 = NATIVE_CALL_CYCLES */
    NativeTryFn try_fn; /* used instead of fn when set (fn is then NULL) */
    /* Natives that do not return (3.21: main loop blocks, wait loops). When stop_hi != 0,
     * --verify runs the original until, after at least one instruction, CS:IP leaves
     * GAME_CS:[stop_lo, stop_hi) or reaches one of `stops` (0-terminated list, e.g. the loop
     * head), instead of until it returns. The native must end at the same CS:IP. */
    uint16_t stop_lo, stop_hi;
    const uint16_t *stops;
    bool exact_cycles; /* --verify also compares the cycles charged with the original's */

    /* run-time state and statistics */
    bool verify;
    uint64_t calls, mismatches, cap_hits, declines;
    uint64_t orig_cycles_min, orig_cycles_max; /* measured by --verify */
} NativeEntry;

/* Installs the dispatch hook on pc->cpu. Call after pc_init. */
void native_init(Pc *pc);
/* NAME or "all". Return false if no entry has that name. */
bool native_set_enabled(const char *name, bool enabled);
bool native_set_verify(const char *name);
void native_list(void);
/* Prints calls and mismatches of every entry being verified. */
void native_verify_summary(void);
void native_shutdown(void);
/* Sum of the calls of every entry (for --stats). */
uint64_t native_total_calls(void);

/* For the C scheduler (3.22): runs the enabled native at CS:IP as one step (as the CPU's
 * pre-execution hook would). False when there is none, or it declined. */
bool native_step(Pc *pc);

/* What a native's "run the original" helper performs per step (3.22): with the C scheduler,
 * the enabled native at CS:IP if there is one, else (and always in the emulator) one
 * original instruction (cpu_step; fatal in the default build, which has no interpreter).
 * Returns the cycles used. */
int native_or_cpu_step(Pc *pc);

/* Calls another native's C implementation from inside a native, as a near CALL would:
 * the caller has already pushed the return address, the callee's RET pops it, and the
 * callee's cycles are charged. Returns false (and does nothing) when no native is enabled
 * at GAME_CS:off or while --verify is stepping original code; the caller must then run
 * the original routine itself. Under --verify the callee's writes are part of the outer
 * native's write log, so nested calls are checked as one unit with their caller. */
bool native_call(Pc *pc, uint16_t off);

/* True inside a --verify run of the native, or nested in a native that keeps a write log
 * (the natives' interrupt hand-back): the call must then run in full, not decline or batch. */
static inline bool native_logged(const Pc *pc) { return pc->cpu.write_log != NULL; }

/* ---- helpers for native routines -------------------------------------------- */

static inline uint16_t mem_read16(Pc *pc, uint16_t seg, uint16_t off)
{
    return cpu_read16(&pc->cpu, cpu_linear(seg, off));
}
static inline void mem_write16(Pc *pc, uint16_t seg, uint16_t off, uint16_t v)
{
    cpu_write16(&pc->cpu, cpu_linear(seg, off), v);
}
static inline uint8_t ds_read8(Pc *pc, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(GAME_DS, off)); }
static inline uint16_t ds_read16(Pc *pc, uint16_t off) { return mem_read16(pc, GAME_DS, off); }
static inline void ds_write8(Pc *pc, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(GAME_DS, off), v); }
static inline void ds_write16(Pc *pc, uint16_t off, uint16_t v) { mem_write16(pc, GAME_DS, off, v); }

/* Near RET: pops IP. */
static inline void native_ret(Pc *pc) { pc->cpu.ip = cpu_pop(&pc->cpu); }
/* Far RET: pops IP and CS. */
static inline void native_retf(Pc *pc)
{
    pc->cpu.ip = cpu_pop(&pc->cpu);
    pc->cpu.sregs[S_CS] = cpu_pop(&pc->cpu);
}

#endif
