/* Panel frame module (S2.19): everything of the clean panel image from line 107 down that lies outside the
 * instrument modules' rects (the glareshield, the double lines between the modules, the box frames, the dotted
 * tracks of the aileron and rudder indicators). It is read from the clean image, once per image:
 *   - horizontal runs (a single missing pixel allowed, so dotted lines count) of 12+ pixels become lines,
 *   - then vertical runs of 8+ pixels,
 *   - what is left (the glareshield's curves) becomes crisp pixel runs on the hi-res grid.
 * Dotted runs are drawn grey, solid ones white. Also draws the aileron marker (descriptor DS:143D, a triangle
 * pointing down at the track on line 160); the rudder marker is drawn by the modules above the bottom track. */
#include "enhanced/panel/frame.h"
#include "enhanced/panel/kit.h"

#include <stdlib.h>
#include <string.h>

#define W 640
#define H 200
#define Y0 107 /* lines 0..106 of the clean image are scratch (the view's working copy), not panel */

typedef struct Seg {
    float x0, y0, x1, y1;
    bool dotted;
} Seg;

typedef struct Run {
    int x0, x1, y;
} Run;

static Seg *segs;
static Run *runs;
static int n_segs, n_runs, cap_segs, cap_runs;
static uint32_t built_hash;
static bool built;

static bool lit(const uint8_t *img, int x, int y) { return x >= 0 && x < W && img[(y & 1) * 0x2000 + (y >> 1) * 80 + (x >> 3)] >> (7 - (x & 7)) & 1; }

static void add_seg(Seg s)
{
    if (n_segs == cap_segs) {
        cap_segs = cap_segs ? cap_segs * 2 : 256;
        segs = realloc(segs, (size_t)cap_segs * sizeof *segs);
    }
    segs[n_segs++] = s;
}

static void add_run(Run r)
{
    if (n_runs == cap_runs) {
        cap_runs = cap_runs ? cap_runs * 2 : 1024;
        runs = realloc(runs, (size_t)cap_runs * sizeof *runs);
    }
    runs[n_runs++] = r;
}

static void build(const uint8_t *img)
{
    static uint8_t mask[H][W];
    n_segs = n_runs = 0;
    memset(mask, 0, sizeof mask);
    for (int y = Y0; y < H; y++)
        for (int x = 0; x < W; x++)
            mask[y][x] = lit(img, x, y) && !cockpit_in_module(x, y);

    for (int y = Y0; y < H; y++) { /* horizontal runs, one gap allowed */
        for (int x = 0; x < W; x++) {
            if (!mask[y][x])
                continue;
            int e = x, count = 1, gaps = 0;
            while (e + 1 < W && (mask[y][e + 1] || (e + 2 < W && mask[y][e + 2] && !mask[y][e + 1]))) {
                if (!mask[y][e + 1])
                    gaps++, e += 2;
                else
                    e++;
                count++;
            }
            if (e - x + 1 >= 12 && count >= 7) {
                add_seg((Seg){ (float)x, (float)y + 0.5f, (float)e + 1.0f, (float)y + 0.5f, gaps > 0 });
                for (int i = x; i <= e; i++)
                    mask[y][i] = 0;
            }
            x = e;
        }
    }
    for (int x = 0; x < W; x++) { /* vertical runs */
        for (int y = Y0; y < H; y++) {
            if (!mask[y][x])
                continue;
            int e = y;
            while (e + 1 < H && mask[e + 1][x])
                e++;
            if (e - y + 1 >= 8) {
                add_seg((Seg){ (float)x + 0.5f, (float)y, (float)x + 0.5f, (float)e + 1.0f, false });
                for (int i = y; i <= e; i++)
                    mask[i][x] = 0;
            }
            y = e;
        }
    }
    for (int y = Y0; y < H; y++) { /* the rest: pixel runs */
        for (int x = 0; x < W; x++) {
            if (!mask[y][x])
                continue;
            int e = x;
            while (e + 1 < W && mask[y][e + 1])
                e++;
            add_run((Run){ x, e, y });
            x = e;
        }
    }
}

void frame_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    /* a hash of the pixels the frame owns (the clean image also changes inside the modules) */
    uint32_t h = 2166136261u;
    for (int y = Y0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (!cockpit_in_module(x, y))
                h = (h ^ (uint32_t)lit(f->clean, x, y)) * 16777619u;
    if (!built || h != built_hash)
        build(f->clean), built_hash = h, built = true;

    for (int i = 0; i < n_segs; i++) {
        const Seg *s = &segs[i];
        bool horizontal = s->y0 == s->y1;
        float w = horizontal ? 2.4f : 1.2f; /* a horizontal line fills its scan line exactly: no halo of the fill behind it */
        /* a pixel-wide run: lines are drawn from end to end, without the cap kit_line adds */
        kit_needle(s->x0, s->y0, s->x1, s->y1, w, w, s->dotted ? kit_grey : kit_white);
    }
    for (int i = 0; i < n_runs; i++)
        kit_fill_box((float)runs[i].x0, (float)runs[i].y, (float)runs[i].x1 + 1.0f, (float)runs[i].y + 1.0f, kit_white);

    /* the aileron marker: a triangle pointing down at the dotted track on line 160 */
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    float cx = 176.0f + (float)mem[ds + 0x143D + 6] + 3.5f;
    float tri[6] = { cx, 160.0f, cx - 3.6f, 156.6f, cx + 3.6f, 156.6f };
    kit_fill_poly(tri, 3, kit_white);
}
