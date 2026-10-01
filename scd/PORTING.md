# scd/ — the Sega CD hardware, from PicoDrive

`scd/cd/` is a vendored copy of PicoDrive's `pico/cd/`
([irixxxx/picodrive](https://github.com/irixxxx/picodrive) @ `26ecb2b6`,
2025-04-03), patched to run on top of the gwenesis core. As with
`gwenesis/PORTING.md`, the aim is that every difference from upstream is
small, marked `PORT:` in the source, and listed here, so an upstream update
can be re-applied.

The files keep upstream's layout, so their `#include "../pico_int.h"` and
`"../memory.h"` resolve to this port's stand-ins in `scd/`:

| stand-in | replaces | what it provides |
|---|---|---|
| `scd/pico_int.h` | `pico/pico_int.h` | types, `mcd_state` & co. (verbatim), the `Sek*` CPU API mapped onto gwenesis' two 68000s, `Pico`/`PicoIn` with only the fields used, `elprintf` (host-only printf) |
| `scd/memory.h` | `pico/memory.h` | the map-building API (`cpu68k_map_all_ram`, `cpu68k_map_all_funcs`, ...), implemented in `port/scd.c` |
| `scd/sound/ym2612.h` | `pico/sound/ym2612.h` | empty; `mcd.c` includes it and uses nothing |

Everything around the vendored code is in `port/`: the session and memory
glue (`scd.c`), the disc layer (`scd_disc.c`, `scd_chd.c`), the BIOS lookup
(`scd_bios.c`), CD audio (`scd_audio.c`), MD+ (`scd_msd.c`). The sub 68000
is `gwenesis/cpus/M68K/s68kcpu.c`.

Also from PicoDrive, for the one cartridge that needs a Sega CD's company
to show what it does: Pier Solar's hardware (`port/gwpier.c`, after
`pico/carthw/carthw.c`'s `carthw_pier_*`) and its SPI EEPROM
(`port/carthw/eeprom_spi.c`, `pico/carthw/eeprom_spi.c` with its header
comment listing the changes).

Licences: `cdc.c`, `cdd.c/.h`, `gfx.c`, `genplus_macros.h` are Eke-Eke's
Genesis Plus GX code (non-commercial licence, in the file headers);
`mcd.c`, `memory.c`, `pcm.c`, `misc.c`, `cell_map.c` are notaz's and
`megasd.c/.h` irixxxx's (MAME licence, `scd/COPYING`).

## Taken and not taken

Taken: `mcd.c cdc.c cdd.c cdd.h gfx.c cell_map.c pcm.c misc.c megasd.c
megasd.h genplus_macros.h memory.c`.

Not taken: `cd_image.c` and `cd_parse.c` (disc images come from
`port/scd_disc.c`, after pico-pcePlus; only the `REM LOOP`/`REM NOLOOP` cue
extensions were carried over), `gfx_dma.c` (VDP DMA from the cell image is
`gwcd_dma_read16()` in `port/scd.c`, used by the gwenesis VDP), `sek.c`
(folded into `port/scd.c`), the ARM assembly versions, and `libchdr/`
(the submodule `external/libchdr` is used instead, as in pico-pcePlus).

## Cycle accounting

The one idea everything below depends on. PicoDrive counts the main CPU in
68000 cycles on a monotonic counter (`Pico.t.m68c_cnt`) and the sub CPU in
12.5 MHz cycles on another (`SekCycleCntS68k`). Here:

- **Main CPU**: master clocks (7 per 68000 cycle), what gwenesis' `m68k.cycles`
  counts. `m68k.cycles` is frame relative -- the frame loop rebases it -- so
  `SekCyclesDone()` is `gwcd_mclk_base + m68k.cycles` and `gwcd_frame_end()`
  moves the base up by the frame just before the rebase. Constants the CD
  code gives in 68000 cycles are scaled with `M68K_CYC()`, and the
  main/sub ratios drop their factor 7.
- **Sub CPU**: `SekCycleCntS68k` exactly as PicoDrive. The sub 68000 is built
  with `MUL (1)`, so it counts its own clock; during `s68k_run()` its
  `s68k.cycles` runs from 0 to the run's length, and `SekCyclesLeftS68k` /
  `SekEndRunS68k()` are derived from that.

The frame is driven by `port/frame_loop.inc`: `gwcd_run_line()` (a port of
`pcd_run_cpus_normal`, in `mcd.c`) replaces `m68k_run()` for each scanline.
The sub CPU runs lazily behind the main one, caught up on every gate array
access (PicoDrive's `pcd_sync_s68k`), at PicoDrive's 5000-cycle drift bound,
and at the end of every line.

## Sessions

`port/scd.c` sets `gwcd_bus_mode`, which the gwenesis bus reads on its cold
paths, for three kinds of session:

- **Disc boot** (`GWCD_BUS_SCD`): `gwcd_scd_start(bios, size, NULL, 0)`
  loads the BIOS as the cartridge; the Mega-CD sits at `$000000`.
- **MD+** (`GWCD_BUS_MDPLUS`): an ordinary cartridge session plus the
  MegaSD interface, `gwcd_mdplus_start()`. No BIOS, no sub CPU.
- **Mode 1** (both flags): the cartridge boots, the Mega-CD sits at
  `$400000` (`gwcd_cd_base`), as PicoDrive does it for a cartridge with a
  Sega CD disc (`PicoLoadMedia()` loading an MSU rom with `PAHW_MCD` set).
  The firmware calls `load_cartridge(cart)`, then `gwcd_scd_start(bios,
  size, cart, size)`, then `gwcd_mdplus_attach()`, which adds the MegaSD
  interface without restarting the CD audio the Mega-CD session already
  owns. Writes
  below `gwcd_cd_base` are the cartridge's (the MegaSD registers only), VDP
  DMA and the cell-image window are relative to `gwcd_cd_base`. The BIOS is
  chosen by the console region the cartridge sets up (`gwcd_cart_region`),
  not by the disc.

## Disc changes

Multi-disc games use the drive exactly as upstream: its "open tray" and
"close tray" commands (`cdd.c`) are unchanged, and PicoDrive's front-end
hook for the first, `PicoIn.mcdTrayOpen`, is `gwcd_disc_tray_opened()`.
What PicoDrive's front ends do there -- ask for a new image and call
`cdd_load()` -- is `port/scd_disc.c`'s disc set: the discs of an `.m3u`, or
the `(Disc N)` siblings of the picked image. A change swaps the image under
the running session at the end of a frame, under the SD lock, and rebuilds
`cdd.toc` without touching the rest of the drive state; the drive then
reports the new disc as not yet scanned (`NO_DISC`), as after
`cdd_load()`, or keeps the tray open when the game opened it. A change from
the settings menu first holds the drive's status at `CD_OPEN` for about a
second, as a model 2 reports its lid while a disc is swapped.

## Wait loops

PicoDrive's poll detection (`m68k_comm_check`, `s68k_poll_detect`) puts a
CPU to sleep when it reads one gate-array register over and over. Much of
the CPUs' time goes to loops it cannot see: the sub CPU waiting on a flag
in PRG-RAM that its interrupt clears (the BIOS), on two registers in turn
(Sonic CD's title), or on the PCM chip's playback position (Sonic CD's
video player); the main CPU waiting for vertical blank on a flag in work
RAM. Both 68000s skip such loops for the rest of their run (the checks are
in the core, `gwenesis/PORTING.md`); sub-CPU writes that change memory and
CDC port reads rule a loop out (`port/scd_s68k_mem.h`, `memory.c`).

A sub CPU found in such a loop is parked with PicoDrive's own sleep flag
(`PCD_ST_S68K_POLL`, `gwcd_s68k_park`), but woken on more: PicoDrive wakes
its sleeps on an interrupt, a graphics event or a write to the register
polled, a parked loop may wait on anything, so `gwcd_s68k_wake()` also runs
after every CD event (`pcd_run_events`), every sub-CPU interrupt
(`SekInterruptS68k`), every main-CPU write to the gate array
(`gwcd_m68k_io_write8/16`), PRG-RAM or Word-RAM (`gwcd_m68k_write8/16`),
and at the end of every frame. Loops that read the stopwatch or the PCM
position (`gwcd_s68k_timeread`) are not parked, only skipped per run.

In Sonic CD this takes the sub CPU from 100% to about 45% of its time in
the opening video and 35% on the title screen, and the main CPU from 100%
to under 10% in the video; in gameplay the sub CPU is started 15 times a
frame instead of 19, and the main CPU skips about 30% of its time. The
BIOS menu keeps the sub CPU fully busy on its own: its animated background
is real work.

## Patches, by file

### `mcd.c`
- `PicoCreateMCD()`: does not allocate; `port/scd.c` puts the state in PSRAM
  first. `PicoExitMCD()`: does not free it.
- `SekRunS68k()`: a `GWENESIS_PICO` branch runs `s68k_run()` (see cycle
  accounting). `pprof_*` calls dropped.
- `PicoMCDPrepare()`: both ratios in master clocks.
- `pcd_cdc_event()`: PicoDrive's bookkeeping of where CD-DA starts within
  the frame (for its mixer and save states) is replaced by
  `gwcd_cdda_drive_state()`, which hands the drive's audio gate
  (`$36` bit 0) and fader (`$34-$35`) to the CD-DA player 75 times a second.
- `pcd_run_cpus_normal/lockstep`, `SekAimM68k`, `SekSyncM68k`, the
  `pico_cmn.c` include, `PicoFrameMCD()` and `pcd_state_loaded()`: `#if 0`.
  New `gwcd_run_line()` after them.
- `pcd_prepare_frame()`: the main CPU's frame base is `SekCyclesDone()`.
- Statistics for `gwcd_stats_take()` (sub CPU executed / idle / halted,
  main CPU polling) in `SekRunS68k`, `pcd_sync_s68k` and `gwcd_run_line`.
  The cycles a wait loop skipped (`gwcd_s68k_skipped`, `gwcd_m68k_skipped`,
  see Wait loops) count as idle.
- Host harness only: profiling hooks after each CPU run
  (`gwcd_s68k_sample`, `gwcd_m68k_sample`, `GEN_SCD_PROFILE`).

### `memory.c` (CRLF, like upstream)
The handlers are upstream's; the map building around them is rebuilt.
- The `s68k_*_map` arrays and the `MAKE_68K_*` accessors are gone: the sub
  CPU's maps and inline accessors are `port/scd_s68k_mem.h`.
- `m68k_comm_check()` / `s68k_poll_detect()`: PicoDrive's "not polling" hint
  (set by DBcc in FAME) is gwenesis' existing idle-loop flag
  `cpu.poll.detected`, which GPGX's DBcc handlers already clear
  (`GWCD_NOT_POLLING`, `GWCD_POLL_CHECKED`); the main CPU's poll window is
  scaled to master clocks.
- The RAM cart handlers: `#if 0` (not emulated).
- `PicoRead8/16_mcd_io`, `PicoWrite8/16_mcd_io` become `gwcd_m68k_io_*`
  without the fallbacks to the Mega Drive I/O handlers: the gwenesis bus
  only routes `$A12000-$A120FF` there.
- New `PicoRead/WriteS68k*_hi` handlers: backup RAM (`$FE0000`) and
  PCM/registers (`$FF0000`) are separate 64 KB maps upstream but share one
  of this port's 128 KB slots.
- The CDC's register and data port reads (`s68k_reg_read16`, $06/$08)
  bump `gwcd_s68k_sidefx`: a read that moves the CDC on is never part of a
  skipped wait loop.
- `PicoMemSetupCD()` is rewritten on this port's maps: PicoDrive's layout,
  without `PicoMemSetup()`, the MSU/MD+ overlay (see Sessions) and the
  CPU-core-specific setup. `pcd_base_address` comes from `gwcd_cd_base`:
  0 for a disc boot, `$400000` in Mode 1.

### `cdc.c`
- The 18.7 KB `cdc` state (with the sector buffer) is PSRAM-allocated:
  `void *gwcd_cdc` plus `#define cdc (*(cdc_t *)gwcd_cdc)`.
- `cdc_host_r()`: the mcd-verificator sync offset scaled to master clocks.

### `cdd.c`, `cdd.h`
- `cdd` is PSRAM-allocated with the disc (`gwcd_cdd`, `#define cdd`).
- `cdd_load()`, `cdd_unload()` and the fake TOCs for audio-less ISO images
  only `cdd_load()` used: `#if 0`. `port/scd_disc.c` fills `cdd.toc` and
  provides `cdd_unload()`.
- `cdd_play_audio()`: no file handle to find (the disc layer reads audio by
  LBA from whichever file holds it); passes the track's first LBA as the
  base to `cdda_start_play()`.
- `cdd_seek()`: no `pm_seek()`. `cdd_read_data()`: `gwcd_disc_read_data()`.
- `cd_parse.h` include dropped.

### `gfx.c`
- The 2.1 KB `gfx` state (lookup tables) is allocated per CD game:
  `void *gwcd_gfx` plus `#define gfx (*(gfx_t *)gwcd_gfx)`.

### `megasd.c`, `megasd.h`
- `Pico_msd.data` is a pointer into the overlay's shadow page
  (`port/scd_msd.c`), where the 68000 reads it with ordinary cartridge
  fetches; `msd_read8/16` are therefore `#if 0`, and the overlay switch at
  `$03F7FA` calls `gwcd_msd_overlay()` instead of changing the memory map.
  `gwcd_msd_sync_regs()` keeps the id, result and command words in the page.
- `cdd_play/pause/resume/stop` drive the CD-DA player directly; the player
  ends or loops a play sample-exact, so `msd_update()` only notices the end.
- `msd_load()`: `#if 0` (no save states).

### Unchanged
`pcm.c`, `misc.c`, `cell_map.c`, `genplus_macros.h`.

## Known differences from PicoDrive

- 128 KB map slots: a range that covers only part of a slot is not mapped.
  The only one is `$0E0000-$0EFFFF` in 1M mode, which PicoDrive points past
  the end of the Word-RAM bank.
- The main CPU's PRG-RAM window and Word-RAM stay readable while the sub
  CPU owns them (writes are dropped); PicoDrive unmaps them. Only the cell
  image is a handler on the main side.
- CD-DA streams from a prefetch ring at the output rate (`port/scd_audio.c`)
  rather than being read per emulated frame; the drive's position still
  advances in emulated time.
- No save states, no RAM cart, no CD+G.

## Verification

`hosttest/` runs all of this on Linux under AddressSanitizer (see the README,
"PC test harness"). Checked with *Sonic CD* (USA): the Sega CD 2 BIOS boots to
its menu; the game boots, plays its opening, title and Palmtree Panic with CD
music and PCM effects; the special stage demo renders through the graphics
chip; a CHD made with `chdman createcd` gives output byte-identical to the
cue/bin set; and a disc started after a cartridge, or after another disc, is
byte-identical to a cold start. *Pier Solar* with its *Enhanced Soundtrack
Disc* (Mode 1, US BIOS): the dump protection lifts, the EEPROM is formatted
on first boot and found on the next, and the title screen streams the
soundtrack from the data track through the PCM chip. MD+ *Moonwalker* plays
its music from `.wav` tracks with `REM LOOP` points, and
`GEN_MSD_SELFTEST` passes.
