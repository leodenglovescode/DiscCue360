#ifndef DISCCUE_DRIVE_H
#define DISCCUE_DRIVE_H

#include "toc.h"
#include <xsdk/kernel.h>

enum {
    CD_SECTOR_BYTES = 2352,
    CD_SECTORS_PER_SECOND = 75,
    CD_SAMPLES_PER_SECTOR = 588,
    CD_SECTORS_PER_READ = 8,
    TRAY_READY = 0,
    TRAY_OPEN = 2,
    TRAY_UNKNOWN = 255,
};

NTSTATUS open_optical(uint32_t *device);
bool read_disc(DiscToc *disc, char *diagnostic, size_t capacity);
bool read_audio_sectors(uint32_t device, uint32_t lba, uint8_t sectors, uint8_t *data);
uint32_t drive_tray_state(void);
bool drive_set_tray(bool open);

#endif
