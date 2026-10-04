/* Display list of the sharp renderer (docs/RENDER.md). The natives record what they draw through the
 * dl_* hooks below; the SDL back end draws the committed lists at window size. Recording is a side
 * channel: it reads emulated memory directly (pc->cpu.mem, no trace bits), never writes it and never
 * charges cycles. With dl_on false (--classic, --verify, sharp renderer off) every hook is a no-op.
 *
 * S1.2: the 3D view recorder and its hooks (each engine call is one ENH: display list line).
 * --dump-dl FILE writes the committed frames as text, --dl-check lists back-buffer writers without a hook. */
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
    DL_F_LEAK = 16,    /* DL_POLY whose flood fill left its outline (or the outline was open): the box
                          x0,y0-x1,y1 of the changed pixels must be shown from the CGA picture */
};

typedef struct DlPt {
    float x, y; /* view space: fat pixels, pixel n covers [n, n+1); screen space: 640x200 */
} DlPt;

/* Separates the rings of a DL_POLY in the vertex pool (x is NaN). */
#define DL_RING_BREAK_X NAN /* <math.h> */

typedef struct DlItem {
    uint8_t kind;    /* DlKind */
    uint8_t flags;   /* DL_F_* */
    uint16_t colour; /* nibble pair, word pattern or colour byte (see DlKind) */
    union {
        struct { DlPt a, b; } line;                              /* DL_LINE, DL_DOT (a) */
        struct { float x, y, w, h; } rect;                       /* DL_FILL, DL_RESTORE */
        struct { DlPt a, b; uint16_t pat_l, pat_r; } sky;        /* DL_SKYGROUND */
        struct { uint16_t first, count, rings; uint8_t x0, y0, x1, y1; } poly; /* DL_POLY: vertices in DlFrame.verts */
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

extern bool dl_on; /* recording enabled (--dump-dl / --dl-check; S1.3 adds the sharp renderer); never under --classic */

/* The last committed view frame (NULL before the first commit). */
const DlFrame *dl_view(void);

/* The back-buffer bytes of the committed frame that the blit copied to B800 (same offsets), for the
 * back end's validity check: a B800 tile that differs from them was overwritten after the blit. */
typedef struct DlSnap {
    uint8_t bytes[0x4000];
    uint16_t off[2], len[2]; /* the two blit ranges, in bytes */
} DlSnap;
const DlSnap *dl_view_snap(void);

/* Options (--dump-dl FILE, --dl-check); called from enh_parse_arg. */
void dl_set_dump(const char *path);
void dl_set_check(void);

/* S2.2: the glyph rows the text drawer wrote into the screen (video RAM byte offset -> the word written).
 * Retained until the back end finds that video RAM no longer holds them (it clears ok[off] then). */
bool dl_text_rows(uint16_t **w, uint8_t **ok);

/* ---- hooks (called by the natives; each is one ENH: line in the engine) --------------------- */

/* Projection (scenery.c f_4D18/f_4CA3/f_4D56): stage the float coordinates of the line about to be
 * drawn or captured. sw = the screen word the original computed (x low byte, y high byte). */
void dl_stage_p1_(Pc *pc, bool from_prev, uint16_t sw);
void dl_stage_p2_(Pc *pc, uint16_t sw1, uint16_t sw2);
/* project_dot (transform.c): float dot for the integer pixel (sx, sy) about to be plotted. */
void dl_stage_dot_(Pc *pc, uint16_t x, uint16_t y, uint16_t z, uint8_t sx, uint8_t sy);
/* Capture mode: keep the staged line for the record written at addr (capture shadow). */
void dl_capture_(Pc *pc, uint16_t addr);
/* draw_line_list: stage the captured floats of the record at addr, if there are any. */
void dl_stage_from_capture_(Pc *pc, uint16_t addr);

/* Drawing into seg (the back buffer [3806] -> view list; anything else is ignored until S2). Hooks run
 * BEFORE the native writes, so --dl-check can tell the writes of the previous hooked writer apart. */
void dl_line_(Pc *pc, int x0, int y0, int x1, int y1, uint16_t seg);
void dl_dot_(Pc *pc, int x, int y, uint16_t seg);
void dl_fill_view_(Pc *pc, uint16_t pattern, uint16_t words);
void dl_fill_rows_(Pc *pc, int first, int last, uint16_t pattern, uint16_t seg);
/* n bytes at back-buffer offset off set to value under mask (FF: both pixels, F0 / 0F: one pixel). */
void dl_fill_bytes_(Pc *pc, uint16_t seg, uint16_t off, int n, uint8_t value, uint8_t mask);
void dl_sky_begin_(Pc *pc);
void dl_sky_ground_(Pc *pc);
void dl_poly_begin_(Pc *pc);
void dl_span_fill_(Pc *pc);
void dl_poly_end_(Pc *pc);
void dl_commit_view_(Pc *pc);
/* print_str family (textdraw.h): one glyph row word w written at es:di. Only B800 is kept. */
void dl_text_row_(Pc *pc, uint16_t es, uint16_t di, uint16_t w);

#define DL_HOOK(call) do { if (dl_on) call; } while (0)
static inline void dl_stage_p1(Pc *pc, bool from_prev, uint16_t sw) { DL_HOOK(dl_stage_p1_(pc, from_prev, sw)); }
static inline void dl_stage_p2(Pc *pc, uint16_t sw1, uint16_t sw2) { DL_HOOK(dl_stage_p2_(pc, sw1, sw2)); }
static inline void dl_stage_dot(Pc *pc, uint16_t x, uint16_t y, uint16_t z, uint8_t sx, uint8_t sy) { DL_HOOK(dl_stage_dot_(pc, x, y, z, sx, sy)); }
static inline void dl_capture(Pc *pc, uint16_t addr) { DL_HOOK(dl_capture_(pc, addr)); }
static inline void dl_stage_from_capture(Pc *pc, uint16_t addr) { DL_HOOK(dl_stage_from_capture_(pc, addr)); }
static inline void dl_line(Pc *pc, int x0, int y0, int x1, int y1, uint16_t seg) { DL_HOOK(dl_line_(pc, x0, y0, x1, y1, seg)); }
static inline void dl_dot(Pc *pc, int x, int y, uint16_t seg) { DL_HOOK(dl_dot_(pc, x, y, seg)); }
static inline void dl_fill_view(Pc *pc, uint16_t pattern, uint16_t words) { DL_HOOK(dl_fill_view_(pc, pattern, words)); }
static inline void dl_fill_rows(Pc *pc, int first, int last, uint16_t pattern, uint16_t seg) { DL_HOOK(dl_fill_rows_(pc, first, last, pattern, seg)); }
static inline void dl_fill_bytes(Pc *pc, uint16_t seg, uint16_t off, int n, uint8_t value, uint8_t mask) { DL_HOOK(dl_fill_bytes_(pc, seg, off, n, value, mask)); }
static inline void dl_sky_begin(Pc *pc) { DL_HOOK(dl_sky_begin_(pc)); }
static inline void dl_sky_ground(Pc *pc) { DL_HOOK(dl_sky_ground_(pc)); }
static inline void dl_poly_begin(Pc *pc) { DL_HOOK(dl_poly_begin_(pc)); }
static inline void dl_span_fill(Pc *pc) { DL_HOOK(dl_span_fill_(pc)); }
static inline void dl_poly_end(Pc *pc) { DL_HOOK(dl_poly_end_(pc)); }
static inline void dl_commit_view(Pc *pc) { DL_HOOK(dl_commit_view_(pc)); }
static inline void dl_text_row(Pc *pc, uint16_t es, uint16_t di, uint16_t w) { DL_HOOK(dl_text_row_(pc, es, di, w)); }

#endif
