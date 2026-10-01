/*
port/gwpier.c — see gwpier.h. The register semantics are PicoDrive's
carthw_pier_*; only the way banks are mapped differs (the gwmapper page
table instead of PicoDrive's 64 KB memory maps).
*/
#include <stdio.h>
#include <string.h>

#include "gwmapper.h"
#include "gwsram.h"
#include "gwpier.h"
#include "scd.h"
#include "carthw/eeprom_spi.h"

int gwpier_active;
uint8_t *gwpier_eeprom;
int gwpier_eeprom_dirty;

static size_t rom_size;
static uint8_t *prot_page; /* 128 KB: the first 32 KB of the ROM, four times */
static uint8_t regs[8];
static int dump_prot;

/* The loader byte-swaps every 16-bit word, so file offset N is at [N ^ 1]. */
static int header_is(const unsigned char *r, uint32_t off, const char *s)
{
    for (; *s; s++, off++)
        if (r[off ^ 1u] != (unsigned char)*s)
            return 0;
    return 1;
}

static void map_bank(uint32_t target, uint32_t base)
{
    uint32_t i;

    if (base >= rom_size)
        return; /* no such bank: PicoDrive's have_bank() */
    for (i = 0; i < GWMAPPER_BANK_SIZE / GWMAPPER_SLOT_SIZE; i++)
        gw_rom.bank[(target >> GWMAPPER_SLOT_SHIFT) + i] = gwmapper_rom_at(base + i * GWMAPPER_SLOT_SIZE);
}

/* The protection's counted reads come through the save-RAM window, which
   Pier Solar (EEPROM, no save RAM) leaves free; gwcd_m68k_window_*() in
   port/scd.c hand them back here. */
static void set_window(int on)
{
    if (on) {
        gwsram_start = 0x010000;
        gwsram_live = 0x10000;
        gwcd_window_cd = GWCD_WINDOW_PIER;
    } else if (gwcd_window_cd == GWCD_WINDOW_PIER) {
        gwsram_start = 0;
        gwsram_live = 0;
        gwcd_window_cd = 0;
    }
}

/* carthw_pier_prot_mem_setup() + carthw_pier_statef() */
static void apply_state(void)
{
    uint32_t i;

    if (dump_prot) {
        for (i = 0; i < 0x400000u >> GWMAPPER_SLOT_SHIFT; i++)
            gw_rom.bank[i] = prot_page;
        set_window(1);
        return;
    }
    set_window(0);
    for (i = 0; i < 0x280000u >> GWMAPPER_SLOT_SHIFT; i++)
        gw_rom.bank[i] = gwmapper_rom_at(i * GWMAPPER_SLOT_SIZE);
    /* setup all banks */
    {
        uint8_t r0 = regs[0];
        gwpier_write8(0xa13001, 3);
        gwpier_write8(0xa13003, regs[1]);
        gwpier_write8(0xa13005, regs[2]);
        gwpier_write8(0xa13007, regs[3]);
        gwpier_write8(0xa13001, r0);
    }
}

void gwpier_write8(unsigned int a, unsigned int d)
{
    unsigned int a8 = a & 0x0f;

    if ((a & 0xffff00u) != 0xa13000u)
        return;
    regs[a8 / 2] = (uint8_t)d;
    switch (a8) {
    case 0x03:
        if (regs[0] & 2)
            map_bank(0x280000, (uint32_t)(d & 0xff) << 19);
        break;
    case 0x05:
        if (regs[0] & 2)
            map_bank(0x300000, (uint32_t)(d & 0xff) << 19);
        break;
    case 0x07:
        if (regs[0] & 2)
            map_bank(0x380000, (uint32_t)(d & 0xff) << 19);
        break;
    case 0x09:
        eeprom_spi_write((unsigned char)d);
        break;
    default:
        break;
    }
}

unsigned int gwpier_read8(unsigned int a)
{
    if (a == 0xa1300b)
        return eeprom_spi_read(a);
    return 0;
}

/* carthw_pier_prot_read8(): the reads that switch the protection off */
unsigned int gwpier_prot_read8(unsigned int a)
{
    unsigned int d = prot_page[(a & 0x7fff) ^ 1u];

    if (dump_prot > 0)
        dump_prot--;
    if (dump_prot == 0) {
        apply_state();
        printf("pier: dump protection off\n");
    }
    return d;
}

/* PicoDrive maps only byte reads to the counter; word reads see the page. */
unsigned int gwpier_prot_read16(unsigned int a)
{
    return *(const uint16_t *)(prot_page + (a & 0x1fffe));
}

void gwpier_release(void)
{
    set_window(0);
    gwcd_port_psram_free(prot_page);
    gwcd_port_psram_free(gwpier_eeprom);
    prot_page = NULL;
    gwpier_eeprom = NULL;
    gwpier_active = 0;
    gwpier_eeprom_dirty = 0;
}

int gwpier_reset(const unsigned char *r, size_t size)
{
    uint32_t i;

    gwpier_release();
    if (size < 0x620 || !header_is(r, 0x150, "PIER") || !header_is(r, 0x610, "Respect"))
        return 0;
    prot_page = gwcd_port_psram_alloc(GWMAPPER_SLOT_SIZE);
    gwpier_eeprom = gwcd_port_psram_alloc(GWPIER_EEPROM_SIZE);
    if (!prot_page || !gwpier_eeprom) {
        printf("pier: no PSRAM for the mapper, running it as a plain cartridge\n");
        gwpier_release();
        return 0;
    }
    rom_size = size;
    for (i = 0; i < GWMAPPER_SLOT_SIZE; i += 0x8000)
        memcpy(prot_page + i, gwmapper_rom_at(0), 0x8000);
    /* Both allocations come zeroed: PicoDrive's calloc'd EEPROM, so a .srm
       moves between the two unchanged. */
    gwpier_eeprom_dirty = 0;
    eeprom_spi_init(NULL);

    /* carthw_pier_startup() + carthw_pier_reset() */
    dump_prot = 3;
    regs[0] = 1;
    regs[1] = regs[2] = regs[3] = 0;
    gwpier_active = 1;
    apply_state();
    printf("pier: Pier Solar mapper, SPI EEPROM\n");
    return 1;
}
