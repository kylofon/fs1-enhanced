/* Attitude indicator module (S2.7), see attitude.c. */
#ifndef PANEL_ATTITUDE_H
#define PANEL_ATTITUDE_H

#include "enhanced/panel/cockpit.h"

void attitude_draw(const CockpitFrame *f, const CockpitModule *m);

#endif
