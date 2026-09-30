/*
port/gwmapper.h — cartridge ROM banking (the "SSF2" mapper) and ROM storage.

A cart larger than the 68000's 4 MB cartridge window pages 512 KB ROM banks
into it. Super Street Fighter II (5 MB) is the only commercial example;
homebrew built for the Everdrive "SSF" extension of the same scheme goes much
further (Demons of Asteborg is 15 MB). The window is eight 512 KB slots: slot
0 ($000000-$07FFFF) is fixed on bank 0, and slot k (1..7) is selected by a
byte write to $A130F1 + 2k, i.e. $A130F3 .. $A130FF.

Every cartridge-ROM read in the core -- 68000 instruction fetch, data reads,
the Z80's bank window and VDP DMA -- goes through FETCH8/16/32ROM in m68k.h,
so the mapper is a page table underneath those macros: one pointer per 512 KB
slot of the 8 MB cartridge half of the address map, plus the in-page mask. For
a cart without the mapper, gwmapper_reset() fills the table so that it gives
exactly what the old single pointer did, ROM_DATA[A & rom_addr_mask],
mirroring included. The per-fetch cost is one extra bit-field extract; table
and mask share a struct so one base register reaches both.

Storage. The image is normally one contiguous, pre-byte-swapped block in PSRAM
or XIP flash. A ROM too large for either alone is split (romflash.cpp): a head
in flash and a tail in PSRAM. The split falls on a 512 KB boundary, so no bank
straddles the two halves; the table simply points each slot at whichever half
holds its bank.

Lifecycle per game:
    gwmapper_set_storage(head, headLen, tail)   the firmware: every game
    load_cartridge() -> gwmapper_reset()        build the table, detect the mapper
    ... bank writes arrive from gwsram_time_write(), the $A13000 region ...

Known limitation: a 32-bit read of the last word of a remapped slot takes its
low half from whatever follows that bank in memory, not from the next slot.
Splitting FETCH32ROM into two lookups would cost every game; no known code
reads across a bank boundary.

SRAM: 68 bytes for the table, a few words of state. Nothing is allocated, and
everything except the table lives in flash -- bank switching is rare next to
the fetches it steers.
*/
#ifndef GWMAPPER_H
#define GWMAPPER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Perf A/B switch, same idea as GENESIS_CART_SRAM. Building with
   -DGENESIS_ROM_MAPPER=0 restores the old single-pointer FETCH*ROM macros, so
   the cost of the page table can be measured on the same board and scene.
   Carts over 4 MB do not work in such a build. */
#ifndef GENESIS_ROM_MAPPER
#define GENESIS_ROM_MAPPER 1
#endif

#define GWMAPPER_BANK_SHIFT 19u                      /* 512 KB */
#define GWMAPPER_BANK_SIZE (1u << GWMAPPER_BANK_SHIFT)

struct gw_rom_map {
    const unsigned char *bank[16]; /* one per 512 KB slot of $000000-$7FFFFF */
    unsigned int page_mask;        /* offset mask within a slot */
};
extern struct gw_rom_map gw_rom;

/* Describe a split image: `headLen` bytes (a multiple of 512 KB) at `head`,
   the rest of the ROM at `tail`; tail NULL means contiguous. Applies to the
   next load_cartridge() whose buffer is `head`, and a load of any other buffer
   is taken as contiguous -- but every cart run from the flash region has the
   same base, so the firmware declares its storage for every game (and clears
   it on the way back to the menu) rather than rely on that. */
void gwmapper_set_storage(const unsigned char *head, size_t headLen,
                          const unsigned char *tail);

/* Called by load_cartridge() once rom_addr_mask is known. Resets every slot to
   its power-on bank, so nothing survives from the previous game or a reset. */
void gwmapper_reset(const unsigned char *rom, size_t size, unsigned int addr_mask);

/* A write to $A130F3-$A130FF (odd addresses). Ignored unless the cart has the
   mapper, and for a bank the ROM does not have. */
void gwmapper_bank_write(unsigned int address, unsigned int value);

/* Non-zero when the loaded cart uses bank switching. */
int gwmapper_active(void);

#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
extern unsigned int gwmapper_bank_writes; /* harness diagnostic */
#endif

#ifdef __cplusplus
}
#endif

#endif /* GWMAPPER_H */
