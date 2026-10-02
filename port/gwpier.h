/*
port/gwpier.h — the Pier Solar cartridge: its bank mapper, its dump
protection and its SPI EEPROM, after PicoDrive (pico/carthw/carthw.c,
"Pier Solar. Based on my own research") and Genesis Plus GX's SPI EEPROM
(port/carthw/eeprom_spi.c).

  $A13001        bit 1: bank registers enabled
  $A13003/5/7    512 KB bank at $280000 / $300000 / $380000
  $A13009        SPI EEPROM lines (write), $A1300B its data out (read)

At power on the cartridge shows only its first 32 KB, repeated over the
whole ROM window, until the game has read $010000-$01FFFF three times; then
the ROM appears linearly up to $27FFFF with the three banks above. Both live
in the gwmapper page table, so the game's own fetches never slow down; the
three counted reads go through the save-RAM window (port/gwsram.h), which
Pier Solar -- a cartridge with EEPROM, not save RAM -- does not otherwise use.

Detected, as PicoDrive does, by "PIER" at $150 and "Respect" at $610. Its
buffers (a 128 KB protection page and the 64 KB EEPROM) are in PSRAM; the
EEPROM is saved as the game's .srm, raw, the way PicoDrive saves it.
*/
#ifndef GWPIER_H
#define GWPIER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int gwpier_active;      /* the loaded cartridge is Pier Solar */
extern uint8_t *gwpier_eeprom; /* 64 KB */
extern int gwpier_eeprom_dirty;

#define GWPIER_EEPROM_SIZE 0x10000u

/* From gwmapper_reset(): detect and power on. Returns 1 for Pier Solar. */
int gwpier_reset(const unsigned char *rom, size_t size);
/* Free the buffers (free_emulator_mem). */
void gwpier_release(void);

/* $A13000-$A130FF, writes (from gwmapper_bank_write) and byte reads (the
   bus's /TIME case). */
void gwpier_write8(unsigned int address, unsigned int value);
unsigned int gwpier_read8(unsigned int address);

/* The protection window, $010000-$01FFFF, while gwcd_window_cd is
   GWCD_WINDOW_PIER (port/scd.h). */
unsigned int gwpier_prot_read8(unsigned int address);
unsigned int gwpier_prot_read16(unsigned int address);

#ifdef __cplusplus
}
#endif

#endif /* GWPIER_H */
