/* Heading indicator module (S2.11): a round bezel (cut off by the bottom of the screen) with a card that turns
 * under the lubber mark, the aircraft symbol and the 3-digit heading. The original shows only the digits (the game
 * keeps them as ASCII in DS:0799, from heading_readout) and the symbol; the card is drawn from the same number.
 * Also draws its part of the rudder track and marker (strips.c). */
#include "enhanced/panel/heading.h"
#include "enhanced/panel/kit.h"
#include "enhanced/panel/strips.h"

#include <math.h>
#include <stdio.h>

#define CX 160.5f /* the bezel of the clean panel spans x 117..202, from line 163 down */
#define CY 181.0f
#define RX 43.0f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */

#define GS_HEADING_DIGITS 0x0799

/* Text centred on a point and turned with the card. The kit anchors text at the top of its caps. */
static void card_text(float a, float f, float h, const char *s, SDL_FColor c)
{
    float x, y, r = a * (float)M_PI / 180.0f;
    kit_dial_point(CX, CY, RX, RY, a, f, &x, &y);
    float ox = h * 0.5f * sinf(r), oy = -h * 0.5f * cosf(r); /* from the text's top centre to its middle, output px */
    kit_text(x + ox, y + oy / KIT_SY, h, a, 1, s, c);
}

void heading_draw(const CockpitFrame *f, const CockpitModule *m)
{
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    char digits[4] = { 0 };
    int hdg = 0;
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        uint8_t ch = mem[ds + GS_HEADING_DIGITS + i];
        ok = ok && ch >= '0' && ch <= '9';
        digits[i] = ok ? (char)ch : '-';
        hdg = hdg * 10 + (ok ? ch - '0' : 0);
    }

    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);

    /* the card: a tick every 10 degrees, numerals every 30 */
    for (int m10 = 0; m10 < 36; m10++) {
        float a = (float)(m10 * 10 - hdg), x0, y0, x1, y1;
        bool major = m10 % 3 == 0;
        kit_dial_point(CX, CY, RX, RY, a, major ? 0.84f : 0.89f, &x0, &y0);
        kit_dial_point(CX, CY, RX, RY, a, 0.95f, &x1, &y1);
        kit_needle(x0, y0, x1, y1, major ? 2.0f : 1.3f, major ? 2.0f : 1.3f, major ? kit_white : kit_grey);
    }
    static const char *const lab[12] = { "N", "3", "6", "E", "12", "15", "S", "21", "24", "W", "30", "33" };
    for (int k = 0; k < 12; k++)
        card_text((float)(k * 30 - hdg), 0.72f, 6.5f, lab[k], k % 3 == 0 ? kit_white : kit_grey);

    /* digital readout and the fixed lubber mark */
    kit_text(CX, 169.6f, 6.8f, 0, 1, digits, kit_white);
    float tri[6] = { CX, 164.6f, CX - 3.5f, 163.3f, CX + 3.5f, 163.3f };
    kit_fill_poly(tri, 3, kit_white);

    /* aircraft symbol: wing, fuselage, tail */
    kit_line(CX - 11.5f, 178.5f, CX + 11.5f, 178.5f, 2.4f, kit_white);
    kit_line(CX, 175.0f, CX, 183.5f, 2.4f, kit_white);
    kit_line(CX - 5.0f, 182.5f, CX + 5.0f, 182.5f, 2.0f, kit_white);

    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);
    strips_rudder(f, (float)m->r.x0, (float)m->r.x1 + 1.0f);
}
