/* Altimeter module (S2.8): a round dial 0-9 (one numeral = 100 ft), a long hand (1000 ft per turn) and a short
 * thick hand (10000 ft per turn). The angles are the original's (upd_altimeter): A = (altitude word + ground
 * elevation + altimeter offset) * 6B8Fh / 8000h in 1/256 turn; the long hand shows A mod 256, the short hand
 * A mod 2550 / 10 (both in 1/256 turn), read from the game state so they are exact, not fitted to the sprites. */
#include "enhanced/panel/altimeter.h"
#include "enhanced/panel/kit.h"

#include <math.h>
#include <stdio.h>

#define CX 264.5f /* the ring of the clean panel: x 213..315, lines 113..157 */
#define CY 135.5f
#define RX 51.0f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */

#define GS_ALTITUDE_HI 0x30D6 /* the metres word inside the 32-bit altitude at 30D5 */
#define GS_GROUND_ELEV 0x0906
#define GS_ALT_OFFSET 0x1E20

static void at(float a, float f, float *x, float *y) { kit_dial_point(CX, CY, RX, RY, a, f, x, y); }

static void spoke(float a, float f0, float f1, float w, SDL_FColor c)
{
    float x0, y0, x1, y1;
    at(a, f0, &x0, &y0);
    at(a, f1, &x1, &y1);
    kit_needle(x0, y0, x1, y1, w, w, c);
}

static int16_t rd16s(const uint8_t *mem, uint32_t o) { return (int16_t)(mem[o] | mem[o + 1] << 8); }

void altimeter_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    int16_t total = (int16_t)(rd16s(mem, ds + GS_ALTITUDE_HI) + rd16s(mem, ds + GS_GROUND_ELEV) + rd16s(mem, ds + GS_ALT_OFFSET));
    float a = (float)total * 0x6B8F / 32768.0f; /* 1/256 turn per unit; 256 units = 1000 ft */
    float large = fmodf(a, 256.0f), small = fmodf(a, 2550.0f);
    if (large < 0)
        large += 256.0f;
    if (small < 0)
        small += 2550.0f;
    float ang_l = large * (360.0f / 256.0f), ang_s = small / 2550.0f * 360.0f;

    /* bezel and face */
    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);

    /* scale: 50 ticks, long and numbered every 5th */
    for (int k = 0; k < 50; k++) {
        if (k % 5 == 0)
            spoke((float)k * 7.2f, 0.84f, 0.95f, 2.0f, kit_white);
        else
            spoke((float)k * 7.2f, 0.90f, 0.95f, 1.2f, kit_grey);
    }
    const float text_h = 7.5f;
    for (int k = 0; k < 10; k++) {
        char s[2] = { (char)('0' + k), 0 };
        float x, y;
        at((float)k * 36.0f, 0.70f, &x, &y);
        kit_text(x, y - text_h / 2 / KIT_SY, text_h, 0, 1, s, kit_white);
    }
    kit_text(CX, CY + 5.0f, 7.0f, 0, 1, "ALT", kit_grey);

    /* short hand: wide and short with a point; long hand: long, tapered, with a tail */
    float tx, ty, bx, by;
    at(ang_s, 0.52f, &tx, &ty);
    at(ang_s + 180, 0.10f, &bx, &by);
    kit_needle(bx, by, tx, ty, 6.0f, 4.5f, kit_bezel);
    float ax, ay;
    at(ang_s, 0.62f, &ax, &ay);
    kit_needle(tx, ty, ax, ay, 4.5f, 0.5f, kit_bezel);
    at(ang_l, 0.88f, &tx, &ty);
    at(ang_l + 180, 0.16f, &bx, &by);
    kit_needle(bx, by, tx, ty, 3.5f, 1.4f, kit_white);
    kit_fill_ellipse(CX, CY, 3.5f, 3.5f / KIT_SY, kit_black);
    kit_fill_ellipse(CX, CY, 2.2f, 2.2f / KIT_SY, kit_bezel);
}
