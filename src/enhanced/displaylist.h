/* Display list of the sharp renderer (docs/RENDER.md). The natives record what they draw through the
 * dl_* hooks below; the SDL back end draws the committed lists at window size. Recording is a side
 * channel: it reads emulated memory directly (pc->cpu.mem, no trace bits), never writes it and never
 * charges cycles. With dl_on false (--classic, --verify, sharp renderer off) every hook is a no-op.
 *
 * S1.1: format and API only. S1.2 implements the recorder and places the hooks (ENH: display list). */
#ifndef DISPLAYLIST_H
#define DISPLAYLIST_H

#include <stdbool.h>
#include <stdint.h>

#include "native.h"

/* ---- format ------------------------------------------------------------------------------- */

typedef enum DlKind {
    DL_FILL,      /* rect, word pattern */
    DL_SKYGROUND, /* view split by the line a->b: pat_l on its left, pat_r on its right */
    DL_LINE,      /* a->b, nibble pair [30C8] */
    DL_DOT,       /* a, nibble pair [30C8] */
    DL_POLY,      /* rings in the vertex pool, colour byte [3159]; drawn under its DL_F_EDGE lines */
    DL_TEXT,      /* S2.2 */
    DL_SPRITE,    /* S2.1 */
    DL_RESTORE,   /* S2.1 */
    DL_NEEDLE,    /* S2.1 */
} DlKind;

enum {
    DL_F_INT = 1,      /* integer coordinates (pixel centres n + 0.5), not from the projection */
    DL_F_EDGE = 2,     /* outline line of the polygon being built */
    DL_F_CAPTURED = 4, /* drawn from a captured list (draw_line_list) */
    DL_F_EMPTY = 8,    /* reserved DL_POLY whose flood fill filled nothing: skip */
};

typedef struct DlPt {
    float x, y; /* view space: fat pixels, pixel n covers [n, n+1); screen space: 640x200 */
} DlPt;

typedef struct DlItem {
    uint8_t kind;    /* DlKind */
    uint8_t flags;   /* DL_F_* */
    uint16_t colour; /* nibble pair, word pattern or colour byte (see DlKind) */
    union {
        struct { DlPt a, b; } line;                              /* DL_LINE, DL_DOT (a) */
        struct { float x, y, w, h; } rect;                       /* DL_FILL, DL_RESTORE */
        struct { DlPt a, b; uint16_t pat_l, pat_r; } sky;        /* DL_SKYGROUND */
        struct { uint16_t first, count, rings; } poly;           /* DL_POLY: vertices in DlFrame.verts */
        struct { uint16_t off, addr; uint8_t w, h, op; } sprite; /* DL_SPRITE */
        struct { uint16_t desc; uint8_t angle; } needle;         /* DL_NEEDLE */
        struct { uint16_t off, mask; int8_t shift; uint8_t len; char s[40]; } text; /* DL_TEXT */
    } u;
} DlItem;

#define DL_MAX_ITEMS 8192
#define DL_MAX_VERTS 16384

typedef struct DlFrame {
    DlItem items[DL_MAX_ITEMS];
    DlPt verts[DL_MAX_VERTS];
    int n_items, n_verts;
    int view_row;    /* screen scan line of view row 0 (row_offsets[0]) */
    uint32_t number; /* commit counter */
    bool incomplete; /* overflow: show this frame from the CGA picture */
} DlFrame;

/* ---- state -------------------------------------------------------------------------------- */

extern bool dl_on; /* recording enabled (sharp renderer selected, not --classic, not verifying) */

/* The last committed view frame (NULL before the first commit). */
const DlFrame *dl_view(void);

/* ---- hooks (called by the natives; each is one ENH: line in the engine) --------------------- */

/* Projection: stage float coordinates for the next draw (scenery.c f_4D18/f_4D56/f_4CA3). */
void dl_stage_line_(Pc *pc, bool from_prev);
void dl_stage_dot_(Pc *pc, uint16_t x, uint16_t y, uint16_t z);
/* Capture mode: keep the staged floats for the record written at addr (capture shadow). */
void dl_capture_(Pc *pc, uint16_t addr);
void dl_stage_from_capture_(Pc *pc, uint16_t addr);

/* Drawing into seg (back buffer [3806] -> view list, B800 -> screen layer). */
void dl_line_(Pc *pc, int x0, int y0, int x1, int y1, uint16_t seg);
void dl_dot_(Pc *pc, int x, int y, uint16_t seg);
void dl_fill_view_(Pc *pc, uint16_t pattern);
void dl_fill_rows_(Pc *pc, int first, int last, uint16_t pattern, uint16_t seg);
void dl_fill_rect_(Pc *pc, int x, int y, int w, int h, uint16_t pattern, uint16_t seg);
void dl_sky_ground_(Pc *pc);
void dl_poly_begin_(Pc *pc);
void dl_poly_end_(Pc *pc, bool filled);
void dl_commit_view_(Pc *pc);

#define DL_HOOK(call) do { if (dl_on) call; } while (0)
static inline void dl_stage_line(Pc *pc, bool from_prev) { DL_HOOK(dl_stage_line_(pc, from_prev)); }
static inline void dl_stage_dot(Pc *pc, uint16_t x, uint16_t y, uint16_t z) { DL_HOOK(dl_stage_dot_(pc, x, y, z)); }
static inline void dl_capture(Pc *pc, uint16_t addr) { DL_HOOK(dl_capture_(pc, addr)); }
static inline void dl_stage_from_capture(Pc *pc, uint16_t addr) { DL_HOOK(dl_stage_from_capture_(pc, addr)); }
static inline void dl_line(Pc *pc, int x0, int y0, int x1, int y1, uint16_t seg) { DL_HOOK(dl_line_(pc, x0, y0, x1, y1, seg)); }
static inline void dl_dot(Pc *pc, int x, int y, uint16_t seg) { DL_HOOK(dl_dot_(pc, x, y, seg)); }
static inline void dl_fill_view(Pc *pc, uint16_t pattern) { DL_HOOK(dl_fill_view_(pc, pattern)); }
static inline void dl_fill_rows(Pc *pc, int first, int last, uint16_t pattern, uint16_t seg) { DL_HOOK(dl_fill_rows_(pc, first, last, pattern, seg)); }
static inline void dl_fill_rect(Pc *pc, int x, int y, int w, int h, uint16_t pattern, uint16_t seg) { DL_HOOK(dl_fill_rect_(pc, x, y, w, h, pattern, seg)); }
static inline void dl_sky_ground(Pc *pc) { DL_HOOK(dl_sky_ground_(pc)); }
static inline void dl_poly_begin(Pc *pc) { DL_HOOK(dl_poly_begin_(pc)); }
static inline void dl_poly_end(Pc *pc, bool filled) { DL_HOOK(dl_poly_end_(pc, filled)); }
static inline void dl_commit_view(Pc *pc) { DL_HOOK(dl_commit_view_(pc)); }

#endif
