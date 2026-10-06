/* OMI/VOR indicator module (S2.9): the round NAV instrument. Read from the game state, as obi_display
 * (gauges.c) shows it: the course (localizer) needle position DS:1A10 (0..60, 30 = centre, one pixel per step),
 * the glide-slope bar DS:1A12 (line 122 + n), the TO/FROM flag DS:1A14 (0 OFF, 1 TO, 2 FROM), and the OBS course (above the card) and its reciprocal (below it) as the ASCII
 * digits of the readout record DS:07B9 (+2 and +8). The marker lights are in the COMPASS box (compass.c). */
#include "enhanced/panel/omi.h"
#include "enhanced/panel/kit.h"

#include <math.h>

#define CX 370.0f /* the ring of the clean panel: x 318..420, lines 113..157 */
#define CY 135.5f
#define RX 51.0f
#define RY (RX / KIT_SY) /* a circle on the 4:3 output */
#define CARD 34.0f       /* radius of the inner card, output pixels */

#define GS_LOC_NEEDLE 0x1A10
#define GS_GS_NEEDLE 0x1A12
#define GS_NAV_FLAG 0x1A14
#define GS_READOUT 0x07B9

void omi_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
    int loc = mem[ds + GS_LOC_NEEDLE], gsl = mem[ds + GS_GS_NEEDLE], flag = mem[ds + GS_NAV_FLAG];
    if (loc > 60)
        loc = 60;

    /* bezel, face and the card */
    kit_fill_ellipse(CX, CY, RX - 1.0f, RY - 1.0f / KIT_SY, kit_face);
    kit_ellipse(CX, CY, RX - 0.5f, RY - 0.5f / KIT_SY, 3.0f, kit_bezel);
    kit_ellipse(CX, CY, CARD, CARD / KIT_SY, 1.5f, kit_grey);

    /* deviation dots and the fixed centre mark */
    for (int s = -1; s <= 1; s += 2)
        for (int k = 0; k < 4; k++)
            kit_fill_ellipse(CX + (float)s * (11.0f + 6.0f * (float)k), CY, 1.4f, 1.4f / KIT_SY, kit_grey);
    kit_box(CX - 5.5f, CY - 2.0f, CX + 5.5f, CY + 2.5f, 1.2f, kit_grey);

    /* glide-slope bar: line 122 + n */
    float gy = 122.5f + (float)gsl;
    kit_line(CX - 15.0f, gy, CX + 15.0f, gy, 1.8f, kit_white);

    /* course needle: a vertical line, as long as the card allows at its position */
    float nx = CX + (float)(loc - 30), dx = nx - CX;
    float half = sqrtf(fmaxf(CARD * CARD - dx * dx, 0)) - 1.5f;
    if (half > 1)
        kit_line(nx, CY - half / KIT_SY, nx, CY + half / KIT_SY, 2.2f, kit_white);

    /* TO / FROM flag */
    const char *txt = flag == 0 ? "OFF" : flag == 1 ? "TO" : "FROM";
    SDL_FColor fc = flag == 0 ? (SDL_FColor){ 1, 0.4f, 0.3f, 1 } : kit_white;
    kit_text(CX - 12.0f, 142.0f, 6.8f, 0, 1, txt, fc);

    /* OBS course (large, in the band above the card) and its reciprocal (below): the ASCII digits of the readout record */
    for (int k = 0; k < 2; k++) {
        char s[4];
        for (int i = 0; i < 3; i++) {
            uint8_t ch = mem[ds + GS_READOUT + (k ? 8 : 2) + i];
            s[i] = ch >= '0' && ch <= '9' ? (char)ch : ' ';
        }
        s[3] = 0;
        if (k == 0)
            kit_text(CX, 118.0f - 4.0f / KIT_SY, 8.0f, 0, 1, s, kit_white);
        else
            kit_text(CX, 153.0f - 3.4f / KIT_SY, 6.8f, 0, 1, s, kit_grey);
    }
}
