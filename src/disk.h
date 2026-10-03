#ifndef FS1_DISK_H
#define FS1_DISK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum DiskFormat {
    DISK_PC_160K,     /* IBM PC 5.25" SS/DD: 40 tracks x 8 sectors x 512 B (.ima/.img) */
    DISK_APPLE2_140K, /* Apple II 5.25": 35 tracks x 16 sectors x 256 B, DOS 3.3 order (.dsk/.do) */
} DiskFormat;

typedef struct Disk {
    DiskFormat format;
    int tracks;
    int sectors_per_track;
    int sector_size;
    size_t size;
    uint8_t *data;
} Disk;

/* Loads a raw sector image; the format is detected from the file size. */
bool disk_load(Disk *disk, const char *path);
void disk_free(Disk *disk);

/* Sector numbers are 0-based for both formats (BIOS int 13h sector N is index N-1). */
const uint8_t *disk_sector(const Disk *disk, int track, int sector);

#endif
