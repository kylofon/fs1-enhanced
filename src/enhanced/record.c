/* Video recording, see record.h. */
#include "enhanced/record.h"

#include <stdio.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

static const char *rec_path;
static long rec_from;
static FILE *rec_pipe;
static int rec_w, rec_h;
static long rec_frames;

void record_set(const char *path)
{
    rec_path = path;
}

void record_set_from(long frame)
{
    rec_from = frame;
}

static bool record_open(int w, int h)
{
    char cmd[1024];
    SDL_snprintf(cmd, sizeof cmd,
                 "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s %dx%d -r 60 -i - "
                 "-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p -movflags +faststart \"%s\"",
                 SDL_min(w, 8192), h, rec_path);
    rec_pipe = popen(cmd, "wb");
    if (!rec_pipe) {
        SDL_Log("--record: cannot start ffmpeg");
        rec_path = NULL;
        return false;
    }
    rec_w = SDL_min(w, 8192), rec_h = h;
    SDL_Log("Recording %dx%d to %s", w, h, rec_path);
    return true;
}

void record_frame(SDL_Renderer *renderer, long frame)
{
    if (!rec_path || frame < rec_from)
        return;
    SDL_Surface *s = SDL_RenderReadPixels(renderer, NULL);
    SDL_Surface *rgb = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24) : NULL;
    SDL_DestroySurface(s);
    if (!rgb)
        return;
    if (!rec_pipe && !record_open(rgb->w & ~1, rgb->h & ~1)) { /* yuv420p needs even sizes */
        SDL_DestroySurface(rgb);
        return;
    }
    for (int y = 0; y < rec_h; y++) { /* a resized window is cropped or padded to the first size */
        static uint8_t row[3 * 8192];
        int n = SDL_min(rgb->w, rec_w) * 3;
        SDL_memset(row, 0, (size_t)rec_w * 3);
        if (y < rgb->h)
            SDL_memcpy(row, (const uint8_t *)rgb->pixels + y * rgb->pitch, (size_t)n);
        fwrite(row, 1, (size_t)rec_w * 3, rec_pipe);
    }
    SDL_DestroySurface(rgb);
    rec_frames++;
}

void record_close(void)
{
    if (!rec_pipe)
        return;
    pclose(rec_pipe);
    rec_pipe = NULL;
    SDL_Log("Recorded %ld frames (%.1f s) to %s", rec_frames, rec_frames / 60.0, rec_path);
}
