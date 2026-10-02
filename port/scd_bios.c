/*
port/scd_bios.c — finding and identifying the Sega CD / Mega-CD BIOS.

Modelled on pico-pcePlus's System Card lookup (pce-go/cd.c: cd_find_bios,
scan_bios_dir): look in the disc's own folder first -- so a game can carry
its own BIOS -- then in /bios/ at the root of the card, which the ROM
browser hides and which PC Engine BIOS files share without clashing (theirs
are .pce, 256 KB). Every candidate is checked by content, not by name:

  - at least 128 KB, a Mega Drive header ("SEGA" at $100) and a region
    letter at $1F0-$1F2 (U, E or J);
  - its region must match the disc's (the security code in the disc's boot
    sector, see gwcd_disc_sector0_region): a BIOS refuses a disc of another
    region. With no match the best BIOS of any region is taken, which works
    with the region-free BIOS mods and is reported in the log;
  - among the matches, a known dump (CRC32 below) wins over an unknown file,
    and a later or better-tested revision over an earlier one.

Only candidates whose header matches are CRC'd, so a card with every BIOS
revision on it costs well under a second. The scan's buffers (two FatFs
handles, a directory, paths) live in a PSRAM block for its duration: it runs
once per game start, and SRAM is what this emulator is short of.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "scd.h"

#define BIOS_SIZE 0x20000

typedef struct {
    uint32_t crc;
    const char *name;
    int priority; /* higher wins */
} bios_entry_t;

/* CRC32 of the whole file. Official dumps (No-Intro names) first, ranked so a
   model 2 BIOS -- the most compatible -- is preferred; region-free mods and
   odd hardware after them. */
static const bios_entry_t known_bios[] = {
    { 0x8AF65F58, "Sega CD 2 (USA) (v2.00)", 90 },
    { 0x2E49D72C, "Sega CD 2 (USA) (v2.11X)", 88 },
    { 0x9F6F6276, "Sega CD 2 (USA) (v2.00W)", 86 },
    { 0xC6D10268, "Sega CD (USA) (v1.10)", 84 },
    { 0xE7E3AFE2, "Sega CD (USA) (v1.00)", 80 },
    { 0x0507B590, "Mega-CD 2 (Europe) (v2.00)", 90 },
    { 0x4D5CB8DA, "Mega-CD 2 (Europe) (v2.00W)", 86 },
    { 0x529AC15A, "Mega-CD (Europe) (v1.00)", 80 },
    { 0xDD6CC972, "Mega-CD 2 (Japan) (v2.00C)", 90 },
    { 0x9BCE40B2, "Mega-CD (Japan) (v1.00G)", 84 },
    { 0x79F85384, "Mega-CD (Japan) (1.00S)", 82 },
    { 0xF18DDE5B, "Mega-CD (Japan) (1.00l)", 81 },
    { 0x2EA250C0, "Mega-CD (Japan) (v1.00O)", 80 },
    { 0x9A9E3AE4, "Mega-CD (Japan) (v1.01)", 78 },
    { 0x550F30BB, "Mega-CD (Asia) (v1.00S)", 76 },
    { 0x8052C7A0, "Mega-CD 2 (Japan) (MPR-15768-T)", 74 },
    { 0x95118134, "Sega CD 2 (USA) (region free 930314)", 60 },
    { 0x7E86C5DA, "Sega CD 2 (USA) (region free 930601)", 60 },
    { 0x73784705, "Sega CD (USA) (region free 921011)", 58 },
    { 0xC7EB4CC9, "Sega CDX (USA) (region free 930907)", 56 },
    { 0x316F3D80, "Mega-CD 2 (Europe) (region free 930330)", 60 },
    { 0x056663AD, "Mega-CD 2 (Europe) (region free 930601)", 60 },
    { 0xC82EE650, "Mega-CD (Europe) (region free 921027)", 58 },
    { 0x9E140799, "Mega-CD 2 (Japan) (region free 921222)", 60 },
    { 0x8E6943FA, "Mega-CD (Japan) (region free 911228)", 58 },
    { 0x1AEE2672, "Mega-CD (Japan) (region free 911217)", 57 },
    { 0x29776266, "Mega-CD (Japan) PAL (region free 911228)", 50 },
    { 0x2B19972F, "Wondermega (Japan) (G303)", 40 },
    { 0xCE60984E, "Sega CD 68K (Unl)", 20 },
};

const char *gwcd_bios_known_name(uint32_t crc, int *priority)
{
    for (size_t i = 0; i < sizeof(known_bios) / sizeof(known_bios[0]); i++) {
        if (known_bios[i].crc == crc) {
            if (priority)
                *priority = known_bios[i].priority;
            return known_bios[i].name;
        }
    }
    if (priority)
        *priority = 10; /* unknown but plausible */
    return NULL;
}

/* The header is as on disk: big-endian, not byte-swapped. */
int gwcd_bios_image_region(const uint8_t *img, size_t size)
{
    if (!img || size < BIOS_SIZE)
        return 0;
    if (memcmp(img + 0x100, "SEGA", 4) != 0)
        return 0;
    for (int i = 0; i < 3; i++) {
        char c = (char)img[0x1F0 + i];
        if (c == 'U' || c == 'E' || c == 'J')
            return c;
    }
    return 0;
}

int gwcd_cart_region(const uint8_t *rom, size_t size)
{
    int country = 0;

    if (!rom || size < 0x200)
        return 'U';
    /* set_region()'s decode (gwenesis_bus.c) of the byte-swapped header */
    for (int i = 0; i < 3; i++) {
        unsigned char c = rom[(0x1F0u + i) ^ 1u];
        if (c == 'U')
            country |= 4;
        else if (c == 'E' || c == 'e')
            country |= 8;
        else if (c == 'J' || c == 'j' || c == 'K' || c == 'k')
            country |= 1;
        else if (c < 16)
            country |= c;
        else if (c >= '0' && c <= '9')
            country |= c - '0';
        else if (c >= 'A' && c <= 'F')
            country |= c - 'A' + 10;
    }
    if (country & 4)
        return 'U';
    if (country & 8)
        return 'E';
    if (country & 1)
        return 'J';
    return 'U';
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* A folder and a file name, each up to FF_MAX_LFN. */
#define SCAN_PATH (2 * FF_MAX_LFN + 16)

typedef struct {
    int score;
    int region;
    char path[SCAN_PATH];
} best_t;

typedef struct {
    FIL f;
    uint8_t buf[512];
    uint8_t hdr[0x200];
    DIR d;
    FILINFO fno;
    char path[SCAN_PATH];
    char open_path[FF_MAX_LFN + 8];
    char dir[FF_MAX_LFN + 8];
    best_t match, any;
} scan_t;

static scan_t *scan; /* PSRAM, for the duration of gwcd_bios_find() */

/* Header check and, when the region fits, the CRC. */
static int probe(const char *path, int *region, uint32_t *crc, int want_region)
{
    FIL *fp = &scan->f;
    uint8_t *buf = scan->buf, *hdr = scan->hdr;
    UINT br = 0;
    uint32_t c = 0;
    FSIZE_t size;

    if (f_open(fp, path, FA_READ) != FR_OK)
        return -1;
    size = f_size(fp);
    if (size < BIOS_SIZE || size > BIOS_SIZE + 0x1000 ||
        f_read(fp, hdr, sizeof(scan->hdr), &br) != FR_OK || br != sizeof(scan->hdr)) {
        f_close(fp);
        return -1;
    }
    /* gwcd_bios_image_region() only looks at the first $200 bytes. */
    if (memcmp(hdr + 0x100, "SEGA", 4) != 0) {
        f_close(fp);
        return -1;
    }
    *region = 0;
    for (int i = 0; i < 3 && !*region; i++) {
        char ch = (char)hdr[0x1F0 + i];
        if (ch == 'U' || ch == 'E' || ch == 'J')
            *region = ch;
    }
    if (!*region) {
        f_close(fp);
        return -1;
    }
    *crc = 0;
    if (want_region && *region != want_region) {
        f_close(fp);
        return 0; /* valid, but not worth a CRC */
    }
    c = crc32_update(0, hdr, sizeof(scan->hdr));
    while (f_read(fp, buf, sizeof(scan->buf), &br) == FR_OK && br > 0)
        c = crc32_update(c, buf, br);
    f_close(fp);
    *crc = c;
    return 0;
}

static int has_rom_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".bin") ||
                   !strcasecmp(dot, ".gen") || !strcasecmp(dot, ".smd") ||
                   !strcasecmp(dot, ".rom"));
}


static void scan_dir(const char *dir, int want_region, best_t *best_match, best_t *best_any)
{
    char *path = scan->path, *open_path = scan->open_path;
    FILINFO *fno = &scan->fno;
    size_t n;

    strncpy(open_path, dir, sizeof(scan->open_path) - 1);
    open_path[sizeof(scan->open_path) - 1] = 0;
    n = strlen(open_path);
    if (n > 1 && open_path[n - 1] == '/')
        open_path[n - 1] = 0;
    if (f_opendir(&scan->d, open_path) != FR_OK)
        return;
    while (f_readdir(&scan->d, fno) == FR_OK && fno->fname[0]) {
        int region = 0, prio = 0, score;
        uint32_t crc = 0;
        const char *name;

        if (fno->fattrib & (AM_DIR | AM_HID) || !has_rom_ext(fno->fname))
            continue;
        snprintf(path, sizeof(scan->path), "%s/%s", open_path, fno->fname);
        if (probe(path, &region, &crc, want_region) != 0)
            continue;
        name = crc ? gwcd_bios_known_name(crc, &prio) : NULL;
        if (!crc)
            prio = 10;
        score = prio * 4 + (region == 'U' ? 2 : region == 'E' ? 1 : 0);
        printf("bios: %s: region %c, %s\n", path, region,
               name ? name : (crc ? "unknown dump" : "other region"));
        if (region == want_region && score > best_match->score) {
            best_match->score = score;
            best_match->region = region;
            strncpy(best_match->path, path, sizeof(best_match->path) - 1);
        }
        if (score > best_any->score) {
            best_any->score = score;
            best_any->region = region;
            strncpy(best_any->path, path, sizeof(best_any->path) - 1);
        }
    }
    f_closedir(&scan->d);
}

int gwcd_bios_find(const char *disc_path, int disc_region, const char *bios_dir,
                   char *out, size_t out_size)
{
    int want = disc_region ? disc_region : 'U';
    const best_t *pick;
    int region = 0;

    scan = gwcd_port_psram_alloc(sizeof(*scan)); /* zeroed */
    if (!scan) {
        printf("bios: out of memory\n");
        return 0;
    }
    best_t *match = &scan->match, *any = &scan->any;
    char *dir = scan->dir;

    /* 1. the disc's own folder */
    if (disc_path) {
        char *slash;
        strncpy(dir, disc_path, sizeof(scan->dir) - 1);
        dir[sizeof(scan->dir) - 1] = 0;
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            scan_dir(dir[0] ? dir : "/", want, match, any);
        }
    }
    /* 2. the BIOS folder, unless the disc folder had one that fits */
    if (!match->score)
        scan_dir(bios_dir ? bios_dir : "/bios", want, match, any);

    pick = match->score ? match : (any->score ? any : NULL);
    if (!pick) {
        printf("bios: no Sega CD BIOS found\n");
    } else {
        if (pick == any)
            printf("bios: no %c BIOS for this disc, trying %s\n", want, pick->path);
        else
            printf("bios: using %s\n", pick->path);
        strncpy(out, pick->path, out_size - 1);
        out[out_size - 1] = 0;
        region = pick->region;
    }
    gwcd_port_psram_free(scan);
    scan = NULL;
    return region;
}

/* Read a BIOS file into a new PSRAM buffer (raw, as on disk). */
uint8_t *gwcd_bios_load(const char *path, size_t *size)
{
    FIL *fp = gwcd_port_psram_alloc(sizeof(FIL));
    uint8_t *p = NULL;
    UINT br = 0;

    *size = 0;
    if (!fp)
        return NULL;
    if (f_open(fp, path, FA_READ) == FR_OK) {
        FSIZE_t n = f_size(fp);
        if (n >= BIOS_SIZE && n <= BIOS_SIZE + 0x1000)
            p = gwcd_port_psram_alloc(BIOS_SIZE);
        if (p && (f_read(fp, p, BIOS_SIZE, &br) != FR_OK || br != BIOS_SIZE)) {
            gwcd_port_psram_free(p);
            p = NULL;
        }
        f_close(fp);
    }
    gwcd_port_psram_free(fp);
    if (p)
        *size = BIOS_SIZE;
    return p;
}
