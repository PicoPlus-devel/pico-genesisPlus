/*
port/scd_s68k_mem.h — the Sega CD sub 68000's view of memory.

The sub CPU is a second compile of the gwenesis 68000 core
(gwenesis/cpus/M68K/s68kcpu.c, GWENESIS_S68K=1). Every memory access that
core makes -- instruction fetch, data reads and writes, PC-relative reads --
lands in the inline functions below, which look the address up in a page
map in the style of PicoDrive's s68k_read8_map & co. (pico/memory.h):

    one entry per 128 KB slot of the 24-bit bus (128 entries)
    even entry  -> pointer to the slot's backing memory (byte-swapped
                   16-bit words, like every other 68000 memory here)
    odd entry   -> (handler index << 1) | 1, into gwcd_s68k_rh / _wh

PicoDrive tags a handler by shifting the function pointer right, which
cannot work on a Cortex-M: Thumb function pointers are odd. An index into
a small table costs one extra load on the slow path only.

Read and write each need one map, not two: every handler comes as an
8/16-bit pair. Both maps are 512 bytes and are allocated per CD game
(port/scd.c) so they cost the menu nothing.

Slot layout (PicoMemSetupCD() in scd/cd/memory.c, PicoDrive's layout):
    $000000-$07FFFF  PRG-RAM (slot 0 writes go through the write-protect handler)
    $080000-$0BFFFF  Word-RAM, 2M mode, or the 1M "dot" image (handlers)
    $0C0000-$0DFFFF  Word-RAM, 1M mode bank
    $FE0000-$FFFFFF  backup RAM ($FE), PCM and gate array ($FF) -- handlers
    everything else  unmapped (handler)
*/
#ifndef SCD_S68K_MEM_H
#define SCD_S68K_MEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S68K_SLOT_SHIFT 17u
#define S68K_SLOT_SIZE (1u << S68K_SLOT_SHIFT)
#define S68K_SLOT_MASK (S68K_SLOT_SIZE - 1u)
#define S68K_SLOTS 128u

typedef struct {
    unsigned int (*r8)(unsigned int a);
    unsigned int (*r16)(unsigned int a);
} gwcd_rhnd_t;

typedef struct {
    void (*w8)(unsigned int a, unsigned int d);
    void (*w16)(unsigned int a, unsigned int d);
} gwcd_whnd_t;

/* All four live in one block allocated per CD game (port/scd.c); the
   handler tables are filled as the CD code maps handlers in. */
extern uintptr_t *gwcd_s68k_rmap; /* [S68K_SLOTS] */
extern uintptr_t *gwcd_s68k_wmap; /* [S68K_SLOTS] */
extern gwcd_rhnd_t *gwcd_s68k_rh;
extern gwcd_whnd_t *gwcd_s68k_wh;

/* Idle-loop detection (gwenesis/cpus/M68K/s68kcpu.c): every write by the
   sub CPU that changes something bumps gwcd_s68k_writes -- any write to a
   handler, a write to memory only when the value differs, so a loop that
   calls a subroutine (pushing the same return address to the same place
   on every pass) still counts as idle; a read that changes something --
   the CDC's register and data ports move on with every read,
   scd/cd/memory.c -- bumps gwcd_s68k_sidefx. The counters live in
   port/scd.c. */
extern unsigned int gwcd_s68k_writes;
extern unsigned int gwcd_s68k_sidefx;
extern unsigned int gwcd_s68k_skipped; /* cycles an idle loop skipped, per run */
extern unsigned int gwcd_s68k_timeread; /* reads of the stopwatch, PCM position */
void gwcd_s68k_park(void);              /* port/scd.c */

#define S68K_SLOT(a) (((a) >> S68K_SLOT_SHIFT) & (S68K_SLOTS - 1u))
#define S68K_IS_HANDLER(v) __builtin_expect((v) & 1u, 0)

static inline unsigned int s68k_read8(unsigned int a)
{
    uintptr_t v = gwcd_s68k_rmap[S68K_SLOT(a)];
    if (S68K_IS_HANDLER(v))
        return gwcd_s68k_rh[v >> 1].r8(a & 0xFFFFFFu);
    return ((const uint8_t *)v)[(a & S68K_SLOT_MASK) ^ 1u];
}

static inline unsigned int s68k_read16(unsigned int a)
{
    uintptr_t v = gwcd_s68k_rmap[S68K_SLOT(a)];
    if (S68K_IS_HANDLER(v))
        return gwcd_s68k_rh[v >> 1].r16(a & 0xFFFFFEu);
    return *(const uint16_t *)((const uint8_t *)v + (a & S68K_SLOT_MASK & ~1u));
}

static inline unsigned int s68k_read32(unsigned int a)
{
    uintptr_t v = gwcd_s68k_rmap[S68K_SLOT(a)];
    if (S68K_IS_HANDLER(v)) {
        const gwcd_rhnd_t *h = &gwcd_s68k_rh[v >> 1];
        return (h->r16(a & 0xFFFFFEu) << 16) | h->r16((a + 2) & 0xFFFFFEu);
    }
    /* Same unaligned-word trick as FETCH32ROM: the two halves are stored
       as swapped 16-bit words. Like PicoDrive, a 32-bit access is assumed
       not to straddle two slots. */
    uint32_t w = *(const uint32_t *)((const uint8_t *)v + (a & S68K_SLOT_MASK & ~1u));
    return (w << 16) | (w >> 16);
}

static inline void s68k_write8(unsigned int a, unsigned int d)
{
    uintptr_t v = gwcd_s68k_wmap[S68K_SLOT(a)];
    uint8_t *p;
    if (S68K_IS_HANDLER(v)) {
        gwcd_s68k_writes++;
        gwcd_s68k_wh[v >> 1].w8(a & 0xFFFFFFu, d & 0xFFu);
        return;
    }
    p = (uint8_t *)v + ((a & S68K_SLOT_MASK) ^ 1u);
    if (*p != (uint8_t)d) {
        *p = (uint8_t)d;
        gwcd_s68k_writes++;
    }
}

static inline void s68k_write16(unsigned int a, unsigned int d)
{
    uintptr_t v = gwcd_s68k_wmap[S68K_SLOT(a)];
    uint16_t *p;
    if (S68K_IS_HANDLER(v)) {
        gwcd_s68k_writes++;
        gwcd_s68k_wh[v >> 1].w16(a & 0xFFFFFEu, d & 0xFFFFu);
        return;
    }
    p = (uint16_t *)((uint8_t *)v + (a & S68K_SLOT_MASK & ~1u));
    if (*p != (uint16_t)d) {
        *p = (uint16_t)d;
        gwcd_s68k_writes++;
    }
}

static inline void s68k_write32(unsigned int a, unsigned int d)
{
    /* Two word writes, high word first, as the 68000 does: a handler (the
       gate array in particular) must see them in bus order. */
    s68k_write16(a, d >> 16);
    s68k_write16(a + 2, d & 0xFFFFu);
}

#ifdef __cplusplus
}
#endif

#endif /* SCD_S68K_MEM_H */
