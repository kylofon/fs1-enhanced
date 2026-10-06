/* The modules of the right-hand panel (S2.14-S2.18), see right.c. */
#ifndef PANEL_RIGHT_H
#define PANEL_RIGHT_H

#include "enhanced/panel/cockpit.h"

void throttle_draw(const CockpitFrame *f, const CockpitModule *m); /* S2.14 flaps / trim / throttle strips */
void compass_draw(const CockpitFrame *f, const CockpitModule *m);  /* S2.15 COMPASS digits + O M I lights */
void fuel_draw(const CockpitFrame *f, const CockpitModule *m);     /* S2.16 FUEL, OIL T, OIL P */
void status_draw(const CockpitFrame *f, const CockpitModule *m);   /* S2.17 GEAR / LIGHTS / MAGS / CARB H */
void radio_draw(const CockpitFrame *f, const CockpitModule *m);    /* S2.18 COM, NAV, TRANSPONDER, TIME */

#endif
