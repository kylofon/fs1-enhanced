#ifndef FS1_RASTER_H
#define FS1_RASTER_H

/* Line rasteriser of the 3D view (subphase 3.4), reusable from C.
 *
 * The view is 160 x 106 "fat" pixels. Each pixel is one nibble of the back buffer, which uses
 * the CGA layout: 80 bytes per row, even rows at +0000, odd rows at +2000, the high nibble is
 * the left pixel. Pixels are drawn with the nibble pair [30C8] (low nibble = right pixel) and
 * [30C9] (high nibble = left pixel), and rows are found through the word table at data:380C.
 *
 * These functions reproduce the original routines exactly: every pixel, the order of the
 * memory accesses that matter, and the final register values. All memory goes through the
 * RasterBus callbacks:
 *   buf_*   back buffer segment ([3806], normally 0C0C). Also holds the polygon-edge scratch
 *           bytes at 1F44..1F49 (between the two banks), used by the polygon path only.
 *   dat_*   game data segment (0618): row table 380C, colours 30C8, flag 31D0, and the polygon
 *           seed list at 3730.
 *   call    optional; told the return address of each internal CALL the original makes (the
 *           polygon path calls helpers), so an emulator can write the same stack word. NULL
 *           is fine for plain C use.
 */

#include <stdint.h>

typedef struct RasterBus {
    void *ctx;
    uint8_t (*buf_rd)(void *ctx, uint16_t off);
    void (*buf_wr)(void *ctx, uint16_t off, uint8_t v);
    uint8_t (*dat_rd)(void *ctx, uint16_t off);
    void (*dat_wr)(void *ctx, uint16_t off, uint8_t v);
    void (*call)(void *ctx, uint16_t ret_ip);
} RasterBus;

/* 8086 registers as draw_line takes and leaves them: BX,BP = x0,y0 and SI,DI = x1,y1. */
typedef struct RasterRegs {
    uint16_t ax, bx, cx, dx, si, di, bp;
} RasterRegs;

/* 0050:569B, the normal path of draw_line (after it sets DS/ES). */
void raster_line_regs(const RasterBus *b, RasterRegs *r);

/* 0050:5060, the polygon-outline path draw_line takes while [31D0] != 0 (after DS/ES). */
void raster_poly_edge_regs(const RasterBus *b, RasterRegs *r);

/* draw_line from C: picks the path by [31D0] like the original. Coordinates must be on the
 * view (x 0..159, y 0..105); the original does no clipping. */
void raster_line(const RasterBus *b, int x0, int y0, int x1, int y1);

#endif
