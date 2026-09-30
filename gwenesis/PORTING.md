# gwenesis core — port notes

This directory is a vendored copy of upstream
[bzhxx/gwenesis](https://github.com/bzhxx/gwenesis) @ `168e466` ("Major
update"), lightly patched for the RP2350 port. The goal is that every
difference from upstream is mechanical, documented here, and re-appliable
to a future upstream update. The sound architecture around the core
(catch-up timestamps, core1 offload, resampling) lives outside the core in
`port/` — the core itself keeps upstream's `GWENESIS_AUDIO_ACCURATE=1`
semantics untouched.

Files NOT copied from upstream: `cpus/Z80/Debug.c`, `cpus/Z80/ConDebug.c`
(debugger, needs AY8910/Console headers that don't exist), `cpus/Z80/Z80.o`
(stray object file).

New files (not upstream): `gwenesis_port.h`, `sound/luts/*.h` (generated
by `hosttest/lutgen`), `CMakeLists.txt`, this file.

## Build contract

The port always defines: `GWENESIS_PICO=1`, `TABLES_FULL=1`,
`GNW_TARGET_MARIO=0`, `GNW_TARGET_ZELDA=0`,
`GWENESIS_PIXEL_FMT=555|444|565`, and on the Linux harness additionally
`GWENESIS_HOST=1`. `GWENESIS_Z80_DIVISOR` (default 14) overrides the Z80
clock divisor. `#pragma GCC optimize("Ofast")` sites (guarded by the GNW
defines, so inactive) are replaced by per-file `-O2` in CMake.

Pico builds additionally define `GWENESIS_LUTS_IN_RAM=1`: XIP flash
bandwidth is the scarce resource (the M68K dispatch tables, the PSRAM ROM
and — in offload mode — core1's synthesis all share the QMI port and a
16 KB XIP cache), so tables read on per-sample/per-instruction hot paths
must not live in flash. The three big YM2612 LUTs get per-game heap
copies (`ym2612_luts_init_ram`, called from `port/buffers.c`); small hot
const tables are placed in RAM at all times via `GW_SRAM_DATA`
(`__not_in_flash`). Leaving these in flash measurably drops core0 below
60 fps in DAC-heavy games (Sonic, Xeno Crisis).

## Patches, by file

### `gwenesis_port.h` (new)
Single adaptation point: SRAM/flash placement macros (`GW_SRAM_FUNC`,
`GW_IN_FLASH` — no-ops on the host), the CRAM→pixel conversion macro set
per display format (RGB565 host / RGB555 HSTX / RGB444 PicoDVI) including
the shadow/highlight mask and the H32 4→5 upscale channel spread macros,
and the sound-seam prototypes (`gwsnd_ym_write` / `gwsnd_ym_read` /
`gwsnd_psg_write`) implemented in `port/gwsnd_core0.c`.

### `bus/gwenesis_bus.c`
- `ROM_DATA` static 8 MB array → `const unsigned char *` pointing into
  PSRAM/XIP flash; new `rom_addr_mask` (pow2ceil(size)−1) bounds/mirrors
  every fetch. `M68K_RAM`/`ZRAM` → extern pointers allocated per game by
  `port/buffers.c` (freed on menu return — non-PSRAM boards need the SRAM
  heap for the menu).
- New `load_cartridge(const unsigned char *, size_t)` variant: no memcpy,
  no ROM_SWAP (pico_shared pre-byte-swaps), sets pointer+mask, resets the
  TMSS latch (survives relaunch-without-reboot on PSRAM boards). Once the
  mask is known it calls `gwmapper_reset()` (`port/gwmapper.c`), which puts
  the ROM page table behind `FETCH*ROM` back to its power-on banks — before
  `set_region()`, which reads the header through it.
- `power_on()`: `memset(&m68k, 0, sizeof m68k)` before `m68k_init()` —
  stale context crashed relaunches (fix carried over from the old port).
- The 6 sound-chip call sites route through the `gwsnd_*` seam with their
  exact `m68k_cycles_master()` timestamps.
- `set_region()` now publishes the console region as VDP status bit 0
  (`VERSION_PAL`) instead of leaving it commented out. The VDP's own
  vcounter and vblank logic already read that bit, so until now every PAL
  branch in `gwenesis_vdp_mem.c` was unreachable. Note the VDP's `mode_pal`
  is *not* the console region despite the name -- it is `REG1_PAL`, the
  240-line display bit, and stays driven by the game.
- `NONE` enum renamed `NONE_` (collides with pico_shared `SaveStateTypes`).
- `GW_SRAM_FUNC` on `m68k_read/write_memory_8/16/32`.
- Cartridge save RAM, all of whose logic lives in `port/gwsram.c`; the core
  only routes to it:
  - `gwenesis_bus_map_address()`: the `range < 0x80` arm returns `SRAM_ADDR`
    when `gwsram_hit(address)`, else `ROM_ADDR` as before. The test is one
    subtract and one compare, and can never match on a cart without save
    RAM, so ordinary ROM reads pay a failed compare.
  - `gwenesis_bus_map_io_address()`: `$A13xxx` (the /TIME region, which
    carries the save RAM control register at `$A130F1`) now returns
    `TIME_CTRL`. Upstream's `address & 0x1000` test routed it to the Z80
    control registers, where it matched neither BUSREQ nor RESET and did
    nothing beyond a `z80_sync()`; reads got `z80_read_ctrl()`'s `0xFF`
    default, which the `TIME_CTRL` read case returns unchanged.
  - `SRAM_ADDR` / `TIME_CTRL` cases in `gwenesis_bus_read_memory_8/16` and
    `gwenesis_bus_write_memory_8/16`. A `TIME_CTRL` write other than
    `$A130F1` goes on to the ROM mapper (`gwmapper_bank_write()`), which owns
    the bank registers `$A130F3-$FF`.

### `bus/gwenesis_bus.h`
- `enum gwenesis_bus_pad_button` gains `PAD_Z`, `PAD_Y`, `PAD_X`, `PAD_M`
  (bits 8-11 of `button_state[]`, see `io/gwenesis_io.c`).
- `GWENESIS_AUDIO_BUFFER_LENGTH_PAL` 1056 → **1072**: a PAL frame
  generates up to 313·3420/1009 = 1060.9 samples; upstream's end-of-frame
  top-up overflowed both audio buffers by ~5 samples.
- `Z80_FREQ_DIVISOR` overridable via `GWENESIS_Z80_DIVISOR` (upstream 14 ≈
  7% fast Z80; 15 is hardware-exact MCLK/15 — affects XGM1 PCM pitch, keep
  14 for upstream parity until A/B-tested on hardware).

### `vdp/gwenesis_vdp_mem.c` (region)
- `gwenesis_vdp_reset()` preserves status bit 0 rather than clearing it:
  `set_region()` runs from `load_cartridge()`, i.e. before
  `reset_emulation()`, so a plain `= 0x3C00` would drop the region on every
  game start.

### `cpus/M68K/m68k.h`
- `GWENESIS_PICO` branch: `ROM_DATA` as const pointer + masked
  `FETCH8/16/32ROM` macros.
- ROM bank switching for carts larger than 4 MB (Super Street Fighter II and
  Everdrive-"SSF" homebrew): `FETCH8/16/32ROM` index a table of sixteen 512 KB
  pages, `gw_rom` in `port/gwmapper.h`, instead of the single pointer. Every
  cartridge-ROM read in the core goes through these three macros — 68000
  instruction fetch, `m68ki_read_*`, PC-relative reads, the bus (and with it
  the Z80 bank window) and VDP DMA — so none of those needed touching. For a
  cart without the mapper the table reproduces `ROM_DATA[A & rom_addr_mask]`
  exactly: every existing test ROM renders and sounds byte-identical. Table
  and mask share a struct, so a fetch site needs one base address where it
  used to need two; this made the opcode handlers 24 KB smaller. Built with
  `-DGENESIS_ROM_MAPPER=0` the old macros return, for a frame-rate A/B.
  The page table is also what lets a ROM too large for PSRAM live half in
  flash, half in PSRAM (`romflash.cpp`): a bank never straddles the two.
  This file is CRLF, like `m68kcpu.{c,h}`.
- `cpu_memory_map memory_map[256]` (5 KB) compiled out of
  `m68ki_cpu_core` — every reader is inside `#if 0` in m68kcpu.h.

### `cpus/M68K/m68kcpu.h`
- `m68ki_read_8/16/32` short-circuit everything below `$800000` straight to
  `FETCH*ROM`, so a 68000 data read never reaches `m68k_read_memory_*` and
  therefore never reaches the bus mapper. Cartridge save RAM lives in that
  window, so these three now test `gwsram_hit()` first and go to
  `gwsram_read8/16` (the 32-bit one delegates to `m68k_read_memory_32`,
  which maps each word separately and so handles a read straddling the
  ROM/save-RAM boundary).

  Without this the writes land — `m68ki_write_*` already routes everything
  below `$FF0000` through `m68k_write_memory_*` — but every read-back
  returns the ROM mirror, so a game stores its save and then cannot see it.
  `hosttest`'s `GEN_SRAM_SELFTEST` reads through `m68ki_read_8` (via the
  `gwenesis_host_cpu_read8` hook in m68kcpu.c) precisely because checking
  only the bus entry point passes while every real game fails.

  This is the one part of save RAM that can cost frames: it is inlined into
  every opcode handler that reads memory. `gwsram_hit()` is wrapped in
  `__builtin_expect(..., 0)` so the save-RAM path stays off the straight-line
  fall-through, and the whole test compiles away under
  `-DGENESIS_CART_SRAM=0` for an on-hardware A/B (see the README).

  Still bypassed, deliberately: `m68k_read_immediate_*` and
  `m68k_read_pcrelative_*`. Both are instruction-stream fetches, and PC
  relative displacement is +/-32 KB, so neither can reach save RAM from
  code running in ROM.

### `cpus/M68K/m68kcpu.c`
- `GW_SRAM_FUNC` on `m68k_run` (dispatch loop only; the opcode handlers
  and the 320 KB `TABLES_FULL` const tables stay in flash).
- Host-only `gwenesis_host_cpu_read8()` test hook exposing `m68ki_read_8`
  (see the m68kcpu.h note above). Compiled out of the firmware.

### `cpus/Z80/Z80.c`
- **Bug fix**: the `GENESIS` opcode-fetch path declared
  `extern byte *Z80_RAM[]` (an 8-entry page table) and indexed
  `Z80_RAM[A>>13]`, but z80inst.c defines `Z80_RAM` as a single base
  pointer — any opcode fetch at A ≥ 0x2000 dereferenced garbage (Xeno
  Crisis executes its driver from the 2000-3FFF RAM mirror; caught by
  ASan in the host harness). `OpZ80` now fetches RAM/mirror below 0x4000
  and falls back to full `RdZ80` decode above.

### `sound/z80inst.c`
- **Bug fix**: `z80_start()` did not clear `Z80_BANK` (or `current_timeslice`).
  The firmware starts a new game without rebooting, so the previous game's
  bank left the Z80's 32 KB window into 68000 space pointing at the wrong
  address — a sound driver reading its samples through that window (XGM)
  got garbage, and Xeno Crisis launched as a second game showed a black
  screen. Upstream masks this with an `assert(0)` in
  `gwenesis_bus_map_z80_address` that is compiled out on the Pico.
- **Bug fix** (carried over from the old port): the Z80 RESET register
  handler pulsed the Z80 on *every* non-zero write; SGDK writes this
  register repeatedly (`Z80_getAndRequestBus` polling), restarting the
  driver forever — SGDK games hung at boot. Reset is now edge-triggered
  (pulse only on asserted→deasserted).
- The 3 chip call sites route through the `gwsnd_*` seam, preserving the
  sub-timeslice timestamps `zclk + current_timeslice − ICount·divisor`
  (this is what makes DAC PCM and XGM sample-accurate).
- `GW_SRAM_FUNC` on `RdZ80`/`WrZ80`/`z80_run`.

### `sound/ym2612.c`
- The three runtime-built LUTs (`tl_tab` 26 KB, `sin_tab` 4 KB,
  `lfo_pm_table` 16 KB, all built with libm) become generated const
  arrays in flash (`sound/luts/*.h`, emitted and checked by
  `hosttest/lutgen`); `init_tables()` keeps only the integer DETUNE
  table. `GWENESIS_HOST_LUTGEN=1` restores the original builders for the
  generator itself.
- Under `GWENESIS_LUTS_IN_RAM` the flash arrays become masters
  (`*_flash`) with per-game RAM working copies wired up by
  `ym2612_luts_init_ram()` (see the build-contract section for why).
- Small per-sample const tables (`sl_table`, `eg_inc`,
  `eg_rate_select/shift`, `lfo_samples_per_step`) placed in RAM via
  `GW_SRAM_DATA`.
- `GW_SRAM_FUNC` on `YM2612Update`/`ym2612_run`/`YM2612Write`/`YM2612Read`.
- No synthesis-logic changes.

### `sound/ym2612.h`, `sound/gwenesis_sn76489.h`
- Audio buffer externs become pointers under `GWENESIS_PICO` (allocated
  per game by `port/buffers.c`).

### Audio buffer bounds (both chips)
- `ym2612_run()` / `gwenesis_SN76489_run()` clamp the per-frame sample
  index to `GWENESIS_AUDIO_BUFFER_MAX`. Both derive the index from a
  caller-supplied master-clock timestamp, so a bad timestamp wrote past
  the audio buffers and corrupted the heap — a failure that only surfaced
  much later inside malloc/free. `gwenesis_audio_report_clamp()` (port)
  prints the offending index/timestamp once per chip.

### `sound/gwenesis_sn76489.c`
- **Bug fix**: `WhiteNoiseFeedback` was never initialized, so the noise
  LFSR shifted in zeros and the white-noise channel (drums, waves,
  explosions) went silent. Set to `0x0009` (bits 0+3, the SMS2/Genesis
  tap pattern) in `gwenesis_SN76489_Reset()`.
- `GW_SRAM_FUNC` on Update/run/Write; `GW_SRAM_DATA` on
  `PSGVolumeValues`.

### `cpus/Z80/Tables.h`
- `GW_SRAM_DATA` on the per-instruction tables (`Cycles*`, `ZSTable`,
  `PZSTable`, ~1.8 KB). `DAATable` (4 KB) stays in flash — the DAA
  opcode is rare.

### `io/gwenesis_io.c`
- **Bug fix**: new `gwenesis_io_reset()` (called from `reset_emulation()`).
  `io_reg[]` and the pad shadow were static initialisers only and never
  re-initialised, so a second game inherited the previous game's port
  direction / TH-select masks and mis-read the pads at boot (Space
  Invaders '91 rendered corrupt when launched after another game). The
  region/version byte is preserved because `set_region()` sets it per ROM.
- **6-button pad** (upstream only has the 3-button protocol). `button_state[]`
  widens from `unsigned char` to `unsigned short`: the low byte is unchanged
  (S A C B R L D U), bits 8-11 carry Z Y X Mode, PicoDrive's MXYZ layout.
  `gwenesis_io_set_six_button(pad, on)` switches ports 1 and 2; with it off the
  read path is the upstream one, bit for bit. With it on, TH rising edges on
  the data port are counted as in PicoDrive (`read_pad_6btn()`,
  `io_ports_write()`): the third TH-low read returns `SA 0000` (the 6-button
  ID), the TH-high read after it `CB MXYZ`, the next TH-low read `SA 1111`.
  The count restarts once the data port has not been written for more than
  25 lines (~1.6 ms, PicoDrive's `PAD_DELAY`), checked lazily from the
  `(frame_counter, scan_line)` stamp of the last write, so the frame loop needs
  no per-line hook. `gwenesis_io_reset()` clears the count and switches both
  ports back to 3 buttons; the port sets the pad type after
  `reset_emulation()`. Verified with `GEN_PAD_SELFTEST=1` (every read of the
  sequence plus the reset, within a frame and across NTSC and PAL frame
  ends) and the *6 Button Controller Demo* ROM, which prints each TH cycle.

### `vdp/gwenesis_vdp_mem.c`
- `VRAM` → extern pointer (port-allocated).
- `gwenesis_vdp_reset()` also clears `fifo[]` and `hvcounter_latch`, which
  upstream missed (same inter-game staleness class as the two above).
- The 4 inline CRAM→RGB565 conversions → `GWENESIS_CRAM_TO_PIXEL()`
  (format selected by `GWENESIS_PIXEL_FMT`).
- `GW_SRAM_FUNC` on `gwenesis_vdp_vram_write`, `gwenesis_vdp_hcounter`,
  `gwenesis_vdp_read/write_memory_16`.

### `vdp/gwenesis_vdp_gfx.c`
- The 16-bit renderer (upstream's Game & Watch branch) is selected under
  `GWENESIS_PICO`; the 24-bit desktop branch compiles out.
- Pixel-format parametrization: highlight mask and `SPACE`/`CONV` (H32
  4→5 upscale) come from `gwenesis_port.h`. The 565 `SPACE` red mask is
  widened from upstream's `0xe800` to the full field so shadowed pixels
  keep their low red bit.
- `buffer_line_H32`/`scaled_buffer_line` made `static` (1.1 KB of stack
  per H32 scanline vs the 3 KB core0 stack).
- **Bug fix**: `draw_line_aw()` drew the window plane starting where plane A
  stopped, which is only where the window starts when it sits on the right.
  With the window on the left (`REG17` bit 7 clear) plane A runs to the right
  edge, so the window was drawn from there on — up to `HPOS*16` bytes past
  the end of `render_buffer`, over the globals that follow it. It now starts
  at `Window_first`. Found by ASan on Demons of Asteborg's pause screen; none
  of the other test ROMs use a left window, and their output is unchanged.
- `GW_SRAM_FUNC` on `gwenesis_vdp_render_line`, `draw_line_b`,
  `draw_line_aw`, `draw_sprites`, `draw_sprites_over_planes`,
  `blit_4to5_line`.

## Known limitations (unchanged from upstream)

- Interlace mode unimplemented (`gwenesis_vdp_render_line` returns —
  Sonic 2 two-player is blank).
- ROM bank switching (`port/gwmapper.h`) switches 512 KB pages, so a 32-bit
  read of the last word of a remapped page takes its low half from whatever
  follows that bank in memory rather than from the next page. Splitting
  `FETCH32ROM` into two lookups would slow every game; no known code reads
  across a bank boundary.
- No serial EEPROM (Wonder Boy in Monster World, NBA Jam, Micro Machines
  2, Mega Man: The Wily Wars). Those carts declare a two-byte range in the
  same header field save RAM uses; `gwsram_detect()` recognises them and
  leaves the window unmapped, so they behave as they did before save RAM
  existed. Supporting them needs an I2C device model and a per-game table.
- Cartridge save RAM is allocated on first use, not at game start, and
  falls back to PSRAM when the SRAM heap cannot take it. SGDK puts a
  save-RAM declaration in every game it builds whether the game uses it or
  not, and its default asks for the whole `$200000-$20FFFF` window: 32 KB
  packed, against roughly 10 KB of SRAM heap left once the fixed buffers
  are up. Chip width comes from the type byte at `$1B2` alone — SGDK's
  header pairs an odd-byte type with an even start and an odd end, so the
  range parity is not a usable cross-check. See `port/gwsram.c`.
- VDP DMA reads cartridge space through `FETCH16ROM()`
  (`gwenesis_vdp_dma_m68k`), bypassing the bus, so a DMA sourced from save
  RAM would transfer ROM. No known game does this — save RAM is byte-wide
  and slow, which is exactly what DMA is not for. (DMA from a switched ROM
  bank is right: the page table sits under `FETCH16ROM()` itself.)
- VDP DMA is instantaneous; FIFO not emulated.
- YM2612 busy flag (status bit 7) not emulated; stereo panning compiled
  out (mono mix).

## Verification

`hosttest/` runs this exact core on Linux under AddressSanitizer, dumps
PPM frames and per-chip WAVs, and regenerates and diffs the LUT headers
(`build.sh check`).

`GEN_VERIFY_SHADOW=1` runs the core0 timer shadow (`port/gwsnd_shadow.c`)
in lockstep with the real chip, comparing every status read and aborting
on the first divergence. 3000 frames each of Thunder Force IV, Streets of
Rage 2, Gunstar Heroes, Sonic 1, Columns, Xeno Crisis and Sonic 3:
1,916,100 status reads, no divergence.

Read that total for what it is. It is not a property of the code but of
the run length, and it is dominated by one game: Thunder Force IV alone
polls 1,464,241 times (76%), Streets of Rage 2 and Gunstar Heroes about
200,000 each, and the rest are noise — Xeno Crisis manages 142 and Sonic 3
never reads the status register at all, so it verifies nothing here.
A game that does not poll cannot corroborate the shadow no matter how long
it runs. To confirm the check is not vacuous, suppress the shadow's timer-A
overflow bit (`sh.status |= 0x01` in `gwsnd_shadow_run`) and rerun Thunder
Force IV: it aborts after 460 reads with `shadow=00 chip=01`.

`GEN_FIRST_ROM=<rom> gen_host <rom2> …` runs one game, tears it down in
the firmware's exact order, then launches a second — the condition that
exposed the inter-game state bugs above. A second game's output must be
byte-identical to launching it alone; the fixes were confirmed by
reproducing the corruption with them disabled and then verifying a
4x4 warm-up/second-game matrix.
