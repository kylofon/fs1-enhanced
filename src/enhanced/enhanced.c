/* Enhanced options, see enhanced.h. */
#include "enhanced/enhanced.h"
#include "enhanced/displaylist.h"
#include "enhanced/sharp.h"

#include <SDL3/SDL.h>

EnhOptions enh;

bool enh_parse_arg(int argc, char *argv[], int *i)
{
    (void)argc;
    if (SDL_strcmp(argv[*i], "--start") == 0 && *i + 1 < argc) {
        enh.start_mode = SDL_atoi(argv[++*i]);
        enh.start_set = enh.start_mode >= 1 && enh.start_mode <= 9; /* 0 is the game's own start */
        return true;
    }
    if (SDL_strcmp(argv[*i], "--classic") == 0) {
        enh.classic = true;
        return true;
    }
    if (SDL_strcmp(argv[*i], "--dump-dl") == 0 && *i + 1 < argc) {
        enh.dump_dl = argv[++*i];
        dl_set_dump(enh.dump_dl);
        return true;
    }
    if (SDL_strcmp(argv[*i], "--dl-check") == 0) {
        enh.dl_check = true;
        dl_set_check();
        return true;
    }
    if (SDL_strcmp(argv[*i], "--line-width") == 0 && *i + 1 < argc) {
        sharp_set_line_width((float)SDL_atof(argv[++*i]));
        return true;
    }
    if (SDL_strcmp(argv[*i], "--verify") == 0 || SDL_strcmp(argv[*i], "--native-off") == 0)
        enh.original_code = true; /* not consumed: main handles it */
    return false;
}

static void update_recording(void)
{
    dl_on = !enh.classic && !enh.original_code && (enh.sharp || enh.dump_dl || enh.dl_check);
}

void enh_start(bool rgb)
{
    enh.sharp = !enh.classic && !enh.original_code && !rgb;
    update_recording();
}

bool enh_cycle_monitor(bool *composite)
{
    if (enh.classic || enh.original_code)
        return false;
    if (enh.sharp) {
        enh.sharp = false; /* sharp -> composite */
    } else if (*composite) {
        *composite = false; /* composite -> RGB */
    } else {
        *composite = true; /* RGB -> sharp */
        enh.sharp = true;
    }
    update_recording();
    SDL_Log("Monitor: %s", enh.sharp ? "sharp" : *composite ? "composite" : "RGB");
    return true;
}
