/* Vertical speed indicator module (S2.12) and the elevator strip next to it. The needle is the original's: the
 * oldest of the ten vertical-speed samples (DS:091B), * 7D0h >> 15 clamped to -7Dh..7Dh, from 9 o'clock,
 * 1/256 turn per unit (upd_vsi). The scale is read from the clean panel: 5, 10, 15 at 45 degrees apart (9 per
 * label unit). The elevator marker is the descriptor DS:1455 (frame = lines above line 187). Draws its part of the
 * rudder track and marker (strips.c). */
#include "enhanced/panel/vsi.h"
#include "enhanced/panel/kit.h"
#include "enhanced/panel/strips.h"

#include <math.h>

#define CX 265.0f /* the bezel of the clean panel spans x 223..307, from line 163 down */
#define CY 181.0f
#define RX 42.5f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */

#define GS_VS_HISTORY 0x091B
#define ELEVATOR_DESC 0x1455

static void at(float a, float f, float *x, float *y) { kit_dial_point(CX, CY, RX, RY, a, f, x, y); }

void vsi_draw(const CockpitFrame *f, const CockpitModule *m)
{
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    int16_t v = (int16_t)(mem[ds + GS_VS_HISTORY] | mem[ds + GS_VS_HISTORY + 1] << 8);
    float units = fminf(fmaxf((float)v * 0x7D0 / 32768.0f, -125.0f), 125.0f);
    float needle = 270.0f + units * 1.40625f;

    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);

    /* scale: a tick per label unit (9 degrees), long and numbered every 5 */
    for (int k = -19; k <= 19; k++) {
        float a = 270.0f + (float)k * 9.0f, x0, y0, x1, y1;
        bool major = k % 5 == 0;
        at(a, major ? 0.84f : 0.90f, &x0, &y0);
        at(a, 0.95f, &x1, &y1);
        kit_needle(x0, y0, x1, y1, major ? 2.0f : 1.2f, major ? 2.0f : 1.2f, major ? kit_white : kit_grey);
    }
    for (int k = -15; k <= 15; k += 5) {
        char s[4];
        int n = k < 0 ? -k : k;
        if (n >= 10)
            s[0] = (char)('0' + n / 10), s[1] = (char)('0' + n % 10), s[2] = 0;
        else
            s[0] = (char)('0' + n), s[1] = 0;
        float x, y;
        at(270.0f + (float)k * 9.0f, 0.68f, &x, &y);
        kit_text(x, y - 3.2f / KIT_SY, 6.4f, 0, 1, s, kit_white);
    }
    kit_text(CX - 2.0f, 175.6f, 6.0f, 0, 1, "UP", kit_grey);
    kit_text(CX - 2.0f, 185.0f, 6.0f, 0, 1, "DN", kit_grey);

    /* needle with a short tail, and the hub */
    float tx, ty, bx, by;
    at(needle, 0.82f, &tx, &ty);
    at(needle + 180.0f, 0.14f, &bx, &by);
    kit_needle(bx, by, tx, ty, 3.5f, 1.4f, kit_white);
    kit_fill_ellipse(CX, CY, 3.2f, 3.2f / KIT_SY, kit_bezel);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);

    /* the elevator strip: a track, three marks and the marker */
    kit_line(212.5f, 165.0f, 212.5f, 187.0f, 1.3f, kit_grey);
    for (int i = 0; i < 3; i++)
        kit_line(210.0f, 165.5f + (float)i * 10.5f, 215.0f, 165.5f + (float)i * 10.5f, 1.2f, kit_grey);
    float my = 187.0f - (float)mem[ds + ELEVATOR_DESC + 6];
    kit_fill_box(208.0f, my - 1.0f, 213.5f, my + 1.0f, kit_white);

    strips_rudder(f, (float)m->r.x0, (float)m->r.x1 + 1.0f);
}
