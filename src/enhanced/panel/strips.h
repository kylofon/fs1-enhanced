/* Pieces shared by neighbouring cockpit modules: the rudder track along the bottom of the panel (S2.11/S2.12). */
#ifndef PANEL_STRIPS_H
#define PANEL_STRIPS_H

#include "enhanced/panel/cockpit.h"

/* The part of the rudder track (line 199, x 186..239) and its marker (indicator DS:1449, frame = x - 176 - 3.5)
 * that falls into x0..x1 (panel pixels). The marker is drawn by the module that holds its centre. */
void strips_rudder(const CockpitFrame *f, float x0, float x1);

#endif
