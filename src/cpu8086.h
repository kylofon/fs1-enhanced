#ifndef FS1_CPU8086_H
#define FS1_CPU8086_H

#include <stdbool.h>
#include <stdint.h>

/* Intel 8086/8088 real-mode interpreter.
 *
 * Opcode 0F (POP CS on a real 8086, never used by the game) is repurposed as a
 * high-level-emulation trap: "0F nn" calls bus.hle(ctx, nn). The PC layer puts
 * "0F nn CF" (trap + IRET) stubs in a fake BIOS ROM so BIOS services run in C.
 */

enum {
    R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI
};
enum {
    S_ES, S_CS, S_SS, S_DS
};
enum {
    F_CF = 0x0001, F_PF = 0x0004, F_AF = 0x0010, F_ZF = 0x0040, F_SF = 0x0080,
    F_TF = 0x0100, F_IF = 0x0200, F_DF = 0x0400, F_OF = 0x0800
};

#define CPU_MEM_SIZE 0x100000u
#define CPU_MEM_MASK 0xFFFFFu

typedef struct CpuBus {
    void *ctx;
    uint8_t (*in8)(void *ctx, uint16_t port);
    void (*out8)(void *ctx, uint16_t port, uint8_t value);
    void (*hle)(void *ctx, uint8_t service);
} CpuBus;

/* Optional undo log of RAM writes (filled by cpu_write8 while set). */
typedef struct CpuWriteLog {
    uint32_t *addr;
    uint8_t *old;
    uint32_t count, cap;
    bool overflow;
} CpuWriteLog;

typedef struct Cpu8086 Cpu8086;

typedef struct Cpu8086 {
    uint16_t regs[8];
    uint16_t sregs[4];
    uint16_t ip;
    uint16_t flags;

    uint8_t *mem;       /* CPU_MEM_SIZE bytes */
    uint32_t rom_start; /* linear writes at or above this are ignored */

    uint64_t cycles;
    bool halted;
    bool int_inhibit; /* set by STI / MOV SS / POP SS: no IRQ before next instruction */

    CpuBus bus;

    uint8_t *trace; /* optional CPU_MEM_SIZE flag map for code/data mapping (T_* flags) */

    /* Pre-execution hook: when hook_map is set, CS == hook_seg and hook_map[IP] != 0,
     * cpu_step calls pre_exec first. If it returns true it has handled the instruction
     * (and added its own cycles); cpu_step returns the cycles it used. */
    const uint8_t *hook_map; /* 64K entries, indexed by IP */
    uint16_t hook_seg;
    bool (*pre_exec)(void *ctx, Cpu8086 *cpu);
    void *hook_ctx;

    CpuWriteLog *write_log; /* optional */

    /* Optional CPU_MEM_SIZE counters: +1 at the linear CS:IP of every original instruction
     * executed (not for steps handled by pre_exec, nor while halted). For --stats. */
    uint32_t *exec_count;
} Cpu8086;

/* Trace flags, one byte per linear address. */
enum {
    T_EXEC = 0x01,  /* an instruction started here */
    T_CALL = 0x02,  /* target of a CALL */
    T_JUMP = 0x04,  /* target of a taken jump */
    T_INT = 0x08,   /* interrupt handler entry */
    T_READ = 0x10,  /* read as data */
    T_WRITE = 0x20, /* written as data */
    T_SMC = 0x40,   /* modified after being executed in the same session (self-modifying code) */
    T_RUN = 0x80    /* executed in the current session; cleared before the map is saved */
};

void cpu_reset(Cpu8086 *cpu);
/* Executes one instruction (a whole REP string op counts as one). Returns cycles used. */
int cpu_step(Cpu8086 *cpu);
/* Performs an interrupt through the IVT (used for hardware IRQs). */
void cpu_interrupt(Cpu8086 *cpu, uint8_t vector);

static inline uint32_t cpu_linear(uint16_t seg, uint16_t off)
{
    return (((uint32_t)seg << 4) + off) & CPU_MEM_MASK;
}

uint8_t cpu_read8(Cpu8086 *cpu, uint32_t addr);
uint16_t cpu_read16(Cpu8086 *cpu, uint32_t addr);
void cpu_write8(Cpu8086 *cpu, uint32_t addr, uint8_t v);
void cpu_write16(Cpu8086 *cpu, uint32_t addr, uint16_t v);
void cpu_push(Cpu8086 *cpu, uint16_t v);
uint16_t cpu_pop(Cpu8086 *cpu);

#endif
