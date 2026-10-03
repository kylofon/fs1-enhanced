#ifndef FS1_NATIVES_PANEL_H
#define FS1_NATIVES_PANEL_H

#include "native.h"

/* Panel blitting helpers from subphase 3.10 (src/natives/indicator.c), for natives whose
 * originals call these routines.
 *
 * Each panel_call_* emulates "CALL routine" made from the current CPU state at the point of
 * the call: it writes ret_ip to SS:SP-2 (as the CALL would), runs the routine in C with the
 * stack writes the original makes below SP, and leaves every register, segment register and
 * memory byte as the original routine does on return. SP is unchanged, IP is not touched.
 * The caller's own .cycles should include what the callee costs; panel_cycles() returns the
 * emulated cycles the last panel_call_* (or native) took in the original. */

/* 0050:4DF6 blit_or: ORs a sprite into ES (normally B800). DS:SI = sprite words,
 * DI = screen offset of the bottom line, DL = words per line, DH = lines. The sprite is
 * drawn bottom-up (CGA interleave: +1FB0 from an even line, +E000 from an odd one). Sets IF. */
void panel_call_blit_or(Pc *pc, uint16_t ret_ip);

/* 0050:4DE0 blit_sprite: ES = B800, then blit_or with the record at DS:BX
 * (+0 screen offset, +2 size word DL/DH, +4 sprite words). */
void panel_call_blit_sprite(Pc *pc, uint16_t ret_ip);

/* 0050:218E indicator_erase: BX = indicator descriptor. Copies the saved rectangle
 * (+8 offset, +A size) from the clean panel copy (segment at 0000:0122) to ES. */
void panel_call_indicator_erase(Pc *pc, uint16_t ret_ip);

/* 0050:2111 draw_indicator: BP = indicator descriptor (see docs/subphases/3.10.md). Leaves
 * DS = word at 0000:0120 (the game data segment). */
void panel_call_draw_indicator(Pc *pc, uint16_t ret_ip);

/* 0050:1D20 draw_needle: AL = angle, AH != 0 also redraws the second needle [090D]
 * through draw_needle2, CX = needle descriptor. Leaves DS = [0000:0120], ES = B800. */
void panel_call_draw_needle(Pc *pc, uint16_t ret_ip);

/* 0050:1DE8 draw_needle2: AL = angle, AH bit 0 = redraw even if unchanged, CX = needle
 * descriptor. Uses the second sprite set (DS:098F); redraws the indicator 13AC when
 * [0914] != 0. Leaves DS = [0000:0120], ES = B800. */
void panel_call_draw_needle2(Pc *pc, uint16_t ret_ip);

/* Emulated cycles of the original routine for the last panel_call_* or panel native. */
uint32_t panel_cycles(void);

#endif
