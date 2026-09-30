/*
port/gwmapper.c — see gwmapper.h.

Detection and the bank-register semantics follow picodrive (pico/cart.c,
pico/carthw/carthw.c), which also treats every ROM over 4 MB as using this
mapper unless it knows better.
*/
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "gwmapper.h"

/* The cartridge half of the 68000 map ($000000-$7FFFFF) in 512 KB slots. */
#define WINDOW_SLOTS 16u
/* Carts up to this size fit the window without banking. */
#define LINEAR_LIMIT 0x400000u

struct gw_rom_map gw_rom;

static const unsigned char *st_head; /* bank 0 onwards */
static const unsigned char *st_tail; /* NULL unless the image is split */
static uint32_t st_head_len;         /* bytes at st_head when split */
static uint32_t rom_size;
static int mapper_on;

#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
unsigned int gwmapper_bank_writes;
#endif

/* The loader byte-swaps every 16-bit word, so file offset N is at [N ^ 1]. */
static int header_is(const unsigned char *rom, uint32_t off, const char *s)
{
    for (; *s; s++, off++)
        if (rom[off ^ 1u] != (unsigned char)*s)
            return 0;
    return 1;
}

/* Where ROM offset `off` lives. Offsets past the end of the image are not
   checked: the table has always pointed past a non power-of-two image (reads
   there are open bus on real hardware), and bank writes are range checked. */
static const unsigned char *bank_ptr(uint32_t off)
{
    if (st_tail && off >= st_head_len)
        return st_tail + (off - st_head_len);
    return st_head + off;
}

void gwmapper_set_storage(const unsigned char *head, size_t headLen,
                          const unsigned char *tail)
{
    st_head = head;
    st_head_len = (uint32_t)headLen;
    st_tail = tail;
}

void gwmapper_reset(const unsigned char *rom, size_t size, unsigned int addr_mask)
{
    uint32_t i;

    /* A split applies only to the image it was declared for; anything else is
       one contiguous block. The firmware never reboots between games, so a
       previous game's split must not leak into this one. */
    if (st_head != rom || !st_tail) {
        st_head = rom;
        st_tail = NULL;
        st_head_len = (uint32_t)size;
    }
    rom_size = (uint32_t)size;

    /* Power-on mapping, identical to ROM_DATA[A & rom_addr_mask]. */
    if (addr_mask < GWMAPPER_BANK_SIZE) {
        gw_rom.page_mask = addr_mask;
        for (i = 0; i < WINDOW_SLOTS; i++)
            gw_rom.bank[i] = st_head;
    } else {
        gw_rom.page_mask = GWMAPPER_BANK_SIZE - 1u;
        for (i = 0; i < WINDOW_SLOTS; i++)
            gw_rom.bank[i] = bank_ptr((i << GWMAPPER_BANK_SHIFT) & addr_mask);
    }

    mapper_on = rom_size > LINEAR_LIMIT ||
                (rom_size >= 0x108 && header_is(st_head, 0x100, "SEGA SSF"));

    if (mapper_on)
        printf("ROM mapper: bank switching on, %u banks of 512 KB%s\n",
               (unsigned)((rom_size + GWMAPPER_BANK_SIZE - 1u) >> GWMAPPER_BANK_SHIFT),
               st_tail ? ", split flash/PSRAM" : "");
    if (st_tail)
        printf("ROM storage: %u KB at %p, %u KB at %p\n",
               (unsigned)(st_head_len / 1024), (const void *)st_head,
               (unsigned)((rom_size - st_head_len) / 1024), (const void *)st_tail);
}

void gwmapper_bank_write(unsigned int address, unsigned int value)
{
    uint32_t slot, off;

    if (!mapper_on)
        return;
    /* $A130F3, F5 .. FF: odd addresses only. $A130F1 is save RAM control and
       never reaches here (slot 0 would be it, and slot 0 is fixed anyway). */
    if ((address & 0xFFF1u) != 0x30F1u)
        return;
    slot = (address & 0x0Eu) >> 1;
    if (slot == 0)
        return;
    off = (uint32_t)(value & 0xFFu) << GWMAPPER_BANK_SHIFT;
    if (off >= rom_size)
        return; /* no such bank: keep the old one, as picodrive does */
    gw_rom.bank[slot] = bank_ptr(off);
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
    gwmapper_bank_writes++;
#endif
}

int gwmapper_active(void)
{
    return mapper_on;
}
