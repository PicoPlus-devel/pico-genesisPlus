# Test ROMs (not committed)

Everything in this directory except this README is gitignored — drop the
test ROM set here. The harness takes any `.md`/`.bin`/`.gen` path, but the
regression matrix assumes:

| File (suggested name)  | Game | Exercises |
|---|---|---|
| `sonic1.md`            | Sonic the Hedgehog | DAC drums |
| `sonic3.md`            | Sonic the Hedgehog 3 | SEGA voice (YM2612 DAC PCM), FM music, cartridge save RAM |
| `sor2.md`              | Streets of Rage 2 / Bare Knuckle 2 | FM + shadow/highlight |
| `gunstar.md`           | Gunstar Heroes | shadow/highlight + heavy DMA |
| `columns.md`           | Columns | H32 (256-wide) mode |
| `xeno_crisis.md`       | Xeno Crisis | SGDK / XGM driver (issue #11 repro) |
| `tf4.md`               | Thunder Force IV / Lightening Force | worst-case load; by far the heaviest YM2612 status poller |
| any EUR ROM            | — | PAL timing (313 lines, 50 Hz) |
| `ssf2.md`              | Super Street Fighter II (5 MB) | ROM bank switching (`port/gwmapper.h`); without it: red screen, no sound |
| `6ButtonControllerDemo.bin` | 6 Button Controller Demo (homebrew) | 6-button pad protocol: prints the port value of every TH cycle and which X/Y/Z/Mode buttons are held. Needs `GEN_PAD=6` (its header does not list a 6-button pad) |
| `doa.bin`              | Demons of Asteborg (15 MB) | bank switching over 30 banks, SGDK save RAM banked via `$A130F1`, a left-side window plane; run it again with `GEN_SPLIT_BANKS=15` (the firmware's flash/PSRAM split) and compare |

SSF2 needs button presses to get past the title into a fight, e.g.
`GEN_PRESS_START="2450:2456,2560:2566,2680:2686,2800:2806,2920:2926,3040:3046"`
over 6000 frames.

With `GEN_PAD=6`, SSF2 reacts to `GEN_PRESS_X/Y/Z` in a fight (e.g. frames
4150:4153 with the START sequence above); with the default 3-button pad the
same presses leave the output byte-identical. `GEN_PAD_SELFTEST=1` checks the
protocol itself without running a game.

Check a ROM before trusting its filename: the game name is at file offset
`$120` and the save-RAM declaration at `$1B0` (`RA` when the cart has save
memory — of the two Sonic ROMs only Sonic 3 does). The two used to be the
wrong way round, which silently pointed the `GEN_SRM` save-RAM example at a
game with no save memory.

Example run:

```bash
./hosttest/build.sh
./hosttest/gen_host hosttest/roms/sonic1.md 600 60 hosttest/out
python3 hosttest/ppm2png.py hosttest/out     # PPM -> PNG
# listen: hosttest/out/mixed.wav (44.1 kHz), ym.wav / psg.wav (chip rate)
```
