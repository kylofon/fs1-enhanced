#ifndef FS1_NATIVES_KEYS_H
#define FS1_NATIVES_KEYS_H

#include "native.h"

/* Helpers for natives bound to interrupt entries (subphase 3.18, src/natives/keys.c).
 *
 * An interrupt handler can be a native: the CPU delivers the interrupt (cpu_interrupt pushes
 * FLAGS, CS, IP, clears IF and TF and jumps to the vector), and the dispatch hook then fires
 * at the handler's first instruction like at any other bound address. The native must end
 * with IRET semantics (native_iret) and its entry needs .far = true, so that --verify finds
 * the return address (IP, then CS) on the stack as it does for RETF; the flags word above
 * them is restored by the IRET. */

/* IRET: pops IP, CS and FLAGS (with the fixed bits cpu8086.c's IRET applies). */
static inline void native_iret(Pc *pc)
{
    pc->cpu.ip = cpu_pop(&pc->cpu);
    pc->cpu.sregs[S_CS] = cpu_pop(&pc->cpu);
    pc->cpu.flags = (uint16_t)((cpu_pop(&pc->cpu) & 0x0FD5) | 0xF002);
}

#endif
