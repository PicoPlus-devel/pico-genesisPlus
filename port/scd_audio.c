/*
port/scd_audio.c — Sega CD audio: CD-DA streaming and the PCM chip's output,
mixed into the 44.1 kHz stream by the resampler (gwsnd_resample_mix_feed).

CD-DA. PicoDrive reads CD-DA straight from the track file in its mixer, one
frame's worth at a time (pico/sound/sound.c). Here the SD card is too slow
to sit in the mixer, so a player keeps a ring of prefetched 2352-byte
sectors in PSRAM, the way pico-pcePlus streams PC Engine CD audio
(pce-go/cd.c: cd_audio_update, cd_audio_generate_samples):

    core0    the drive or MegaSD posts commands: play from an LBA (to an
             end, optionally looping), stop, pause; and, 75 times a second,
             the drive's audio gate and fader (gwcd_cdda_drive_state)
    core1    gwcd_cdda_service() -- from the HSTX background task, after
             the sound engine -- applies commands and refills the ring;
             gwcd_audio_mix() consumes one sample per output sample

The ring is filled and drained on the same core, so the only cross-core
traffic is the command block, published with a sequence number. In sync
mode (host harness) both run on the calling core.

PCM. The RF5C164 is emulated by PicoDrive's pcm.c on core0, in sub-CPU
time; at every frame end the frame's output is resampled to the frame's
share of 44.1 kHz (pcd_pcm_update, as PicoDrive does) and queued for the
mixer in a small ring, also in PSRAM.

Levels. PicoDrive sums FM, PSG, PCM and CD-DA into one 32-bit buffer and
clips; the gwenesis FM/PSG mix is scaled down by the resampler's volume
shift, so PCM and CD-DA get the same attenuation to keep the balance.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico_int.h"
#include "cd/genplus_macros.h"
#include "cd/cdd.h"
#include "scd.h"
#include "scd_disc.h"

#define CDDA_RING_SECTORS 32              /* ~0.43 s, 75 KB of PSRAM */
#define CDDA_FRAMES_PER_SECTOR 588        /* stereo frames */
#define CDDA_READS_PER_SERVICE 2          /* bound core1's time per call */

#define PCM_RING_FRAMES 2048u             /* stereo frames, ~46 ms */
#define PCM_RING_TARGET 1600u             /* drop above this: bound latency */

/* Mixing attenuation (>> n), matching the resampler's FM/PSG volume shift. */
#define CDDA_SHIFT 1
#define PCM_SHIFT 1

#if !defined(GWENESIS_HOST) || GWENESIS_HOST == 0
#include "hardware/sync.h"
#define gwcd_dmb() __dmb()
#else
#define gwcd_dmb() __sync_synchronize()
#endif

/* --------------------------- CD-DA player ------------------------------ */

/* Command block: written by core0, then `seq` is bumped. */
static volatile uint32_t cmd_seq;
static volatile int cmd_start, cmd_end, cmd_loop_lba, cmd_loop, cmd_stop;
static volatile int cmd_paused;

/* Drive gate + fader (Sega CD), published at 75 Hz. MD+ never closes the
   gate. */
static volatile int gate_open = 1;
static volatile int fader = 0x400;

/* core1 state */
static uint8_t *ring;          /* [CDDA_RING_SECTORS][2352] */
static uint32_t applied_seq;
static int active;             /* a play command is running */
static int read_lba;           /* next sector to prefetch */
static int end_lba, loop_lba, looping;
static uint32_t ring_w, ring_r; /* sectors written / consumed */
static int frame_in_sector;    /* next stereo frame in the current sector */
static int volatile finished;  /* a non-looping play reached its end */
static int cur_vol = 0x400;    /* the fader ramps one step per sample */

static volatile uint32_t cdda_underruns;

/* The lead-out: a play "to the end" stops there. */
static int disc_end(void)
{
    return gwcd_cdd ? cdd.toc.end : 0;
}

static void post(int start, int end, int loop_to, int loop, int stop)
{
    cmd_start = start;
    cmd_end = end;
    cmd_loop_lba = loop_to;
    cmd_loop = loop;
    cmd_stop = stop;
    cmd_paused = 0;
    finished = 0;
    gwcd_dmb();
    cmd_seq++;
}

void gwcd_cdda_play(int start_lba, int end_lba, int loop_to, int loop)
{
    post(start_lba, end_lba, loop_to, loop, 0);
}

void gwcd_cdda_stop(void)
{
    post(0, 0, 0, 0, 1);
}

void gwcd_cdda_pause(int paused)
{
    cmd_paused = paused;
}

/* MD+ status: still playing (a looping play never finishes). */
int gwcd_cdda_busy(void)
{
    return (cmd_stop == 0) && !finished;
}

/* The CDD's seek to an audio sector (cdd_play_audio). PicoDrive re-seeks
   its stream on every call; the drive calls again at every track change,
   and the ring already holds that continuation, so a start the player is
   about to reach anyway is not a restart. */
void cdda_start_play(int lba_base, int lba_offset, int lb_len)
{
    int lba = lba_base + lba_offset;
    int pos = active ? (int)(read_lba - (int)(ring_w - ring_r)) : -1;
    (void)lb_len;

    if (active && !cmd_stop && lba >= pos - 2 && lba <= read_lba)
        return;
    post(lba, disc_end(), 0, 0, 0);
}

void gwcd_cdda_drive_state(int playing, unsigned int fader_reg)
{
    gate_open = playing;
    /* $34-$35: fader volume in bits 4-14, 0-1024 (LC7883). */
    fader = (fader_reg >> 4) & 0x7FF;
    if (fader > 0x400)
        fader = 0x400;
}

static void apply_command(void)
{
    uint32_t seq = cmd_seq;
    gwcd_dmb();
    if (seq == applied_seq)
        return;
    applied_seq = seq;
    ring_w = ring_r = 0;
    frame_in_sector = 0;
    if (cmd_stop) {
        active = 0;
        return;
    }
    read_lba = cmd_start;
    end_lba = cmd_end > 0 ? cmd_end : disc_end();
    loop_lba = cmd_loop_lba;
    looping = cmd_loop;
    active = 1;
}

void gwcd_cdda_service(void)
{
    int reads = 0;

    if (!ring || gwcd_bus_mode == GWCD_BUS_CART)
        return;
    apply_command();
    while (active && ring_w - ring_r < CDDA_RING_SECTORS && reads < CDDA_READS_PER_SERVICE) {
        if (read_lba >= end_lba) {
            if (!looping)
                break;
            read_lba = loop_lba;
        }
        if (gwcd_disc_read_audio(read_lba,
                                 ring + (ring_w % CDDA_RING_SECTORS) * GWCD_RAW_SECTOR) != 0)
            break;
        read_lba++;
        ring_w++;
        reads++;
    }
}

static inline int32_t sat16(int32_t v)
{
    return v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
}

static inline void cdda_next(int32_t *l, int32_t *r)
{
    const int16_t *s;
    int mul;

    if (!active || !gate_open || cmd_paused)
        return;
    if (ring_w == ring_r) {
        /* Nothing buffered: either still filling after a seek or the end
           of a non-looping play. */
        if (read_lba >= end_lba && !looping) {
            active = 0;
            finished = 1;
        } else {
            cdda_underruns++;
        }
        return;
    }
    s = (const int16_t *)(ring + (ring_r % CDDA_RING_SECTORS) * GWCD_RAW_SECTOR) +
        frame_in_sector * 2;

    /* CD-DA fader (cdd.c's cdd_read_audio): one step per sample towards
       the target, multiplier 0,1,2,3,4,8,12,...,1024 */
    if (cur_vol < fader)
        cur_vol++;
    else if (cur_vol > fader)
        cur_vol--;
    mul = (cur_vol & 0x7FC) ? (cur_vol & 0x7FC) : (cur_vol & 0x03);

    *l += ((s[0] * mul) >> 10) >> CDDA_SHIFT;
    *r += ((s[1] * mul) >> 10) >> CDDA_SHIFT;

    if (++frame_in_sector >= CDDA_FRAMES_PER_SECTOR) {
        frame_in_sector = 0;
        ring_r++;
    }
}

/* ------------------------------- PCM ----------------------------------- */

static int16_t *pcm_ring;      /* [PCM_RING_FRAMES][2] */
static volatile uint32_t pcm_w, pcm_r;
static s32 *pcm_frame;         /* one frame, stereo, core0 scratch */

void gwcd_pcm_frame_end(int is_pal)
{
    int n = is_pal ? 44100 / 50 : 44100 / 60;
    uint32_t w, level;

    if (!pcm_ring || !Pico_mcd)
        return;
    memset(pcm_frame, 0, (size_t)n * 2 * sizeof(s32));
    pcd_pcm_update(pcm_frame, n, 1);

    w = pcm_w;
    level = w - pcm_r;
    for (int i = 0; i < n; i++) {
        if (level >= PCM_RING_FRAMES)
            break;
        pcm_ring[(w % PCM_RING_FRAMES) * 2] = (int16_t)sat16(pcm_frame[i * 2]);
        pcm_ring[(w % PCM_RING_FRAMES) * 2 + 1] = (int16_t)sat16(pcm_frame[i * 2 + 1]);
        w++;
        level++;
    }
    gwcd_dmb();
    pcm_w = w;
}

static inline void pcm_next(int32_t *l, int32_t *r)
{
    uint32_t rd = pcm_r;
    uint32_t level = pcm_w - rd;

    if (!level)
        return;
    /* The mixer runs at the sink's pace, the PCM at the emulator's; drop a
       frame when the backlog grows so the latency stays bounded. */
    if (level > PCM_RING_TARGET)
        rd++;
    *l += pcm_ring[(rd % PCM_RING_FRAMES) * 2] >> PCM_SHIFT;
    *r += pcm_ring[(rd % PCM_RING_FRAMES) * 2 + 1] >> PCM_SHIFT;
    gwcd_dmb();
    pcm_r = rd + 1;
}

/* ------------------------------ mixing --------------------------------- */

#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
/* Host harness: each source separately, for per-source WAVs. */
void (*gwcd_audio_tap)(int32_t pcm_l, int32_t pcm_r, int32_t cd_l, int32_t cd_r);
#endif

void GW_SRAM_FUNC(gwcd_audio_mix)(int32_t *l, int32_t *r)
{
    if (gwcd_bus_mode == GWCD_BUS_CART)
        return;
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
    if (gwcd_audio_tap) {
        int32_t pl = 0, pr = 0, cl = 0, cr = 0;
        if (pcm_ring)
            pcm_next(&pl, &pr);
        if (ring)
            cdda_next(&cl, &cr);
        gwcd_audio_tap(pl, pr, cl, cr);
        *l += pl + cl;
        *r += pr + cr;
        return;
    }
#endif
    if (pcm_ring)
        pcm_next(l, r);
    if (ring)
        cdda_next(l, r);
}

/* ---------------------------- lifecycle -------------------------------- */

int gwcd_audio_start(int with_pcm)
{
    gwcd_audio_stop();
    ring = gwcd_port_psram_alloc(CDDA_RING_SECTORS * GWCD_RAW_SECTOR);
    if (!ring)
        return -1;
    if (with_pcm) {
        pcm_ring = gwcd_port_psram_alloc(PCM_RING_FRAMES * 2 * sizeof(int16_t));
        pcm_frame = gwcd_port_psram_alloc((44100 / 50) * 2 * sizeof(s32));
        if (!pcm_ring || !pcm_frame) {
            gwcd_audio_stop();
            return -1;
        }
    }
    cmd_seq = applied_seq = 0;
    cmd_stop = 1;
    cmd_paused = 0;
    active = 0;
    finished = 0;
    ring_w = ring_r = 0;
    frame_in_sector = 0;
    gate_open = 1;
    fader = cur_vol = 0x400;
    pcm_w = pcm_r = 0;
    cdda_underruns = 0;
    return 0;
}

void gwcd_audio_stop(void)
{
    /* The caller has detached the core1 engine (gwsnd_shutdown) first, so
       nothing on core1 still touches these. */
    active = 0;
    if (ring)
        gwcd_port_psram_free(ring);
    if (pcm_ring)
        gwcd_port_psram_free(pcm_ring);
    if (pcm_frame)
        gwcd_port_psram_free(pcm_frame);
    ring = NULL;
    pcm_ring = NULL;
    pcm_frame = NULL;
}

unsigned int gwcd_cdda_underruns(void)
{
    return cdda_underruns;
}
