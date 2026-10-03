#ifndef FS1_TEXTDRAW_H
#define FS1_TEXTDRAW_H

/* Text renderer of the original program (3.9), reusable by other natives and later phases.
 *
 * Font: 96 glyphs for ASCII 20h..7Fh, 10 bytes each (5 words, one per pixel row), at
 * DS:font_base + (ch - 20h) * 10. Each word is one 16-pixel row of the 640x200 CGA mode in
 * screen byte order: the low byte is the left 8 pixels, MSB first. The font base is the
 * 16-bit displacement of `mov ax,[bx+disp]` in print_str_mask (0050:1247, then +2, +4,
 * +6, +8 at 1252/125D/1268/1273). Only the boot loader writes it, as 0000: the font is the
 * first 960 bytes of the data segment, wherever DS is. That is 0686:0000 (linear 06860)
 * during the startup menus and 0618:0000 (linear 06180) after store_ds_for_isr moves the
 * data segment down at 0050:01F2, over the startup code.
 *
 * A string record is {word CGA offset, ASCII bytes..., terminator}; the terminator is the
 * first byte that is not 20h..7Fh (the original tests it with SUB AL,20h / JGE). A glyph
 * cell is 2 bytes wide and 5 scan lines high; consecutive lines alternate between the
 * two CGA banks (B800:0000 even lines, B800:2000 odd lines). */

#include "native.h"

#define TEXT_FONT_DISP_OFF 0x1247 /* 0050:1247, disp16 of print_str_mask's first font load */
#define TEXT_GLYPH_BYTES 10
#define TEXT_CELL_ROWS 5
#define TEXT_STEP_TO_ODD 0x1FFE  /* after a stosw on an even line: next line, odd bank */
#define TEXT_STEP_TO_EVEN 0xE04E /* after a stosw on an odd line: next line, even bank */
#define TEXT_STEP_NEXT 0xFF60    /* after the fifth row: next cell on the first row */

/* Register state of a print_str-family loop. */
typedef struct TextDraw {
    uint16_t ds, si;  /* string pointer */
    uint16_t es, di;  /* screen pointer */
    uint16_t mask;    /* AND mask applied to each glyph word (CX or BP in the original) */
    uint16_t disp[5]; /* displacement of each row's font load (from the instruction bytes) */
    bool odd_first;   /* first row on an odd line: steps E04E,1FFE,... instead of 1FFE,E04E,... */
    int shift;        /* -1: plain; else each glyph word is byte-swapped, SHR by shift, swapped back */
    uint16_t ax, bx;  /* AX and BX as the original leaves them */
    int glyphs;       /* number of glyphs drawn */
} TextDraw;

/* Reads the 16-bit displacements of the five font loads from code at 0050:insn_disp[i]. */
static inline void textdraw_read_disp(Pc *pc, const uint16_t insn_disp[5], uint16_t disp[5])
{
    for (int i = 0; i < 5; i++)
        disp[i] = mem_read16(pc, GAME_CS, insn_disp[i]);
}

/* Offset of the font in the data segment, as print_str uses it. */
static inline uint16_t textdraw_font_base(Pc *pc) { return mem_read16(pc, GAME_CS, TEXT_FONT_DISP_OFF); }

/* The five row words of glyph ch (20h..7Fh) from the game's font in DS 0618 (valid once
 * the data segment has been moved there, i.e. after the startup menus). */
static inline void textdraw_glyph(Pc *pc, uint8_t ch, uint16_t rows[5])
{
    uint16_t at = (uint16_t)(textdraw_font_base(pc) + (ch - 0x20) * TEXT_GLYPH_BYTES);
    for (int i = 0; i < 5; i++)
        rows[i] = mem_read16(pc, GAME_DS, (uint16_t)(at + 2 * i));
}

static inline bool textdraw_is_glyph(uint8_t ch) { return ch >= 0x20 && ch < 0x80; }

/* Draws the string at t->ds:t->si to t->es:t->di exactly as the original loop: SI ends
 * past the terminator, DI after the last cell, AX/BX as the last LODSB/glyph left them.
 * The record's leading screen offset is not read here (callers do LODSW themselves).
 * dir is -1 if DF is set (LODSB/STOSW then step backwards), else 1. */
static inline void textdraw_string(Pc *pc, TextDraw *t, int dir)
{
    for (;;) {
        uint8_t al = cpu_read8(&pc->cpu, cpu_linear(t->ds, t->si));
        t->si = (uint16_t)(t->si + dir);
        t->ax = (uint16_t)((t->ax & 0xFF00) | (uint8_t)(al - 0x20));
        if (!textdraw_is_glyph(al))
            return;
        t->bx = (uint16_t)((al - 0x20) * TEXT_GLYPH_BYTES);
        for (int r = 0; r < TEXT_CELL_ROWS; r++) {
            uint16_t w = (uint16_t)(mem_read16(pc, t->ds, (uint16_t)(t->bx + t->disp[r])) & t->mask);
            if (t->shift >= 0) {
                w = (uint16_t)(w >> 8 | w << 8);
                w = t->shift >= 16 ? 0 : (uint16_t)(w >> t->shift);
                w = (uint16_t)(w >> 8 | w << 8);
            }
            mem_write16(pc, t->es, t->di, w);
            t->di = (uint16_t)(t->di + 2 * dir);
            t->ax = w;
            bool odd = (r & 1) != t->odd_first;
            t->di = (uint16_t)(t->di + (r == 4 ? TEXT_STEP_NEXT : odd ? TEXT_STEP_TO_EVEN : TEXT_STEP_TO_ODD));
        }
        t->glyphs++;
    }
}

#endif
