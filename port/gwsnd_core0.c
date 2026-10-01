/*
port/gwsnd_core0.c — the sound seam the core calls (see gwsnd.h).

Sync mode is the upstream GWENESIS_AUDIO_ACCURATE=1 behavior: every seam
call forwards to the chip with its exact timestamp, so the chips catch up
sample-by-sample to the moment of each register access.

Offload mode (HSTX builds, GWSND_OFFLOAD=1) forwards writes as timestamped
FIFO events to the core1 engine (port/gwsnd_core1.c) and answers status
reads from the core0 timer shadow (port/gwsnd_shadow.c).

On the host harness, GEN_VERIFY_SHADOW=1 (environment variable) runs the
shadow in lockstep with the real chip in sync mode and aborts on the first
status divergence — this is the proof that offload-mode reads are exact.
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "gwenesis_bus.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwsnd.h"
#include "scd.h"

static int gwsnd_offload_active = 0;
static int gwsnd_cur_pal = 0;
static void (*gwsnd_frame_tap)(const int16_t *ym, const int16_t *psg, int samples);

/* Read by gwsnd_core1.c (drift trim runs on core1 in offload mode). */
int (*gwsnd_fill_query_fn)(void);

void gwsnd_set_frame_tap(void (*fn)(const int16_t *ym, const int16_t *psg, int samples))
{
    gwsnd_frame_tap = fn;
}

void gwsnd_set_fill_query(int (*fn)(void))
{
    gwsnd_fill_query_fn = fn;
}

#if GWSND_OFFLOAD
/* Implemented in port/gwsnd_core1.c */
int gwsnd_offload_start(int is_pal);
void gwsnd_offload_stop(void);
void gwsnd_offload_ym_write(unsigned int a, unsigned int v, int ts);
unsigned int gwsnd_offload_ym_read(int ts);
void gwsnd_offload_psg_write(unsigned int v, int ts);
void gwsnd_offload_frame_end(int sc, int is_pal);
void gwsnd_offload_line_tick(int sc);
#endif

#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
/* Host-only lockstep verification of the timer shadow. */
static int verify_shadow = -1;
static long long verify_reads = 0;

static int verify_enabled(void)
{
    if (verify_shadow < 0) {
        const char *v = getenv("GEN_VERIFY_SHADOW");
        verify_shadow = (v && v[0] == '1') ? 1 : 0;
    }
    return verify_shadow;
}
#else
static inline int verify_enabled(void) { return 0; }
#endif

void gwsnd_init(int is_pal, int offload)
{
    gwsnd_cur_pal = is_pal;
    gwsnd_resample_reset(is_pal);
#if GWSND_OFFLOAD
    gwsnd_offload_active = offload ? gwsnd_offload_start(is_pal) : 0;
    if (offload && !gwsnd_offload_active)
        printf("gwsnd: core1 offload unavailable, using sync mode\n");
#else
    if (offload)
        printf("gwsnd: offload requested but not compiled in, using sync mode\n");
    gwsnd_offload_active = 0;
#endif
    if (verify_enabled())
        gwsnd_shadow_reset();
}

void gwsnd_shutdown(void)
{
#if GWSND_OFFLOAD
    if (gwsnd_offload_active)
        gwsnd_offload_stop();
#endif
    gwsnd_offload_active = 0;
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
    if (verify_enabled())
        printf("GEN_VERIFY_SHADOW: %lld status reads verified, 0 divergences\n",
               verify_reads);
#endif
}

void GW_SRAM_FUNC(gwsnd_line_tick)(int system_clock)
{
#if GWSND_OFFLOAD
    if (gwsnd_offload_active) {
        gwsnd_offload_line_tick(system_clock);
        return;
    }
#endif
    (void)system_clock;
}

void gwsnd_set_pal(int is_pal)
{
    if (is_pal == gwsnd_cur_pal)
        return;
    gwsnd_cur_pal = is_pal;
    if (!gwsnd_offload_active) {
        /* offload mode applies it on core1 at the next frame boundary */
        gwsnd_resample_reset(is_pal);
    }
}

void GW_SRAM_FUNC(gwsnd_ym_write)(unsigned int a, unsigned int v, int target_mclk)
{
#if GWSND_OFFLOAD
    if (gwsnd_offload_active) {
        gwsnd_offload_ym_write(a, v, target_mclk);
        return;
    }
#endif
    YM2612Write(a, v, target_mclk);
    if (verify_enabled())
        gwsnd_shadow_write(a, v, target_mclk);
}

unsigned int GW_SRAM_FUNC(gwsnd_ym_read)(int target_mclk)
{
#if GWSND_OFFLOAD
    if (gwsnd_offload_active)
        return gwsnd_offload_ym_read(target_mclk);
#endif
    unsigned int real = YM2612Read(target_mclk);
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
    if (verify_enabled()) {
        gwsnd_shadow_run(target_mclk);
        unsigned int mine = gwsnd_shadow_status();
        verify_reads++;
        if (mine != real) {
            fprintf(stderr,
                    "GEN_VERIFY_SHADOW: status divergence after %lld reads: "
                    "shadow=%02x chip=%02x (ts=%d)\n",
                    verify_reads, mine, real, target_mclk);
            abort();
        }
    }
#endif
    return real;
}

void GW_SRAM_FUNC(gwsnd_psg_write)(unsigned int v, int target_mclk)
{
#if GWSND_OFFLOAD
    if (gwsnd_offload_active) {
        gwsnd_offload_psg_write(v, target_mclk);
        return;
    }
#endif
    gwenesis_SN76489_Write(v, target_mclk);
}

void gwsnd_frame_end(int system_clock)
{
#if GWSND_OFFLOAD
    if (gwsnd_offload_active) {
        gwsnd_offload_frame_end(system_clock, gwsnd_cur_pal);
        return;
    }
#endif
    /* Top both chips up to the end of the frame. Note: for any target t,
       chip_index ends at floor(t / AUDIO_FREQ_DIVISOR) regardless of the
       catch-up path taken, so both buffers are equally full here. */
    gwenesis_SN76489_run(system_clock);
    ym2612_run(system_clock);

    if (verify_enabled()) {
        gwsnd_shadow_run(system_clock);
        gwsnd_shadow_frame_reset();
    }

    int samples = ym2612_index;
    if (gwsnd_frame_tap)
        gwsnd_frame_tap(gwenesis_ym2612_buffer, gwenesis_sn76489_buffer, samples);
#if GENESIS_SEGACD
    /* Sync mode has no core1 task to prefetch CD-DA; top the ring up here,
       a frame's worth of sectors plus slack. */
    if (gwcd_bus_mode != GWCD_BUS_CART)
        for (int i = 0; i < 4; i++)
            gwcd_cdda_service();
#endif
    gwsnd_resample_mix_feed(0, samples);
    if (gwsnd_fill_query_fn)
        gwsnd_set_fill_permille(gwsnd_fill_query_fn());

    /* Rebase the chip clocks for the next frame (the frame loop rebases
       system_clock and zclk the same way). */
    ym2612_clock = 0;
    ym2612_index = 0;
    sn76489_clock = 0;
    sn76489_index = 0;
}
