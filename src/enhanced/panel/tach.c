/* Tachometer module (S2.13). The needle is the original's (upd_tach): the byte engine_rpm (DS:1DC0) * 3B6h >> 8
 * plus 8Ah, 1/256 turn per unit, 0 = up. The scale 5..35 sweeps 270 -> 502 degrees (7.73 per label unit), read from
 * the clean panel's numerals; the needle at idle (rpm 16) reads 6, at full power (58) 34. */
#include "enhanced/panel/tach.h"
#include "enhanced/panel/kit.h"

#include <math.h>
#include <stdio.h>

#define CX 370.0f /* the bezel of the clean panel spans x 329..411, from line 163 down */
#define CY 181.0f
#define RX 41.5f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */

#define GS_ENGINE_RPM 0x1DC0
#define LABEL_START 270.0f
#define DEG_PER_LABEL 7.73f

static void at(float a, float f, float *x, float *y) { kit_dial_point(CX, CY, RX, RY, a, f, x, y); }

static float label_angle(float k) { return LABEL_START + (k - 5.0f) * DEG_PER_LABEL; }

void tach_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    float units = fmodf((float)mem[ds + GS_ENGINE_RPM] * 0x3B6 / 256.0f + 0x8A, 256.0f);
    float needle = units * (360.0f / 256.0f);

    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);

    for (int k = 5; k <= 35; k++) {
        float a = label_angle((float)k), x0, y0, x1, y1;
        bool major = k % 5 == 0;
        at(a, major ? 0.84f : 0.90f, &x0, &y0);
        at(a, 0.95f, &x1, &y1);
        kit_needle(x0, y0, x1, y1, major ? 2.0f : 1.2f, major ? 2.0f : 1.2f, major ? kit_white : kit_grey);
    }
    for (int k = 5; k <= 35; k += 5) {
        char s[4];
        snprintf(s, sizeof s, "%d", k);
        float x, y;
        at(label_angle((float)k), 0.68f, &x, &y);
        kit_text(x, y - 3.2f / KIT_SY, 6.4f, 0, 1, s, kit_white);
    }
    kit_text(CX, 191.5f, 6.5f, 0, 1, "RPM", kit_grey);

    float tx, ty, bx, by;
    at(needle, 0.82f, &tx, &ty);
    at(needle + 180.0f, 0.14f, &bx, &by);
    kit_needle(bx, by, tx, ty, 3.5f, 1.4f, kit_white);
    kit_fill_ellipse(CX, CY, 3.2f, 3.2f / KIT_SY, kit_bezel);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);
}
