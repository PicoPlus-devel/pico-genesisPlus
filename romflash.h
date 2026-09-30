/* pico-genesisPlus — ROMs too large for PSRAM, held in XIP flash (plus PSRAM).
 *
 * HSTX boards with PSRAM and 16 MB of flash only; not built for PicoDVI, whose
 * scan-out runs from flash and cannot ride out the XIP-off windows of a write.
 *
 * Ported from pico_snesPlus/romflash.cpp. A board with PSRAM normally gets the
 * whole ROM preloaded into it by the menu, but flashromtoPsram() refuses any
 * file larger than the free PSRAM less a 512 KB margin and leaves
 * ROM_FILE_ADDR == 0. Such a cart is written once into a region of the board's
 * 16 MB flash that is clear of both flash layouts (standalone app at
 * 0x10000000, bootloader app partition at 0x10080000) and of FlashParams:
 *
 *   0x10000000  bootloader / app       (~1 MB + FlashParams sector)
 *   0x10800000  record sector          64 KB, describes what is in the region
 *   0x10810000  ROM image              up to 8 MB - 64 KB
 *   0x11000000  end of flash
 *
 * A ROM larger than the region itself (Demons of Asteborg, 15 MB) is split:
 * the first whole 512 KB banks that fit go to flash (15 banks, 7.5 MB) and the
 * rest is read into PSRAM on every launch. The cartridge mapper pages ROM in
 * 512 KB banks (port/gwmapper.h), so a bank never straddles the two.
 *
 * Unlike the SNES port the image is stored byte-swapped, exactly as the
 * menu's PSRAM preload leaves it, because the core fetches 16-bit words
 * little-endian (see isValidGenesisRom() in main.cpp).
 *
 * The record lives in flash rather than on the SD card so the region is
 * self-describing: swapping cards cannot make it lie. It is erased before the
 * first image sector is touched and written only after the image verifies, so
 * a record that exists always describes complete flash content. Its magic
 * differs from pico_snesPlus's, which uses the same region: whichever emulator
 * finds the other's image there simply rewrites it.
 *
 * ROM_FILE_ADDR is deliberately NOT used to carry the flash pointer: main.cpp
 * calls Frens::f_free(ROM_FILE_ADDR) whenever PSRAM is enabled, which would
 * hand a flash address to lwmem. */

#ifndef PICO_GENESISPLUS_ROMFLASH_H
#define PICO_GENESISPLUS_ROMFLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest image the flash region can hold, or 0 when this board has no room
 * for the region at all (less than 16 MB of flash, going by the chip itself). */
size_t romflash_capacity(void);

/* How many bytes of a `size`-byte ROM go to flash: all of it when it fits,
 * otherwise the whole 512 KB banks that do. The rest is the PSRAM tail. */
size_t romflash_head_len(size_t size);

/* True when the region already holds exactly this file (record magic, path,
 * size, modification time, then a CRC over the flash image itself). */
bool romflash_holds(const char *path, size_t size);

/* Base of the image in the XIP aperture. Only meaningful after
 * romflash_holds() has returned true. */
const uint8_t *romflash_image(void);

/* Path of the cart the region holds, or NULL if the record is not valid.
 * Points into flash; copy it if it has to outlive a rewrite. Ignores
 * ROMFLASH_FORCE_REWRITE -- this answers "what is in the region", not
 * "should it be rewritten". */
const char *romflash_recorded_path(void);

/* Progress callback, called between flash operations with XIP back at a safe
 * timing, so it may run from flash. phase is ROMFLASH_ERASE or _WRITE. */
#define ROMFLASH_ERASE 0
#define ROMFLASH_WRITE 1
typedef void (*romflash_progress_fn)(int phase, uint32_t done, uint32_t total);

/* Program the flash part of `path` (romflash_head_len(size) bytes) into the
 * region. QMI M0's timing and read format are saved before the first erase and
 * restored after the last program. The image is verified before the record is
 * written, so a false return always leaves no record and the next attempt
 * simply rewrites. The caller is expected to reboot afterwards: a PIO USB host
 * does not survive the interrupt blackouts an erase needs. */
bool romflash_program(const char *path, size_t size, romflash_progress_fn progress);

/* Read the part of `path` beyond `headLen` into a fresh PSRAM block,
 * byte-swapped like the rest of the image. NULL when the board has no PSRAM,
 * the block cannot be had, or the read fails -- nothing is left allocated
 * then. Release with romflash_free_tail(). */
uint8_t *romflash_load_tail(const char *path, size_t size, size_t headLen);
void romflash_free_tail(uint8_t *tail);

#ifdef __cplusplus
}
#endif

#endif
