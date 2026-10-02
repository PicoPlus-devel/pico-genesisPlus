/*
port/scd_msd.c — MD+: cartridge games patched to play CD audio through the
MegaSD's API ("MegaSD DEV Manual Rev.2"), after PicoDrive's pico/cd/megasd.c
(vendored in scd/cd/megasd.c).

An MD+ game is an ordinary cartridge -- it runs on the normal cartridge
path, save RAM and SSF2 mapper included -- plus a disc image that holds its
music. The patched game switches an overlay on by writing $CD54 to $03F7FA;
from then on $03F7F6-$03F7FF read "RATE", $CD54, a result and a command word,
and $03F800-$03FFFF a 2 KB data area, and it talks to the MegaSD by writing
the command port $03F7FE.

PicoDrive swaps read handlers into its memory map for the overlay. Here the
68000's reads of cartridge space never leave the FETCH*ROM fast path, so the
overlay is a PSRAM copy of the 128 KB page $020000-$03FFFF (slot 1 of the
page table, which the SSF2 mapper never switches) with the MegaSD's words
kept inside it; turning the overlay on or off points the slot at the copy or
back at the ROM. CPU reads, the Z80 window and VDP DMA all see it without a
single extra instruction on their paths. Writes to cartridge space reach
gwcd_msd_write8/16() through the bus's ROM-write case, which only an MD+ or
Sega CD session enables.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ff.h"
#include "pico_int.h"
#include "cd/genplus_macros.h"
#include "cd/cdd.h"
#include "cd/megasd.h"
#include "gwmapper.h"
#include "scd.h"

#define OVERLAY_SLOT 1u                   /* $020000-$03FFFF */
#define OVERLAY_BASE (OVERLAY_SLOT * GWMAPPER_SLOT_SIZE)
#define DATA_OFFSET (0x03F800u - OVERLAY_BASE)
#define REGS_OFFSET (0x03F7F6u - OVERLAY_BASE)

static uint8_t *shadow;                   /* 128 KB, PSRAM */
static const unsigned char *rom_page;     /* what slot 1 held before */
static unsigned int tick_acc;             /* Q16 75 Hz accumulator */

void gwcd_msd_sync_regs(void)
{
    uint16_t *r;

    if (!shadow)
        return;
    r = (uint16_t *)(shadow + REGS_OFFSET);
    r[0] = 0x5241;           /* $03F7F6 "RA" */
    r[1] = 0x5445;           /* $03F7F8 "TE" */
    r[2] = 0xCD54;           /* $03F7FA */
    r[3] = Pico_msd.result;  /* $03F7FC */
    r[4] = Pico_msd.command; /* $03F7FE */
}

void gwcd_msd_overlay(int on)
{
    if (!shadow)
        return;
    if (on) {
        /* Seeded from the ROM, as PicoDrive seeds its data area. */
        if (gw_rom.bank[OVERLAY_SLOT] != shadow) {
            rom_page = gw_rom.bank[OVERLAY_SLOT];
            memcpy(shadow, rom_page, GWMAPPER_SLOT_SIZE);
        }
        gw_rom.bank[OVERLAY_SLOT] = shadow;
        gwcd_msd_sync_regs();
    } else if (gw_rom.bank[OVERLAY_SLOT] == shadow) {
        gw_rom.bank[OVERLAY_SLOT] = rom_page;
    }
}

/* Cartridge-space writes in an MD+ session. The MegaSD answers only in the
   64 KB PicoDrive maps it in ($030000-$03FFFF); everything else is a write
   to ROM, which the bus has always dropped. */
void gwcd_msd_write8(unsigned int a, unsigned int d)
{
    if ((a & 0xFF0000u) == 0x030000u)
        msd_write8(a, d);
}

void gwcd_msd_write16(unsigned int a, unsigned int d)
{
    if ((a & 0xFF0000u) == 0x030000u)
        msd_write16(a, d);
}

void gwcd_msd_frame_end(int is_pal)
{
    /* msd_update() is PicoDrive's 75 Hz CDD-interrupt-rate tick. */
    tick_acc += (75u << 16) / (is_pal ? 50u : 60u);
    while (tick_acc >= (1u << 16)) {
        tick_acc -= 1u << 16;
        msd_update();
    }
}

/* The MegaSD for the cartridge load_cartridge() has mapped. */
int gwcd_mdplus_attach(const uint8_t *rom, size_t rom_size)
{
    if (!gwcd_disc_is_open())
        return -1;
    shadow = gwcd_port_psram_alloc(GWMAPPER_SLOT_SIZE);
    if (!shadow)
        return -1;
    if (!(gwcd_bus_mode & GWCD_BUS_SCD)) {
        /* a Mode 1 session has set these already */
        Pico.rom = rom;
        Pico.romsize = (unsigned int)rom_size;
    }
    rom_page = gw_rom.bank[OVERLAY_SLOT];
    tick_acc = 0;

    memset(&Pico_msd, 0, sizeof(Pico_msd));
    Pico_msd.data = (u16 *)(shadow + DATA_OFFSET);
    Pico_msd.index = -1;
    msd_reset();

    gwcd_bus_mode |= GWCD_BUS_MDPLUS;
    printf("mdplus: %d CD tracks\n", gwcd_disc_tracks());
    return 0;
}

int gwcd_mdplus_start(const uint8_t *rom, size_t rom_size)
{
    gwcd_stop();
    memset(&Pico, 0, sizeof(Pico));
    memset(&PicoIn, 0, sizeof(PicoIn));
    if (gwcd_audio_start(0) != 0 || gwcd_mdplus_attach(rom, rom_size) != 0) {
        printf("mdplus: out of memory\n");
        gwcd_stop();
        return -1;
    }
    return 0;
}

void gwcd_msd_stop(void)
{
    if (shadow) {
        if (gw_rom.bank[OVERLAY_SLOT] == shadow)
            gw_rom.bank[OVERLAY_SLOT] = rom_page;
        gwcd_port_psram_free(shadow);
        shadow = NULL;
    }
    Pico_msd.data = NULL;
    Pico_msd.state = 0;
}

static int has_rom_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && (!strcasecmp(dot, ".md") || !strcasecmp(dot, ".bin") ||
                   !strcasecmp(dot, ".gen") || !strcasecmp(dot, ".smd"));
}

/* A Mega Drive ROM, and not a Sega CD BIOS (whose name field at $120 reads
   "... BOOT ROM", "CD-ROM BIOS" and the like). */
static int is_cart_rom(FIL *f, const char *path, uint8_t *hdr)
{
    UINT br = 0;
    int ok = 0;

    if (f_open(f, path, FA_READ) != FR_OK)
        return 0;
    if (f_size(f) >= 0x200 && f_read(f, hdr, 0x200, &br) == FR_OK && br == 0x200 &&
        !memcmp(hdr + 0x100, "SEGA", 4)) {
        ok = 1;
        for (int i = 0x120; i < 0x150 - 3; i++)
            if (!memcmp(hdr + i, "BOOT", 4) || !memcmp(hdr + i, "BIOS", 4))
                ok = 0;
    }
    f_close(f);
    return ok;
}

typedef struct {
    FILINFO fno;
    DIR d;
    FIL f;
    uint8_t hdr[0x200];
    char path[2 * FF_MAX_LFN + 16];
    char dir[FF_MAX_LFN + 8];
} rom_scan_t;

int gwcd_mdplus_rom_path(const char *disc_path, char *out, size_t out_size)
{
    static const char *exts[] = { ".md", ".bin", ".gen", ".smd",
                                  ".MD", ".BIN", ".GEN", ".SMD" };
    /* ~1.5 KB of scratch: PSRAM, not the 3 KB stack or static SRAM */
    rom_scan_t *sc;
    const char *dot = strrchr(disc_path, '.');
    const char *slash = strrchr(disc_path, '/');
    size_t base = dot ? (size_t)(dot - disc_path) : strlen(disc_path);
    int found = 0, count = 0;

    out[0] = 0;
    if (base + 5 > out_size || !(sc = gwcd_port_psram_alloc(sizeof(*sc))))
        return 0;

    /* 1. the same name as the disc image */
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]) && !found; i++) {
        memcpy(out, disc_path, base);
        strcpy(out + base, exts[i]);
        found = f_stat(out, &sc->fno) == FR_OK && !(sc->fno.fattrib & AM_DIR) &&
                !gwcd_disc_uses_file(sc->fno.fname) && is_cart_rom(&sc->f, out, sc->hdr);
    }

    /* 2. the only Mega Drive rom in the folder that is not part of the disc:
       MD+ packs do not always name the rom after the .cue */
    if (!found) {
        size_t n = slash ? (size_t)(slash - disc_path) : 0;
        if (n >= sizeof(sc->dir))
            n = sizeof(sc->dir) - 1;
        memcpy(sc->dir, disc_path, n);
        sc->dir[n] = 0;
        if (f_opendir(&sc->d, n ? sc->dir : "/") == FR_OK) {
            while (f_readdir(&sc->d, &sc->fno) == FR_OK && sc->fno.fname[0]) {
                if (sc->fno.fattrib & (AM_DIR | AM_HID) || !has_rom_ext(sc->fno.fname) ||
                    gwcd_disc_uses_file(sc->fno.fname))
                    continue;
                snprintf(sc->path, sizeof(sc->path), "%s/%s", sc->dir, sc->fno.fname);
                if (!is_cart_rom(&sc->f, sc->path, sc->hdr))
                    continue;
                if (++count == 1 && strlen(sc->path) < out_size)
                    strcpy(out, sc->path);
            }
            f_closedir(&sc->d);
        }
        found = count == 1;
        if (count > 1)
            printf("mdplus: %d roms next to the disc, cannot tell which one goes with it\n", count);
    }
    gwcd_port_psram_free(sc);
    if (!found)
        out[0] = 0;
    return found;
}
