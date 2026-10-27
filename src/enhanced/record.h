/* --record FILE: the window picture of every frame piped to ffmpeg (60 fps, H.264). A side channel: the
 * emulation is not touched. --record-from N starts at frame N (e.g. after the --start menus). */
#ifndef RECORD_H
#define RECORD_H

#include <SDL3/SDL.h>

void record_set(const char *path);
void record_set_from(long frame);
/* Called with the finished frame in the back buffer, before SDL_RenderPresent. */
void record_frame(SDL_Renderer *renderer, long frame);
void record_close(void);

#endif
