/* ======================================================================== */
/*                     SEGA CD / MEGA-CD SUB 68000                          */
/* ======================================================================== */
/*
 * PORT (not upstream gwenesis): the Sega CD's second 68000, at 12.5 MHz.
 *
 * This is the same Musashi core as the main CPU, compiled a second time --
 * the way Genesis Plus GX, which this core was cut down from, builds its
 * own s68k. Everything the core defines at file scope is either static
 * (opcode handlers, jump table, cycle table) and so private to this
 * translation unit, or renamed below from m68k_* to s68k_*.
 *
 * GWENESIS_S68K switches four things in the shared sources:
 *   - MUL is 1 and the cycle table is s68ki_cycles_full.h: the sub CPU
 *     counts its own clock, which is also what PicoDrive's CD code expects;
 *   - every memory access goes through the sub CPU's page map
 *     (port/scd_s68k_mem.h), not the main CPU's ROM/RAM shortcuts;
 *   - a taken backward branch checks for an idle loop (below);
 *   - nothing else. Interrupts, the run loop and m68k_end_timeslice() work
 *     exactly as they do for the main CPU.
 *
 * Flash: a second 256 KB jump table, a 64 KB cycle table and the opcode
 * handlers. Only Sega CD builds (GENESIS_SEGACD) compile this file.
 */

#define GWENESIS_S68K 1

#ifndef GWCD_IDLE_SKIP
#define GWCD_IDLE_SKIP 1 /* port/scd.h */
#endif

#define m68k                        s68k
#define m68k_run                    s68k_run
#define m68k_init                   s68k_init
#define m68k_pulse_reset            s68k_pulse_reset
#define m68k_pulse_halt             s68k_pulse_halt
#define m68k_clear_halt             s68k_clear_halt
#define m68k_cycles                 s68k_cycles
#define m68k_cycles_run             s68k_cycles_run
#define m68k_cycles_master          s68k_cycles_master
#define m68k_end_timeslice          s68k_end_timeslice
#define m68k_get_reg                s68k_get_reg
#define m68k_set_reg                s68k_set_reg
#define m68k_set_irq                s68k_set_irq
#define m68k_set_irq_delay          s68k_set_irq_delay
#define m68k_update_irq             s68k_update_irq
#define m68k_set_int_ack_callback   s68k_set_int_ack_callback
#define m68k_set_reset_instr_callback s68k_set_reset_instr_callback
#define m68k_set_tas_instr_callback s68k_set_tas_instr_callback
#define m68k_set_fc_callback        s68k_set_fc_callback
#define gwenesis_m68k_save_state    gwenesis_s68k_save_state
#define gwenesis_m68k_load_state    gwenesis_s68k_load_state
#define gwenesis_host_cpu_read8     gwenesis_host_s68k_read8

#include "m68kcpu.c"

/* ------------------------------------------------------------------------ */
/* Idle loops                                                               */
/* ------------------------------------------------------------------------ */
/*
 * The sub CPU spends much of its time waiting: for its interrupt, polling a
 * flag in PRG-RAM that the handler clears (the BIOS menu), or for the main
 * CPU, polling the communication registers -- sometimes two in turn (the
 * Sonic CD title). PicoDrive's poll detection (scd/cd/memory.c) catches a
 * loop that reads one register over and over, nothing else, and at 12.5 MHz
 * the rest costs as much emulation as the whole main CPU.
 *
 * Nothing else runs while the sub CPU runs: the main CPU, the CD events
 * (CDC, CDD, timer, graphics chip, DMA) and interrupts all happen between
 * runs, and a run always ends at the next event, including one the sub CPU
 * schedules itself (SekEndRunS68k). So when the sub CPU comes back to the
 * same loop head with the same registers and flags, having changed nothing
 * in memory and read nothing that changes something (the CDC's ports), the
 * next pass is the same as this one, and so is every pass after it until
 * the run ends. The rest of the run is skipped: the time passes, nothing is
 * executed. The next run starts at the loop head with whatever changed in
 * between.
 *
 * Skipping the run is not enough: the scheduler would start the sub CPU
 * again at the next run, a scanline later, only for it to find the same
 * loop -- 260 runs a frame of a few instructions each, which on the Pico
 * cost more than the loop did, and PicoDrive's own poll detection, which
 * sleeps the sub CPU outright, never gets to count its reads. So the sub
 * CPU is parked (port/scd.c): it sleeps until an interrupt, a CD event or
 * a main-CPU write to anything it can see.
 *
 * Two values do move without any of those: the stopwatch and the PCM
 * chip's playback position, both worked out from the sub CPU's clock on
 * every read. A loop that reads one of them (Sonic CD's video player waits
 * for PCM playback to reach half its buffer) is not parked, only skipped
 * to the end of the run, and so sees the change at most one run -- about
 * a scanline, 64 us -- late.
 *
 * Checked on every taken backward branch (m68ki_branch_8/16): a compare
 * against the last loop head and the two counters, and a register compare
 * only when a loop comes round with neither counter moved.
 */
static struct {
  uint pc;                /* loop head of the last backward branch */
  unsigned int writes, sidefx, timeread;
  int have;               /* regs/sr hold the state at the last pass */
  uint sr;
  uint regs[16];
} s68ki_idle;

static void s68ki_idle_check(void)
{
  uint sr;

  if (!GWCD_IDLE_SKIP)
    return;
  if (REG_PC != s68ki_idle.pc || gwcd_s68k_writes != s68ki_idle.writes ||
      gwcd_s68k_sidefx != s68ki_idle.sidefx) {
    s68ki_idle.pc = REG_PC;
    s68ki_idle.writes = gwcd_s68k_writes;
    s68ki_idle.sidefx = gwcd_s68k_sidefx;
    s68ki_idle.timeread = gwcd_s68k_timeread;
    s68ki_idle.have = 0;
    return;
  }
  sr = m68ki_get_sr();
  if (s68ki_idle.have && sr == s68ki_idle.sr &&
      !memcmp(s68ki_idle.regs, REG_DA, sizeof(s68ki_idle.regs))) {
    if (m68k.cycles < m68k.cycle_end) {
      gwcd_s68k_skipped += m68k.cycle_end - m68k.cycles;
      m68k.cycles = m68k.cycle_end;
    }
    if (gwcd_s68k_timeread == s68ki_idle.timeread)
      gwcd_s68k_park();
    s68ki_idle.timeread = gwcd_s68k_timeread;
    return;
  }
  s68ki_idle.timeread = gwcd_s68k_timeread;
  memcpy(s68ki_idle.regs, REG_DA, sizeof(s68ki_idle.regs));
  s68ki_idle.sr = sr;
  s68ki_idle.have = 1;
}
