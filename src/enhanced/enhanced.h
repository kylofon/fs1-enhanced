/* Enhanced options (PLAN.md). Every enhancement reads its switch here; --classic turns them all off
 * and the engine then runs exactly as the faithful port v0.1.0. */
#ifndef ENHANCED_H
#define ENHANCED_H

#include <stdbool.h>

typedef struct {
    bool classic;   /* --classic: the original picture and behaviour, every enhancement off */
    const char *dump_dl; /* --dump-dl FILE: write the recorded 3D view frames as text (S1.2) */
    bool dl_check;       /* --dl-check: list back-buffer writers without a display-list hook (S1.2) */
    bool sharp;          /* sharp 3D view (S1.3): default on; F11 cycles sharp / composite / RGB */
    bool start_set;      /* --start N: once the menus are over, recall user mode N (0..9) in the editor */
    int start_mode;
    bool original_code;  /* --verify / --native-off: original code runs, nothing is recorded */
} EnhOptions;

extern EnhOptions enh;

/* Consumes argv[*i] (and its value) when it is an enhanced option; returns false otherwise. */
bool enh_parse_arg(int argc, char *argv[], int *i);

/* After the command line: settles sharp / recording from --classic, --rgb and the original-code options. */
void enh_start(bool rgb);
/* F11. Returns true when handled (sharp -> composite -> RGB -> sharp); false under --classic. */
bool enh_cycle_monitor(bool *composite);

#endif
