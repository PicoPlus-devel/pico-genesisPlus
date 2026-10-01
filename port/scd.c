/*
port/scd.c — the Sega CD session around the vendored PicoDrive CD code.

  - the Pico / PicoIn shims and the sub 68000 glue PicoDrive keeps in
    pico/cd/sek.c (cycle counters, interrupts, reset);
  - the map-building API scd/memory.h declares, on the sub CPU's page maps
    (port/scd_s68k_mem.h) and on the main CPU's gw_rom table plus a CD write
    map;
  - the main-CPU hooks the gwenesis bus and VDP call in CD mode;
  - allocation and teardown of everything a CD game uses.

Memory. The big blocks (mcd_state with BIOS, PRG-RAM, Word-RAM, PCM RAM:
~1.1 MB; the CDC's sector buffer; the drive's TOC; a 128 KB page of zeros
for unmapped main-CPU slots) go to PSRAM through gwcd_port_psram_alloc().
The page maps and the graphics ASIC's tables (~3.8 KB, touched on every
sub-CPU memory access or ASIC pixel) come from the SRAM heap when it has
room, PSRAM otherwise. Nothing is static beyond a few words, so a cartridge
session and the menu pay nothing.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico_int.h"   /* scd/pico_int.h */
#include "memory.h"     /* scd/memory.h */
#include "cd/genplus_macros.h"
#include "cd/cdd.h"
#include "gwenesis_bus.h" /* load_cartridge */
#include "cd/megasd.h"
#include "gwmapper.h"
#include "gwpier.h"
#include "gwsram.h"
#include "scd_s68k_mem.h"

/* Sub-CPU API from s68kcpu.c (the second compile of the 68000 core). */
void s68k_run(unsigned int cycles);
void s68k_init(void);
void s68k_pulse_reset(void);
void s68k_set_irq(unsigned int int_level);
void s68k_set_int_ack_callback(int (*callback)(int int_level));

/* ------------------------------ shims ---------------------------------- */

int gwcd_bus_mode = GWCD_BUS_CART;
unsigned int gwcd_cd_base;
int gwcd_window_cd;

struct gwcd_pico_shim Pico;
struct gwcd_picoin_shim PicoIn;
void (*PicoResetHook)(void);

#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
unsigned int gwcd_log_mask; /* host harness: GEN_SCD_LOG */
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
void (*gwcd_s68k_sample)(unsigned int pc, unsigned int cycles);
void (*gwcd_m68k_sample)(unsigned int pc, unsigned int mclk);
unsigned int gwcd_host_s68k_runs;
/* PRG-RAM as the sub CPU sees it (16-bit words in host order), for the
   harness's profile dump */
const uint8_t *gwcd_host_prg_ram(void)
{
    return Pico_mcd ? Pico_mcd->prg_ram : NULL;
}
#endif
#endif

unsigned int gwcd_mclk_base;
struct gwcd_stats_t gwcd_stats;

void gwcd_stats_take(unsigned int *s68k_run, unsigned int *s68k_idle,
                     unsigned int *s68k_halted, unsigned int *m68k_idle)
{
    *s68k_run = gwcd_stats.s68k_run;
    *s68k_idle = gwcd_stats.s68k_idle;
    *s68k_halted = gwcd_stats.s68k_halted;
    *m68k_idle = gwcd_stats.m68k_idle;
    memset(&gwcd_stats, 0, sizeof(gwcd_stats));
}
unsigned int SekCycleCntS68k;
unsigned int SekCycleAimS68k;
int gwcd_s68k_in_run;
unsigned int gwcd_s68k_writes, gwcd_s68k_sidefx, gwcd_s68k_skipped;
int gwcd_m68k_idle;               /* main-CPU wait loops skipped: Sega CD sessions */
unsigned int gwcd_m68k_skipped;
unsigned int gwcd_s68k_timeread;
int gwcd_s68k_parked;

/* The sub CPU is in a wait loop that only something outside it can end
   (s68kcpu.c): put it to sleep the way PicoDrive's poll detection does, so
   the scheduler stops running it at all. PicoDrive wakes its own sleeps on
   an interrupt, a graphics-chip event or a write to the one register
   polled; a loop found here may wait on anything the sub CPU sees, so any
   interrupt, any CD event and any main-CPU write to the gate array,
   PRG-RAM or Word-RAM wakes it (gwcd_s68k_wake), and so does the end of
   every frame, as a safety net. */
void gwcd_s68k_park(void)
{
    Pico_mcd->m.state_flags |= PCD_ST_S68K_POLL;
    gwcd_s68k_parked = 1;
}

void gwcd_s68k_unpark(void)
{
    gwcd_s68k_parked = 0;
    Pico_mcd->m.state_flags &= ~PCD_ST_S68K_POLL;
    Pico_mcd->m.s68k_poll_cnt = 0;
}

/* State the vendored files keep private (their types are local to them),
   allocated here: scd/cd/cdc.c and scd/cd/gfx.c. */
extern const unsigned int gwcd_cdc_size, gwcd_gfx_size;
extern void *gwcd_cdc;
extern void *gwcd_gfx;

PICO_INTERNAL_ASM void memcpy16bswap(unsigned short *dest, void *src, int count)
{
    unsigned char *src_ = src;

    for (; count; count--, src_ += 2)
        *dest++ = (src_[0] << 8) | src_[1];
}

/* --------------------------- cycle glue -------------------------------- */

void GW_SRAM_FUNC(gwcd_m68k_end_run)(unsigned int after_mclk)
{
    /* Only meaningful inside m68k_run(); outside it cycle_end is stale and
       the next m68k_run() sets it afresh anyway. */
    if (m68k.cycle_end > m68k.cycles + after_mclk)
        m68k.cycle_end = m68k.cycles + after_mclk;
}

void GW_SRAM_FUNC(gwcd_s68k_end_run)(unsigned int after)
{
    int left;

    if (!gwcd_s68k_in_run)
        return;
    left = (int)(s68k.cycle_end - s68k.cycles);
    if (left > (int)after) {
        SekCycleCntS68k -= left - after;
        s68k.cycle_end = s68k.cycles + after;
    }
}

/* ------------------------ sub-CPU interrupts --------------------------- */
/* pico/cd/sek.c, Musashi flavour. */

static int new_irq_level(int level)
{
    int level_new = 0, irqs;
    Pico_mcd->m.s68k_pend_ints &= ~(1 << level);
    if (level == 2) // clear pending bit
        Pico_mcd->m.state_flags &= ~PCD_ST_S68K_IFL2;
    irqs = Pico_mcd->m.s68k_pend_ints;
    irqs &= Pico_mcd->s68k_regs[0x33];
    while ((irqs >>= 1))
        level_new++;

    return level_new;
}

static int SekIntAckMS68k(int level)
{
    int level_new = new_irq_level(level);
    elprintf(EL_INTS, "s68kACK %i -> %i", level, level_new);
    s68k.int_level = level_new << 8;
    return M68K_INT_ACK_AUTOVECTOR;
}

PICO_INTERNAL void SekInitS68k(void)
{
    memset(&s68k, 0, sizeof(s68k));
    s68k_init();
    s68k_set_int_ack_callback(SekIntAckMS68k);
}

PICO_INTERNAL int SekResetS68k(void)
{
    s68k.sp[0] = 0;
    s68k_set_irq(0);
    s68k_pulse_reset();
    /* The reset consumed cycles in s68k.cycles, which outside a run belongs
       to nobody: the counters live in SekCycleCntS68k. */
    s68k.cycles = s68k.cycle_end = 0;
    return 0;
}

PICO_INTERNAL int SekInterruptS68k(int irq)
{
    int irqs, real_irq = 1;
    gwcd_s68k_wake();
    Pico_mcd->m.s68k_pend_ints |= 1 << irq;
    irqs = Pico_mcd->m.s68k_pend_ints >> 1;
    while ((irqs >>= 1))
        real_irq++;

    // avoid m68k_set_irq() for delaying to work
    s68k.int_level = real_irq << 8;
    return 0;
}

void SekInterruptClearS68k(int irq)
{
    int level_new = new_irq_level(irq);

    s68k.int_level = level_new << 8;
}

/* ---------------------------- page maps -------------------------------- */

#define GWCD_MAX_HANDLERS 24
#define MAIN_SLOTS GWMAPPER_SLOTS /* 64 x 128 KB = $000000-$7FFFFF */

uintptr_t *gwcd_s68k_rmap;
uintptr_t *gwcd_s68k_wmap;
gwcd_rhnd_t *gwcd_s68k_rh;
gwcd_whnd_t *gwcd_s68k_wh;

/* Main CPU, CD session: reads of a plain slot go through gw_rom (the
   FETCH*ROM fast path); m_wmap[] steers writes (same encoding as the sub
   maps); m_rh_idx[] names the read handler of a handler slot, which the
   CPU reaches through the save-RAM window (only the cell image uses it). */
struct gwcd_maps {
    uintptr_t s_rmap[S68K_SLOTS];
    uintptr_t s_wmap[S68K_SLOTS];
    uintptr_t m_wmap[MAIN_SLOTS];
    gwcd_rhnd_t rh[GWCD_MAX_HANDLERS];
    gwcd_whnd_t wh[GWCD_MAX_HANDLERS];
    uint8_t m_rh_idx[MAIN_SLOTS];
    uint8_t n_rh, n_wh;
};

static struct gwcd_maps *maps;
static int maps_in_psram;
static uint8_t *zero_page;     /* 128 KB of zeros: unmapped main-CPU slots */

static int handler_r(u32 (*r8)(u32), u32 (*r16)(u32))
{
    int i;
    for (i = 0; i < maps->n_rh; i++)
        if (maps->rh[i].r8 == r8 && maps->rh[i].r16 == r16)
            return i;
    if (maps->n_rh >= GWCD_MAX_HANDLERS) {
        printf("scd: out of read handler slots\n");
        return 0;
    }
    maps->rh[i].r8 = r8;
    maps->rh[i].r16 = r16;
    return maps->n_rh++;
}

static int handler_w(void (*w8)(u32, u32), void (*w16)(u32, u32))
{
    int i;
    for (i = 0; i < maps->n_wh; i++)
        if (maps->wh[i].w8 == w8 && maps->wh[i].w16 == w16)
            return i;
    if (maps->n_wh >= GWCD_MAX_HANDLERS) {
        printf("scd: out of write handler slots\n");
        return 0;
    }
    maps->wh[i].w8 = w8;
    maps->wh[i].w16 = w16;
    return maps->n_wh++;
}

#define HANDLER_ENTRY(i) (((uintptr_t)(i) << 1) | 1u)

/* Slots entirely inside [start, end]. PicoDrive maps in 64 KB units, and a
   range that covers part of a 128 KB slot here would have mapped only part
   of it there; such a slot is left alone (only $0E0000-$0EFFFF in 1M mode,
   which PicoDrive points past the end of the bank anyway). */
static int slot_range(u32 start, u32 end, u32 *first, u32 *last)
{
    u32 f = (start + S68K_SLOT_SIZE - 1) >> S68K_SLOT_SHIFT;
    u32 l = ((end + 1) >> S68K_SLOT_SHIFT);
    if (l <= f)
        return 0;
    *first = f;
    *last = l - 1;
    return 1;
}

/* The cell-image window through the save-RAM overlay test (port/gwsram.h).
   A disc boot has no cartridge, so the window is free; in Mode 1 it is only
   free when the cartridge has no save RAM, and a cartridge that has some
   keeps it (the cell image then reads 0 -- a combination no known game
   uses). */
static void main_window(u32 start, u32 span)
{
    if (span) {
        if (gwsram_span || gwcd_window_cd == GWCD_WINDOW_PIER) {
            printf("scd: cartridge save RAM holds the window, cell image not mapped\n");
            return;
        }
        gwsram_start = start;
        gwsram_live = span;
        gwcd_window_cd = GWCD_WINDOW_CELL;
    } else if (gwcd_window_cd == GWCD_WINDOW_CELL) {
        gwsram_start = 0;
        gwsram_live = 0;
        gwcd_window_cd = 0;
    }
}

static void main_slot_ram(u32 slot, uint8_t *p)
{
    gw_rom.bank[slot] = p;
    maps->m_wmap[slot] = (uintptr_t)p;
    if (gwcd_window_cd == GWCD_WINDOW_CELL && (slot << S68K_SLOT_SHIFT) == gwsram_start)
        main_window(0, 0);
}

void cpu68k_map_all_ram(u32 start_addr, u32 end_addr, void *ptr, int is_sub)
{
    u32 s, first, last;
    uint8_t *p = ptr;

    if (!slot_range(start_addr, end_addr, &first, &last))
        return;
    for (s = first; s <= last; s++, p += S68K_SLOT_SIZE) {
        if (is_sub) {
            maps->s_rmap[s] = (uintptr_t)p;
            maps->s_wmap[s] = (uintptr_t)p;
        } else if (s < MAIN_SLOTS) {
            main_slot_ram(s, p);
        }
    }
}

void cpu68k_map_read_mem(u32 start_addr, u32 end_addr, void *ptr, int is_sub)
{
    u32 s, first, last;
    uint8_t *p = ptr;

    if (!slot_range(start_addr, end_addr, &first, &last))
        return;
    for (s = first; s <= last; s++, p += S68K_SLOT_SIZE) {
        if (is_sub)
            maps->s_rmap[s] = (uintptr_t)p;
        else if (s < MAIN_SLOTS)
            gw_rom.bank[s] = p;
    }
}

/* A NULL pair leaves that direction's mapping alone. */
void cpu68k_map_all_funcs(u32 start_addr, u32 end_addr,
    u32 (*r8)(u32), u32 (*r16)(u32), void (*w8)(u32, u32), void (*w16)(u32, u32),
    int is_sub)
{
    u32 s, first, last;
    int ri = -1, wi = -1;

    if (!slot_range(start_addr, end_addr, &first, &last))
        return;
    if (r8 && r16)
        ri = handler_r(r8, r16);
    if (w8 && w16)
        wi = handler_w(w8, w16);
    for (s = first; s <= last; s++) {
        if (is_sub) {
            if (ri >= 0)
                maps->s_rmap[s] = HANDLER_ENTRY(ri);
            if (wi >= 0)
                maps->s_wmap[s] = HANDLER_ENTRY(wi);
        } else if (s < MAIN_SLOTS) {
            if (ri >= 0) {
                /* FETCH*ROM (instruction fetch, DMA) sees zeros; data reads
                   go through the window to the handler. */
                gw_rom.bank[s] = zero_page;
                maps->m_rh_idx[s] = (uint8_t)ri;
            }
            if (wi >= 0)
                maps->m_wmap[s] = HANDLER_ENTRY(wi);
        }
    }
    if (!is_sub && ri >= 0)
        main_window(first << S68K_SLOT_SHIFT, (last - first + 1) << S68K_SLOT_SHIFT);
}

static void m68k_unmapped_write(u32 a, u32 d)
{
    (void)a;
    (void)d;
}

void m68k_map_unmap(u32 start_addr, u32 end_addr)
{
    u32 s, first, last;
    int wi;

    if (!slot_range(start_addr, end_addr, &first, &last))
        return;
    wi = handler_w(m68k_unmapped_write, m68k_unmapped_write);
    for (s = first; s <= last && s < MAIN_SLOTS; s++) {
        gw_rom.bank[s] = zero_page;
        maps->m_wmap[s] = HANDLER_ENTRY(wi);
        if (gwcd_window_cd == GWCD_WINDOW_CELL && (s << S68K_SLOT_SHIFT) == gwsram_start)
            main_window(0, 0);
    }
}

/* ------------------------ main-CPU bus hooks --------------------------- */

void GW_SRAM_FUNC(gwcd_m68k_write8)(unsigned int a, unsigned int d)
{
    uintptr_t v;

    /* The cartridge side: ROM, apart from the MegaSD registers (MD+). */
    if (!(gwcd_bus_mode & GWCD_BUS_SCD) || a < gwcd_cd_base) {
        if (gwcd_bus_mode & GWCD_BUS_MDPLUS)
            gwcd_msd_write8(a, d);
        return;
    }
    v = maps->m_wmap[(a >> S68K_SLOT_SHIFT) & (MAIN_SLOTS - 1)];
    if (S68K_IS_HANDLER(v))
        maps->wh[v >> 1].w8(a, d & 0xFF);
    else
        ((uint8_t *)v)[(a & S68K_SLOT_MASK) ^ 1u] = (uint8_t)d;
    gwcd_s68k_wake();
}

void GW_SRAM_FUNC(gwcd_m68k_write16)(unsigned int a, unsigned int d)
{
    uintptr_t v;

    if (!(gwcd_bus_mode & GWCD_BUS_SCD) || a < gwcd_cd_base) {
        if (gwcd_bus_mode & GWCD_BUS_MDPLUS)
            gwcd_msd_write16(a, d);
        return;
    }
    v = maps->m_wmap[(a >> S68K_SLOT_SHIFT) & (MAIN_SLOTS - 1)];
    if (S68K_IS_HANDLER(v))
        maps->wh[v >> 1].w16(a & ~1u, d & 0xFFFF);
    else
        *(uint16_t *)((uint8_t *)v + (a & S68K_SLOT_MASK & ~1u)) = (uint16_t)d;
    gwcd_s68k_wake();
}

unsigned int gwcd_m68k_window_read8(unsigned int a)
{
    if (gwcd_window_cd == GWCD_WINDOW_PIER)
        return gwpier_prot_read8(a);
    return maps->rh[maps->m_rh_idx[(a >> S68K_SLOT_SHIFT) & (MAIN_SLOTS - 1)]].r8(a);
}

unsigned int gwcd_m68k_window_read16(unsigned int a)
{
    if (gwcd_window_cd == GWCD_WINDOW_PIER)
        return gwpier_prot_read16(a);
    return maps->rh[maps->m_rh_idx[(a >> S68K_SLOT_SHIFT) & (MAIN_SLOTS - 1)]].r16(a & ~1u);
}

void gwcd_m68k_window_write8(unsigned int a, unsigned int d)
{
    if (gwcd_window_cd == GWCD_WINDOW_PIER)
        return; /* ROM */
    gwcd_m68k_write8(a, d);
}

void gwcd_m68k_window_write16(unsigned int a, unsigned int d)
{
    if (gwcd_window_cd == GWCD_WINDOW_PIER)
        return;
    gwcd_m68k_write16(a, d);
}

/* VDP DMA from 68000 space in a CD session (PicoDrive's DmaSlow):
   Word-RAM, 2M or 1M, is read one word late -- the DMA sees source - 2 --
   and wraps within its 128 KB bank; the cell image is read through the cell
   mapping, also a word late. BIOS and the PRG-RAM window read as they sit
   in the page table (PicoDrive's check order never reaches its PRG-RAM
   case, so it applies no lag there either). */
#include "cd/cell_map.c"

unsigned int gwcd_dma_read16(unsigned int src)
{
    u8 r3;
    u32 s;

    src &= 0xFFFFFE;
    if ((src & 0xFC0000) != gwcd_cd_base + 0x200000)
        return FETCH16ROM(src);

    r3 = Pico_mcd->s68k_regs[3];
    s = (src & ~0x1FFFFu) | ((src - 2) & 0x1FFFFu);
    if (!(r3 & 4)) /* 2M */
        return *(const u16 *)(Pico_mcd->word_ram2M + (src & 0x20000) + (s & 0x1FFFF));
    if ((src & 0xFE0000) < gwcd_cd_base + 0x220000) /* 1M bank */
        return *(const u16 *)(Pico_mcd->word_ram1M[r3 & 1] + (s & 0x1FFFF));
    /* cell image */
    s = src - 2;
    s = (cell_map(s >> 2) << 2) | (s & 2);
    return *(const u16 *)(Pico_mcd->word_ram1M[r3 & 1] + (s & 0x1FFFF));
}

/* ------------------------------ session -------------------------------- */

static void *alloc_hot(size_t size, int *in_psram)
{
    /* The SRAM heap first: these are touched on every sub-CPU memory access
       or ASIC pixel. PSRAM when the heap cannot take them. malloc may not be
       the SDK's panicking one here: this runs where a failure is expected
       and handled (see the PICO_MALLOC_PANIC note in the memory file). */
    void *p = NULL;
#if !defined(GWENESIS_HOST) || GWENESIS_HOST == 0
    extern size_t gwcd_port_sram_free(void);
    if (gwcd_port_sram_free() > size + 4096)
#endif
        p = calloc(1, size);
    *in_psram = 0;
    if (!p) {
        p = gwcd_port_psram_alloc(size);
        *in_psram = 1;
    }
    return p;
}

static void free_hot(void *p, int in_psram)
{
    if (!p)
        return;
    if (in_psram)
        gwcd_port_psram_free(p);
    else
        free(p);
}

static int gfx_in_psram;
static mcd_state *mcd_mem;
static int bios_region;

static void free_all(void)
{
    if (gwcd_cdc) {
        gwcd_port_psram_free(gwcd_cdc);
        gwcd_cdc = NULL;
    }
    free_hot(gwcd_gfx, gfx_in_psram);
    gwcd_gfx = NULL;
    free_hot(maps, maps_in_psram);
    maps = NULL;
    gwcd_s68k_rmap = gwcd_s68k_wmap = NULL;
    gwcd_s68k_rh = NULL;
    gwcd_s68k_wh = NULL;
    if (zero_page) {
        gwcd_port_psram_free(zero_page);
        zero_page = NULL;
    }
    if (mcd_mem) {
        gwcd_port_psram_free(mcd_mem);
        mcd_mem = NULL;
    }
    Pico_mcd = NULL;
}

int gwcd_bios_region(void)
{
    return bios_region;
}

int gwcd_scd_start(const uint8_t *bios, size_t bios_size,
                   const uint8_t *cart, size_t cart_size)
{
    size_t i;
    extern unsigned short gwenesis_vdp_status;

    gwcd_stop();
    if (!gwcd_disc_is_open()) {
        printf("scd: no disc mounted\n");
        return -1;
    }
    bios_region = gwcd_bios_image_region(bios, bios_size);
    if (!bios_region) {
        printf("scd: not a Sega CD BIOS\n");
        return -1;
    }

    mcd_mem = gwcd_port_psram_alloc(sizeof(mcd_state));
    zero_page = gwcd_port_psram_alloc(S68K_SLOT_SIZE);
    gwcd_cdc = gwcd_port_psram_alloc(gwcd_cdc_size);
    maps = alloc_hot(sizeof(*maps), &maps_in_psram);
    gwcd_gfx = alloc_hot(gwcd_gfx_size, &gfx_in_psram);
    if (!mcd_mem || !zero_page || !gwcd_cdc || !maps || !gwcd_gfx) {
        printf("scd: out of memory (state %u KB)\n", (unsigned)(sizeof(mcd_state) / 1024));
        free_all();
        return -1;
    }
    printf("scd: state %u KB in PSRAM, maps %u B in %s, gfx %u B in %s\n",
           (unsigned)(sizeof(mcd_state) / 1024), (unsigned)sizeof(*maps),
           maps_in_psram ? "PSRAM" : "SRAM", gwcd_gfx_size,
           gfx_in_psram ? "PSRAM" : "SRAM");

    gwcd_s68k_rmap = maps->s_rmap;
    gwcd_s68k_wmap = maps->s_wmap;
    gwcd_s68k_rh = maps->rh;
    gwcd_s68k_wh = maps->wh;

    memset(&Pico, 0, sizeof(Pico));
    memset(&PicoIn, 0, sizeof(PicoIn));
    PicoIn.opt = POPT_EN_MCD_PCM | POPT_EN_MCD_CDDA | POPT_EN_MCD_GFX;
    PicoIn.mcdTrayOpen = gwcd_disc_tray_opened; /* multi-disc games */
    PicoResetHook = NULL;
    if (cart) {
        /* Mode 1: the cartridge the caller has loaded sets the region */
        Pico.rom = cart;
        Pico.romsize = (unsigned int)cart_size;
        Pico.m.pal = gwenesis_vdp_status & 1;
        gwcd_cd_base = 0x400000;
    } else {
        Pico.m.pal = bios_region == 'E';
        gwcd_cd_base = 0;
    }

    /* PicoDrive loads the BIOS through its cartridge loader, which byte-swaps
       every 16-bit word; do the same so it matches the rest of 68000 memory
       here. */
    Pico_mcd = mcd_mem;
    PicoCreateMCD((unsigned char *)bios, (int)bios_size);
    for (i = 0; i + 1 < sizeof(Pico_mcd->bios); i += 2) {
        u8 t = Pico_mcd->bios[i];
        Pico_mcd->bios[i] = Pico_mcd->bios[i + 1];
        Pico_mcd->bios[i + 1] = t;
    }

    if (gwcd_audio_start(1) != 0) {
        printf("scd: out of memory (audio)\n");
        free_all();
        return -1;
    }

    gwcd_mclk_base = 0;
    SekCycleCntS68k = SekCycleAimS68k = 0;
    gwcd_s68k_in_run = 0;
    PicoInitMCD();
    gwcd_bus_mode = GWCD_BUS_SCD;
    gwcd_m68k_idle = GWCD_IDLE_SKIP;

    /* A disc boot: the main CPU sees the BIOS as its cartridge.
       load_cartridge() points the page table at it and set_region() takes
       the console region from its header ($1F0: U, E or J). In Mode 1 the
       caller has loaded the real cartridge instead. PicoMemSetupCD() lays
       out the Mega-CD side in gwcd_power_on(). */
    if (!cart) {
        gwmapper_set_storage(Pico_mcd->bios, sizeof(Pico_mcd->bios), NULL);
        load_cartridge(Pico_mcd->bios, sizeof(Pico_mcd->bios));
    }
    printf("scd: %s, BIOS region %c\n", cart ? "Mode 1 (cartridge + Sega CD)" : "disc boot",
           bios_region);
    return 0;
}

void gwcd_power_on(void)
{
    if (!(gwcd_bus_mode & GWCD_BUS_SCD))
        return;
    main_window(0, 0);
    /* Maps first: PicoPowerMCD() resets the sub CPU, which fetches its
       vectors through them. It then puts the gate array in the state
       PicoMemSetupCD() just mapped (2M Word-RAM to the main CPU, sub CPU
       in reset), as PicoDrive's load-then-power order does. */
    PicoMemSetupCD();
    PicoPowerMCD();
    PicoMCDPrepare();
    pcd_prepare_frame();
    Pico.sv.changed = 0;
}

void gwcd_stop(void)
{
    if (gwcd_bus_mode & GWCD_BUS_SCD) {
        if (Pico_mcd)
            PicoExitMCD();
        main_window(0, 0);
    }
    if (gwcd_bus_mode & GWCD_BUS_MDPLUS)
        gwcd_msd_stop();
    gwcd_bus_mode = GWCD_BUS_CART;
    gwcd_m68k_idle = 0;
    gwcd_cd_base = 0;
    gwcd_audio_stop();
    free_all();
}

/* ------------------------- frame loop hooks ---------------------------- */

void gwcd_frame_start(void)
{
    if (gwcd_bus_mode & GWCD_BUS_SCD)
        pcd_prepare_frame();
}

void gwcd_frame_end(unsigned int system_clock, int is_pal)
{
    if (gwcd_bus_mode & GWCD_BUS_SCD) {
        if (Pico.m.pal != is_pal) {
            Pico.m.pal = is_pal;
            PicoMCDPrepare();
        }
        gwcd_pcm_frame_end(is_pal);
        gwcd_disc_frame_end();
        gwcd_s68k_wake(); /* a parked sub CPU sleeps a frame at most */
    }
    if (gwcd_bus_mode & GWCD_BUS_MDPLUS)
        gwcd_msd_frame_end(is_pal);
    /* The core rebases m68k.cycles by the frame right after this; moving
       the base up by the same amount keeps SekCyclesDone() continuous. */
    gwcd_mclk_base += system_clock;
}

/* ----------------------------- backup RAM ------------------------------ */

uint8_t *gwcd_bram(void)
{
    return Pico_mcd ? Pico_mcd->bram : NULL;
}

size_t gwcd_bram_size(void)
{
    return Pico_mcd ? sizeof(Pico_mcd->bram) : 0;
}

int gwcd_bram_dirty(void)
{
    return Pico.sv.changed;
}

void gwcd_bram_clean(void)
{
    Pico.sv.changed = 0;
}
