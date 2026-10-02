/*
port/scd.h — Sega CD / Mega-CD and MD+ support around the gwenesis core.

The CD hardware itself (gate array, CDC, CDD, graphics ASIC, PCM) is a
vendored copy of PicoDrive's pico/cd in scd/cd/ (see scd/PORTING.md). This
header is the seam between that code, the gwenesis core, and the firmware or
host harness:

  - the session lifecycle (one disc or MD+ game, allocated per game in PSRAM);
  - the frame-loop hooks (port/frame_loop.inc);
  - the main-CPU bus hooks the gwenesis bus calls in CD / MD+ mode;
  - the disc layer (cue/bin and CHD, port/scd_disc.c) and BIOS lookup
    (port/scd_bios.c).

What a session has, as flags in gwcd_bus_mode:
  GWCD_BUS_SCD     the Mega-CD hardware: BIOS, sub 68000, gate array, CD
                   drive, graphics chip, PCM. At $000000 for a Sega CD disc
                   (no cartridge), at $400000 in "Mode 1", where a cartridge
                   boots and uses the Sega CD (Pier Solar's soundtrack disc);
                   gwcd_cd_base says which.
  GWCD_BUS_MDPLUS  the MegaSD's CD audio interface for a cartridge (MD+),
                   registers at $03F7F6-$03FFFF. No BIOS, no sub CPU.
A cartridge next to a disc gets MD+, and Mode 1 as well when the disc is a
Sega CD disc and a BIOS is at hand.

Everything here compiles only with GENESIS_SEGACD (HSTX builds and the
host harness); cartridge-only builds never see it.
*/
#ifndef GWENESIS_PORT_SCD_H
#define GWENESIS_PORT_SCD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GENESIS_SEGACD
#define GENESIS_SEGACD 0
#endif

/* CHD disc images (libchdr, port/scd_chd.c); the rom browser lists .chd
   only when they are built in. */
#ifndef GWCD_ENABLE_CHD
#define GWCD_ENABLE_CHD 0
#endif
#if GWCD_ENABLE_CHD
#define GWCD_CHD_EXT " .chd"
#else
#define GWCD_CHD_EXT ""
#endif

enum gwcd_bus_mode {
    GWCD_BUS_CART = 0,   /* plain cartridge: nothing here is active */
    GWCD_BUS_SCD = 1,    /* flag: Mega-CD hardware, at gwcd_cd_base */
    GWCD_BUS_MDPLUS = 2, /* flag: MegaSD CD audio (MD+) */
};

/* Wait-loop skipping in both 68000s (gwenesis/PORTING.md). Building with
   -DGWCD_IDLE_SKIP=0 turns it off, for an A/B on the same board. */
#ifndef GWCD_IDLE_SKIP
#define GWCD_IDLE_SKIP 1
#endif

/* What the session has (flags above). Read by the gwenesis bus on its cold
   paths. */
extern int gwcd_bus_mode;
/* Where the Mega-CD sits in the main CPU's map: 0 for a disc, $400000 in
   Mode 1. */
extern unsigned int gwcd_cd_base;
/* Who the save-RAM window (port/gwsram.h) serves instead of cartridge save
   RAM: nobody (0), the Mega-CD's 1M cell image, or Pier Solar's dump
   protection (port/gwpier.h). gwsram.c only tests it for non-zero and hands
   the access to gwcd_m68k_window_*(), which are in flash. */
#define GWCD_WINDOW_CELL 1
#define GWCD_WINDOW_PIER 2
extern int gwcd_window_cd;

/* ---------------- platform hooks (main.cpp / hosttest) ----------------- */

/* A non-panicking allocator for the large, per-game CD buffers: PSRAM on
   the firmware, plain calloc on the host. Must return zeroed memory or
   NULL. */
void *gwcd_port_psram_alloc(size_t size);
void gwcd_port_psram_free(void *p);

/* Serialise SD card (FatFs) access between core0 (data sectors) and core1
   (CD-DA prefetch). No-ops on the host. */
void gwcd_port_sd_lock(void);
void gwcd_port_sd_unlock(void);
/* Non-blocking variant for core1: returns 1 with the lock held, 0 when core0
   has it (a data sector, or anything else on the card -- main.cpp holds it
   across its per-frame work, where the in-game menu and the hotkeys that
   save settings run). The CD-DA prefetch then just tries again later. */
int gwcd_port_sd_trylock(void);

/* Call fn(arg) on a caller-owned stack (8-byte aligned, `size` bytes), for
   code that needs more stack than the cores have -- libchdr's decoders
   (port/scd_chd.c). On the Pico the thread runs on the process stack
   pointer set to that memory, so interrupt handlers keep running on their
   own SRAM stack. A plain call on the host. */
void gwcd_port_big_stack_call(void (*fn)(void *), void *arg, void *stack, size_t size);

/* ------------------------------ session -------------------------------- */

/* Disc image (cue/bin or chd) -> mounted, TOC built. 0 on success. */
int gwcd_disc_open(const char *path);
void gwcd_disc_close(void);
int gwcd_disc_is_open(void);
/* Region of a mounted Sega CD disc from its security code: 'U', 'E', 'J',
   or 0 when the disc has no Sega CD boot sector (e.g. an MD+ audio disc). */
int gwcd_disc_region(void);
/* Track count of the mounted disc. */
int gwcd_disc_tracks(void);
/* Whether a file in the disc's folder is one of its track files. */
int gwcd_disc_uses_file(const char *name);

/* Multi-disc games (port/scd_disc.c). gwcd_disc_open_set() mounts what the
   rom browser picked -- a .cue or .chd, or an .m3u playlist (its first
   disc) -- and remembers the other discs of the game: the playlist's, or
   the files next to it that differ only in a "(Disc N)" tag. The image
   actually mounted is gwcd_disc_path(). */
#define GWCD_DISCSET_MAX 8
int gwcd_disc_open_set(const char *picked);
const char *gwcd_disc_path(void);
/* Discs in the set (1 for a single disc), and the one in the drive, or
   going in, counting from 0. */
int gwcd_disc_count(void);
int gwcd_disc_index(void);
/* The player swaps discs: the drive reports its lid open for about a
   second, then disc `index`. Takes effect between frames. */
void gwcd_disc_change(int index);
/* PicoIn.mcdTrayOpen: the game opened the tray, the next disc goes in. */
void gwcd_disc_tray_opened(void);
/* From gwcd_frame_end(): carries out a pending change. */
void gwcd_disc_frame_end(void);

/* Start a Mega-CD session. `bios` is the raw 128 KB image as read from the
   file (not byte-swapped); it is copied. The disc must already be open.
   Call between init_emulator_mem() and power_on(). With `cart` NULL this is
   a disc boot, and the BIOS becomes the main CPU's "cartridge"; otherwise
   Mode 1, for the cartridge load_cartridge() has just mapped (`cart`,
   `cart_size` as given to it). 0 on success. */
int gwcd_scd_start(const uint8_t *bios, size_t bios_size,
                   const uint8_t *cart, size_t cart_size);

/* Start an MD+ session for the cartridge that load_cartridge() has just
   mapped (`rom`, `rom_size` as given to it). The disc must be open. */
int gwcd_mdplus_start(const uint8_t *rom, size_t rom_size);
/* Add MD+ to a running Mode 1 session (call after gwcd_power_on()). */
int gwcd_mdplus_attach(const uint8_t *rom, size_t rom_size);

/* After gwenesis power_on()/reset_emulation(): power on the CD side. */
void gwcd_power_on(void);

/* End the session and free everything it allocated. Safe to call when no
   session is running. The disc stays open; close it separately. */
void gwcd_stop(void);

/* ------------------------- frame loop hooks ---------------------------- */

void gwcd_frame_start(void);
/* One scanline of both CPUs: replaces m68k_run(target) in CD mode. */
void gwcd_run_line(unsigned int target);
/* Frame end, before the core rebases its clocks by `system_clock`. */
void gwcd_frame_end(unsigned int system_clock, int is_pal);

/* ---------------------- main-CPU bus (gwenesis) ------------------------ */

/* $A12000-$A120FF, the main CPU's gate array registers (CD mode). */
unsigned int gwcd_m68k_io_read8(unsigned int a);
unsigned int gwcd_m68k_io_read16(unsigned int a);
void gwcd_m68k_io_write8(unsigned int a, unsigned int d);
void gwcd_m68k_io_write16(unsigned int a, unsigned int d);

/* Writes below $800000 (CD mode: PRG-RAM window, Word-RAM; MD+: the MegaSD
   registers). The gwenesis bus used to drop them as ROM writes. */
void gwcd_m68k_write8(unsigned int a, unsigned int d);
void gwcd_m68k_write16(unsigned int a, unsigned int d);

/* The one main-CPU region that cannot be a plain page: the 1M-mode cell
   image at $220000-$23FFFF. It is reached through the save-RAM overlay
   window (port/gwsram.h), which a CD session borrows since it has no
   cartridge. */
unsigned int gwcd_m68k_window_read8(unsigned int a);
unsigned int gwcd_m68k_window_read16(unsigned int a);
void gwcd_m68k_window_write8(unsigned int a, unsigned int d);
void gwcd_m68k_window_write16(unsigned int a, unsigned int d);

/* VDP DMA from 68000 space in a CD session, one word: Word-RAM is read one
   word late and the cell image through its mapping (PicoDrive's DmaSlow);
   everything else as FETCH16ROM. */
unsigned int gwcd_dma_read16(unsigned int src);

/* Where the CPU time went since the last call: sub-CPU cycles executed,
   skipped while it polled or slept, and while halted/in reset; main-CPU
   master clocks skipped while it polled. */
void gwcd_stats_take(unsigned int *s68k_run, unsigned int *s68k_idle,
                     unsigned int *s68k_halted, unsigned int *m68k_idle);

/* ------------------------------- audio --------------------------------- */

/* Session audio buffers (PSRAM): the CD-DA ring, and with `with_pcm` the
   PCM chip's output ring. Stop only after the core1 engine is detached. */
int gwcd_audio_start(int with_pcm);
void gwcd_audio_stop(void);

/* The CD-DA player (port/scd_audio.c), for the MegaSD (MD+). `end_lba` is
   exclusive; 0 plays to the lead-out. */
void gwcd_cdda_play(int start_lba, int end_lba, int loop_lba, int loop);
void gwcd_cdda_stop(void);
void gwcd_cdda_pause(int paused);
int gwcd_cdda_busy(void);
unsigned int gwcd_cdda_underruns(void);

/* Mixed into the 44.1 kHz output by the resampler (port/gwsnd_resample.c),
   one call per output sample: PCM (Sega CD) and CD-DA (Sega CD and MD+).
   Adds to *l / *r. Runs on core1 in offload mode. */
void gwcd_audio_mix(int32_t *l, int32_t *r);
/* Sync mode (host harness): refill the CD-DA ring from the calling core.
   In offload mode core1 calls this from its background task. */
void gwcd_cdda_service(void);

/* ----------------------------- backup RAM ------------------------------ */

/* The 8 KB internal backup RAM, raw (GPGX/PicoDrive .brm layout). */
uint8_t *gwcd_bram(void);
size_t gwcd_bram_size(void);
int gwcd_bram_dirty(void);
void gwcd_bram_clean(void);
/* Region letter of the running BIOS ('U', 'E', 'J'), for the .brm name. */
int gwcd_bios_region(void);

/* ------------------------------- BIOS ---------------------------------- */

/* Region letter ('U', 'E', 'J') from a BIOS image header, or 0 if the
   image is not a Sega CD BIOS. */
int gwcd_bios_image_region(const uint8_t *img, size_t size);
/* The console region a cartridge boots in ('U', 'E', 'J'), by the rule
   set_region() applies: USA over Europe over Japan. In Mode 1 the BIOS must
   match the console, not the disc. `rom` is the byte-swapped image. */
int gwcd_cart_region(const uint8_t *rom, size_t size);
/* Region from a disc's first data sector (2048 bytes of user data). */
int gwcd_disc_sector0_region(const uint8_t *sector);
/* Human-readable name of a known BIOS dump by CRC32, or NULL. */
const char *gwcd_bios_known_name(uint32_t crc, int *priority);
/* Find the BIOS for a disc (port/scd_bios.c): the disc's folder first, then
   `bios_dir` (NULL = "/bios"). Writes the path, returns its region letter,
   0 when there is none. */
int gwcd_bios_find(const char *disc_path, int disc_region, const char *bios_dir,
                   char *out, size_t out_size);
/* Read a BIOS file into a new PSRAM buffer; free with gwcd_port_psram_free. */
uint8_t *gwcd_bios_load(const char *path, size_t *size);

/* ------------------------------- MD+ ----------------------------------- */

/* For a cartridge picked through its .cue/.chd (call with the disc open):
   the ROM next to the disc with the same basename (.md .bin .gen .smd), or
   else the one Mega Drive ROM in the disc's folder that is neither a track
   of the disc nor a Sega CD BIOS. 0 if there is none (or several). */
int gwcd_mdplus_rom_path(const char *disc_path, char *out, size_t out_size);
/* port/scd_msd.c internals used by port/scd.c */
void gwcd_msd_write8(unsigned int a, unsigned int d);
void gwcd_msd_write16(unsigned int a, unsigned int d);
void gwcd_msd_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* GWENESIS_PORT_SCD_H */
