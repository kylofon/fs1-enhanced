#include "enhanced/panel/strips.h"
#include "enhanced/panel/kit.h"

#define RUDDER_DESC 0x1449 /* draw_indicator descriptor: +6 = the frame (one pixel per frame) */

void strips_rudder(const CockpitFrame *f, float x0, float x1)
{
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    float a = x0 > 186.0f ? x0 : 186.0f, b = x1 < 240.0f ? x1 : 240.0f;
    if (b > a)
        kit_line(a, 199.4f, b, 199.4f, 1.2f, kit_grey);
    float cx = 176.0f + (float)mem[ds + RUDDER_DESC + 6] + 3.5f;
    if (cx >= x0 && cx < x1) { /* a triangle pointing down at the track */
        float xy[6] = { cx, 199.2f, cx - 3.6f, 195.8f, cx + 3.6f, 195.8f };
        kit_fill_poly(xy, 3, kit_white);
    }
}
