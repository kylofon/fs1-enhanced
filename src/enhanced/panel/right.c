/* The right-hand panel modules (S2.14-S2.18). Everything dynamic comes from the game state: the slider
 * indicators are draw_indicator descriptors (12 bytes at DS:1401.., +6 = the frame, one pixel or line per frame),
 * the texts are the ASCII records the game prints with print_str (DS:079F compass digits, 07A5/07AB COM, 07B0/07B6
 * NAV, 07C7 transponder, 07D1/07D6/07DB time; the game's records are header word + characters), the lights and switches are single bytes. Layout measured from the
 * clean panel (docs/PANEL.md); the static art (boxes, tracks, letters) is redrawn as vectors. */
#include "enhanced/panel/right.h"
#include "enhanced/panel/kit.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t dseg(const CockpitFrame *f)
{
    const uint8_t *mem = f->pc->mem;
    return (uint32_t)(mem[0x120] | mem[0x121] << 8) << 4;
}

/* The frame of the indicator descriptor at DS:desc. */
static int frame_of(const CockpitFrame *f, uint16_t desc) { return f->pc->mem[dseg(f) + desc + 6]; }

/* A record's ASCII characters (n of them) as a string. */
static void rec_text(const CockpitFrame *f, uint16_t off, int n, char *out)
{
    const uint8_t *mem = f->pc->mem + dseg(f);
    for (int i = 0; i < n; i++) {
        uint8_t ch = mem[off + i];
        out[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : ' ';
    }
    out[n] = 0;
}

/* ---- S2.14: flaps, trim and throttle strips (x 428..441) ------------------------------------ */

static void slider(float x, float y, SDL_FColor c) /* the 5 x 2 line marker, centred at line y */
{
    kit_fill_box(x - 3.0f, y - 1.0f, x + 3.0f, y + 1.0f, c);
}

void throttle_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const float x = 433.0f; /* the stems of the clean panel */
    kit_text_mid(435.0f, 113.0f, 9.0f, 1, "F", kit_grey);
    kit_line(x, 116.5f, x, 128.0f, 1.3f, kit_grey);
    for (int k = 0; k < 6; k++) /* the six flap detents, every 2 lines */
        kit_line(x, 117.5f + 2.0f * (float)k, x + 3.5f, 117.5f + 2.0f * (float)k, 1.2f, kit_grey);
    slider(435.0f, 126.0f - (float)frame_of(f, 0x146D), kit_white);

    kit_text_mid(435.0f, 134.6f, 8.5f, 1, "U", kit_grey); /* the trim strip: U ... D */
    kit_line(x, 138.5f, x, 153.0f, 1.3f, kit_grey);
    kit_line(x, 138.5f, x + 3.5f, 138.5f, 1.2f, kit_grey);
    kit_line(x, 152.5f, x + 3.5f, 152.5f, 1.2f, kit_grey);
    kit_text_mid(435.0f, 156.0f, 8.5f, 1, "D", kit_grey);
    slider(435.0f, 153.0f - (float)frame_of(f, 0x1479), kit_white);

    const float tx = 435.0f; /* the throttle strip: T at the top, a stem with a mid mark down to line 199 */
    kit_text_mid(tx, 164.0f, 8.5f, 1, "T", kit_grey);
    kit_line(tx, 167.0f, tx, 199.0f, 1.3f, kit_grey);
    kit_line(tx - 1.5f, 168.5f, tx + 1.5f, 168.5f, 1.2f, kit_grey);
    kit_line(tx - 1.5f, 198.5f, tx + 1.5f, 198.5f, 1.2f, kit_grey);
    slider(tx, 199.0f - (float)frame_of(f, 0x1461), kit_white);
}

/* ---- S2.15: COMPASS ------------------------------------------------------------------------- */

static void lamp(float x, float y, bool on, SDL_FColor lit)
{
    kit_fill_ellipse(x, y, 6.0f, 6.0f / KIT_SY, kit_grey);
    kit_fill_ellipse(x, y, 5.0f, 5.0f / KIT_SY, on ? lit : kit_face);
}

void compass_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = dseg(f);
    kit_text_mid(480.0f, 112.5f, 9.0f, 1, "COMPASS", kit_white);

    /* the window frame of the digits: a bowl, open at the top */
    kit_line(446.5f, 116.5f, 452.5f, 124.5f, 1.3f, kit_grey);
    kit_line(452.5f, 124.5f, 503.5f, 124.5f, 1.3f, kit_grey);
    kit_line(503.5f, 124.5f, 511.5f, 116.5f, 1.3f, kit_grey);
    char d[4];
    rec_text(f, 0x79F, 3, d);
    for (int i = 0; i < 3; i++) {
        char s[2] = { d[i], 0 };
        kit_text_mid(462.5f + 16.0f * (float)i, 120.5f, 10.0f, 1, s, kit_white);
    }

    /* the beacon lights: O (outer, blue), M (middle, amber), I (inner, white) and their letters */
    int mk = mem[ds + 0x1A16];
    static const char *const letter[3] = { "O", "M", "I" };
    const SDL_FColor lit[3] = { { 0.25f, 0.5f, 1, 1 }, { 1, 0.7f, 0.1f, 1 }, kit_white };
    for (int i = 0; i < 3; i++) {
        float x = 462.5f + 16.0f * (float)i;
        kit_text_mid(x, 128.5f, 8.5f, 1, letter[i], kit_grey);
        lamp(x, 136.5f, mk >> i & 1, lit[i]);
    }
}

/* ---- S2.16: FUEL, OIL T, OIL P --------------------------------------------------------------- */

static void gauge_box(const CockpitFrame *f, float x0, float y0, uint16_t desc, int base_x, const char *l, const char *r)
{
    kit_box(x0, y0, x0 + 55.0f, y0 + 8.5f, 1.3f, kit_grey);
    kit_text_mid(x0 + 11.0f, y0 + 4.5f, 8.0f, 1, l, kit_grey);
    kit_text_mid(x0 + 43.0f, y0 + 4.5f, 8.0f, 1, r, kit_grey);
    /* the bar marker: 4 wide, 5 lines, left edge at base_x + frame, bottom line at the descriptor's base (y0 + 7) */
    float bx = (float)base_x + (float)frame_of(f, desc);
    kit_fill_box(bx, y0 + 1.0f, bx + 4.0f, y0 + 7.0f, kit_white);
}

void fuel_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    kit_text_mid(574.5f, 112.5f, 9.0f, 1, "FUEL", kit_white);
    gauge_box(f, 522.5f, 116.5f, 0x1401, 528, "E", "F");
    gauge_box(f, 580.5f, 116.5f, 0x140D, 584, "E", "F");
    gauge_box(f, 522.5f, 126.5f, 0x1419, 528, "C", "H");
    gauge_box(f, 580.5f, 126.5f, 0x1425, 584, "L", "H");
    kit_text_mid(547.0f, 138.5f, 9.0f, 1, "OIL T", kit_white);
    kit_text_mid(605.0f, 138.5f, 9.0f, 1, "OIL P", kit_white);
}

/* ---- S2.17: GEAR / LIGHTS / MAGS / CARB H ---------------------------------------------------- */

void status_draw(const CockpitFrame *f, const CockpitModule *m)
{
    (void)m;
    const uint8_t *mem = f->pc->mem;
    uint32_t ds = dseg(f);
    static const char *const mags[5] = { "OFF", "L", "R", "BOTH", "ST" };
    int mg = mem[ds + 0x583];
    const char *label[4] = { "GEAR", "LIGHTS", "MAGS", "CARB H" };
    const char *value[4] = { mem[ds + 0x57E] ? "DN" : "UP", mem[ds + 0x582] ? "ON" : "OFF", mags[mg > 4 ? 0 : mg],
                             mem[ds + 0x577] ? "ON" : "OFF" };
    for (int i = 0; i < 4; i++) {
        float y = 146.5f + 6.0f * (float)i;
        kit_text_mid(546.0f, y, 9.0f, 0, label[i], kit_white);
        kit_text_mid(634.0f, y, 8.0f, 2, value[i], kit_white); /* right-aligned: BOTH must stay left of the frame at x 637 */
    }
}

/* ---- S2.18: the radio stack ------------------------------------------------------------------ */

void radio_draw(const CockpitFrame *f, const CockpitModule *m)
{
    char a[8], b[8], c[8], v[24];
    float y = (float)m->r.y0; /* the rect starts one line inside the box (box lines 142/156/170/184) */
    float label_y = y + 3.5f, value_y = y + 9.5f;
    if (strcmp(m->name, "com") == 0 || strcmp(m->name, "nav") == 0) {
        bool com = m->name[0] == 'c';
        rec_text(f, com ? 0x7A5 : 0x7B0, 3, a);
        rec_text(f, com ? 0x7AB : 0x7B6, 2, b);
        snprintf(v, sizeof v, "%s.%s", a, b);
        kit_text_mid(451.0f, label_y, 9.0f, 0, com ? "COM" : "NAV", kit_white);
        kit_text_mid(456.0f, value_y, 10.0f, 0, v, kit_white);
    } else if (strcmp(m->name, "transponder") == 0) {
        rec_text(f, 0x7C7, 7, a); /* "1 2 0 0": the digits at every second character */
        v[0] = a[0], v[1] = a[2], v[2] = a[4], v[3] = a[6], v[4] = 0;
        kit_text_mid(451.0f, label_y, 9.0f, 0, "TRANSPONDER", kit_white);
        kit_text_mid(456.0f, value_y, 10.0f, 0, v, kit_white);
    } else { /* time: hours, minutes, seconds */
        rec_text(f, 0x7D1, 2, a);
        rec_text(f, 0x7D6, 2, b);
        rec_text(f, 0x7DB, 2, c);
        snprintf(v, sizeof v, "%s:%s:%s", a, b, c);
        kit_text_mid(451.0f, label_y, 9.0f, 0, "TIME", kit_white);
        kit_text_mid(456.0f, value_y, 10.0f, 0, v, kit_white);
    }
}
