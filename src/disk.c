#include "disk.h"

#include <SDL3/SDL.h>

bool disk_load(Disk *disk, const char *path)
{
    size_t size = 0;
    void *buf = SDL_LoadFile(path, &size);
    if (!buf) {
        SDL_Log("Cannot read disk image '%s': %s", path, SDL_GetError());
        return false;
    }

    switch (size) {
    case 40 * 8 * 512:
        *disk = (Disk){ DISK_PC_160K, 40, 8, 512, size, buf };
        return true;
    case 35 * 16 * 256:
        *disk = (Disk){ DISK_APPLE2_140K, 35, 16, 256, size, buf };
        return true;
    default:
        SDL_Log("'%s' is %u bytes; expected a 160K PC image or a 140K Apple II image", path, (unsigned)size);
        SDL_free(buf);
        return false;
    }
}

void disk_free(Disk *disk)
{
    SDL_free(disk->data);
    disk->data = NULL;
}

const uint8_t *disk_sector(const Disk *disk, int track, int sector)
{
    if (track < 0 || track >= disk->tracks || sector < 0 || sector >= disk->sectors_per_track)
        return NULL;
    return disk->data + ((size_t)track * disk->sectors_per_track + sector) * disk->sector_size;
}
