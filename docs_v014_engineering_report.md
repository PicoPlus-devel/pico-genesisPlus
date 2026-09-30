# pico-genesisPlus v0.14 — engineering report

**Scope:** every emulator-specific change between **v0.13** and **v0.14**
(32 commits on the `revamp` branch, squash-merged as `c281bdc`).
Target: RP2350 (Raspberry Pi Pico 2 class), 520 KB SRAM, dual Cortex-M33.

This document is written for someone — human or AI — who has to maintain,
extend or re-derive this work. It records not just *what* changed but *why*,
which measurements justified each decision, and which plausible-sounding
theories were tested and **disproved**. The user-facing summary is in
`CHANGELOG.md`; the authoritative list of deltas against upstream is
`gwenesis/PORTING.md`. Read those two first if you only read two things.

---

## 1. Starting point and the decision to rebuild

v0.13 ran a heavily hand-modified copy of
[bzhxx/gwenesis](https://github.com/bzhxx/gwenesis). Sound was the known
weak point, and the modifications had drifted far enough from upstream
(~1,100 changed lines in `ym2612.c` alone, ~970 in `gwenesis_vdp_gfx.c`)
that upstream fixes could no longer be merged in.

Root cause of the bad audio, as it stood in v0.13:

- `ym2612_run()` had been reduced to ticking Timer A/B only. FM sample
  generation was driven from the PSG's cadence instead.
- Samples were produced **once per frame** at
  `GWENESIS_AUDIO_SAMPLING_DIVISOR = 6`, i.e. 148 unique samples per NTSC
  frame ≈ **8.9 kHz**, then zero-order-held ×6 to fill 888 slots.
- Those 888 samples/frame (≈53,280 Hz) were pushed into sinks configured
  for 44,100 Hz, and the surplus was absorbed by silent drop-on-full.

Upstream gets this right with a mechanism the fork had removed:
**catch-up-on-register-write** (`GWENESIS_AUDIO_ACCURATE=1`). Every
YM2612/SN76489 access carries the exact master-clock timestamp of the
access (`m68k_cycles_master()` for the 68000; for the Z80 the sub-instruction
`zclk + current_timeslice - ICount * Z80_FREQ_DIVISOR`), and the chip
synthesises forward to that instant before the register takes effect. This
is what makes YM2612 DAC PCM (the "SEGAAA!" voice) and SGDK's XGM driver
sound correct, because both depend on *when* a write lands, not just that
it landed.

**Decision:** re-vendor the core clean from upstream `168e466` and keep the
port changes minimal and documented, with all Pico-specific machinery moved
outside the core into a new `port/` directory. `gwenesis/PORTING.md` exists
so that a future upstream refresh is a re-apply, not an archaeology project.

---

## 2. Architecture as shipped

```
main.cpp            pico_shared glue: boot, menu loop, input, sinks, pacing,
                    FPS overlay, save-file I/O, diagnostics
port/               Pico-specific layer (not upstream, not pico_shared)
  frame_loop.inc      one emulated frame; #included by BOTH main.cpp and the
                      host harness so they can never diverge
  buffers.c/.h        per-game heap allocation + heap guard words
  gwsnd.h             sound engine public interface ("the seam")
  gwsnd_core0.c       seam implementation; sync mode; shadow verification
  gwsnd_core1.c       HSTX core1 sound engine, SPSC event FIFO, sample bridge
  gwsnd_fifo.h        lock-free SPSC ring (barriers are load-bearing)
  gwsnd_shadow.c      core0 replica of YM2612 timers/status
  gwsnd_resample.c    53.267 kHz -> 44.1 kHz streaming resampler + drift trim
  gwsram.c/.h         cartridge save RAM (detect, map, pack, import/export)
  savestate_stubs.c   no-op SaveState primitives (save states not implemented)
gwenesis/           vendored upstream core + documented patches
  gwenesis_port.h     the single adaptation point (placement, pixel format, seam)
  sound/luts/*.h      generated const LUTs (see §4)
  PORTING.md          every delta vs upstream, with rationale
hosttest/           Linux harness: same core, same frame loop, PPM+WAV output
```

### The sound seam

The core never calls the sound chips directly. All nine call sites
(`gwenesis_bus.c` ×6, `z80inst.c` ×3) go through:

```c
void     gwsnd_ym_write (unsigned a, unsigned v, int ts_mclk);
unsigned gwsnd_ym_read  (int ts_mclk);
void     gwsnd_psg_write(unsigned v, int ts_mclk);
void     gwsnd_frame_end(int system_clock);
void     gwsnd_line_tick(int system_clock);   /* per scanline */
```

Two implementations behind that seam:

- **Sync mode** (PicoDVI builds and the host harness): forwards straight to
  the chips — upstream behaviour, everything on core 0.
- **Offload mode** (HSTX builds, `GWSND_OFFLOAD=1`): writes become
  timestamped events in an SPSC FIFO drained by **core 1**, registered as the
  `pico_hdmi` driver's background task (`video_output_set_background_task()`,
  which already existed — no pico_shared change was needed for the hook).

Offload is HSTX-only because on PicoDVI core 1 is saturated doing software
TMDS encoding; on HSTX core 1 only services scanout DMA interrupts and has
roughly 37% idle time.

### Why core 0 needs a YM2612 timer shadow

The 68000/Z80 poll the YM2612 status register constantly — SGDK's XGM2 PCM
loop spins on the Timer A overflow bit at ~13 kHz. Those reads cannot wait
for core 1. `port/gwsnd_shadow.c` keeps a ~80-line replica of the only state
a status read can observe (mode, TA/TB, reloads, status flags) and answers
locally.

It is exact by construction: `ym2612_run()` maps a timestamp to a sample
index as `floor(target / 1009)`, which is path-independent, and Timer A ticks
once per generated sample. The shadow replays the same integer recurrence at
the same event timestamps.

**Verified, not assumed.** `GEN_VERIFY_SHADOW=1` runs shadow and real chip in
lockstep in sync mode and aborts on the first divergence: 3000 frames each of
seven games, 1,916,100 status reads, no divergence.

Read that number honestly, and this is the sort of caveat worth propagating:
it is dominated by one game. Thunder Force IV alone accounts for 1,464,241
reads (76%); Xeno Crisis manages 142 and Sonic 3 never reads the status
register at all, so it verifies nothing. A game that does not poll cannot
corroborate the shadow however long it runs. To prove the check is not
vacuous, suppress `sh.status |= 0x01` in `gwsnd_shadow_run()` and rerun
Thunder Force IV: it aborts after 460 reads with `shadow=00 chip=01`.

### Video

The renderer writes **directly into the display framebuffer**. Upstream's
16-bit render path (originally the Game & Watch branch) is selected by
`GWENESIS_PICO`, and the CRAM→pixel conversion is parametrised by
`GWENESIS_PIXEL_FMT`: **555** for HSTX (`pico_hdmi`), **444** for PicoDVI,
**565** on the host. This removed the old port's separate per-line
index→RGB pass on core 0.

Adopting upstream's renderer also brought its H32 handling: 256-pixel-wide
games are upscaled 4→5 to the full 320 (`blit_4to5_line`) instead of being
pillarboxed. Confirmed on hardware with *Columns*.

---

## 3. Bugs found in upstream gwenesis

All four were found with the host harness, three of them by AddressSanitizer
or a deliberate A/B, before any hardware flash.

| Bug | Effect | Fix |
|---|---|---|
| `OpZ80` declared `extern byte *Z80_RAM[]` (a page table) and indexed `Z80_RAM[A>>13]`, but `z80inst.c` defines `Z80_RAM` as a single base pointer | Any opcode fetch at ≥ 0x2000 dereferenced garbage. Xeno Crisis runs its driver from the 0x2000-0x3FFF RAM mirror | fetch RAM/mirror below 0x4000, full `RdZ80` decode above |
| Z80 RESET register pulsed the Z80 on *every* non-zero write | SGDK writes it repeatedly (`Z80_getAndRequestBus` polling), restarting the driver forever — SGDK games hung at boot | edge-triggered: pulse only on asserted→deasserted |
| `SN76489.WhiteNoiseFeedback` never initialised | Noise LFSR shifted in zeros: the white-noise channel (drums, explosions, waves) was **silent in every game** | set `0x0009` (SMS2/Genesis taps) in reset |
| `GWENESIS_AUDIO_BUFFER_LENGTH_PAL = 1056` | A PAL frame generates up to 313·3420/1009 = 1060.9 samples; the end-of-frame top-up overran both audio buffers | 1072 |

Two further upstream hazards were defused rather than "fixed":

- `m68ki_instruction_jump_table.h` / `m68ki_cycles.h` hold **61376** entries,
  not 65536, and `REG_IR` is a full 16-bit opcode. Any opcode ≥ 0xEFC0 (the
  line-F traps) indexed past the end and jumped through whatever `.rodata`
  followed. The port builds with `TABLES_FULL=1`.
- `assert(0)` in `gwenesis_bus_map_z80_address()` is compiled out on the
  Pico, so a bad Z80 bank returns garbage silently instead of trapping. That
  is why the stale-bank bug below presented as a black screen.

---

## 4. Memory: the binding constraint

Nothing about this port is interesting until you accept that **SRAM and XIP
flash bandwidth, not CPU cycles, are the scarce resources.**

Current Fruit Jam (HW_CONFIG 8, HSTX, save RAM on): **FLASH 979,192 B
(11.7% of 8 MB); RAM 256,288 B (48.9% of 512 KB)**; heap runs from `end` to
`__StackLimit`. At game start the runtime reports roughly
`SRAM arena=233K in=223K free=10K largest=10K`.

### Rules (two of them are the user's, and they are not negotiable)

1. **All per-game buffers are heap-allocated at game start and freed on
   return to the menu.** On boards without PSRAM the pico_shared menu
   (RomLister, screen buffer, artwork) allocates from the same SRAM heap;
   keeping ~150 KB statically reserved would OOM the menu.
2. **Plain `malloc`, never `Frens::f_malloc`, for hot data** — `f_malloc`
   lands allocations in PSRAM, which is far slower and shares the QMI port.
3. Overclock frequency/voltage pairs come from
   `pico_shared/FlashParams.cpp`, which validates stored params against
   exactly those pairs. HSTX is **378 MHz @ VREG_VOLTAGE_1_50** (v0.13's
   `main.cpp` carried a stale 1_60); PicoDVI is 324 MHz @ 1_30.

### Per-game allocation (HSTX)

| Buffer | Size |
|---|---|
| `M68K_RAM` | 64 KB |
| `VRAM` | 64 KB |
| `ZRAM` | 8 KB |
| YM + PSG sample buffers (1072 × int16 each) | 4.3 KB |
| YM2612 LUT working copies (see below) | 47 KB |
| sound event FIFO + core1→core0 bridge | 12 KB |
| cartridge save RAM | 0 unless the cart declares it (§7) |

### Flash bandwidth, and the frame-rate bug it caused

**Symptom:** after the rewrite, HSTX dropped to a steady 50 fps in exactly
the two DAC-heavy games (Sonic, Xeno Crisis).

**Cause:** core 1's synthesis was reading the YM2612 lookup tables (47 KB)
from XIP flash on *every sample* — ~20,000+ table reads per frame —
contending with core 0's 68000 dispatch tables and the PSRAM ROM. All three
share one QMI port and a 16 KB XIP cache shared between cores. The busier
the music, the more core 0's instruction fetches stalled.

**Fix:** `GWENESIS_LUTS_IN_RAM=1`. The generated LUT headers become flash
*masters* (`tl_tab_flash` etc.) and `ym2612_luts_init_ram()` copies them into
per-game heap buffers. Small per-sample const tables (YM envelope tables,
PSG volumes, Z80 cycle/flag tables — but **not** the 4 KB `DAATable`, whose
opcode is rare) are pinned in RAM permanently via `GW_SRAM_DATA`
(`__not_in_flash`). Result: solid 60 fps.

The generated LUTs also removed ~47 KB of runtime `libm` table building.
`hosttest/lutgen` emits them and `hosttest/build.sh check` re-derives and
diffs them against the committed headers, so they cannot silently rot.

Also reclaimed: `cpu_memory_map memory_map[256]` (5 KB) is compiled out of
`m68ki_cpu_core` — every reader is inside `#if 0` in `m68kcpu.h`.

---

## 5. Heap corruption: three defects and the instrumentation that found them

This was the hardest sequence in the project, and the lessons generalise.

**Lesson 0, and it cost hours:** `PICO_MALLOC_PANIC` defaults to **1**, so
the SDK wraps `malloc` and *panics* on failure. Graceful `if (!p)` handling in
emulator code is dead code on this target — and, crucially, an
"out of memory panic" may actually be heap **corruption**, not exhaustion.
`_sbrk` itself never panics; it returns failure.

The failures presented as hard faults deep inside `free()`, `malloc()` and
`mallinfo()`, i.e. arbitrarily far from the cause. What made them tractable
was adding a **guard word past every heap buffer**, verified once per frame
and again at free time (`check_emulator_mem()` in `port/buffers.c`). One run
then printed `HEAP GUARD CLOBBERED (free): psg buffer overran, guard=099e099e`
— and `0x099e` = 2462 is a plausible PSG sample value, naming both the buffer
and the writer. The guards are still in the code; they cost ~10 comparisons
per frame.

### 5.1 SPSC FIFO: a missing barrier (the root cause)

`gwsnd_fifo_pop()` read the head index, then the event payload, with its only
barrier *after* the payload read. That barrier ordered the tail publish; it
did not order the payload read against the head load. Core 1 could therefore
read an entry before core 0 published it — and on the ring's first lap those
entries were freshly `malloc`'d, i.e. **uninitialised**. A garbage timestamp
drives `index += (target - clock) / 1009` far past the frame, and
`gwenesis_SN76489_Update()` writes past the 1072-sample buffer into malloc's
chunk header.

Fixes: a barrier *after* the head load and before the payload read; the ring
is now `calloc`'d so an unpublished read yields `ts = 0` (harmless). Both
barriers are load-bearing and commented as such.

The host harness never reproduced this — sync mode has no FIFO.

### 5.2 core1 detach race (use-after-free)

`gwsnd_core1_task()` tested `engine_attached` *before* setting `in_task = 1`.
Core 0's detach could slip through that window: clear the flag, observe
`in_task == 0`, and free the audio buffers while core 1 was already past its
check and about to synthesise into them.

Fix: claim `in_task` first, `dmb`, then test the gate; on the detach side
clear the gate, `dmb`, detach, then wait for `in_task`. If core 1 fails to
stop within 250 ms, **leak the buffers with a loud message** rather than free
memory another core may be writing — a leak is recoverable, corruption is not.

Related latent hazard, still present in pico_shared: `background_task` in
`video_output.c` is not `volatile`, so core 1 may cache it and keep calling
the pointer briefly after core 0 sets it to NULL. The `engine_attached` gate
makes that harmless here, but it is worth a `volatile` for other emulators.

### 5.3 Defence in depth

- Both chips now clamp the per-frame sample index to
  `GWENESIS_AUDIO_BUFFER_MAX`, so no timestamp can overrun the audio buffers
  again; `gwenesis_audio_report_clamp()` prints the offending index/timestamp
  once per chip instead of corrupting memory.
- The frame loop clamps rendering to the frame's own `screen_height`. The
  host places the output buffer using `screen_height` sampled at frame start,
  but the core bounds its writes with the *live* `REG1_PAL` 240-line bit,
  which games toggle. A mid-frame flip wrote ~4.4 KB past the static
  framebuffer — into neighbouring `.bss`, where newlib's malloc state lives.

---

## 6. Stale state between games

**Symptoms:** *Space Invaders '91* rendered corrupt when started after any
other game; *Xeno Crisis* showed a black screen as the second game after
power-on (with `unhandled gwenesis_vdp_read_data_port_16(4)` spam).

**Cause:** the firmware starts a new game **without rebooting**, and upstream
never resets several globals:

- **`Z80_BANK`** — the Z80's 32 KB window into 68000 space. Stale, a sound
  driver reading samples through that window (XGM) fetches garbage. Now
  cleared in `z80_start()` along with `current_timeslice`.
- **`io_reg[16]` and the pad shadow** — static initialisers only, so port
  direction and TH-select masks survived into the next game. New
  `gwenesis_io_reset()`, called from `reset_emulation()`, preserving the
  region byte that `set_region()` sets per ROM.
- **VDP `fifo[]` and `hvcounter_latch`** — missed by `gwenesis_vdp_reset()`.
- Also: `memset(&m68k, 0, sizeof m68k)` before `m68k_init()`, and the TMSS
  latch reset in `load_cartridge()`.

**How this was verified — the reusable part.** The harness gained a
sequential-launch mode: `GEN_FIRST_ROM=<rom> gen_host <rom2> …` runs one
game, tears it down in the firmware's exact order, then launches the second.
A second game's output must be **byte-identical** to launching it alone.

Both bugs were reproduced with the fixes *disabled* (Space Invaders rendered
differently; Xeno Crisis hit the Z80 mapper assert) and byte-identical with
them enabled, then confirmed across a 4×4 warm-up/second-game matrix. Prove
causation by A/B, not by "it works now".

A subtle related fix: `gwenesis_vdp_reset()` preserves status bit 0 rather
than clearing it, because `set_region()` runs from `load_cartridge()` — i.e.
*before* `reset_emulation()` — so `= 0x3C00` would drop the region every game.

---

## 7. Cartridge save RAM

New in v0.14 (`port/gwsram.c/.h`, ~500 lines). The vendored core gained only
`SRAM_ADDR`/`TIME_CTRL` cases in the bus mapper.

- **Detection** follows Genesis Plus GX's `sram_init()`: the header at `$1B0`
  declares whether the cart has save RAM and over what range.
- **Storage is packed.** Real carts wire a byte-wide SRAM to one half of the
  68000 data bus, so only half the declared range is backed by silicon.
  Genesis Plus GX keeps a flat 64 KB array and fills the rest with `0xFF`;
  520 KB of on-chip SRAM cannot afford that. Indexing `(addr - start) >> 1`
  costs Sonic 3 **512 B instead of 1 KB**, and a full-window cart 32 KB
  instead of 64 KB. Unmapped halves read back `0xFF`, matching the GPGX
  filler, so nothing is lost.
- **`.srm` files stay interchangeable with PC emulators**: `gwsram_export`
  / `gwsram_import` re-interleave to the flat 64 KB layout. Files live in
  `/SAVES`. Written on exit/reset/menu-open only, never mid-game.
- **Allocated on first use, not at game start.** SGDK declares save RAM in
  *every* game it builds, used or not (`"RA", 0xF820, 0x00200000,
  0x0020FFFF`), and its default asks for the whole 32 KB window — against
  roughly 10 KB of free SRAM heap. So allocation is triggered by the first
  bus write or the presence of a `.srm`, with a `mallinfo().keepcost`
  pre-check (because `malloc` panics) and a **PSRAM fallback** via
  `PicoPlusPsram::getInstance().Malloc()` — deliberately *not*
  `Frens::f_malloc`, which panics on failure.
- **Chip width comes from the `$1B2` type byte alone.** SGDK pairs an
  odd-byte type (0xF8) with an *even* start and an *odd* end, so
  cross-checking type against range parity falls back to word-wide and asks
  for 64 KB. Sonic 3 happens to agree (odd start *and* odd end), which is
  exactly why the naive rule looked correct.

### The gotcha that cost a hardware round-trip

`m68ki_read_8/16/32` in `m68kcpu.h` short-circuit everything below
`$800000` straight to `FETCH*ROM`, so a 68000 **data read never reaches**
`m68k_read_memory_*` and therefore never reaches the bus mapper. Writes were
fine (`m68ki_write_*` does route through the bus). Net effect: Sonic 3 stored
its save, wrote a correct `.srm`, reloaded it — and could not see it, because
every read-back returned the ROM mirror.

Those three functions now test `gwsram_hit()` first. This is the one part of
save RAM that can cost frames, since it inlines into every memory-reading
opcode handler: the test is wrapped in `__builtin_expect(..., 0)` and compiles
away entirely under `-DGENESIS_CART_SRAM=0` for an on-hardware A/B.
`hosttest`'s `GEN_SRAM_SELFTEST` deliberately reads through `m68ki_read_8`
(via a host-only hook) because testing the bus entry point passes while every
real game fails.

Deliberately still unsupported: the SSF2 mapper (>4 MB ROMs) and serial
EEPROM carts (Wonder Boy in Monster World, NBA Jam, Micro Machines 2, Mega
Man: The Wily Wars — `gwsram_detect()` recognises their two-byte declaration
and leaves the window unmapped). VDP DMA reads cart space via `FETCH16ROM()`
and so bypasses save RAM; no known game DMAs from save RAM.

---

## 8. Audio delivery: the crackle investigation

Sound was correct at the chip level (the harness output was always clean) but
crackled on hardware. Two independent causes, **both in sink buffering**, plus
one still-unexplained I2S effect. This section is the one most likely to save
someone else time.

### 8.1 Intra-frame production hole

Core 0 emulated a frame flat out (~7 ms of 16.67) then idled in the frame
pacer. Core 1 may only synthesise up to core 0's per-line watermark, so
**~9.6 ms of every frame produced no audio** while sinks drained at
44.1 samples/ms.

**Fix: sub-frame pacing.** `GWENESIS_LINE_PACE` in `port/frame_loop.inc`
calls `paceScanline()` every `PACE_LINE_CHUNK` lines, sleeping to a
proportional intra-frame deadline so emulation is spread across the frame and
core 1 always has work. Cost: **+12 bytes of bss.**

Bigger buffers were rejected: there is almost no SRAM left, and the HDMI
data-island ring already costs 36 KB for 1024 samples.

Note `paceScanline` never stretches a late frame (`if (now >= deadline)
return;`), and it drains the core1→core0 bridge before parking.

### 8.2 Cross-frame queue walk

A frame that overruns its period costs ~20 packets of backlog, while the
drift trim's ±0.5% could only refill ~0.9 packets/frame — so heavy scenes
walked the queue empty regardless. Fixed by raising the DI target 96→160
packets and the watermark 200→240 (`HSTX_AUDIO_DI_HIGH_WATERMARK=240` in
CMake), giving the trim ±1% authority, and using a fixed per-packet error
sensitivity so loop gain no longer shrinks as the target grows.

**Why ±1% and not ±0.5%:** the Fruit Jam TLV320 clocks about **0.61% below
44100** (measured: ring stable at target while delivering 43,830 samples/s).
±0.5% authority could never hold the ring even with a perfect fill reading,
because the offset exceeded the correction range. ±1% tracks it with ~0.39%
to spare — that is the real headroom constraint for any new board.

PicoDVI shares `gwsnd_set_fill_permille()`, so its error term is halved in
`sinkFillPermille()` to keep its effective response identical; it leans on
the trim continuously and would otherwise detune twice as hard.

### 8.3 I2S starvation — fixed, mechanism NOT understood

Headphone output starved: the I2S ring pinned at 0-12% instead of ~50%. Fixed
by `publishI2sFill()` in `main.cpp` — core 0 samples the ring level with
interrupts held and hands core 1 a single word.

**Do not "fix" this again the obvious way.** The natural theory — that
`audio_i2s_get_fill_permille()` reads `write_index`/`read_index` as two words
and a reader straddling the DMA IRQ sees "almost full" — was implemented as a
seqlock-style retry *inside the driver* (verified present in the
disassembly) and **did not work**, while the app-side publish does.

The real clue: production settles at ~43,830/s either way, so the loop
equilibrates in both cases. For a P-controller that requires `used ≈ 563`,
meaning **core 1 computes ~563 while core 0 computes ~51 for the same ring** —
a near-constant offset, not intermittent corruption. The next step is to
instrument what core 1 actually computes (`dbgTrimUsed` / `dbgTrimPermille`
in `sinkFillPermille`) rather than theorise again.

### 8.4 Residual underruns: a production deficit, not delivery

The plausible remaining theory was that `paceScanline` returns early on a
late frame *before* draining the bridge, so audio only reaches the sinks at
the end-of-frame drain. **Implemented, measured, disproved on hardware:**
hoisting the drain above the early-out changed nothing, and `di=0` still
coincided with every underrun burst. It was reverted.

The actual mechanism is arithmetic. Underruns track frame rate almost
perfectly (71 windows, Sonic 3): `frames=61` → mean 5.0 underruns, 32/47
windows at zero; `frames=60` → mean 68.1, **0/21** at zero; `frames=59` →
mean 284. A frame taking 20 ms of wall clock still synthesises only one frame
period of audio: it consumes 20 × 44.1 = 882 samples and produces 735, a
**147-sample deficit per late frame**, so a 640-sample DI queue empties after
~4 consecutive late frames. No delivery change can create samples that were
never generated. The only real remedies are making heavy scenes fit the frame
budget, or frameskip — which is why frameskip clears it subjectively.

**Judgement call worth preserving:** with frameskip on, Sonic 3 runs a steady
60 fps with no audible glitches. The underrun counter is far more pessimistic
than what reaches the speakers. Do not treat a non-zero count as a defect
absent a reported audible symptom.

---

## 9. PAL

PAL games now run at **50 Hz** (`frame_period_us = is_pal ? 20000 : 16667`),
with `lines_per_frame` 313 and the resampler retuned per mode
(`gwsnd_set_pal()`; in offload mode the switch is applied on core 1 at the
next frame boundary). Note the audio buffers are sized for PAL (1072) in both
modes because the mode can change at runtime.

---

## 10. Diagnostics that shipped

Kept deliberately, because each caught a real bug and none is measurable in
frame time:

- **Heap guard words** past every emulator buffer, checked per frame.
- **`AUDIO CLAMP: <chip> index=… target=… frame=… line=…`** — once per chip.
- **One `[heap]` line per game start** — SRAM arena/in-use/free/largest plus
  PSRAM totals and lwmem alloc/free counts. Especially useful on the
  still-untested non-PSRAM boards.
- **SELECT+DOWN in-game perf line**, once per second: `emu avg/max`,
  `loop avg/max`, sound FIFO high-water, drops, `di=min/max`, underruns,
  HSTX resyncs.

**How to read the perf line** (this trips people up): `emu avg ≈ 16.4 ms` is
*not* load — `paceScanline` sleeps *inside* `gwenesis_frame_run`, so `emu_us`
is dominated by deliberate sub-frame pacing. Only `emu avg` **exceeding** the
frame period means genuine overload, because the pacer bails out on a late
frame rather than stretching it. Judge by `max`, `frames`, `underruns` and
`di` min instead. `di=min` is a true intra-frame trough; a `max` nearing 240
means packets are being dropped (no counter exists for that).

---

## 11. The host harness

`hosttest/` compiles the *same* core sources and the *same* frame loop
(`port/frame_loop.inc`) as a native Linux binary with AddressSanitizer, RGB565
output, PPM frame dumps and WAV audio. It found three of the four upstream
bugs and both inter-game state bugs.

```bash
./hosttest/build.sh                                  # build gen_host
./hosttest/build.sh check                            # re-derive + diff LUT headers
./hosttest/gen_host <rom> 600 60 hosttest/out        # 600 frames, dump every 60th
python3 hosttest/ppm2png.py 'hosttest/out/*.ppm'
python3 hosttest/wavglitch.py <capture.wav>          # glitch analysis
```

Outputs `mixed.wav` (final 44.1 kHz), plus `ym.wav` / `psg.wav` (each chip at
its native rate) — invaluable for attributing a sound problem to one chip.

Env knobs: `GEN_FIRST_ROM` (sequential launch, §6), `GEN_VERIFY_SHADOW`
(§2), `GEN_SRAM_SELFTEST` and `GEN_SRM` (§7), `GEN_PRESS_START/A/B/C`
(frame-ranged input injection).

`wavglitch.py` is what identified the crackle class: glitches folding at
exactly 60.0 Hz with dip widths in multiples of 4 mean HDMI silence-packet
insertion — a **sink underrun**, not a chip bug.

Test ROMs in `hosttest/roms/` are gitignored (note the `.md` extension
collides with Markdown, so the ignore rule is scoped to that directory rather
than global).

---

## 12. Build knobs

| Define | Default | Meaning |
|---|---|---|
| `GWENESIS_PICO` | 1 | select the port branches in the vendored core |
| `TABLES_FULL` | 1 | full 65536-entry M68K tables (see §3) |
| `GWENESIS_PIXEL_FMT` | 555 HSTX / 444 DVI / 565 host | CRAM→framebuffer format |
| `GWSND_OFFLOAD` | 1 on HSTX | core1 sound engine |
| `GWENESIS_LUTS_IN_RAM` | 1 on Pico | YM2612 LUT RAM copies (§4) |
| `GWENESIS_Z80_DIVISOR` | 14 | upstream MCLK/14; 15 is hardware-exact, affects XGM1 PCM pitch — **still not A/B'd on hardware** |
| `GENESIS_CART_SRAM` | 1 | save RAM; 0 compiles the 68000 read test out for perf A/B |
| `HSTX_AUDIO_DI_HIGH_WATERMARK` | 240 | HDMI data-island drop threshold (§8.2) |
| `GENESIS_OVERCLOCK_HSTX_FIX` | 1 | route `clk_hstx` from PLL_USB at 378 MHz (PIO-USB builds only) |

`./bld.sh` does **not** forward `-D`, so for an A/B build:
`./bld.sh -c8` once, then
`cmake -S . -B build -DGENESIS_CART_SRAM=0 && cmake --build build -j`.

---

## 13. Traps for the next maintainer

1. **`PICO_MALLOC_PANIC=1`** — malloc panics; your NULL checks are dead code;
   an OOM panic may be corruption (§5).
2. **Never text-mode round-trip `gwenesis/cpus/M68K/m68kcpu.{c,h}`.** Those
   two vendored files are CRLF. A plain Python read/write rewrote all 3950
   lines to LF, burying a 29-line patch and destroying the
   minimal-diff-from-upstream property `PORTING.md` exists to preserve. Use
   binary mode; check with `git diff --stat` / `--ignore-cr-at-eol`.
3. **`m68ki_read_*` bypasses the bus mapper below `$800000`** (§7). Any new
   memory-mapped device in cart space must patch those too, and must be
   tested through them.
4. **Save-RAM accessors must stay out of line.** Inlining them cost 1.7 KB of
   SRAM text, because the 32-bit bus variants expand the 16-bit path twice.
5. **Bugs that only appear on the second game are stale globals** — reach for
   `GEN_FIRST_ROM` before hardware (§6).
6. **Any new per-frame work that makes core 0 bursty, or pushes frames over
   their period, re-opens the audio hole in §8.1.**
7. **Judge audio on a TV, not a monitor's speakers.** A cheap monitor made
   perfectly good audio sound choppy and sent one whole investigation down a
   blind alley.
8. **Test-ROM names lie:** `hosttest/roms/sonic1.md` is actually Sonic 3 and
   `sonic3.md` is Sonic 1 (checked at header `$150`); `tf4.md` is
   "Lightening Force". None of the local set is PAL.

---

## 14. State at v0.14

**Verified on hardware** (Adafruit Fruit Jam, HSTX): 60 fps including
DAC-heavy scenes; FM, PSG noise, DAC PCM and SGDK/XGM audio correct on a TV;
10+ exit/restart cycles without a crash; H32 full-width; Sonic 3 saves and
reloads via `.srm`.

**Verified on host**: shadow lockstep (§2), LUT re-derivation, ASan-clean
multi-thousand-frame runs, 4×4 sequential-launch matrix, save-RAM self-test.

**Not yet verified on hardware:** PicoDVI boards (sync-mode audio on core 0,
RGB444 palette, frameskip behaviour), PAL end-to-end, non-PSRAM boards,
bootloader launch, `GWENESIS_Z80_DIVISOR=15`.

**Known limitations:** interlace unimplemented (Sonic 2 two-player); no SSF2
mapper (>4 MB ROMs); no serial EEPROM carts; VDP DMA instantaneous and FIFO
unemulated; YM2612 busy flag unemulated and stereo panning compiled out
(mono mix); save states not implemented (`port/savestate_stubs.c`); PicoDVI
runs the display at 77.1 Hz.

---

## 15. Where to look first

| Question | File |
|---|---|
| What differs from upstream gwenesis, and why | `gwenesis/PORTING.md` |
| How a frame is emulated | `port/frame_loop.inc` |
| How sound gets from a register write to a speaker | `port/gwsnd.h` then `gwsnd_core0.c` / `gwsnd_core1.c` |
| Why the buffers are sized the way they are | `port/buffers.c`, §4 here |
| Save RAM | `port/gwsram.h` header comment |
| ROMs over 4 MB (bank switching; added after v0.14) | `port/gwmapper.h`, then `romflash.h` for ROMs too large for PSRAM |
| Reproducing a bug without hardware | `hosttest/` and §11 here |
| User-facing changes | `CHANGELOG.md` |
