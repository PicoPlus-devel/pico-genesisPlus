/*
port/scd_disc.c — the disc layer for Sega CD and MD+: cue/bin and CHD images
read straight from the SD card with FatFs.

The cue parser and the LBA-to-file mapping are pico-pcePlus's
(pce-go/cd.c: cd_load_cue, probe_bin_file, read_sector), which already
handles what real dumps throw at it: Redump's one FILE per track, one FILE
for every track, WAV audio files, INDEX 00 pregaps inside the file, PREGAP
directives, MODE1/2048 and 2352 data. PicoDrive's MegaSD cue extensions
(REM LOOP / REM NOLOOP, for MD+) are parsed on top.

The result feeds PicoDrive's drive (scd/cd/cdd.c): cdd.toc gets every
track's first and last LBA and type, and the drive reads through
gwcd_disc_read_data() (2048-byte data sectors, core0, from the CDC's 75 Hz
event) and gwcd_disc_read_audio() (2352-byte CD-DA sectors, from the CD-DA
prefetch, core1 on HSTX). Each side has its own file handle, so neither
seeks the other's file; the SD lock serialises the FatFs calls themselves.

All tables and both file handles live in PSRAM (~16 KB), allocated at open,
and so does the scratch the cue parser needs while it runs: a card read runs
three calls deep into FatFs on a 3 KB stack, and SRAM is what this emulator
is short of, so none of it is static or on the stack.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "pico_int.h"
#include "cd/genplus_macros.h"
#include "cd/cdd.h"
#include "scd_disc.h"

#define RAW_SECTOR GWCD_RAW_SECTOR
#define DATA_SECTOR GWCD_DATA_SECTOR
#define NAME_MAX_ GWCD_DISC_NAME_MAX

/* gwcd_cdd (the drive state, scd/cd/cdd.c) is allocated here: its TOC is the
   disc's, and it lives as long as the disc is open. */

typedef struct {
    int ntracks;
    int is_chd;
    uint32_t total_lba;
    char image[FF_MAX_LFN + 2];    /* the .cue or .chd in the drive */
    char dir[FF_MAX_LFN + 2];      /* its folder, trailing '/' */
    disc_track_t tracks[GWCD_DISC_MAX_TRACKS];

    /* One open file per reader, and each reader's path buffer. */
    FIL data_fil, audio_fil;
    char path[2][FF_MAX_LFN + NAME_MAX_ + 4];
#if GWCD_ENABLE_CHD
    uint8_t chd_sector[RAW_SECTOR]; /* core0: a raw frame, before the header is cut */
#endif
    int data_file_track, audio_file_track; /* track whose file is open, -1 none */
    uint64_t data_pos, audio_pos;          /* where the next read starts */
    int data_hint, audio_hint;             /* last track found, for find_track */
} disc_t;

static disc_t *disc;
static int region;

/* Scratch for gwcd_disc_open(), PSRAM, freed when it returns. */
typedef struct {
    FIL f;
    char path[FF_MAX_LFN + NAME_MAX_ + 4];
    char line[320];
    char cur[NAME_MAX_];
    uint8_t sector[DATA_SECTOR];
} open_scratch_t;

static open_scratch_t *scratch;

/* ------------------------------- cue ----------------------------------- */

/* Hand-rolled instead of sscanf(): newlib's scanf brings its locale and
   floating-point machinery along, 4 KB of flash and 364 bytes of SRAM for
   parsing a few integers. */
static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *ltrim(char *s)
{
    while (*s && is_ws(*s))
        s++;
    return s;
}

/* Unsigned decimal at *pp (after blanks); advances *pp. -1 if none. */
static int parse_uint(const char **pp, int *out)
{
    const char *p = *pp;
    int v = 0, n = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p >= '0' && *p <= '9' && n < 9) {
        v = v * 10 + (*p++ - '0');
        n++;
    }
    if (!n)
        return -1;
    *pp = p;
    *out = v;
    return 0;
}

/* The next blank-delimited word at *pp into out; advances *pp. */
static int parse_word(const char **pp, char *out, size_t size)
{
    const char *p = *pp;
    size_t n = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    while (*p && !is_ws(*p) && n + 1 < size)
        out[n++] = *p++;
    out[n] = 0;
    *pp = p;
    return n ? 0 : -1;
}

static int parse_msf(const char *s, uint32_t *out)
{
    int m, sec, f;
    if (parse_uint(&s, &m) || *s++ != ':' || parse_uint(&s, &sec) ||
        *s++ != ':' || parse_uint(&s, &f))
        return -1;
    if (sec >= 60 || f >= 75)
        return -1;
    *out = (uint32_t)(m * 60 * 75 + sec * 75 + f);
    return 0;
}

static int extract_filename(const char *line, char *out, size_t size)
{
    const char *p = strchr(line, '"');
    const char *q;
    size_t n;

    if (!p) {
        /* unquoted: FILE name.bin BINARY */
        p = line + 4;
        while (*p == ' ' || *p == '\t')
            p++;
        q = p;
        while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n')
            q++;
    } else {
        p++;
        q = strchr(p, '"');
        if (!q)
            return -1;
    }
    n = (size_t)(q - p);
    if (n == 0 || n + 1 > size)
        return -1;
    memcpy(out, p, n);
    out[n] = '\0';
    return 0;
}

/* Size of a track file and, for a RIFF/WAVE file, the offset of its PCM
   payload (pcePlus probe_bin_file). */
static int probe_file(const char *name, FSIZE_t *size, uint32_t *wav_off)
{
    FIL *fp = &scratch->f;
    char *path = scratch->path;
    uint8_t hdr[12];
    UINT br = 0;

    snprintf(path, sizeof(scratch->path), "%s%s", disc->dir, name);
    if (f_open(fp, path, FA_READ) != FR_OK)
        return -1;
    *size = f_size(fp);
    *wav_off = 0;
    if (*size >= 12 && f_read(fp, hdr, 12, &br) == FR_OK && br == 12 &&
        !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
        uint32_t pos = 12;
        for (int i = 0; i < 32 && pos + 8 <= *size; i++) {
            uint8_t ck[8];
            if (f_lseek(fp, pos) != FR_OK || f_read(fp, ck, 8, &br) != FR_OK || br != 8)
                break;
            uint32_t len = ck[4] | (ck[5] << 8) | (ck[6] << 16) | ((uint32_t)ck[7] << 24);
            if (!memcmp(ck, "data", 4)) {
                *wav_off = pos + 8;
                break;
            }
            pos += 8 + len + (len & 1);
        }
    }
    f_close(fp);
    return 0;
}

static int load_cue(const char *cue_path)
{
    FIL *cuep = &scratch->f;
    char *line = scratch->line;
    char *cur = scratch->cur;
    int current = -1;

    if (f_open(cuep, cue_path, FA_READ) != FR_OK) {
        printf("disc: cannot open %s\n", cue_path);
        return -1;
    }
    cur[0] = 0;
    while (f_gets(line, sizeof(scratch->line), cuep)) {
        char *p = ltrim(line);

        if (!strncasecmp(p, "FILE", 4)) {
            if (extract_filename(p, cur, sizeof(scratch->cur)) != 0) {
                printf("disc: bad FILE line: %s", line);
                f_close(cuep);
                return -1;
            }
        } else if (!strncasecmp(p, "TRACK", 5)) {
            int no;
            char type[32] = {0};
            const char *q = p + 5;
            disc_track_t *t;
            if (parse_uint(&q, &no) || parse_word(&q, type, sizeof(type)))
                continue;
            if (disc->ntracks >= GWCD_DISC_MAX_TRACKS) {
                printf("disc: more than %d tracks\n", GWCD_DISC_MAX_TRACKS);
                break;
            }
            if (!cur[0]) {
                printf("disc: TRACK before FILE\n");
                f_close(cuep);
                return -1;
            }
            current = disc->ntracks++;
            t = &disc->tracks[current];
            memset(t, 0, sizeof(*t));
            t->number = (uint8_t)no;
            strncpy(t->name, cur, sizeof(t->name) - 1);
            if (!strcasecmp(type, "AUDIO")) {
                t->audio = 1;
                t->raw = 1;
            } else if (!strcasecmp(type, "MODE1/2048")) {
                t->raw = 0;
            } else {
                /* MODE1/2352, MODE2/2352 and anything unknown */
                t->raw = 1;
            }
        } else if (!strncasecmp(p, "INDEX", 5)) {
            int idx;
            char msf[32] = {0};
            const char *q = p + 5;
            uint32_t lba;
            if (current < 0 || parse_uint(&q, &idx) || parse_word(&q, msf, sizeof(msf)) || idx != 1)
                continue;
            if (parse_msf(msf, &lba) == 0)
                disc->tracks[current].file_lba = lba;
        } else if (!strncasecmp(p, "PREGAP", 6)) {
            char msf[32] = {0};
            const char *q = p + 6;
            uint32_t lba;
            if (current >= 0 && parse_word(&q, msf, sizeof(msf)) == 0 && parse_msf(msf, &lba) == 0)
                disc->tracks[current].pregap = lba;
        } else if (!strncasecmp(p, "REM NOLOOP", 10)) {
            /* MegaSD extension (PicoDrive cd_parse.c) */
            if (current >= 0)
                disc->tracks[current].loop = -1;
        } else if (!strncasecmp(p, "REM LOOP", 8)) {
            int lba = 0;
            const char *q = p + 8;
            if (current >= 0) {
                if (parse_uint(&q, &lba))
                    lba = 0;
                disc->tracks[current].loop = 1;
                disc->tracks[current].loop_lba = lba;
            }
        }
    }
    f_close(cuep);
    if (disc->ntracks == 0) {
        printf("disc: no tracks in %s\n", cue_path);
        return -1;
    }

    /* Disc LBAs, as pcePlus computes them: a new file starts where the
       previous track ended, plus any PREGAP directive, plus its own INDEX 01
       offset (the pregap stored in the file, which becomes a gap between
       tracks); tracks sharing a file follow their INDEX 01 times. */
    uint32_t running = 0;
    for (int i = 0; i < disc->ntracks; i++) {
        disc_track_t *t = &disc->tracks[i];
        uint32_t sec = t->raw ? RAW_SECTOR : DATA_SECTOR;
        FSIZE_t size = 0;
        uint32_t wav = 0;
        int have = probe_file(t->name, &size, &wav) == 0;
        int same_prev = i > 0 && !strcmp(t->name, disc->tracks[i - 1].name);
        int same_next = i + 1 < disc->ntracks && !strcmp(t->name, disc->tracks[i + 1].name);
        uint32_t len = 0;

        t->lba_start = running + t->pregap + (same_prev ? 0 : t->file_lba);
        t->file_offset = (uint64_t)wav + (uint64_t)t->file_lba * sec;
        if (same_next) {
            uint32_t nf = disc->tracks[i + 1].file_lba;
            if (nf >= t->file_lba)
                len = nf - t->file_lba;
        } else if (have && size > t->file_offset) {
            len = (uint32_t)((size - t->file_offset) / sec);
        } else {
            printf("disc: cannot read %s%s\n", disc->dir, t->name);
            return -1;
        }
        t->lba_end = t->lba_start + len;
        running = t->lba_end;
    }
    disc->total_lba = running;
    return 0;
}

/* ----------------------------- sectors --------------------------------- */

static int find_track(uint32_t lba, int *hint)
{
    int h = *hint;
    if (h >= 0 && h < disc->ntracks &&
        lba >= disc->tracks[h].lba_start && lba < disc->tracks[h].lba_end)
        return h;
    for (int i = 0; i < disc->ntracks; i++) {
        if (lba >= disc->tracks[i].lba_start && lba < disc->tracks[i].lba_end) {
            *hint = i;
            return i;
        }
    }
    return -1;
}

/* Read `len` bytes at `off` of track `ti`'s file through a reader's own
   handle; reopen only when the file changes and seek only when not already
   there (FatFs seeks backwards from the start of the cluster chain). */
static int read_file(FIL *fil, int *file_track, uint64_t *pos, int ti,
                     uint64_t off, void *dst, UINT len, int try_lock)
{
    disc_track_t *t = &disc->tracks[ti];
    UINT br = 0;
    int ok;

    if (!try_lock)
        gwcd_port_sd_lock();
    else if (!gwcd_port_sd_trylock())
        return -2; /* busy: the caller retries */
    if (*file_track < 0 || strcmp(disc->tracks[*file_track].name, t->name) != 0) {
        char *p = disc->path[fil == &disc->audio_fil];
        if (*file_track >= 0)
            f_close(fil);
        *file_track = -1;
        snprintf(p, sizeof(disc->path[0]), "%s%s", disc->dir, t->name);
        if (f_open(fil, p, FA_READ) != FR_OK) {
            gwcd_port_sd_unlock();
            printf("disc: cannot open %s\n", p);
            return -1;
        }
        *file_track = ti;
        *pos = 0;
    }
    ok = 1;
    if (*pos != off)
        ok = f_lseek(fil, off) == FR_OK;
    if (ok)
        ok = f_read(fil, dst, len, &br) == FR_OK && br == len;
    *pos = ok ? off + len : (uint64_t)-1;
    gwcd_port_sd_unlock();
    return ok ? 0 : -1;
}

void gwcd_disc_read_data(int lba, uint8 *dst)
{
    int ti;
    disc_track_t *t;

    if (!disc || lba < 0)
        return;
    ti = find_track((uint32_t)lba, &disc->data_hint);
    if (ti < 0) {
        memset(dst, 0, DATA_SECTOR);
        return;
    }
    t = &disc->tracks[ti];
#if GWCD_ENABLE_CHD
    if (disc->is_chd) {
        uint8_t *raw = disc->chd_sector;
        if (gwcd_chd_read_raw(t->chd_frame + ((uint32_t)lba - t->lba_start), 0, raw) == 0)
            memcpy(dst, t->raw ? raw + 16 : raw, DATA_SECTOR);
        else
            memset(dst, 0, DATA_SECTOR);
        return;
    }
#endif
    {
        uint32_t sec = t->raw ? RAW_SECTOR : DATA_SECTOR;
        uint64_t off = t->file_offset + (uint64_t)((uint32_t)lba - t->lba_start) * sec +
                       (t->raw ? 16 : 0);
        if (read_file(&disc->data_fil, &disc->data_file_track, &disc->data_pos,
                      ti, off, dst, DATA_SECTOR, 0) != 0)
            memset(dst, 0, DATA_SECTOR);
    }
}

static int read_audio(int lba, uint8_t *dst)
{
    int ti;
    disc_track_t *t;

    if (!disc || lba < 0)
        return -1;
    ti = find_track((uint32_t)lba, &disc->audio_hint);
    if (ti < 0 || !disc->tracks[ti].audio) {
        /* a gap between tracks, or a data track: silence */
        memset(dst, 0, RAW_SECTOR);
        return 0;
    }
    t = &disc->tracks[ti];
#if GWCD_ENABLE_CHD
    if (disc->is_chd)
        return gwcd_chd_read_raw(t->chd_frame + ((uint32_t)lba - t->lba_start), 1, dst);
#endif
    return read_file(&disc->audio_fil, &disc->audio_file_track, &disc->audio_pos, ti,
                     t->file_offset + (uint64_t)((uint32_t)lba - t->lba_start) * RAW_SECTOR,
                     dst, RAW_SECTOR, 1);
}

/* The audio reader runs on core1 (HSTX): it never waits for core0, and it
   holds the SD lock from the track lookup on, because a disc change
   (core0, under the same lock) replaces the track table. The lock is
   recursive, so the file and CHD reads below take it again. */
int gwcd_disc_read_audio(int lba, uint8_t *dst)
{
    int rc;

    if (!gwcd_port_sd_trylock())
        return -2;
    rc = read_audio(lba, dst);
    gwcd_port_sd_unlock();
    return rc;
}

/* --------------------------- open / close ------------------------------ */

/* The disc's region, from the security code at the end of its boot sector
   (Genesis Plus GX, loadrom.c): the three regions' codes differ at $20B.
   0 when sector 0 is not a Sega CD boot sector. */
int gwcd_disc_sector0_region(const uint8_t *s)
{
    if (memcmp(s, "SEGADISCSYSTEM", 14) != 0)
        return 0;
    switch (s[0x20B]) {
    case 0x64: return 'E';
    case 0xA1: return 'J';
    default:   return 'U';
    }
}

/* Fill the drive's TOC from the track table (PicoDrive's cdd_load()),
   leaving the rest of the drive state alone. */
static void load_toc(void)
{
    int i;

    memset(&cdd.toc, 0, sizeof(cdd.toc));
    for (i = 0; i < disc->ntracks && i < CD_MAX_TRACKS - 1; i++) {
        track_t *ct = &cdd.toc.tracks[i];
        disc_track_t *t = &disc->tracks[i];
        ct->type = t->audio ? CT_RAW : (t->raw ? CT_BIN : CT_ISO);
        ct->start = (int)t->lba_start;
        ct->end = (int)t->lba_end;
        ct->loop = t->loop;
        ct->loop_lba = t->loop_lba;
    }
    cdd.toc.last = i;
    cdd.toc.end = (int)disc->total_lba;
    cdd.toc.tracks[cdd.toc.last].start = cdd.toc.end; /* lead-out */
    cdd.sectorSize = disc->ntracks && !disc->tracks[0].raw ? DATA_SECTOR : RAW_SECTOR;
    cdd.loaded = disc->ntracks > 0;
}

/* Read the image at `path` into the (allocated, closed) disc: the track
   table, or the CHD. Needs `scratch`. */
static int load_image(const char *path)
{
    size_t n = strlen(path);
    char *slash;

    disc->ntracks = 0;
    disc->is_chd = 0;
    disc->total_lba = 0;
    memset(disc->tracks, 0, sizeof(disc->tracks));
    disc->data_file_track = disc->audio_file_track = -1;
    disc->data_pos = disc->audio_pos = 0;
    disc->data_hint = disc->audio_hint = 0;
    strncpy(disc->image, path, sizeof(disc->image) - 1);
    disc->image[sizeof(disc->image) - 1] = 0;
    strcpy(disc->dir, disc->image);
    slash = strrchr(disc->dir, '/');
    if (slash)
        slash[1] = 0;
    else
        disc->dir[0] = 0;

    if (n >= 4 && !strcasecmp(path + n - 4, ".chd")) {
#if GWCD_ENABLE_CHD
        if (gwcd_chd_open(path, disc->tracks, &disc->ntracks, &disc->total_lba) != 0) {
            disc->ntracks = 0;
            return -1;
        }
        disc->is_chd = 1;
        return 0;
#else
        printf("disc: CHD support not built in\n");
        return -1;
#endif
    }
    if (load_cue(path) != 0) {
        disc->ntracks = 0;
        return -1;
    }
    return 0;
}

/* Close the image's files (and the CHD); the disc structure stays. */
static void unload_image(void)
{
    gwcd_port_sd_lock();
    if (disc->data_file_track >= 0)
        f_close(&disc->data_fil);
    if (disc->audio_file_track >= 0)
        f_close(&disc->audio_fil);
    disc->data_file_track = disc->audio_file_track = -1;
#if GWCD_ENABLE_CHD
    if (disc->is_chd)
        gwcd_chd_close();
#endif
    disc->is_chd = 0;
    disc->ntracks = 0;
    gwcd_port_sd_unlock();
}

/* Region from the boot sector, when track 1 is a data track. */
static int read_region(void)
{
    if (!disc->ntracks || disc->tracks[0].audio)
        return 0;
    gwcd_disc_read_data((int)disc->tracks[0].lba_start, scratch->sector);
    return gwcd_disc_sector0_region(scratch->sector);
}

static void print_disc(void)
{
    printf("disc: %s: %d tracks, %lu sectors (%lu MB), %s, region %c\n", disc->image,
           disc->ntracks, (unsigned long)disc->total_lba,
           (unsigned long)((uint64_t)disc->total_lba * RAW_SECTOR >> 20),
           disc->is_chd ? "CHD" : "cue", region ? region : '-');
    for (int i = 0; i < disc->ntracks; i++) {
        const disc_track_t *t = &disc->tracks[i];
        printf("  T%02u %s lba %6lu..%6lu%s\n", t->number,
               t->audio ? "audio" : (t->raw ? "data/2352" : "data/2048"),
               (unsigned long)t->lba_start, (unsigned long)t->lba_end,
               t->loop > 0 ? " (loop)" : (t->loop < 0 ? " (no loop)" : ""));
    }
}

/* Mount `path`; everything else is closed. */
static int open_disc(const char *path)
{
    disc = gwcd_port_psram_alloc(sizeof(*disc));
    if (disc) {
        memset(disc, 0, sizeof(*disc));
        disc->data_file_track = disc->audio_file_track = -1;
    }
    gwcd_cdd = gwcd_port_psram_alloc(sizeof(*gwcd_cdd));
    scratch = gwcd_port_psram_alloc(sizeof(*scratch));
    if (!disc || !gwcd_cdd || !scratch) {
        printf("disc: out of memory\n");
        return -1;
    }
    if (load_image(path) != 0)
        return -1;

    memset(gwcd_cdd, 0, sizeof(*gwcd_cdd));
    load_toc();
    cdd.status = NO_DISC; /* not scanned yet */

    region = read_region();
    gwcd_port_psram_free(scratch);
    scratch = NULL;
    print_disc();
    return 0;
}

int gwcd_disc_open(const char *path)
{
    gwcd_disc_close();
    if (open_disc(path) != 0) {
        gwcd_disc_close();
        return -1;
    }
    return 0;
}

/* ------------------------- multi-disc games ---------------------------- */

/* A game on several discs is a set: the discs of an .m3u playlist, or the
   files next to the picked one whose names differ only in the number of a
   Redump/TOSEC "(Disc N)" tag. A disc change swaps the image under the
   running session, the way a player swaps discs on the console; the drive
   state, the CD's memory and the game all carry on.

   Two ways in:
     - the game opens the tray itself (CDD command "open tray", which
       PicoDrive reports through PicoIn.mcdTrayOpen): the next disc of the
       set goes in, and the game finds it when it closes the tray;
     - the player picks a disc in the settings menu (gwcd_disc_change): the
       drive reports its lid open for LID_OPEN_FRAMES, as a model 2 does
       when a disc is being swapped, then the new disc, not yet scanned.
   Either way the swap itself happens between frames (gwcd_disc_frame_end),
   under the SD lock, so core1's CD audio reader never sees a half-built
   track table. */

#define SET_MAX GWCD_DISCSET_MAX
#define LID_OPEN_FRAMES 60 /* about a second */

typedef struct {
    int count, current;
    int target;     /* disc to insert, -1 when no change is pending */
    int by_user;    /* the change came from the menu: hold the lid open */
    int lid_frames; /* frames of that still to go */
    int num[SET_MAX];
    char path[SET_MAX][FF_MAX_LFN + 2];
    /* scratch while the set is built */
    DIR dir;
    FILINFO fno;
    FIL f;
    char line[FF_MAX_LFN + 8];
} discset_t;

static discset_t *set;

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static int is_disc_image(const char *name)
{
    return has_ext(name, ".cue") || has_ext(name, ".chd");
}

static int set_add(const char *dir, const char *name, int num)
{
    int i;

    if (set->count >= SET_MAX || strlen(dir) + strlen(name) + 1 >= sizeof(set->path[0]))
        return -1;
    /* in disc-number order */
    for (i = set->count; i > 0 && set->num[i - 1] > num; i--) {
        set->num[i] = set->num[i - 1];
        memcpy(set->path[i], set->path[i - 1], sizeof(set->path[0]));
    }
    set->num[i] = num;
    snprintf(set->path[i], sizeof(set->path[0]), "%s%s", dir, name);
    set->count++;
    return i;
}

/* The folder part of `path`, with its trailing '/', into `out`. */
static void dir_of(const char *path, char *out, size_t size)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) + 1 : 0;

    if (n >= size)
        n = size - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

/* An .m3u playlist: one disc per line, relative to the playlist's folder
   or absolute; blank lines and #comments skipped. */
static int load_m3u(const char *path)
{
    char *dir = scratch->path;
    int num = 0;

    dir_of(path, dir, sizeof(scratch->path));
    if (f_open(&set->f, path, FA_READ) != FR_OK) {
        printf("disc: cannot open %s\n", path);
        return -1;
    }
    while (f_gets(set->line, sizeof(set->line), &set->f)) {
        char *p = set->line, *e;
        if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
            p += 3; /* UTF-8 byte order mark */
        p = ltrim(p);
        e = p + strlen(p);
        while (e > p && (e[-1] == '\r' || e[-1] == '\n' || is_ws(e[-1])))
            *--e = 0;
        if (!*p || *p == '#' || !is_disc_image(p))
            continue;
        if (set_add(*p == '/' ? "" : dir, p, num++) < 0)
            break;
    }
    f_close(&set->f);
    set->current = 0;
    return set->count ? 0 : -1;
}

/* The "(Disc N)" siblings of `path` in its folder, `path` included. */
static void find_siblings(const char *path)
{
    char *dir = scratch->path;
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    const char *tag = NULL, *suffix;
    size_t plen;

    for (const char *p = name; *p; p++) {
        if (!strncasecmp(p, "(disc ", 6) && p[6] >= '0' && p[6] <= '9') {
            tag = p + 6;
            break;
        }
    }
    dir_of(path, dir, sizeof(scratch->path));
    if (!tag) {
        set_add(dir, name, 0);
        return;
    }
    plen = (size_t)(tag - name);
    suffix = tag;
    while (*suffix >= '0' && *suffix <= '9')
        suffix++;
    if (f_opendir(&set->dir, dir[0] ? dir : "/") != FR_OK) {
        set_add(dir, name, 0);
        return;
    }
    while (f_readdir(&set->dir, &set->fno) == FR_OK && set->fno.fname[0]) {
        const char *f = set->fno.fname, *d = f + plen;
        int num = 0;
        if ((set->fno.fattrib & AM_DIR) || strncasecmp(f, name, plen) != 0 ||
            *d < '0' || *d > '9')
            continue;
        while (*d >= '0' && *d <= '9')
            num = num * 10 + (*d++ - '0');
        if (!strcasecmp(d, suffix))
            set_add(dir, f, num);
    }
    f_closedir(&set->dir);
    if (!set->count)
        set_add(dir, name, 0); /* the folder could not be read */
}

int gwcd_disc_open_set(const char *picked)
{
    int rc;

    gwcd_disc_close();
    set = gwcd_port_psram_alloc(sizeof(*set));
    scratch = gwcd_port_psram_alloc(sizeof(*scratch));
    if (!set || !scratch) {
        printf("disc: out of memory\n");
        gwcd_disc_close();
        return -1;
    }
    set->target = -1;
    if (has_ext(picked, ".m3u")) {
        rc = load_m3u(picked);
    } else {
        const char *name = strrchr(picked, '/') ? strrchr(picked, '/') + 1 : picked;
        find_siblings(picked);
        rc = set->count ? 0 : -1;
        for (int i = 0; i < set->count; i++) {
            const char *s = strrchr(set->path[i], '/');
            if (!strcasecmp(s ? s + 1 : set->path[i], name))
                set->current = i;
        }
    }
    gwcd_port_psram_free(scratch);
    scratch = NULL;
    if (rc != 0) {
        printf("disc: no disc images in %s\n", picked);
        gwcd_disc_close();
        return -1;
    }
    if (set->count > 1) {
        printf("disc: disc %d of %d\n", set->current + 1, set->count);
        for (int i = 0; i < set->count; i++)
            printf("  %d: %s\n", i + 1, set->path[i]);
    }
    if (open_disc(set->path[set->current]) != 0) {
        gwcd_disc_close();
        return -1;
    }
    return 0;
}

const char *gwcd_disc_path(void)
{
    return disc ? disc->image : "";
}

int gwcd_disc_count(void)
{
    return set ? set->count : (disc ? 1 : 0);
}

int gwcd_disc_index(void)
{
    if (!set)
        return 0;
    return set->target >= 0 ? set->target : set->current;
}

void gwcd_disc_change(int index)
{
    if (!set || !gwcd_cdd || index < 0 || index >= set->count)
        return;
    set->target = index;
    set->by_user = 1;
    set->lid_frames = LID_OPEN_FRAMES;
    printf("disc: lid open, disc %d going in\n", index + 1);
}

void gwcd_disc_tray_opened(void)
{
    if (!set || set->count < 2 || set->target >= 0)
        return;
    set->target = (set->current + 1) % set->count;
    set->by_user = 0;
    printf("disc: tray opened by the game, disc %d going in\n", set->target + 1);
}

/* Replace the image under the running session; the drive state stays. */
static int swap_image(const char *path)
{
    int rc = -1;

    gwcd_cdda_stop();
    gwcd_port_sd_lock();
    unload_image();
    scratch = gwcd_port_psram_alloc(sizeof(*scratch));
    if (scratch)
        rc = load_image(path);
    load_toc();
    region = rc == 0 ? read_region() : 0;
    gwcd_port_psram_free(scratch);
    scratch = NULL;
    gwcd_port_sd_unlock();
    if (rc == 0)
        print_disc();
    else
        printf("disc: cannot read %s, the drive stays empty\n", path);
    return rc;
}

void gwcd_disc_frame_end(void)
{
    if (!set || set->target < 0 || !gwcd_cdd || !Pico_mcd)
        return;
    if (set->by_user) {
        if (set->lid_frames == LID_OPEN_FRAMES) {
            gwcd_cdda_stop();
            Pico_mcd->s68k_regs[0x36 + 0] = 0x01; /* no audio track playing */
            cdd.loaded = 0;
        }
        cdd.status = CD_OPEN; /* held against the game's own commands */
        if (--set->lid_frames > 0)
            return;
    }
    swap_image(set->path[set->target]);
    set->current = set->target;
    set->target = -1;
    /* The lid is shut again (or the game shut the tray before the swap
       landed): a disc the drive has not scanned yet. A tray the game
       opened stays open until the game closes it. */
    if (set->by_user || cdd.status != CD_OPEN)
        cdd.status = NO_DISC;
}

void gwcd_disc_close(void)
{
    if (scratch) {
        gwcd_port_psram_free(scratch);
        scratch = NULL;
    }
    if (disc) {
        unload_image();
        gwcd_port_psram_free(disc);
        disc = NULL;
    }
    if (gwcd_cdd) {
        gwcd_port_psram_free(gwcd_cdd);
        gwcd_cdd = NULL;
    }
    if (set) {
        gwcd_port_psram_free(set);
        set = NULL;
    }
    region = 0;
}

int gwcd_disc_is_open(void)
{
    return disc != NULL;
}

/* Whether `name` (a file in the disc's folder) is one of the disc's own track
   files. */
int gwcd_disc_uses_file(const char *name)
{
    if (!disc || disc->is_chd)
        return 0;
    for (int i = 0; i < disc->ntracks; i++)
        if (!strcasecmp(disc->tracks[i].name, name))
            return 1;
    return 0;
}

int gwcd_disc_region(void)
{
    return region;
}

int gwcd_disc_tracks(void)
{
    return disc ? disc->ntracks : 0;
}

/* PicoDrive's cdd_unload(), for PicoExitMCD(). The image and its TOC stay
   until gwcd_disc_close(): a game reset starts a new session on the same
   disc, and cdd_reset() at power on clears the drive's own state. */
int cdd_unload(void)
{
    return gwcd_cdd && cdd.loaded;
}
