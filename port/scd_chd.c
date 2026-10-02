/*
port/scd_chd.c — MAME CHD disc images for the Sega CD / MD+ disc layer
(port/scd_disc.c), through libchdr (external/libchdr).

A port of pico-pcePlus's pce-go/cd_chd.c:
  - libchdr reads the file through FatFs callbacks, one persistent FIL;
  - a small LRU of decompressed hunks in PSRAM serves the sector reads (a
    Sega CD CHD hunk is 4 or 8 frames of 2448 bytes: 2352 of sector, 96 of
    subcode);
  - the CHT2/CHTR track metadata becomes the disc layer's track table, with
    pcePlus's pregap arithmetic;
  - CD-DA comes out of chdman big-endian and is swapped to little-endian.

Two differences, both for the two-core use here. Core0 reads data sectors
and core1 streams CD-DA through the same cache and the same libchdr state,
so every read holds the SD lock. And decompressing a hunk -- FLAC for audio
especially -- needs 6-10 KB of stack, more than either core has (core0 3 KB,
core1 4 KB). pcePlus relaunches core1 on an 8 KB SRAM stack; this emulator
has no 8 KB of SRAM to spare during a game, so chd_read() runs on a 16 KB
PSRAM stack instead, through gwcd_port_big_stack_call(). Only one core can
be inside chd_read() at a time (the lock), so one stack serves both.

libchdr's own allocations (the hunk map, ~450 KB for a full disc, and the
codec state) go to PSRAM through external/libchdr_alloc.h.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "scd.h"
#include "scd_disc.h"

#if GWCD_ENABLE_CHD

#include "libchdr/chd.h"
#include "libchdr/cdrom.h"
#include "libchdr/coretypes.h"

#define CHD_CACHE_ENTRIES 4
#define BIG_STACK_BYTES (16 * 1024)

typedef struct {
    uint32_t hunk;
    uint32_t age;
    uint8_t *buf;
    int valid;
} chd_cache_entry_t;

/* All in PSRAM, allocated at open. */
typedef struct {
    FIL fil;
    chd_file *chd;
    uint32_t hunkbytes;
    uint32_t frames_per_hunk;
    uint32_t age_tick;
    chd_cache_entry_t cache[CHD_CACHE_ENTRIES];
    uint32_t read_hunk;   /* argument/result of the big-stack call */
    uint8_t *read_buf;
    chd_error read_err;
} chd_state_t;

static chd_state_t *st;
static uint8_t *big_stack;

/* ---------------------- FatFs <-> libchdr adapter ----------------------- */

static uint64_t cb_fsize(void *fp)
{
    return (uint64_t)f_size((FIL *)fp);
}

static int cb_fclose(void *fp)
{
    (void)fp;
    return 0; /* the FIL is ours */
}

static size_t cb_fread(void *buf, size_t sz, size_t cnt, void *fp)
{
    UINT br = 0;
    if (sz == 0 || cnt == 0)
        return 0;
    if (f_read((FIL *)fp, buf, (UINT)(sz * cnt), &br) != FR_OK)
        return 0;
    return br / sz;
}

static int cb_fseek(void *fp, int64_t off, int whence)
{
    FIL *f = (FIL *)fp;
    FSIZE_t target;
    switch (whence) {
    case SEEK_SET: target = (FSIZE_t)off; break;
    case SEEK_CUR: target = f_tell(f) + (FSIZE_t)off; break;
    case SEEK_END: target = f_size(f) + (FSIZE_t)off; break;
    default: return -1;
    }
    return f_lseek(f, target) == FR_OK ? 0 : -1;
}

static const core_file_callbacks fatfs_cb = {
    cb_fsize, cb_fread, cb_fclose, cb_fseek,
};

/* ------------------------------ hunks ---------------------------------- */

static void do_chd_read(void *arg)
{
    (void)arg;
    st->read_err = chd_read(st->chd, st->read_hunk, st->read_buf);
}

/* Look up or fetch a hunk; the caller holds the SD lock. */
static uint8_t *cache_get(uint32_t hunk)
{
    int victim = 0;
    uint32_t victim_age = UINT32_MAX;

    for (int i = 0; i < CHD_CACHE_ENTRIES; i++) {
        if (st->cache[i].valid && st->cache[i].hunk == hunk) {
            st->cache[i].age = ++st->age_tick;
            return st->cache[i].buf;
        }
    }
    for (int i = 0; i < CHD_CACHE_ENTRIES; i++) {
        uint32_t age = st->cache[i].valid ? st->cache[i].age : 0;
        if (age < victim_age) {
            victim = i;
            victim_age = age;
        }
    }
    st->read_hunk = hunk;
    st->read_buf = st->cache[victim].buf;
    gwcd_port_big_stack_call(do_chd_read, NULL, big_stack, BIG_STACK_BYTES);
    if (st->read_err != CHDERR_NONE) {
        printf("chd: hunk %lu: %s\n", (unsigned long)hunk, chd_error_string(st->read_err));
        st->cache[victim].valid = 0;
        return NULL;
    }
    st->cache[victim].valid = 1;
    st->cache[victim].hunk = hunk;
    st->cache[victim].age = ++st->age_tick;
    return st->cache[victim].buf;
}

int gwcd_chd_read_raw(uint32_t frame, int audio, uint8_t *dst)
{
    uint8_t *hunk;
    int rc = -1;

    if (!st)
        return -1;
    /* audio is core1's CD-DA prefetch, which never waits for core0 */
    if (!audio)
        gwcd_port_sd_lock();
    else if (!gwcd_port_sd_trylock())
        return -2;
    hunk = cache_get(frame / st->frames_per_hunk);
    if (hunk) {
        memcpy(dst, hunk + (size_t)(frame % st->frames_per_hunk) * CD_FRAME_SIZE,
               GWCD_RAW_SECTOR);
        rc = 0;
    }
    gwcd_port_sd_unlock();
    if (rc == 0 && audio) {
        /* chdman keeps CD-DA big-endian (MAME's convention) */
        for (int i = 0; i < GWCD_RAW_SECTOR; i += 2) {
            uint8_t t = dst[i];
            dst[i] = dst[i + 1];
            dst[i + 1] = t;
        }
    }
    return rc;
}

/* --------------------------- track metadata ---------------------------- */

static int track_type(const char *s, uint8_t *audio, uint8_t *raw)
{
    *audio = 0;
    *raw = 1;
    if (!strcmp(s, "AUDIO")) {
        *audio = 1;
        return 0;
    }
    if (!strcmp(s, "MODE1") || !strcmp(s, "MODE2_FORM1")) {
        *raw = 0; /* 2048 bytes of user data at the start of the frame */
        return 0;
    }
    if (!strcmp(s, "MODE1_RAW") || !strcmp(s, "MODE2") || !strcmp(s, "MODE2_FORM2") ||
        !strcmp(s, "MODE2_RAW") || !strcmp(s, "MODE2_FORM_MIX"))
        return 0;
    return -1;
}

/* pcePlus load_tracks_from_metadata(): CHT2's FRAMES includes the pregap;
   tracks are padded to CD_TRACK_PADDING frames in the file. */
static int load_tracks(disc_track_t *tracks, int *ntracks, uint32_t *total_lba)
{
    uint32_t running = 0, cursor = 0;
    int n = 0;

    for (uint32_t i = 0; i < GWCD_DISC_MAX_TRACKS; i++) {
        char meta[256], type[32] = {0}, subtype[32] = {0}, pgtype[32] = {0}, pgsub[32] = {0};
        uint32_t len = 0, tag = 0;
        uint8_t flags = 0;
        int trk = 0, frames = 0, pregap = 0, postgap = 0;
        int cht2 = chd_get_metadata(st->chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof(meta),
                                    &len, &tag, &flags) == CHDERR_NONE;
        disc_track_t *t;

        if (!cht2 && chd_get_metadata(st->chd, CDROM_TRACK_METADATA_TAG, i, meta, sizeof(meta),
                                      &len, &tag, &flags) != CHDERR_NONE)
            break;
        meta[sizeof(meta) - 1] = 0;
        if (cht2) {
            if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &trk, type, subtype, &frames,
                       &pregap, pgtype, pgsub, &postgap) < 4)
                continue;
        } else if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &trk, type, subtype, &frames) < 4) {
            continue;
        }
        t = &tracks[n++];
        memset(t, 0, sizeof(*t));
        t->number = (uint8_t)trk;
        if (track_type(type, &t->audio, &t->raw) != 0)
            printf("chd: track %d: unknown type %s, taking it as MODE1_RAW\n", trk, type);
        t->pregap = (uint32_t)pregap;
        t->chd_frame = cursor + (uint32_t)pregap;
        t->lba_start = running + (uint32_t)pregap;
        t->lba_end = t->lba_start + (uint32_t)(frames - pregap);
        running = t->lba_end;
        cursor += ((uint32_t)frames + CD_TRACK_PADDING - 1) & ~(uint32_t)(CD_TRACK_PADDING - 1);
    }
    *ntracks = n;
    *total_lba = running;
    return n ? 0 : -1;
}

/* ---------------------------- open / close ----------------------------- */

void gwcd_chd_close(void)
{
    if (!st)
        return;
    gwcd_port_sd_lock();
    if (st->chd)
        chd_close(st->chd);
    f_close(&st->fil);
    gwcd_port_sd_unlock();
    for (int i = 0; i < CHD_CACHE_ENTRIES; i++)
        gwcd_port_psram_free(st->cache[i].buf);
    gwcd_port_psram_free(st);
    st = NULL;
    gwcd_port_psram_free(big_stack);
    big_stack = NULL;
}

int gwcd_chd_open(const char *path, disc_track_t *tracks, int *ntracks, uint32_t *total_lba)
{
    const chd_header *h;
    chd_error err;

    gwcd_chd_close();
    st = gwcd_port_psram_alloc(sizeof(*st));
    big_stack = gwcd_port_psram_alloc(BIG_STACK_BYTES);
    if (!st || !big_stack) {
        gwcd_chd_close();
        return -1;
    }
    if (f_open(&st->fil, path, FA_READ) != FR_OK) {
        printf("chd: cannot open %s\n", path);
        gwcd_port_psram_free(st);
        st = NULL;
        gwcd_chd_close();
        return -1;
    }
    err = chd_open_core_file_callbacks(&fatfs_cb, &st->fil, CHD_OPEN_READ, NULL, &st->chd);
    if (err != CHDERR_NONE) {
        printf("chd: %s: %s\n", path, chd_error_string(err));
        st->chd = NULL;
        gwcd_chd_close();
        return -1;
    }
    h = chd_get_header(st->chd);
    if (!h || h->hunkbytes % CD_FRAME_SIZE) {
        printf("chd: %s is not a CD image\n", path);
        gwcd_chd_close();
        return -1;
    }
    st->hunkbytes = h->hunkbytes;
    st->frames_per_hunk = h->hunkbytes / CD_FRAME_SIZE;
    for (int i = 0; i < CHD_CACHE_ENTRIES; i++) {
        st->cache[i].buf = gwcd_port_psram_alloc(st->hunkbytes);
        if (!st->cache[i].buf) {
            gwcd_chd_close();
            return -1;
        }
    }
    printf("chd: v%lu, %lu hunks of %lu bytes\n", (unsigned long)h->version,
           (unsigned long)h->totalhunks, (unsigned long)h->hunkbytes);
    if (load_tracks(tracks, ntracks, total_lba) != 0) {
        printf("chd: no CD tracks in %s\n", path);
        gwcd_chd_close();
        return -1;
    }
    return 0;
}

#endif /* GWCD_ENABLE_CHD */
