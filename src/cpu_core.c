/* The 8086 register file and memory accessors (3.22). These are plain data helpers that the
 * natives use as their calling convention and data model; they are in every build. The
 * interpreter (cpu_step, cpu8086.c) is only in the FS1_EMULATOR build. */
#include "cpu8086.h"

#include <string.h>

/* ---- memory ---------------------------------------------------------- */

static void trace_target(Cpu8086 *c, uint8_t flag)
{
    if (c->trace)
        c->trace[cpu_linear(c->sregs[S_CS], c->ip)] |= flag;
}

uint8_t cpu_read8(Cpu8086 *c, uint32_t a)
{
    if (c->trace)
        c->trace[a & CPU_MEM_MASK] |= T_READ;
    return c->mem[a & CPU_MEM_MASK];
}

uint16_t cpu_read16(Cpu8086 *c, uint32_t a)
{
    return (uint16_t)(c->mem[a & CPU_MEM_MASK] | c->mem[(a + 1) & CPU_MEM_MASK] << 8);
}

void cpu_write8(Cpu8086 *c, uint32_t a, uint8_t v)
{
    a &= CPU_MEM_MASK;
    if (c->trace) {
        c->trace[a] |= T_WRITE;
        if ((c->trace[a] & T_RUN) && c->mem[a] != v)
            c->trace[a] |= T_SMC;
    }
    if (a < c->rom_start) {
        CpuWriteLog *log = c->write_log;
        if (log) {
            if (log->count < log->cap) {
                log->addr[log->count] = a;
                log->old[log->count++] = c->mem[a];
            } else {
                log->overflow = true;
            }
        }
        c->mem[a] = v;
    }
}

void cpu_write16(Cpu8086 *c, uint32_t a, uint16_t v)
{
    cpu_write8(c, a, (uint8_t)v);
    cpu_write8(c, a + 1, (uint8_t)(v >> 8));
}

void cpu_push(Cpu8086 *c, uint16_t v)
{
    c->regs[R_SP] -= 2;
    cpu_write16(c, cpu_linear(c->sregs[S_SS], c->regs[R_SP]), v);
}

uint16_t cpu_pop(Cpu8086 *c)
{
    uint16_t v = cpu_read16(c, cpu_linear(c->sregs[S_SS], c->regs[R_SP]));
    c->regs[R_SP] += 2;
    return v;
}

/* ---- interrupts ---------------------------------------------------------- */

void cpu_interrupt(Cpu8086 *c, uint8_t vector)
{
    cpu_push(c, c->flags);
    c->flags &= (uint16_t)~(F_IF | F_TF);
    cpu_push(c, c->sregs[S_CS]);
    cpu_push(c, c->ip);
    c->ip = cpu_read16(c, (uint32_t)vector * 4);
    c->sregs[S_CS] = cpu_read16(c, (uint32_t)vector * 4 + 2);
    trace_target(c, T_INT);
    c->halted = false;
    c->cycles += 51;
}

void cpu_reset(Cpu8086 *c)
{
    memset(c->regs, 0, sizeof c->regs);
    memset(c->sregs, 0, sizeof c->sregs);
    c->sregs[S_CS] = 0xFFFF;
    c->ip = 0;
    c->flags = 0xF002;
    c->halted = false;
    c->int_inhibit = false;
}

