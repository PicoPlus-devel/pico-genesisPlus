/*
port/gwsram.c — see gwsram.h.

Header detection mirrors Genesis Plus GX's sram_init(), including its fixups
for the many carts that declare a nonsensical range.
*/
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if !defined(GWENESIS_HOST) || GWENESIS_HOST == 0
#include <malloc.h>
#endif

#include "gwenesis_port.h"
#include "gwsram.h"
#include "gwmapper.h"
#include "scd.h"

/* Cartridge save RAM can only live in the $200000-$3FFFFF half of the cart
   window; anything else in the header is a broken image, not a mapping. */
#define SRAM_WINDOW_LOW 0x200000u
#define SRAM_WINDOW_HIGH 0x3FFFFFu

uint8_t *gwsram_data;
uint32_t gwsram_start;
uint32_t gwsram_span;
uint32_t gwsram_bytes;
uint32_t gwsram_live;
int gwsram_packed;
int gwsram_odd;
int gwsram_bankable;
int gwsram_battery;
int gwsram_protect;
int gwsram_dirty;
int gwsram_oom;
int gwsram_in_psram;

/* Trailing guard word, same idea as port/buffers.c: an overrun would land on
   the allocator's chunk header and only surface much later as an
   unattributable fault. */
#define GWSRAM_GUARD 0xA5C3F00Du

/* The loader byte-swaps every 16-bit word of the image for the core's
   little-endian fetches, so the byte at file offset N lives at [N ^ 1] —
   the same idiom isValidGenesisRom() uses in main.cpp. */
static uint8_t hdr8(const unsigned char *rom, uint32_t off)
{
    return rom[off ^ 1];
}

static uint32_t hdr32(const unsigned char *rom, uint32_t off)
{
    return ((uint32_t)hdr8(rom, off) << 24) | ((uint32_t)hdr8(rom, off + 1) << 16) |
           ((uint32_t)hdr8(rom, off + 2) << 8) | (uint32_t)hdr8(rom, off + 3);
}

void gwsram_detect(const unsigned char *rom, size_t romSize)
{
    uint32_t type, start, end;

    /* The firmware never reboots between games, so every global has to be put
       back rather than assumed zero. */
    gwsram_data = NULL;
    gwsram_start = 0;
    gwsram_span = 0;
    gwsram_bytes = 0;
    gwsram_live = 0;
    gwsram_packed = 0;
    gwsram_odd = 0;
    gwsram_bankable = 0;
    gwsram_battery = 0;
    gwsram_protect = 0;
    gwsram_dirty = 0;
    gwsram_oom = 0;
    gwsram_in_psram = 0;

    if (!rom || romSize < 0x200)
        return;

    /* "RA" at $1B0 is the only marker. Genesis Plus GX also enables a blanket
       64 KB for every ROM below 2 MB that lacks it; that trade is wrong here,
       where the 64 KB would come out of the same heap the menu needs. */
    if (hdr8(rom, 0x1B0) != 'R' || hdr8(rom, 0x1B1) != 'A')
        return;

    type = hdr8(rom, 0x1B2);
    start = hdr32(rom, 0x1B4);
    end = hdr32(rom, 0x1B8);

    if (start < SRAM_WINDOW_LOW || start > SRAM_WINDOW_HIGH) {
        printf("SRAM: header claims save RAM at %06x-%06x, outside the cart window - ignored\n",
               (unsigned)start, (unsigned)end);
        return;
    }
    /* Same fixups Genesis Plus GX applies to bad headers. */
    if (end < start || (end - start) >= 0x10000u)
        end = start + 0xFFFFu;
    if (end > SRAM_WINDOW_HIGH)
        end = SRAM_WINDOW_HIGH;

    /* Chip width, from bits 4-3 of the type byte: %11 = a chip on the odd
       bytes, %10 = on the even bytes, %00 = a word-wide device.

       The declared range is not a usable second opinion. SGDK's default header
       pairs an odd-byte type with the whole $200000-$20FFFF window, so its
       start and end have different parities; treating that disagreement as
       "assume word-wide" doubled the allocation to 64 KB and mapped a parity
       the chip does not drive. The range is only consulted when the type byte
       encodes no valid width. */
    switch (type & 0x18u) {
    case 0x18u: /* odd bytes only -- Sonic 3, and SGDK's default */
        gwsram_packed = 1;
        gwsram_odd = 1;
        break;
    case 0x10u: /* even bytes only */
        gwsram_packed = 1;
        gwsram_odd = 0;
        break;
    case 0x00u: /* word wide */
        gwsram_packed = 0;
        gwsram_odd = 0;
        break;
    default: /* %01 is not a defined width: fall back on the range's parity */
        gwsram_packed = ((start ^ end) & 1u) == 0;
        gwsram_odd = gwsram_packed ? (int)(start & 1u) : 0;
        break;
    }

    gwsram_start = start & ~1u;
    gwsram_span = (end | 1u) - gwsram_start + 1u;

    /* The save window is a single 64 KB page — that is what the flat .srm
       layout addresses — so a header whose range would run past the end of it
       gets truncated rather than wrapped. */
    if ((gwsram_start & 0xFFFFu) + gwsram_span > 0x10000u)
        gwsram_span = 0x10000u - (gwsram_start & 0xFFFFu);

    /* A two-byte range is a serial EEPROM sharing the same header field, not
       an SRAM chip. Emulating those needs an I2C device model and a per-game
       table; leaving it unmapped keeps the pre-existing behaviour. */
    if (gwsram_span <= 2) {
        printf("SRAM: %06x-%06x is a serial EEPROM, not supported - no saves for this game\n",
               (unsigned)start, (unsigned)end);
        gwsram_start = 0;
        gwsram_span = 0;
        gwsram_packed = 0;
        gwsram_odd = 0;
        return;
    }

    gwsram_bytes = gwsram_packed ? (gwsram_span >> 1) : gwsram_span;
    gwsram_battery = (type & 0x40u) != 0;

    /* When the ROM itself reaches into the save window the two overlap, and
       the cart uses $A130F1 to switch between them (Phantasy Star IV is 3 MB).
       ROM wins until the game asks for save RAM. */
    gwsram_bankable = romSize > gwsram_start;

    /* The window is mapped from the start; the buffer behind it is not
       allocated until something actually needs it (see gwsram_ensure_buffer).
       A bankable cart stays switched out until the game asks via $A130F1. */
    gwsram_live = gwsram_bankable ? 0 : gwsram_span;

    printf("SRAM: %06x-%06x, %s, %s, %u bytes%s\n",
           (unsigned)gwsram_start, (unsigned)(gwsram_start + gwsram_span - 1),
           gwsram_packed ? (gwsram_odd ? "odd bytes" : "even bytes") : "word wide",
           gwsram_battery ? "battery backed" : "no battery",
           (unsigned)gwsram_bytes,
           gwsram_bankable ? ", banked over ROM via $A130F1" : "");
}

/* Would `need` bytes fit on the SRAM heap? keepcost is the top-most free
   block, and by this point every fixed emulator buffer is already taken, so
   that block is the remaining free space. Underestimating only sends the
   buffer to PSRAM; overestimating panics, because PICO_MALLOC_PANIC is 1. */
static int fits_in_sram_heap(size_t need)
{
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
    (void)need;
    return 1;
#else
    struct mallinfo mi = mallinfo();
    return (size_t)mi.keepcost >= need + 2048; /* leave the heap some room */
#endif
}

int gwsram_ensure_buffer(void)
{
    size_t need;

    if (gwsram_data)
        return 1;
    if (!gwsram_span || gwsram_oom)
        return 0;

    need = gwsram_bytes + 4; /* + guard word */

    if (fits_in_sram_heap(need)) {
        gwsram_data = malloc(need);
        if (gwsram_data)
            printf("cart SRAM: %u bytes allocated in SRAM\n",
                   (unsigned)gwsram_bytes);
    } else {
        printf("cart SRAM: %u bytes will not fit the SRAM heap, trying PSRAM\n",
               (unsigned)gwsram_bytes);
    }

    if (!gwsram_data) {
        gwsram_data = gwsram_port_psram_alloc(need);
        if (gwsram_data) {
            gwsram_in_psram = 1;
            printf("cart SRAM: %u bytes allocated in PSRAM (save RAM is only "
                   "touched when a game loads or stores its progress)\n",
                   (unsigned)gwsram_bytes);
        }
    }

    if (!gwsram_data) {
        gwsram_oom = 1;
        printf("cart SRAM: could not allocate %u bytes in SRAM or PSRAM - this "
               "game will run but cannot save\n", (unsigned)gwsram_bytes);
        return 0;
    }

    /* An SRAM chip that has never been written reads back as 0xFF; games check
       for their own magic and format it themselves. */
    memset(gwsram_data, 0xFF, gwsram_bytes);
    *(volatile uint32_t *)(gwsram_data + gwsram_bytes) = GWSRAM_GUARD;
    return 1;
}

void gwsram_release(void)
{
    if (gwsram_data) {
        if (*(volatile uint32_t *)(gwsram_data + gwsram_bytes) != GWSRAM_GUARD)
            printf("HEAP GUARD CLOBBERED: cart SRAM overran\n");
        printf("cart SRAM: releasing %u bytes from %s\n",
               (unsigned)gwsram_bytes, gwsram_in_psram ? "PSRAM" : "SRAM");
        if (gwsram_in_psram)
            gwsram_port_psram_free(gwsram_data);
        else
            free(gwsram_data);
    }
    gwsram_data = NULL;
    gwsram_in_psram = 0;
    gwsram_live = 0;
}

/* ------------------------------------------------------------------ */
/* Bus accessors. Reached only after gwsram_hit(), i.e. only for the    */
/* carts that have save RAM and only inside their mapped range.         */
/* ------------------------------------------------------------------ */

unsigned int GW_SRAM_FUNC(gwsram_read8)(unsigned int address)
{
    unsigned int off;

#if GENESIS_SEGACD
    /* A Mega-CD session borrows the window for the 1M-mode cell image when
       there is no cartridge save RAM to serve (port/scd.c), Pier Solar for
       its dump protection (port/gwpier.c). */
    if (gwcd_window_cd)
        return gwcd_m68k_window_read8(address);
#endif

    /* Nothing written yet, so nothing allocated yet: an unwritten chip. */
    if (!gwsram_data)
        return 0xFF;

    off = address - gwsram_start;
    if (gwsram_packed) {
        if ((int)(off & 1u) != gwsram_odd)
            return 0xFF; /* the half the chip does not drive */
        off >>= 1;
    }
    return gwsram_data[off];
}

void GW_SRAM_FUNC(gwsram_write8)(unsigned int address, unsigned int value)
{
    unsigned int off;

#if GENESIS_SEGACD
    if (gwcd_window_cd) {
        gwcd_m68k_window_write8(address, value);
        return;
    }
#endif

    if (gwsram_protect)
        return;
    /* First write is what proves the game really uses its save RAM. */
    if (!gwsram_data && !gwsram_ensure_buffer())
        return;
    off = address - gwsram_start;
    if (gwsram_packed) {
        if ((int)(off & 1u) != gwsram_odd)
            return;
        off >>= 1;
    }
    gwsram_data[off] = (uint8_t)value;
    gwsram_dirty = 1;
}

/* Word accesses are even aligned on the 68000; masking guarantees the second
   byte stays inside the range, since both start and span are even. */
unsigned int GW_SRAM_FUNC(gwsram_read16)(unsigned int address)
{
    unsigned int a = address & ~1u;

#if GENESIS_SEGACD
    if (gwcd_window_cd)
        return gwcd_m68k_window_read16(a);
#endif

    return (gwsram_read8(a) << 8) | gwsram_read8(a + 1);
}

void GW_SRAM_FUNC(gwsram_write16)(unsigned int address, unsigned int value)
{
    unsigned int a = address & ~1u;

#if GENESIS_SEGACD
    if (gwcd_window_cd) {
        gwcd_m68k_window_write16(a, value);
        return;
    }
#endif

    gwsram_write8(a, (value >> 8) & 0xFF);
    gwsram_write8(a + 1, value & 0xFF);
}

/* Clamp a caller's window to what is actually mapped. */
static uint32_t clamp_window(uint32_t offset, uint32_t n)
{
    if (!gwsram_data || offset >= gwsram_span)
        return 0;
    if (n > gwsram_span - offset)
        n = gwsram_span - offset;
    return n;
}

void gwsram_export(uint32_t offset, uint8_t *flat, uint32_t n)
{
    uint32_t i;

    n = clamp_window(offset, n);
    for (i = 0; i < n; i++) {
        uint32_t at = offset + i;
        if (gwsram_packed) {
            if ((int)(at & 1u) != gwsram_odd) {
                flat[i] = 0xFF; /* the chip drives nothing here */
                continue;
            }
            at >>= 1;
        }
        flat[i] = gwsram_data[at];
    }
}

void gwsram_import(uint32_t offset, const uint8_t *flat, uint32_t n)
{
    uint32_t i;

    n = clamp_window(offset, n);
    for (i = 0; i < n; i++) {
        uint32_t at = offset + i;
        if (gwsram_packed) {
            if ((int)(at & 1u) != gwsram_odd)
                continue;
            at >>= 1;
        }
        gwsram_data[at] = flat[i];
    }
}

void GW_SRAM_FUNC(gwsram_time_write)(unsigned int address, unsigned int value)
{
    /* $A130F1 is the save RAM control register. Everything else in the /TIME
       region goes to the ROM mapper, which owns $A130F3-$FF (the bank
       registers of carts over 4 MB) and ignores the rest. */
    if ((address & 0xFFu) != 0xF1u) {
        gwmapper_bank_write(address, value);
        return;
    }

    /* Only carts whose ROM overlaps the save window are wired to this
       register; Genesis Plus GX does not even install a handler for the
       others. Honouring it regardless would let a stray write protect or
       unmap save RAM the cart has no way of switching. */
    if (!gwsram_span || !gwsram_bankable)
        return;

    gwsram_protect = (value & 2u) != 0;
    gwsram_live = (value & 1u) ? gwsram_span : 0;
}
