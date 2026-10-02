/*
port/gwsnd_resample.c — streaming linear-interpolation resampler.

Input: the two mono int16 chip buffers (gwenesis_ym2612_buffer /
gwenesis_sn76489_buffer) filled at the chip-native rate of one sample per
1009 master clocks (~53.28 kHz NTSC-paced, ~53.05 kHz PAL-paced given the
60/50 Hz frame pacing). Output: 44.1 kHz stereo (L = R) through a callback.

The ratio is phrased per frame so it is exact for the emulator's own pacing:
    NTSC: 896040/1009 in-samples per frame vs 44100/60 out-samples
    PAL: 1070460/1009 in-samples per frame vs 44100/50 out-samples
A small trim (+/-0.5% max) driven by the sink's fill level absorbs clock
drift so the output queue neither runs dry nor overflows — samples are
never dropped on the floor.
*/
#include <stdint.h>
#include "gwenesis_bus.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwsnd.h"
#include "scd.h"

static void (*output_fn)(int16_t l, int16_t r);
static int volume_shift = 1;

/* Q16 fixed point: input samples advanced per output sample. */
static uint32_t step_q16;
static uint32_t base_step_q16;
/* Phase within the current input interval [last_in, next]. */
static uint32_t phase_q16;
static int16_t last_in;

void gwsnd_set_output(void (*fn)(int16_t l, int16_t r)) { output_fn = fn; }

void gwsnd_set_volume_shift(int shift) { volume_shift = shift; }

void gwsnd_resample_reset(int is_pal)
{
    uint64_t in_per_frame_q16;
    uint32_t out_per_frame;
    if (is_pal) {
        in_per_frame_q16 = ((uint64_t)(LINES_PER_FRAME_PAL * VDP_CYCLES_PER_LINE) << 16) / AUDIO_FREQ_DIVISOR;
        out_per_frame = 44100 / GWENESIS_REFRESH_RATE_PAL;
    } else {
        in_per_frame_q16 = ((uint64_t)(LINES_PER_FRAME_NTSC * VDP_CYCLES_PER_LINE) << 16) / AUDIO_FREQ_DIVISOR;
        out_per_frame = 44100 / GWENESIS_REFRESH_RATE_NTSC;
    }
    base_step_q16 = (uint32_t)(in_per_frame_q16 / out_per_frame);
    step_q16 = base_step_q16;
    phase_q16 = 0;
    last_in = 0;
}

void gwsnd_set_fill_permille(int permille)
{
    /* Proportional trim, clamped to +/-1%. Queue above target -> step up
       (each output consumes more input -> fewer output samples per frame
       -> queue drains); below target -> step down.
       0.5% was too weak to matter: it moves the backlog ~0.9 packets per
       frame, while a single frame that overruns its period costs ~20, so
       a run of heavy frames walked the queue empty faster than the trim
       could refill it. 1% is ~1.8 packets/frame, and the caller only asks
       for full authority when the level is far off target, so the pitch
       deviation is a transient during recovery rather than a steady
       detune. */
    int err = permille - 1000;
    if (err > 500) err = 500;
    if (err < -500) err = -500;
    /* base/1000 * err/500 * 10 => max +/-1% */
    int32_t trim = (int32_t)((int64_t)base_step_q16 * err / 50000);
    step_q16 = base_step_q16 + trim;
}

static inline int16_t sat16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void GW_SRAM_FUNC(gwsnd_resample_mix_feed)(int from, int to)
{
    /* Snapshot the sink: in offload mode this runs on core1 while core0
       may swap it (headphone hot-plug, audio toggled off), and reloading
       it per sample would let a chunk be split across two sinks or call
       through a NULL that appeared mid-loop. Same pattern as
       gwsnd_bridge_drain(). */
    void (*out)(int16_t, int16_t) = output_fn;
    if (!out)
        return;
    const int16_t *ym = gwenesis_ym2612_buffer;
    const int16_t *sn = gwenesis_sn76489_buffer;

    for (int i = from; i < to; i++) {
        int32_t mixed = ((int32_t)ym[i] + (int32_t)sn[i]) >> volume_shift;
        int16_t s = sat16(mixed);

        /* Emit every output sample whose position falls before s. */
        while (phase_q16 < 0x10000u) {
            int32_t d = (int32_t)s - (int32_t)last_in;
            int16_t s_out = (int16_t)(last_in + ((d * (int32_t)phase_q16) >> 16));
#if GENESIS_SEGACD
            /* Sega CD PCM and CD-DA, and MD+ CD-DA: already at 44.1 kHz,
               one stereo sample per output sample (port/scd_audio.c). */
            if (gwcd_bus_mode != GWCD_BUS_CART) {
                int32_t l = s_out, r = s_out;
                gwcd_audio_mix(&l, &r);
                out(sat16(l), sat16(r));
            } else
#endif
            out(s_out, s_out);
            phase_q16 += step_q16;
        }
        phase_q16 -= 0x10000u;
        last_in = s;
    }
}
