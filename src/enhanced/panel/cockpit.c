/* Cockpit modules, see cockpit.h and docs/PANEL.md. */
#include "enhanced/panel/cockpit.h"
#include "enhanced/panel/kit.h"

#include <string.h>

/* The module rects, measured from the clean panel image (docs/PANEL.md "Layout"). Each module subtask
 * (S2.6-S2.19) sets its draw function here and may refine its rect. */
static CockpitModule modules[] = {
    { "airspeed", "S2.6 airspeed indicator", { 4, 113, 106, 168 }, NULL },
    { "attitude", "S2.7 attitude indicator", { 109, 113, 211, 158 }, NULL },
    { "altimeter", "S2.8 altimeter", { 212, 113, 316, 158 }, NULL },
    { "omi", "S2.9 OMI/VOR indicator", { 317, 113, 421, 158 }, NULL },
    { "turn", "S2.10 turn coordinator", { 12, 169, 100, 199 }, NULL },
    { "heading", "S2.11 heading indicator", { 116, 163, 203, 199 }, NULL },
    { "vsi", "S2.12 vertical speed + elevator trim", { 204, 163, 308, 199 }, NULL },
    { "tach", "S2.13 tachometer", { 328, 163, 412, 199 }, NULL },
    { "throttle", "S2.14 throttle/flaps strip", { 428, 109, 441, 199 }, NULL },
    { "compass", "S2.15 COMPASS + OMI lights", { 445, 109, 514, 141 }, NULL },
    { "fuel", "S2.16 FUEL, OIL T, OIL P", { 518, 109, 636, 141 }, NULL },
    { "status", "S2.17 GEAR/LIGHTS/MAGS/CARB H", { 541, 143, 636, 169 }, NULL },
    { "com", "S2.18 radio stack: COM", { 445, 143, 537, 155 }, NULL },
    { "nav", "S2.18 radio stack: NAV", { 445, 157, 537, 169 }, NULL },
    { "transponder", "S2.18 radio stack: TRANSPONDER", { 445, 171, 561, 183 }, NULL },
    { "time", "S2.18 radio stack: TIME", { 445, 185, 561, 197 }, NULL },
};
#define N_MODULES (int)(sizeof modules / sizeof *modules)

static bool off[N_MODULES];
static bool outline;

void cockpit_set_off(const char *names)
{
    for (int i = 0; i < N_MODULES; i++) {
        size_t n = strlen(modules[i].name);
        for (const char *p = names; *p;) {
            const char *e = strchr(p, ',');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if ((len == 3 && strncmp(p, "all", 3) == 0) || (len == n && strncmp(p, modules[i].name, n) == 0))
                off[i] = true;
            p += len + (e != NULL);
        }
    }
}

void cockpit_set_outline(bool on) { outline = on; }

#define MAX_TEXT 512
#define MAX_NEEDLE 8
static CockpitText texts[MAX_TEXT];
static CockpitNeedle needles[MAX_NEEDLE];
static CockpitFrame frame;
static bool active; /* the panel is on screen this frame */

static bool drawn(int i) { return modules[i].draw && !off[i]; }

void cockpit_begin(const Pc *pc, const uint8_t *clean)
{
    frame = (CockpitFrame){ pc, clean, texts, 0, needles, 0 };
    active = true;
}

bool cockpit_owns(float x, float line)
{
    if (!active)
        return false;
    for (int i = 0; i < N_MODULES; i++) {
        const CockpitRect *r = &modules[i].r;
        if (drawn(i) && x >= (float)r->x0 && x < (float)r->x1 + 1 && line >= (float)r->y0 && line < (float)r->y1 + 1)
            return true;
    }
    return false;
}

void cockpit_add_text(int x, int line, uint8_t ch, uint8_t dim)
{
    if (active && frame.n_text < MAX_TEXT)
        texts[frame.n_text++] = (CockpitText){ x, line, ch, dim };
}

void cockpit_add_needle(uint16_t desc, float hx, float hy, float tx, float ty)
{
    if (active && frame.n_needle < MAX_NEEDLE)
        needles[frame.n_needle++] = (CockpitNeedle){ desc, hx, hy, tx, ty };
}

void cockpit_end(uint8_t *vram)
{
    for (int i = 0; i < N_MODULES; i++) {
        if (!drawn(i))
            continue;
        const CockpitRect *r = &modules[i].r;
        for (int y = r->y0; y <= r->y1; y++) {
            uint8_t *row = vram + (y & 1) * 0x2000 + (y >> 1) * 80;
            for (int x = r->x0; x <= r->x1; x++)
                row[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
}

void cockpit_draw(SDL_Renderer *r)
{
    if (!active)
        return;
    active = false; /* until the next cockpit_begin */
    kit_set_font((const uint8_t (*)[8])frame.pc->font);
    for (int i = 0; i < N_MODULES; i++)
        if (drawn(i))
            modules[i].draw(&frame, &modules[i]);
    if (outline) { /* --module-outline: every module's rect, green when drawn, orange when not done yet */
        static const SDL_FColor done = { 0.2f, 1, 0.3f, 1 }, todo = { 1, 0.55f, 0.1f, 1 };
        for (int i = 0; i < N_MODULES; i++) {
            const CockpitRect *m = &modules[i].r;
            kit_box((float)m->x0, (float)m->y0, (float)m->x1 + 1, (float)m->y1 + 1, 1.0f, drawn(i) ? done : todo);
        }
    }
    kit_flush(r);
}
