/*
port/scd_disc.h — internals shared by the disc layer (port/scd_disc.c), its
CHD backend (port/scd_chd.c) and the CD audio player (port/scd_audio.c).
The public side is in port/scd.h.
*/
#ifndef GWENESIS_PORT_SCD_DISC_H
#define GWENESIS_PORT_SCD_DISC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GWCD_ENABLE_CHD
#define GWCD_ENABLE_CHD 0
#endif

#define GWCD_RAW_SECTOR 2352
#define GWCD_DATA_SECTOR 2048
#define GWCD_DISC_MAX_TRACKS 99
#define GWCD_DISC_NAME_MAX 96

typedef struct {
    uint8_t number;       /* track number in the cue */
    uint8_t audio;        /* 1 = CD-DA */
    uint8_t raw;          /* 1 = 2352-byte sectors in the file, 0 = 2048 */
    uint8_t pad;
    int loop;             /* MD+: REM LOOP (1) / REM NOLOOP (-1) */
    int loop_lba;
    uint32_t file_lba;    /* INDEX 01 within its file (parse pass only) */
    uint32_t pregap;      /* PREGAP directive, sectors */
    uint32_t lba_start;   /* disc LBAs [start, end) */
    uint32_t lba_end;
    uint64_t file_offset; /* byte offset of lba_start in its file */
    uint32_t chd_frame;   /* CHD: frame of lba_start */
    char name[GWCD_DISC_NAME_MAX]; /* file, relative to the cue */
} disc_track_t;

/* A 2352-byte CD-DA sector (little-endian 16-bit stereo, 588 frames) at a
   disc LBA; silence in a gap or on a data track. 0 on success, -2 when the
   SD card is busy (core1 never waits for it: try again), other values on
   error. Takes the SD lock itself. */
int gwcd_disc_read_audio(int lba, uint8_t *dst);

#if GWCD_ENABLE_CHD
int gwcd_chd_open(const char *path, disc_track_t *tracks, int *ntracks, uint32_t *total_lba);
/* One raw 2352-byte frame; `audio` byte-swaps chdman's big-endian CD-DA. */
int gwcd_chd_read_raw(uint32_t frame, int audio, uint8_t *dst);
void gwcd_chd_close(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GWENESIS_PORT_SCD_DISC_H */
