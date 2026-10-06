/* Airspeed indicator module (S2.6): a round dial, 40-200 knots over 289 degrees, as the original's. The needle
 * angle is the original's own (upd_airspeed: airspeed * 28Fh >> 16 clamped to 33h..100h, minus 19h, 256 steps
 * per turn), read from the game state, so the needle and the scale are exact and not fitted to sprite pixels. */
#include "enhanced/panel/airspeed.h"
#include "enhanced/panel/kit.h"

#include <math.h>
#include <stdio.h>

#define CX 55.0f        /* panel units: the ring of the clean panel spans x 5..105, lines 123..167 */
#define CY 145.0f
#define RX 50.0f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */

#define GS_AIRSPEED 0x08A8

/* The needle angle of an indicated airspeed (knots) in degrees clockwise from up. ias = knots * 1.28 (the
 * game's units), the dial turns 360/256 degrees per unit, starting 25 units before 40 knots. */
static float knots_angle(float knots) { return (knots * 1.28f - 25.0f) * (360.0f / 256.0f); }

static void at(float a, float f, float *x, float *y) { kit_dial_point(CX, CY, RX, RY, a, f, x, y); }

static void tick(float knots, float r_in, float r_out, float w, SDL_FColor c)
{
    float a = knots_angle(knots), x0, y0, x1, y1;
    at(a, r_in, &x0, &y0);
    at(a, r_out, &x1, &y1);
    kit_needle(x0, y0, x1, y1, w, w, c);
}

void airspeed_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    uint16_t v = (uint16_t)(mem[ds + GS_AIRSPEED] | mem[ds + GS_AIRSPEED + 1] << 8);
    float ias = (float)v * 0x28F / 65536.0f; /* the game's indicated-airspeed units */
    ias = fminf(fmaxf(ias, 0x33), 0x100);
    float needle = (ias - 25.0f) * (360.0f / 256.0f);

    /* bezel and face */
    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);

    /* scale: a tick every 10 knots, long and numbered every 20 */
    for (int k = 40; k <= 200; k += 10) {
        if (k % 20 == 0)
            tick((float)k, 0.84f, 0.95f, 2.0f, kit_white);
        else
            tick((float)k, 0.90f, 0.95f, 1.5f, kit_grey);
    }
    const float text_h = 6.8f; /* cap height, output pixels */
    for (int k = 40; k <= 200; k += 20) {
        char s[8];
        snprintf(s, sizeof s, "%d", k);
        float x, y;
        at(knots_angle((float)k), 0.64f, &x, &y);
        kit_text(x, y - text_h / 2 / KIT_SY, text_h, 0, 1, s, kit_white);
    }
    kit_text(CX, CY + 5.5f, 7.0f, 0, 1, "KNOTS", kit_grey);

    /* needle: tapered, with a short tail and a hub cap */
    float tx, ty, bx, by;
    at(needle, 0.86f, &tx, &ty);
    at(needle + 180, 0.14f, &bx, &by);
    kit_needle(bx, by, tx, ty, 3.5f, 1.5f, kit_white);
    kit_fill_ellipse(CX, CY, 3.5f, 3.5f / KIT_SY, kit_bezel);
}
