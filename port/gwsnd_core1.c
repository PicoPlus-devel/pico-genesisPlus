/*
port/gwsnd_core1.c — HSTX-only core1 sound engine (GWSND_OFFLOAD=1).

Core0 (68000/Z80/VDP) never synthesizes audio in this mode: every sound
register write becomes a timestamped event in an SPSC FIFO, and this
engine — running as the pico_hdmi driver's core1 background task between
scanout interrupts — replays them against the real chips with the exact
same catch-up semantics the sync path uses, then mixes, resamples and
pushes samples to the sink incrementally.

Cross-core rules kept here:
  - hstx_push_audio_sample() is called from core1 only (its DI ring is
    consumed by the core1 DMA IRQ — single-core SPSC, safe).
  - I2S and the VU meter must stay on core0: samples for them go through
    the bridge ring, drained once per frame by gwsnd_bridge_drain() from
    the emulate loop.
  - The fill-level query (drift trim) runs on core1; it must only read
    volatile counters (DI queue level / I2S ring indices) — both do.
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/time.h"

#include "gwenesis_bus.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwsnd.h"
#include "gwsnd_fifo.h"
#include "video_output.h"
#include "scd.h"

/* implemented in gwsnd_core0.c */
extern int (*gwsnd_fill_query_fn)(void);

static gwsnd_fifo_t fifo;
static volatile uint32_t fifo_highwater; /* max level seen (core0 writes) */
static volatile int engine_attached;
static volatile int in_task;
static int consumed;     /* samples already mixed+resampled this frame */
static int engine_pal = 0;

/* Per-line synthesis watermark, published by core0 from gwsnd_line_tick:
   bits 21..0 = master-clock position both CPUs have reached this frame
   (max 1,070,460 < 2^22), bits 31..22 = frame epoch. Core1 may run the
   chips up to the watermark once the FIFO is fully drained — this keeps
   samples flowing continuously (a whole-frame burst overshoots the DI
   queue's drop watermark and chops the audio). The epoch guards against
   applying a stale pre-FRAME_END watermark to freshly reset chips. */
#define GWSND_WM_CLOCK_BITS 22
static volatile uint32_t line_watermark;
static uint32_t core0_epoch; /* core0 writes (frame_end) */
static uint32_t core1_epoch; /* core1 writes (FRAME_END event) */

/* ---------------- core1 -> core0 sample bridge (I2S / VU) ------------- */

/* Stereo frames, power of two. Drained per scanline by gwsnd_line_tick,
   so it holds a few samples at a time; 1024 (4 KB) is already generous. */
#define GWSND_BRIDGE_LEN 1024
static int16_t *bridge; /* [GWSND_BRIDGE_LEN][2] */
static volatile uint32_t bridge_head, bridge_tail;
static volatile uint32_t bridge_drops; /* silently discarded samples */

void GW_SRAM_FUNC(gwsnd_bridge_push)(int16_t l, int16_t r)
{
    if (!bridge)
        return;
    uint32_t head = bridge_head;
    if (head - bridge_tail >= GWSND_BRIDGE_LEN) {
        bridge_drops++; /* core0 not draining (menu open) — drop */
        return;
    }
    bridge[(head & (GWSND_BRIDGE_LEN - 1)) * 2] = l;
    bridge[(head & (GWSND_BRIDGE_LEN - 1)) * 2 + 1] = r;
    gwsnd_dmb();
    bridge_head = head + 1;
}

static void (*bridge_sink)(int16_t l, int16_t r);

void gwsnd_set_bridge_sink(void (*fn)(int16_t l, int16_t r))
{
    bridge_sink = fn;
}

void GW_SRAM_FUNC(gwsnd_bridge_drain)(void)
{
    if (!bridge)
        return;
    void (*fn)(int16_t, int16_t) = bridge_sink;
    uint32_t tail = bridge_tail;
    while (tail != bridge_head) {
        int16_t l = bridge[(tail & (GWSND_BRIDGE_LEN - 1)) * 2];
        int16_t r = bridge[(tail & (GWSND_BRIDGE_LEN - 1)) * 2 + 1];
        if (fn)
            fn(l, r);
        gwsnd_dmb();
        bridge_tail = ++tail;
    }
}

/* --------------------------- core1 engine ----------------------------- */

static inline void feed_up_to(int n)
{
    if (n > consumed) {
        gwsnd_resample_mix_feed(consumed, n);
        consumed = n;
    }
}

static void GW_SRAM_FUNC(gwsnd_core1_task)(void)
{
    /* Claim the task BEFORE testing engine_attached, so the detach side
       can never observe in_task==0 while this invocation is about to
       touch the buffers. With the reverse order (test, then claim) core0
       could pass its wait between our test and our claim, free the audio
       buffers, and leave us writing into freed memory — which corrupted
       the heap and hardfaulted the next free(). */
    in_task = 1;
    gwsnd_dmb();
    if (!engine_attached) {
        in_task = 0;
        return;
    }

    /* Snapshot the watermark BEFORE draining: every event with
       ts <= this watermark was pushed (and made visible) before it was
       published, so a full drain below guarantees none are missed. */
    uint32_t wm_word = line_watermark;

    gwsnd_event_t e;
    /* Bound the number of events per invocation so the scanout loop's
       other duties never starve; the task is re-entered immediately. */
    int budget = 256;
    while (budget-- > 0 && gwsnd_fifo_pop(&fifo, &e)) {
        switch (e.kind) {
        case GWSND_EV_YM_W0:
        case GWSND_EV_YM_W1:
        case GWSND_EV_YM_W2:
        case GWSND_EV_YM_W3:
            /* YM2612Write runs the chip up to e.ts itself */
            YM2612Write(e.kind, e.val, e.ts);
            break;
        case GWSND_EV_PSG_W:
            gwenesis_SN76489_Write(e.val, e.ts);
            break;
        case GWSND_EV_FRAME_END:
            /* top up to the frame boundary and emit the tail */
            ym2612_run(e.ts);
            gwenesis_SN76489_run(e.ts);
            if (e.val != engine_pal) {
                engine_pal = e.val;
                gwsnd_resample_reset(engine_pal);
            }
            feed_up_to(ym2612_index);
            if (gwsnd_fill_query_fn)
                gwsnd_set_fill_permille(gwsnd_fill_query_fn());
            ym2612_clock = 0;
            ym2612_index = 0;
            sn76489_clock = 0;
            sn76489_index = 0;
            consumed = 0;
            core1_epoch++;
            break;
        default:
            break;
        }
    }

    /* With the FIFO fully drained, both chips may synthesize up to the
       published per-line watermark of the current frame — this is what
       keeps the sink fed continuously (~3.4 samples per scanline). A
       partially drained FIFO (budget exhausted) may still hold events at
       or below the watermark, so skip and catch up next invocation. */
    if (budget > 0 &&
        (wm_word >> GWSND_WM_CLOCK_BITS) ==
            (core1_epoch & ((1u << (32 - GWSND_WM_CLOCK_BITS)) - 1))) {
        int wm = (int)(wm_word & ((1u << GWSND_WM_CLOCK_BITS) - 1));
        ym2612_run(wm);
        gwenesis_SN76489_run(wm);
        feed_up_to(ym2612_index < sn76489_index ? ym2612_index : sn76489_index);
    }

#if GENESIS_SEGACD
    /* CD-DA prefetch from SD (port/scd_audio.c), after the synthesis so a
       sector read never delays it; a couple of sectors at most per call. */
    if (gwcd_bus_mode != GWCD_BUS_CART)
        gwcd_cdda_service();
#endif

    in_task = 0;
}

/* ------------------------- core0-side control ------------------------- */

int gwsnd_offload_start(int is_pal)
{
    /* Zeroed, not malloc'd: a timestamp read from an entry that was
       never written must be harmless (ts 0 advances nothing) rather than
       arbitrary. */
    fifo.ev = calloc(GWSND_FIFO_LEN, sizeof(gwsnd_event_t));
    bridge = malloc(GWSND_BRIDGE_LEN * 2 * sizeof(int16_t));
    if (!fifo.ev || !bridge) {
        free(fifo.ev);
        free(bridge);
        fifo.ev = NULL;
        bridge = NULL;
        return 0; /* caller falls back to sync mode */
    }
    fifo.head = fifo.tail = 0;
    fifo.drop_count = 0;
    fifo_highwater = 0;
    bridge_head = bridge_tail = 0;
    consumed = 0;
    line_watermark = 0;
    core0_epoch = 0;
    core1_epoch = 0;
    engine_pal = is_pal;
    gwsnd_shadow_reset();
    gwsnd_dmb();
    engine_attached = 1;
    video_output_set_background_task(gwsnd_core1_task);
    return 1;
}

void gwsnd_offload_stop(void)
{
    /* Order matters: clear the gate first so any invocation that starts
       from here on bails out immediately, then detach the task, then
       wait for an invocation already past the gate to finish. */
    engine_attached = 0;
    gwsnd_dmb();
    video_output_set_background_task(NULL);

    absolute_time_t deadline = make_timeout_time_ms(250);
    while (in_task && absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        tight_loop_contents();
    }
    if (fifo.drop_count) {
        printf("gwsnd: %u events dropped (FIFO overflow)\n", (unsigned)fifo.drop_count);
    }
    if (in_task) {
        /* Core1 is wedged. Leaking these is bad, but freeing memory it may
           still be writing to corrupts the heap — which is worse. */
        printf("gwsnd: core1 task did not stop; leaking sound buffers\n");
    } else {
        free(fifo.ev);
        free(bridge);
    }
    fifo.ev = NULL;
    bridge = NULL;
}

/* Push with a short grace period: a healthy core1 drains hundreds of
   events per millisecond, so a full FIFO resolves almost immediately. */
static inline void GW_SRAM_FUNC(push_event)(int32_t ts, uint8_t kind, uint8_t val)
{
    if (gwsnd_fifo_push(&fifo, ts, kind, val)) {
        uint32_t level = fifo.head - fifo.tail;
        if (level > fifo_highwater)
            fifo_highwater = level;
        return;
    }
    absolute_time_t deadline = make_timeout_time_ms(2);
    while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
        if (gwsnd_fifo_push(&fifo, ts, kind, val))
            return;
        tight_loop_contents();
    }
    fifo.drop_count++;
}

void GW_SRAM_FUNC(gwsnd_offload_ym_write)(unsigned int a, unsigned int v, int ts)
{
    gwsnd_shadow_write(a, v, ts);
    push_event(ts, (uint8_t)(a & 3), (uint8_t)v);
}

unsigned int GW_SRAM_FUNC(gwsnd_offload_ym_read)(int ts)
{
    gwsnd_shadow_run(ts);
    return gwsnd_shadow_status();
}

void GW_SRAM_FUNC(gwsnd_offload_psg_write)(unsigned int v, int ts)
{
    push_event(ts, GWSND_EV_PSG_W, (uint8_t)v);
}

void gwsnd_offload_frame_end(int system_clock, int is_pal)
{
    gwsnd_shadow_run(system_clock);
    gwsnd_shadow_frame_reset();
    push_event(system_clock, GWSND_EV_FRAME_END, (uint8_t)is_pal);
    /* Line ticks published from here on belong to the next frame. */
    core0_epoch++;
}

/* Called from the frame loop (core0) after both CPUs finished a line. */
void GW_SRAM_FUNC(gwsnd_offload_line_tick)(int system_clock)
{
    /* All of this line's events are in the FIFO (dmb'd); publish how far
       core1 may synthesize. */
    gwsnd_dmb();
    line_watermark = (core0_epoch << GWSND_WM_CLOCK_BITS) |
                     ((uint32_t)system_clock & ((1u << GWSND_WM_CLOCK_BITS) - 1));

    /* Keep the core0-side sink (I2S/VU) fed incrementally as well —
       bursting a frame's worth into the I2S ring overflows it just the
       same. Cheap: the bridge holds only a few samples per line. */
    gwsnd_bridge_drain();
}

/* -------- perf diagnostics (SELECT+DOWN debug output in main.cpp) ----- */

unsigned int gwsnd_stats_fifo_highwater(void)
{
    unsigned int hw = fifo_highwater;
    fifo_highwater = 0;
    return hw;
}

unsigned int gwsnd_stats_drops(void)
{
    return fifo.drop_count;
}

unsigned int gwsnd_stats_bridge_drops(void)
{
    unsigned int d = bridge_drops;
    bridge_drops = 0;
    return d;
}
