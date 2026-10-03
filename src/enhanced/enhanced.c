/* Enhanced options, see enhanced.h. */
#include "enhanced/enhanced.h"

#include <SDL3/SDL.h>

EnhOptions enh;

bool enh_parse_arg(int argc, char *argv[], int *i)
{
    (void)argc;
    if (SDL_strcmp(argv[*i], "--classic") == 0) {
        enh.classic = true;
        return true;
    }
    return false;
}
