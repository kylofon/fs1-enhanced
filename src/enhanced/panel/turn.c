/* Turn coordinator module (S2.10): a round bezel (cut off by the bottom of the screen), the aircraft symbol
 * (wing and fin) that tilts with the turn rate, the L/R rate marks and the inclinometer with its ball.
 * From the game state as turn_needle / the ball code (gauges.c) use it: tilt step t = turn_rate (DS:085A) * 190h
 * >> 16 clamped to -6..6, the sprites tilt 7.3 degrees per step (43.8 at 6, measured from the 13 sprites at
 * DS:16DD, the first one has the left end low); the ball = the byte at DS:08F7, one pixel per unit. */
#include "enhanced/panel/turn.h"
#include "enhanced/panel/kit.h"

#include <math.h>

#define CX 56.0f /* the bezel of the clean panel spans x 13..99, from line 169 down */
#define CY 187.1f
#define RX 43.5f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */

#define GS_TURN_RATE 0x085A
#define GS_BALL_VALUE 0x08F7

#define PIVOT_X 55.5f /* the wing's centre in the clean panel */
#define PIVOT_Y 178.5f
#define WING 21.5f      /* half length of the wing, output pixels */
#define DEG_PER_STEP 7.3f

void turn_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    int16_t rate = (int16_t)(mem[ds + GS_TURN_RATE] | mem[ds + GS_TURN_RATE + 1] << 8);
    int8_t ball = (int8_t)mem[ds + GS_BALL_VALUE];
    float t = fminf(fmaxf((float)rate * 0x190 / 65536.0f, -6.0f), 6.0f);

    /* bezel and face */
    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);

    /* rate marks and L / R */
    for (int s = -1; s <= 1; s += 2) {
        float mx = PIVOT_X + (float)s * 29.0f;
        kit_line(mx - 3.0f, 177.5f, mx + 3.0f, 177.5f, 2.2f, kit_white);
        kit_text(PIVOT_X + (float)s * 31.0f, 182.2f, 6.5f, 0, 1, s < 0 ? "L" : "R", kit_grey);
    }

    /* inclinometer: a box with the two centre ticks, and the ball */
    const float bx = 58.0f; /* ball sprite 6 wide at x = 24 + frame, frame = ball + 31 */
    kit_box(bx - 33.0f, 187.5f, bx + 33.0f, 194.5f, 1.2f, kit_grey);
    kit_line(bx - 6.5f, 188.5f, bx - 6.5f, 193.5f, 1.5f, kit_white);
    kit_line(bx + 6.5f, 188.5f, bx + 6.5f, 193.5f, 1.5f, kit_white);
    kit_fill_ellipse(bx + (float)ball, 191.0f, 3.6f, 3.6f / KIT_SY, kit_white);

    /* the aircraft symbol: wing and fin, tilted by the turn rate */
    float a = -t * DEG_PER_STEP * (float)M_PI / 180.0f, ca = cosf(a), sa = sinf(a);
    float wx = WING * ca, wy = WING * sa / KIT_SY; /* left end low for a negative step */
    kit_line(PIVOT_X - wx, PIVOT_Y + wy, PIVOT_X + wx, PIVOT_Y - wy, 2.6f, kit_white);
    float fl = 10.0f; /* the fin, output pixels, perpendicular to the wing */
    kit_line(PIVOT_X, PIVOT_Y, PIVOT_X - fl * sa, PIVOT_Y - fl * ca / KIT_SY, 2.6f, kit_white);
    kit_fill_ellipse(PIVOT_X, PIVOT_Y, 2.6f, 2.6f / KIT_SY, kit_bezel);
}
