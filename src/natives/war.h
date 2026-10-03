#ifndef FS1_NATIVES_WAR_H
#define FS1_NATIVES_WAR_H

#include "native.h"

/* War-mode helpers from subphase 3.17 (src/natives/war.c, docs/subphases/3.17.md) for natives
 * whose originals reach these routines.
 *
 * Each war_call_* emulates "CALL routine" from the current CPU state: it charges the CALL,
 * pushes ret_ip, runs the routine in C (including the stack words it writes and its RET)
 * and adds the original's cycles to pc->cpu.cycles. Registers, segment registers and memory
 * end as the original leaves them; SP is back to its value before the call. IP is set to
 * ret_ip, as after the original's RET. */

/* 0050:1BB0 war_ground_reset: repairs the aircraft (fuel gauges, damage [1DC8], hits [1E5C])
 * and puts all six enemy records back to state 1. Called by ground_service and by the
 * editor's apply-state code (0050:3C1C). */
void war_call_ground_reset(Pc *pc, uint16_t ret_ip);

/* 0050:3575 war_score_add: AX points added to the score [1E57] (wraps at 10000) and the
 * digits written to the status text at 1EC5. Called from 0050:3C19 with AX = 0. */
void war_call_score_add(Pc *pc, uint16_t ret_ip);

/* 0050:3595 war_ammo_dec: [1E51] - 1 (not below 0) and its digits into "AMMO : nnn". */
void war_call_ammo_dec(Pc *pc, uint16_t ret_ip);

/* 0050:1B84..1BAE, the tail of flight_params: refuel/repair (and in war mode re-arm) when the
 * aircraft stands still on a ground-service box ([0406] = 1 or 2). Runs the code up to, but
 * not including, the RET at 1BAF; adds its cycles. For a C flight_params (3.14). */
void war_ground_service_body(Pc *pc);

#endif
