/* Enhanced options (PLAN.md). Every enhancement reads its switch here; --classic turns them all off
 * and the engine then runs exactly as the faithful port v0.1.0. */
#ifndef ENHANCED_H
#define ENHANCED_H

#include <stdbool.h>

typedef struct {
    bool classic;   /* --classic: the original picture and behaviour, every enhancement off */
} EnhOptions;

extern EnhOptions enh;

/* Consumes argv[*i] (and its value) when it is an enhanced option; returns false otherwise. */
bool enh_parse_arg(int argc, char *argv[], int *i);

#endif
