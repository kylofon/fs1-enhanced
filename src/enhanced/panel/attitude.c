/* Attitude indicator module (S2.7): the artificial horizon as a vector picture. The original (gauges.c
 * `attitude`) draws a disc whose sky/ground boundary is a chord between two points of the disc's rim, 256
 * angle units per turn (counter-clockwise from the right), from view_bank (high byte) and view_pitch:
 *   n  = 6 - view_pitch / 128, d = +-table[|n|] (DS:1CCB, |n| <= 31)
 *   chord end points at  -bank - d  and  -bank + d + 128
 * and the sky is the part of the disc the rim arc from the first to the second point (counter-clockwise)
 * cuts off. The same formula is used here with the fraction kept (sub-pixel), checked against the game's own
 * row records (DS:1BAB) for several bank/pitch values. */
#include "enhanced/panel/attitude.h"
#include "enhanced/panel/kit.h"

#include <math.h>

#define CX 160.5f /* panel units: the outer ring spans x 110..210, lines 113..157 */
#define CY 135.5f
#define RX 50.0f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */
#define DISC 0.82f       /* the sky/ground disc, as a fraction of the ring */

#define GS_VIEW_PITCH 0x30F1
#define GS_VIEW_BANK 0x30F3
#define GS_AI_TABLE 0x1CCB  /* pitch -> chord offset, 32 bytes */
#define GS_AI_COLOURS 0x1CEB /* ground word, sky word */

#define UNIT 1.40625f /* degrees per angle unit (256 per turn) */

static void at(float a, float f, float *x, float *y) { kit_dial_point(CX, CY, RX, RY, a, f, x, y); }

/* The dial angle (0 = up, clockwise) of an angle unit v (counter-clockwise from the right). */
static float unit_dial(float v) { return 90.0f - v * UNIT; }

/* Convex polygon: the rim arc from unit v0 to v1 (counter-clockwise), closed by the chord. */
static void segment(float v0, float v1, SDL_FColor c)
{
    enum { MAXP = 160 };
    float xy[2 * MAXP];
    int n = (int)ceilf(fabsf(v1 - v0) / 2.0f) + 1;
    if (n < 2)
        n = 2;
    if (n > MAXP)
        n = MAXP;
    for (int i = 0; i < n; i++)
        at(unit_dial(v0 + (v1 - v0) * (float)i / (float)(n - 1)), DISC, &xy[2 * i], &xy[2 * i + 1]);
    kit_fill_poly(xy, n, c);
}

static void triangle(float a, float f_tip, float f_base, float half_w, SDL_FColor c)
{
    float tx, ty, bx, by, ox = 0, oy = 0;
    at(a, f_tip, &tx, &ty);
    at(a, f_base, &bx, &by);
    /* the base is half_w output pixels to each side, perpendicular to the pointer (in output pixels) */
    float dx = tx - bx, dy = (ty - by) * KIT_SY, len = sqrtf(dx * dx + dy * dy);
    if (len > 0)
        ox = -dy / len * half_w, oy = dx / len * half_w / KIT_SY;
    float xy[6] = { tx, ty, bx + ox, by + oy, bx - ox, by - oy };
    kit_fill_poly(xy, 3, c);
}

void attitude_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    int16_t pitch = (int16_t)(mem[ds + GS_VIEW_PITCH] | mem[ds + GS_VIEW_PITCH + 1] << 8);
    uint16_t bank = (uint16_t)(mem[ds + GS_VIEW_BANK] | mem[ds + GS_VIEW_BANK + 1] << 8);

    /* chord offset: the game's table, interpolated for a fractional pitch step */
    float n = 6.0f - (float)pitch / 128.0f, an = fminf(fabsf(n), 31.0f);
    int i = (int)an, j = i < 31 ? i + 1 : 31;
    float t = an - (float)i, d = (float)mem[ds + GS_AI_TABLE + i] * (1 - t) + (float)mem[ds + GS_AI_TABLE + j] * t;
    if (n < 0)
        d = -d;
    float bh = (float)bank / 256.0f; /* view_bank = -bank, 256 units per turn */
    float v1 = -bh - d, v2 = -bh + d + 128.0f;
    while (v2 < v1)
        v2 += 256.0f;

    uint8_t gn = mem[ds + GS_AI_COLOURS] >> 4, sn = mem[ds + GS_AI_COLOURS + 2] >> 4; /* ground, sky patterns */
    SDL_FColor ground = kit_composite(&gn, 1), sky = kit_composite(&sn, 1);

    /* ring and disc: ground everywhere, the sky segment over it */
    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);
    kit_fill_ellipse(CX, CY, RX * DISC, RY * DISC, ground);
    segment(v1, v2, sky);
    float x1, y1, x2, y2;
    at(unit_dial(v1), DISC, &x1, &y1);
    at(unit_dial(v2), DISC, &x2, &y2);
    kit_line(x1, y1, x2, y2, 1.6f, kit_white);
    kit_ellipse(CX, CY, RX * DISC, RY * DISC, 1.5f, kit_bezel);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);

    /* fixed roll scale on the ring: 10-degree ticks to 60, long at 30 and 60, the zero index above */
    for (int a = -60; a <= 60; a += 10) {
        if (a == 0)
            continue;
        float x0, y0, xe, ye;
        at((float)a, (a % 30 == 0) ? 0.86f : 0.89f, &x0, &y0);
        at((float)a, 0.95f, &xe, &ye);
        kit_needle(x0, y0, xe, ye, (a % 30 == 0) ? 2.0f : 1.5f, (a % 30 == 0) ? 2.0f : 1.5f, kit_white);
    }
    triangle(0.0f, 0.855f, 0.955f, 4.0f, kit_white); /* fixed zero index, pointing at the disc */

    /* the pointer that turns with the horizon: at the "up" of the disc, i.e. the bank angle */
    triangle(bh * UNIT, 0.80f, 0.68f, 3.5f, kit_white);

    /* fixed marks and the aircraft symbol */
    for (int s = -1; s <= 1; s += 2) {
        for (int k = 1; k <= 2; k++) {
            float y = CY + (float)s * (float)k * 5.0f / 1.0f, w = (k == 1) ? 7.0f : 12.0f;
            kit_line(CX - w / 2, y, CX + w / 2, y, 1.2f, kit_white);
        }
        float xw = CX + (float)s * 24.0f; /* wing: a bar from the tip towards the centre, a short stub down */
        kit_line(xw, CY, CX + (float)s * 8.0f, CY, 3.0f, kit_black);
        kit_line(xw, CY, CX + (float)s * 8.0f, CY, 1.8f, kit_white);
        kit_line(xw, CY, xw, CY + 3.5f, 1.8f, kit_white);
    }
    kit_fill_ellipse(CX, CY, 3.0f, 3.0f / KIT_SY, kit_black);
    kit_fill_ellipse(CX, CY, 2.0f, 2.0f / KIT_SY, kit_white);
}
