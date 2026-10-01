/*
 * PicoDrive
 * (C) notaz, 2007,2013
 * (C) irixxxx, 2019-2024
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include "../pico_int.h"
#include "../sound/ym2612.h"
#include "megasd.h"

extern unsigned char formatted_bram[4*0x10];

static unsigned int mcd_m68k_cycle_mult;
static unsigned int mcd_s68k_cycle_mult;
static unsigned int mcd_m68k_cycle_base;
static unsigned int mcd_s68k_cycle_base;

mcd_state *Pico_mcd;

PICO_INTERNAL void PicoCreateMCD(unsigned char *bios_data, int bios_size)
{
  /* PORT: port/scd.c allocates the state in PSRAM before calling this. */
  if (!Pico_mcd)
    return;
  memset(Pico_mcd, 0, sizeof(mcd_state));

  if (bios_data && bios_size > 0) {
    if (bios_size > sizeof(Pico_mcd->bios))
      bios_size = sizeof(Pico_mcd->bios);
    memcpy(Pico_mcd->bios, bios_data, bios_size);
  }
}

PICO_INTERNAL void PicoInitMCD(void)
{
  SekInitS68k();
}

PICO_INTERNAL void PicoExitMCD(void)
{
  cdd_unload();
  /* PORT: port/scd.c owns (and frees) the PSRAM state. */
  Pico_mcd = NULL;
}

PICO_INTERNAL void PicoPowerMCD(void)
{
  int fmt_size;

  SekResetS68k();
  SekCycleCntS68k = SekCycleAimS68k = 0;

  fmt_size = sizeof(formatted_bram);
  memset(Pico_mcd->prg_ram,    0, sizeof(Pico_mcd->prg_ram));
  memset(Pico_mcd->word_ram2M, 0, sizeof(Pico_mcd->word_ram2M));
  memset(Pico_mcd->pcm_ram,    0, sizeof(Pico_mcd->pcm_ram));
  memset(Pico_mcd->bram, 0, sizeof(Pico_mcd->bram));
  memcpy(Pico_mcd->bram + sizeof(Pico_mcd->bram) - fmt_size,
    formatted_bram, fmt_size);
  memset(Pico_mcd->s68k_regs, 0, sizeof(Pico_mcd->s68k_regs));
  memset(&Pico_mcd->pcm, 0, sizeof(Pico_mcd->pcm));
  memset(&Pico_mcd->m, 0, sizeof(Pico_mcd->m));

  cdc_init();
  gfx_init();

  // cold reset state (tested)
  Pico_mcd->m.state_flags = PCD_ST_S68K_RST;
  Pico_mcd->m.busreq = 2;     // busreq on, s68k in reset
  Pico_mcd->s68k_regs[3] = 1; // 2M word RAM mode, m68k access
  if (Pico.romsize == 0) // no HINT vector from gate array for MSU
    memset(Pico_mcd->bios + 0x70, 0xff, 4);
  pcd_event_schedule_s68k(PCD_EVENT_CDC, 12500000/75);

  cdc_reset();
  cdd_reset();
}

void pcd_soft_reset(void)
{
  elprintf(EL_CD, "cd: soft reset");

  Pico_mcd->m.s68k_pend_ints = 0;
  cdc_reset();
  cdd_reset();
#ifdef _ASM_CD_MEMORY_C
  //PicoMemResetCDdecode(1); // don't have to call this in 2M mode
#endif

  memset(&Pico_mcd->s68k_regs[0x38], 0, 9);
  Pico_mcd->s68k_regs[0x38+9] = 0x0f;  // default checksum

  pcd_event_schedule_s68k(PCD_EVENT_CDC, 12500000/75);

  // TODO: test if register state/timers change
}

PICO_INTERNAL int PicoResetMCD(void)
{
  // reset button doesn't affect MCD hardware

  // use Pico.sv.data for RAM cart
  if (Pico.romsize == 0) {
    if (PicoIn.opt & POPT_EN_MCD_RAMCART) {
      if (Pico.sv.data == NULL)
        Pico.sv.data = calloc(1, 0x12000);
    }
    else if (Pico.sv.data != NULL) {
      free(Pico.sv.data);
      Pico.sv.data = NULL;
    }
    Pico.sv.start = Pico.sv.end = 0; // unused
  }

  return 0;
}

static void SekRunS68k(unsigned int to)
{
  int cyc_do;

  SekCycleAimS68k = to;
  if ((cyc_do = SekCycleAimS68k - SekCycleCntS68k) <= 0)
    return;

  SekCycleCntS68k += cyc_do;
#if defined(GWENESIS_PICO)
  /* PORT: the gwenesis sub 68000 (s68kcpu.c). s68k.cycles counts up from 0
     to the run's length; SekEndRunS68k() may shorten it. What it actually
     executed, overshoot included, is where the count ends up. */
  {
    unsigned int start = SekCycleCntS68k - cyc_do;
    s68k.cycles = 0;
    gwcd_s68k_skipped = 0;
    gwcd_s68k_in_run = 1;
    s68k_run(cyc_do);
    gwcd_s68k_in_run = 0;
    SekCycleCntS68k = start + s68k.cycles;
    /* an idle loop (s68kcpu.c) may have skipped the end of the run */
    gwcd_stats.s68k_run += s68k.cycles - gwcd_s68k_skipped;
    gwcd_stats.s68k_idle += gwcd_s68k_skipped;
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
    gwcd_host_s68k_runs++;
    /* host harness: GEN_SCD_PROFILE samples where the sub CPU spends time */
    if (gwcd_s68k_sample)
      gwcd_s68k_sample(s68k.pc, s68k.cycles);
#endif
    s68k.cycles = s68k.cycle_end = 0;
  }
#elif defined(EMU_C68K)
  PicoCpuCS68k.cycles = cyc_do;
  CycloneRun(&PicoCpuCS68k);
  SekCycleCntS68k -= PicoCpuCS68k.cycles;
#elif defined(EMU_M68K)
  m68k_set_context(&PicoCpuMS68k);
  SekCycleCntS68k += m68k_execute(cyc_do) - cyc_do;
  m68k_set_context(&PicoCpuMM68k);
#elif defined(EMU_F68K)
  SekCycleCntS68k += fm68k_emulate(&PicoCpuFS68k, cyc_do, 0) - cyc_do;
#endif
#if !defined(GWENESIS_PICO)
  SekCyclesLeftS68k = 0;
#endif
}

void PicoMCDPrepare(void)
{
  // 12500000/(osc/7), ~1.63 for NTSC, ~1.645 for PAL
#define DIV_ROUND(x,y) ((x)+(y)/2) / (y) // round to nearest, x/y+0.5 -> (x+y/2)/y
  unsigned int osc = (Pico.m.pal ? OSC_PAL : OSC_NTSC);
  /* PORT: the main side counts master clocks here, not 68000 cycles
     (scd/pico_int.h), so the 7 drops out of both ratios. */
  mcd_m68k_cycle_mult = DIV_ROUND(12500000ull << 16, osc);
  mcd_s68k_cycle_mult = DIV_ROUND(1ull * osc << 16, 12500000);
}

unsigned int pcd_cycles_m68k_to_s68k(unsigned int c)
{
  return (long long)c * mcd_m68k_cycle_mult >> 16;
}

/* events */
static void pcd_cdc_event(unsigned int now)
{
  int audio = Pico_mcd->s68k_regs[0x36] & 0x1;

  // 75Hz CDC update
  cdd_update();

  /* PORT: PicoDrive tracks where in the frame CD-DA starts, for its mixer
     and for save states. Here CD-DA streams on its own (port/scd_cdda.c);
     it only needs to know whether the drive is playing, and the fader. */
  (void)audio;
  gwcd_cdda_drive_state(!(Pico_mcd->s68k_regs[0x36] & 0x1),
                        (Pico_mcd->s68k_regs[0x34] << 8) | Pico_mcd->s68k_regs[0x35]);

  /* check if a new CDD command has been processed */
  if (!(Pico_mcd->s68k_regs[0x4b] & 0xf0))
  {
    /* reset CDD command wait flag */
    Pico_mcd->s68k_regs[0x4b] = 0xf0;
  }

  if ((Pico_mcd->s68k_regs[0x33] & PCDS_IEN4) && (Pico_mcd->s68k_regs[0x37] & 4)) {
    elprintf(EL_INTS|EL_CD, "s68k: cdd irq 4");
    pcd_irq_s68k(4, 1);
  }

  msd_update();

  pcd_event_schedule(now, PCD_EVENT_CDC, 12500000/75);
}

static void pcd_int3_timer_event(unsigned int now)
{
  if (Pico_mcd->s68k_regs[0x33] & PCDS_IEN3) {
    elprintf(EL_INTS|EL_CD, "s68k: timer irq 3");
    pcd_irq_s68k(3, 1);
  }

  if (Pico_mcd->s68k_regs[0x31] != 0)
    pcd_event_schedule(now, PCD_EVENT_TIMER3,
      (Pico_mcd->s68k_regs[0x31]+1) * 384);
}

static void pcd_dma_event(unsigned int now)
{
  cdc_dma_update();
}

typedef void (event_cb)(unsigned int now);

/* times are in s68k (12.5MHz) cycles */
unsigned int pcd_event_times[PCD_EVENT_COUNT];
static unsigned int event_time_next;
static event_cb *pcd_event_cbs[PCD_EVENT_COUNT] = {
  pcd_cdc_event,            // PCD_EVENT_CDC
  pcd_int3_timer_event,     // PCD_EVENT_TIMER3
  gfx_update,               // PCD_EVENT_GFX
  pcd_dma_event,            // PCD_EVENT_DMA
};

void pcd_event_schedule(unsigned int now, enum pcd_event event, int after)
{
  unsigned int when;

  if ((now|after) == 0) {
    // event cancelled
    pcd_event_times[event] = 0;
    return;
  }

  when = now + after;
  when |= 1;

  elprintf(EL_CD, "cd: new event #%u %u->%u", event, now, when);
  pcd_event_times[event] = when;

  if (event_time_next == 0 || CYCLES_GT(event_time_next, when))
    event_time_next = when;
}

void pcd_event_schedule_s68k(enum pcd_event event, int after)
{
  SekEndRunS68k(after);

  pcd_event_schedule(SekCyclesDoneS68k(), event, after);
}

static void pcd_run_events(unsigned int until)
{
  int oldest, oldest_diff, time;
  int i, diff;

  while (1) {
    oldest = -1, oldest_diff = 0x7fffffff;

    for (i = 0; i < PCD_EVENT_COUNT; i++) {
      if (pcd_event_times[i]) {
        diff = pcd_event_times[i] - until;
        if (diff < oldest_diff) {
          oldest_diff = diff;
          oldest = i;
        }
      }
    }

    if (oldest_diff <= 0) {
      time = pcd_event_times[oldest];
      pcd_event_times[oldest] = 0;
      elprintf(EL_CD, "cd: run event #%d %u", oldest, time);
      pcd_event_cbs[oldest](time);
      gwcd_s68k_wake(); /* PORT: the event may end a parked wait loop */
    }
    else if (oldest_diff < 0x7fffffff) {
      event_time_next = pcd_event_times[oldest];
      break;
    }
    else {
      event_time_next = 0;
      break;
    }
  }

  if (oldest != -1)
    elprintf(EL_CD, "cd: next event #%d at %u",
      oldest, event_time_next);
}

void pcd_irq_s68k(int irq, int state)
{
  if (state) {
    SekInterruptS68k(irq);
    Pico_mcd->m.state_flags &= ~PCD_ST_S68K_POLL;
    Pico_mcd->m.s68k_poll_cnt = 0;
  } else
    SekInterruptClearS68k(irq);
}

int pcd_sync_s68k(unsigned int m68k_target, int m68k_poll_sync)
{
  #define now SekCycleCntS68k
  unsigned int s68k_target;
  unsigned int target;

  target = m68k_target - mcd_m68k_cycle_base;
  s68k_target = mcd_s68k_cycle_base +
    ((unsigned long long)target * mcd_m68k_cycle_mult >> 16);

  elprintf(EL_CD, "s68k sync to %u, %u->%u",
    m68k_target, now, s68k_target);

  if (Pico_mcd->m.busreq != 1) { /* busreq/reset */
    gwcd_stats.s68k_halted += s68k_target - now; /* PORT: stats */
    SekCycleCntS68k = SekCycleAimS68k = s68k_target;
    pcd_run_events(s68k_target);
    return 0;
  }

  while (CYCLES_GT(s68k_target, now)) {
    if (event_time_next && CYCLES_GE(now, event_time_next))
      pcd_run_events(now);

    target = s68k_target;
    if (event_time_next && CYCLES_GT(target, event_time_next))
      target = event_time_next;

    if (Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) {
      gwcd_stats.s68k_idle += target - now; /* PORT: stats */
      SekCycleCntS68k = SekCycleAimS68k = target;
    } else
      SekRunS68k(target);

    if (m68k_poll_sync && Pico_mcd->m.m68k_poll_cnt == 0)
      break;
  }

  return s68k_target - now;
  #undef now
}

#if 0 /* PORT: the frame is driven by port/frame_loop.inc; see gwcd_run_line() below */
#define pcd_run_cpus_normal pcd_run_cpus
//#define pcd_run_cpus_lockstep pcd_run_cpus

static void SekAimM68k(int cyc, int mult);
static int SekSyncM68k(int once);

void pcd_run_cpus_normal(int m68k_cycles)
{
  SekAimM68k(m68k_cycles, 0x108);

  while (CYCLES_GT(Pico.t.m68c_aim, Pico.t.m68c_cnt)) {
    if (SekShouldInterrupt()) {
      Pico_mcd->m.state_flags &= ~PCD_ST_M68K_POLL;
      Pico_mcd->m.m68k_poll_cnt = 0;
    }

#ifdef USE_POLL_DETECT
    if (Pico_mcd->m.state_flags & PCD_ST_M68K_POLL) {
      int s68k_left;
      // main CPU is polling, (wake and) run sub only
      if (Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) {
        Pico_mcd->m.state_flags &= ~(PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP);
        Pico_mcd->m.s68k_poll_cnt = 0;
      }
      s68k_left = pcd_sync_s68k(Pico.t.m68c_aim, 1);

      Pico.t.m68c_cnt = Pico.t.m68c_aim;
      if (s68k_left > 0)
        Pico.t.m68c_cnt -= ((long long)s68k_left * mcd_s68k_cycle_mult >> 16);
      if (Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) {
        // slave has stopped, wake master to avoid lockups
        Pico_mcd->m.state_flags &= ~PCD_ST_M68K_POLL;
        Pico_mcd->m.m68k_poll_cnt = 0;
      }

      elprintf(EL_CDPOLL, "m68k poll [%02x] x%d @%06x",
        Pico_mcd->m.m68k_poll_a, Pico_mcd->m.m68k_poll_cnt, SekPc);
    } else
#endif
    {
      SekSyncM68k(1);
      // make sure sub doesn't get too far out of sync with main
      if (!(Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) &&
          pcd_cycles_m68k_to_s68k(Pico.t.m68c_aim - mcd_m68k_cycle_base) >
                           5000 + SekCycleAimS68k - mcd_s68k_cycle_base)
        pcd_sync_s68k(Pico.t.m68c_cnt, 0);
    }
    if (Pico_mcd->m.state_flags & PCD_ST_S68K_SYNC) {
      Pico_mcd->m.state_flags &= ~PCD_ST_S68K_SYNC;
      pcd_sync_s68k(Pico.t.m68c_cnt, 0);
    }
  }
}

void pcd_run_cpus_lockstep(int m68k_cycles)
{
  unsigned int target = Pico.t.m68c_aim + m68k_cycles;

  while (CYCLES_GT(target, Pico.t.m68c_aim)) {
    int cycles = target - Pico.t.m68c_aim;
    if (cycles > 8) cycles = 8;
    SekAimM68k(cycles, 0x108);
    SekSyncM68k(1);
    pcd_sync_s68k(Pico.t.m68c_cnt, 0);
  }
}

#define PICO_CD
#define CPUS_RUN(m68k_cycles) \
  pcd_run_cpus(m68k_cycles)

#include "../pico_cmn.c"


#endif

/* PORT: one scanline of both CPUs, called by port/frame_loop.inc in place
   of m68k_run(). This is pcd_run_cpus_normal() above with the main CPU's
   PicoDrive timing (Pico.t.m68c_*) replaced by the gwenesis core's:
   `target` is the line's end in the frame's master clocks, m68k.cycles the
   main CPU's position, SekCyclesDone() the same on the monotonic scale.
   The sub CPU still runs lazily behind the main one, caught up on every
   gate array access and, here, at the end of every line. */
void gwcd_run_line(unsigned int target)
{
  if (!(gwcd_bus_mode & GWCD_BUS_SCD)) {
    m68k_run(target);
    return;
  }

  while ((int)(target - m68k.cycles) > 0) {
    if (SekShouldInterrupt()) {
      Pico_mcd->m.state_flags &= ~PCD_ST_M68K_POLL;
      Pico_mcd->m.m68k_poll_cnt = 0;
    }

    if (Pico_mcd->m.state_flags & PCD_ST_M68K_POLL) {
      int s68k_left;
      // main CPU is polling, (wake and) run sub only
      if (Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) {
        Pico_mcd->m.state_flags &= ~(PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP);
        Pico_mcd->m.s68k_poll_cnt = 0;
      }
      s68k_left = pcd_sync_s68k(gwcd_mclk_base + target, 1);

      {
        unsigned int before = m68k.cycles;
        m68k.cycles = target;
        if (s68k_left > 0)
          m68k.cycles -= ((long long)s68k_left * mcd_s68k_cycle_mult >> 16);
        gwcd_stats.m68k_idle += m68k.cycles - before;
      }
      if (Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) {
        // slave has stopped, wake master to avoid lockups
        Pico_mcd->m.state_flags &= ~PCD_ST_M68K_POLL;
        Pico_mcd->m.m68k_poll_cnt = 0;
      }

      elprintf(EL_CDPOLL, "m68k poll [%02x] x%d @%06x",
        Pico_mcd->m.m68k_poll_a, Pico_mcd->m.m68k_poll_cnt, SekPc);
    } else {
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
      unsigned int before = m68k.cycles;
#endif
      gwcd_m68k_skipped = 0;
      m68k_run(target);
      /* PORT: a wait loop may have skipped the end of the run (m68kcpu.c) */
      gwcd_stats.m68k_idle += gwcd_m68k_skipped;
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
      if (gwcd_m68k_sample) /* host harness: GEN_SCD_PROFILE */
        gwcd_m68k_sample(m68k.pc, m68k.cycles - before);
#endif
      // make sure sub doesn't get too far out of sync with main
      if (!(Pico_mcd->m.state_flags & (PCD_ST_S68K_POLL|PCD_ST_S68K_SLEEP)) &&
          pcd_cycles_m68k_to_s68k(SekCyclesDone() - mcd_m68k_cycle_base) >
                           5000 + SekCycleAimS68k - mcd_s68k_cycle_base)
        pcd_sync_s68k(SekCyclesDone(), 0);
    }
    if (Pico_mcd->m.state_flags & PCD_ST_S68K_SYNC) {
      Pico_mcd->m.state_flags &= ~PCD_ST_S68K_SYNC;
      pcd_sync_s68k(SekCyclesDone(), 0);
    }
  }

  pcd_sync_s68k(SekCyclesDone(), 0);
}

void pcd_prepare_frame(void)
{
  // need this because we can't have direct mapping between
  // master<->slave cycle counters because of overflows
  mcd_m68k_cycle_base = SekCyclesDone(); /* PORT: frame start, master clocks */
  mcd_s68k_cycle_base = SekCycleAimS68k;
}

#if 0 /* PORT: no PicoDrive frame driver, no save states */
PICO_INTERNAL void PicoFrameMCD(void)
{
  PicoFrameStart();

  pcd_prepare_frame();
  PicoFrameHints();
}

void pcd_state_loaded(void)
{
  unsigned int cycles;

  pcd_state_loaded_mem();

  memset(Pico_mcd->pcm_mixbuf, 0, sizeof(Pico_mcd->pcm_mixbuf));
  Pico_mcd->pcm_mixbuf_dirty = 0;
  Pico_mcd->pcm_mixpos = 0;
  Pico_mcd->pcm_regs_dirty = 1;

  // old savestates..
  cycles = pcd_cycles_m68k_to_s68k(Pico.t.m68c_aim);
  if (CYCLES_GE(cycles - SekCycleAimS68k, 12500000/60)) {
    SekCycleCntS68k = SekCycleAimS68k = cycles;
  }
  if (pcd_event_times[PCD_EVENT_CDC] == 0) {
    pcd_event_schedule(SekCycleAimS68k, PCD_EVENT_CDC, 12500000/75);

    if (Pico_mcd->s68k_regs[0x31])
      pcd_event_schedule(SekCycleAimS68k, PCD_EVENT_TIMER3,
        (Pico_mcd->s68k_regs[0x31]+1) * 384);
  }

  if (CYCLES_GE(cycles - Pico_mcd->pcm.update_cycles, 12500000/50))
    Pico_mcd->pcm.update_cycles = cycles;

  if (Pico_mcd->m.need_sync) {
    Pico_mcd->m.state_flags |= PCD_ST_S68K_SYNC;
    Pico_mcd->m.need_sync = 0;
  }

  // reschedule
  event_time_next = 0;
  pcd_run_events(SekCycleCntS68k);

  // msd
  msd_load();
}

#endif

// vim:shiftwidth=2:ts=2:expandtab
