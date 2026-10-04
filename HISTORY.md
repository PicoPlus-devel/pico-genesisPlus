# History of changes.

# v0.14 Release notes

Games now run at full speed on HSTX boards, in busy scenes as well. The emulator
core has been rebuilt from the upstream
[Gwenesis](https://github.com/bzhxx/gwenesis) sources with a new sound engine, so
sound should be close to what the real console does — including the games that had
no sound effects at all before. Two more big changes on top of that: games can save
their progress, and European (PAL) games run at the speed they were made for.

<a name="performance"></a>
## Performance

- Full speed on HSTX boards, including in games with heavy sound activity: a
  Raspberry Pi Pico 2 or Pimoroni Pico Plus 2 on the PicoNES PCB or on a breadboard,
  the Adafruit Fruit Jam, the Adafruit Metro RP2350 and the Murmulator M2.
- **Games run slower on boards without HSTX** — the Pimoroni Pico DV Demo Base
  (`-c1`), Waveshare RP2350-Zero / PicoNES Mini (`-c6`), Waveshare RP2350-USB-A /
  PicoNES Micro (`-c9`), Spotpear HDMI board (`-c10`) and Murmulator M1 (`-c12`).
  Putting the picture on screen takes so much of the board's attention that the
  emulator does not get enough left over, so the action, the music and the sound all
  drag a little. The games are still playable, and this is not something that can be
  tuned away. See
  [Speed on PicoDVI boards](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#speed-on-picodvi-boards)
  in the readme for the full story and the list of boards that do run at full speed.

## Sound

- **New sound engine.** Music and sound effects are much closer to what the real
  console produces.
- **The "SEGAAA!" voice and other digitized sounds now play correctly.**
- **Drums, explosions, waves and other noise effects are back.** That part of the
  sound chip was completely silent before, so those effects were missing from every
  game.
- **Games developed with [SGDK](https://github.com/Stephane-D/SGDK), such as
  *Xeno Crisis*, now have their sound effects and music.** This closes
  [#11](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/11).
- Sound no longer drops out in busy scenes.
- On HSTX boards the sound is produced on the second processor core, which leaves
  more room for the game itself.

## Saved games

- **Games with a battery-backed cartridge memory can now save, to the SD card.**
  *Sonic the Hedgehog 3*, *Sonic & Knuckles*, the *Phantasy Star* and *Shining
  Force* games, *Story of Thor*, the NHL series and many others were previously
  unable to keep any progress. Nothing to switch on; the emulator recognises the
  cartridges that have save memory and gives them a file of their own.
- The save is written when you quit the game, when you reset it, and when you open
  the settings menu with SELECT + START — so leaving through **Enter bootsel mode**
  or **Return to emulator selection menu**, which restart the board immediately,
  keeps it too. Nothing is written while you play, so a board switched off mid-game
  loses whatever was saved since the menu was last opened.
- The files are in `/SAVES` on the card, named after the rom with a `.srm`
  extension, in the same format Genesis Plus GX and Kega use — so a save can be
  carried to a PC emulator and back.
- A few games — mostly homebrew ones — ask for far more save memory than they ever
  use. On a board with PSRAM they save normally; without PSRAM they play fine but
  cannot save, and say so on the serial console.
- Cartridges with a serial EEPROM rather than a RAM chip (*Wonder Boy in Monster
  World*, *NBA Jam*, *Micro Machines 2*, *Mega Man: The Wily Wars*) still cannot
  save. They need a different chip emulated, which is not in this release.

## PAL games

- **Games for European (PAL) consoles now run at 50 Hz.** They used to run at 60 Hz
  — too fast, with the music and sound pitched up to match. A rom marked as Europe
  now runs at the correct speed. Roms marked for more than one region (`JUE`) keep
  running at 60 Hz, as they would on an American console.
- PAL games leave more time for each frame, so **Frame Skip** in the settings menu
  can usually be switched off for them, which gives a smoother picture.

## Video

- **256-pixel-wide games now fill the screen.** Games such as *Columns* were
  previously shown with black borders on the left and right; they are now scaled to
  the full width, as on real hardware.
- Fixed a corrupted picture in games that change the screen height while the picture
  is being drawn.

## Stability

- Fixed crashes and out-of-memory errors when leaving a game and starting another
  one. Games can now be started and exited as often as you like.
- Fixed *Space Invaders '91* showing a corrupted screen when started after another
  game had been played.
- Fixed *Xeno Crisis* showing a black screen when started as the second game after
  power-on.
- Fixed a crash when opening a file that is not a rom.

## Controllers

- **NES controllers can now press C.** Controllers on the NES/SNES GPIO port and the
  AliExpress NES USB controller have no third button, which left the Genesis C
  button out of reach. SELECT now doubles as C while a game runs. Every SELECT + ...
  combination keeps working, and C is held back while START is down, so
  SELECT + START still opens the settings menu. USB SNES controllers are unaffected —
  they have a real X button — and nothing changes in the menu.
- **Controllers on the NES/SNES GPIO port now use the same buttons as everywhere else.**
  A SNES pad there was read as if it were a NES pad, so only the first eight buttons it
  sends were used: B worked as the Genesis A button, but Y ended up as B and the pad's
  own A and X did nothing at all. NES pads had their two buttons the other way round
  from every USB controller. The port now works out which pad is plugged in and both
  follow the button table in the readme: **on a NES pad B is Genesis A and A is Genesis
  B; on a SNES pad B is Genesis A, A is Genesis B and X is Genesis C.** SNES Y, L and R
  are not used — the Genesis pad has three buttons. SELECT still doubles as C on both,
  and nothing changes in the menu.
- **The C button on a Genesis controller now opens the recently played list in the
  menu.**

## Menu

- **Recently played games.** The menu now remembers the last 20 games you started,
  most recent first. Open the list with Button3 in the rom browser, or with
  **Recently played** in the settings menu (SELECT) — the settings route also works
  on controllers without a third button, such as a NES pad on the GPIO port. Games
  can be started from the list or removed from it with SELECT. The list is stored as
  plain text in `/recent_MD.txt` on the SD card, so it can be edited or deleted from
  a PC.
- **Boards without PSRAM no longer copy the rom to flash when it is already there.**
  Restarting the game you just played, or picking it again from the recently played
  list where it is marked `[READY]`, now takes about a second instead of the several
  seconds of blank screen the flash write used to cost. The rom already in flash is
  reused only when it really is the same file.
- **Files that are not Mega Drive roms are refused.** The rom list goes by file name
  only, and both `.md` and `.bin` match plenty of files that are not games — a
  markdown README shows up in the list. Picking one used to crash the emulator; it
  now says what is wrong and returns to the menu.
- **Game audio and Frame Skip can be switched on and off again** in the settings
  menu. Frame Skip draws two out of every three frames to keep games running at
  speed, and is on by default.

## Hardware

- **The PicoNES PCB now takes a Pimoroni Pico Plus 2.** Design **v2.6**
  (`pico_nesPCB_v2.6.zip`, attached to this release) added through-holes, so the
  Pico can be mounted on male headers instead of soldered flat — which is what a
  Pimoroni Pico Plus 2 needs, its SP/CE connector prevents it from lying against
  the PCB.
  That gets you 8 MB of PSRAM on the PCB: games start the moment you select them,
  with none of the flash copying a plain Pico 2 has to do, and larger roms fit.
  No separate binary is needed — `picogenesisPlus_AdafruitDVISD_pico2_arm.uf2`
  reads the flash size and detects PSRAM at boot.
- All three PCB designs — the PicoNES, the PicoNES Mini (Waveshare RP2350-Zero) and
  the PicoNES Micro (Waveshare RP2350-USB-A) — are attached to this release, and the
  readme has a new
  [Custom PCBs](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#custom-pcbs)
  section covering mounting, parts, which binary to flash and the 3D-printed cases.

## Known limitations

- Cartridges with a serial EEPROM instead of a save memory chip still cannot save:
  *Wonder Boy in Monster World*, *NBA Jam*, *Micro Machines 2* and *Mega Man: The
  Wily Wars*. Ordinary battery-backed cartridges do save, see
  [Saved games](#saved-games) above.
  ([#20](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/20))
- Roms larger than 4 MB do not work: they need bank switching that is not emulated,
  so a game such as *Super Street Fighter II* breaks once it reaches past the first
  4 MB. On a board with PSRAM such a rom does fit in memory and will start.
  ([#21](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/21))
- Sound is mono: both sound chips are mixed into one channel that goes to the left
  and the right speaker alike, so the stereo effects in games such as *Sonic* and
  *Streets of Rage* play in the middle.
  ([#22](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/22))
- Games that use interlace mode are still not supported; for example the two-player
  levels of *Sonic the Hedgehog 2* show a blank screen.
  ([#23](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/23))
- The region comes from the rom itself. A Europe-only rom runs at 50 Hz, everything
  else at 60 Hz; there is no setting to force one or the other.
  ([#24](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/24))
- Non-HSTX (PicoDVI) boards still run the display at 77.1 Hz, see
  [#4](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/4).
- Games do not run at full speed on non-HSTX (PicoDVI) boards, see
  [Performance](#performance).
- A NES pad clone on the NES/SNES GPIO port can lose its B button. The port now tells
  the two pad types apart by the shift register outputs a NES pad does not use, which an
  original Nintendo pad grounds — as do most aftermarket pads. One that leaves them
  floating is taken for a SNES pad, where that button is Y and the Genesis has nowhere
  to put it. Its A button and SELECT still work, and the pad behaves normally in the
  menu, so only the in-game B button is affected. To check a pad, open
  **Settings > Controller Test**, press a button and read the `Sent by pad:` line: a top
  digit of `F` means the pad identifies itself properly.
  ([#28](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/28))

## For developers

- Every difference from the upstream Gwenesis core is documented in
  `gwenesis/PORTING.md`, so the core can be updated from upstream again.
- New PC test harness in `hosttest/`: the same emulator core built for Linux, which
  makes it possible to find emulation bugs without hardware.

# v0.13 Release notes

The notes below cover all changes since **v0.11**. The items that were first published in v0.12 are repeated here, so that this list is complete for users upgrading directly from v0.11.

## pico-bootLoader

- This emulator can now be used with the new [pico-bootLoader](https://github.com/PicoPlus-devel/pico-bootLoader). The bootloader lets you keep several emulators, and a *Doom* port, on one board and choose which one to start from an on-screen menu at power-on. Switching between them no longer requires a computer.
- The bootloader and an SD card archive containing this emulator are available on the [pico-bootLoader releases page](https://github.com/PicoPlus-devel/pico-bootLoader/releases). Installation is described in the readme of that project.
- When the emulator is started from the bootloader, the settings menu contains an extra item, **Return to emulator selection**, to return to the boot menu.
- The binaries listed at the end of this page are standalone versions and are installed via BOOTSEL as before. To build a bootloader version yourself, add `-DBUILD_FOR_BOOTLOADER=ON` to the cmake command line, or use `./bld.sh -2 -c <HW_CONFIG> -b`.

## Video and HDMI

- For the boards that use HSTX instead of PicoDVI, HDMI audio is supported via the HSTX video driver. Thanks to [@fliperama86](https://github.com/fliperama86) for the [pico_hdmi](https://github.com/fliperama86/pico_hdmi) driver that made this possible and for helping out.
  - Adafruit Fruit Jam.
  - Murmulator M2.
- Other RP2350 configurations that use HSTX (GPIO 12 - 19) instead of PicoDVI:
  - [Breadboard](https://github.com/PicoPlus-devel/pico-infonesPlus?tab=readme-ov-file#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard)
  - [PCB](https://github.com/PicoPlus-devel/pico-infonesPlus?tab=readme-ov-file#pcb-with-raspberry-pi-pico-or-pico-2)
  - [Adafruit Metro RP2350](https://github.com/PicoPlus-devel/pico-infonesPlus?tab=readme-ov-file#adafruit-metro-rp2350)

  All the other boards still use PicoDVI. To enable audio over HDMI, make sure external audio is disabled in the settings menu.
- HDMI audio on HSTX boards is more reliable: audio dropouts are resolved and more TVs and AV receivers are accepted.
- Fixed dots and dotted lines that could appear in the picture on some HDMI monitors.
- New **Display Mode** setting on HSTX boards, to choose between HDMI and DVI output. DVI has slightly lower latency but carries no audio.

## Settings menu

- New layout, with a SAVE / CANCEL / DEFAULT row and a scrollable list of options.
- The scanlines on/off option has been replaced by **Screen Mode**, which offers 1:1 with and without scanlines.
- New **Scanline Type** option on HSTX boards: *Simple* or *LCD*.
- New **Controller Test** screen. It shows a gamepad on screen that follows the controller you last pressed a button on, and lists the connected controllers. This makes it possible to check wiring and button mappings without starting a game. Hold SELECT+START for 2 seconds to leave the screen.
- Added an option to enter BOOTSEL mode for flashing firmware.
- Added an option to return to the boot menu when the emulator was started from the bootloader.
- The game list now starts in the `/roms/MD` folder instead of the root of the SD card. When that folder does not exist, the root folder is used. Placing your ROMs in `/roms/MD` is the recommended layout.
- When leaving a subfolder, that folder stays selected in the list instead of returning to the top.
- Settings are saved correctly when a game is reset, and settings changed with in-game button combinations are saved when returning to the menu.
- The software version is shown on the splash screen.
- Note: the format of the settings file has changed. Existing settings in `/settings_md.dat` are reset to their default values the first time this version starts.

## Controllers

- Retro-bit Mega Drive Arcade pad: the X, Y, Z, L and R buttons now work.
- DualShock 4 / DualSense: the L2 and R2 triggers now act as L and R.
- PlayStation Classic controller: the Square button now works.
- Wii Classic controller: the L and R shoulder buttons now work, as do ZL and ZR.
- SNES controllers can now be used on the controller port of the PCB and breadboard setups. NES controllers keep working as before and are recognised automatically.
- USB keyboard: added V, Q and W.
- Fruit Jam: fixed the sound chip failing to start when an SNES Classic Mini controller is connected at power-on. That controller can now also be used from the moment the menu appears.

## Games

- Games developed with [SGDK](https://github.com/Stephane-D/SGDK), such as *Xeno Crisis*, now start and are playable. Sound effects in these games are still missing. [#11](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/11)
- Starting a second game without switching the board off in between no longer leaves data of the previous game behind.

## Other

- More stable SD card access.
- Several stability fixes in the menu and the video output.

# v0.12 Release notes

For the boards that use HSTX in stead of PicoDVI: HDMI audio is now supported via the new HSTX video driver. Huge thanks to [@fliperama86](https://github.com/fliperama86) for the awesome [pico_hdmi](https://github.com/fliperama86/pico_hdmi) driver that made this possible and for helping out.

- Adafruit Fruit Jam.
- Murmulator M2. 

Other RP2350 configurations that now use HSTX (GPIO 12 - 19) in stead of PicoDVI:

- [Breadboard](https://github.com/PicoPlus-devel/pico-infonesPlus?tab=readme-ov-file#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard)
- [PCB](https://github.com/PicoPlus-devel/pico-infonesPlus?tab=readme-ov-file#pcb-with-raspberry-pi-pico-or-pico-2)
- [Adafruit Metro RP2350](https://github.com/PicoPlus-devel/pico-infonesPlus?tab=readme-ov-file#adafruit-metro-rp2350)
  
All the other boards still use PicoDVI.

To enable audio over hdmi, make sure external audio is disabled in the settings menu.

- Added option in settings menu to enter bootsel mode for flashing firmware. 
- Partially fixed: Games developed with SGDK (like XenoCrisis) are now playable. However soundeffects are missing. [#11](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/11)

# v0.11 Release notes

- Added support for [Murmulator M1 and M2 boards](https://murmulator.ru). [@javavi](https://github.com/javavi)  [#150](https://github.com/PicoPlus-devel/pico-infonesPlus/issues/150)
  - M1: RP2040/RP2350
  - M2: RP2350 only
  **Note**: These Murmulator M1 and M2 builds are untested. Please report any issues.
- **Fruit Jam only**: Add volume controls to settings menu. Can also be changed in-game via (START + LEFT/RIGHT). Note that too high volume levels may cause distortion. (Ext speaker, advised 16 db max, internal advised 18 dB max). Latest metadata package includes a sample.wav file to test the volume level.
- Updated GenesisPlusMetaData.zip: Added **sample.wav**. This sample will be played when using the Fruit Jam volume control in the settings menu. Note when **/soundrecorder.wav** is found, this file will be played in stead.
- Updated the menu to also list .wav audio files.
- Added basic wav audio playback from within the menu. Press BUTTON2 or START to play the wav file. Tested with https://lonepeakmusic.itch.io/retro-midi-music-pack-1 The wav file must have the following specs:
  - 16/24 bit PCM wav files only.  (24 bit files are downsampled to 16 bit) 
  - 2ch stereo only.
  - Sample rate supported: 44100.
- **RP2350 with PSRAM only**: Record about 30 seconds of audio by pressing START to pause the game and then START + BUTTON1. Audio is recorded to **/soundrecorder.wav** on the SD-card.

>[!NOTE]
> Currently wav playback is too fast. 

## Fixes

- Fruit Jam audio fixes.
- Settings changed by in-game button combos are saved when exiting to menu.

# v0.10 Release notes

## Features

- Settings are saved to /settings_md.dat instead of /settings.dat. This allows to have separate settings files for different emulators (e.g. pico-infonesPlus and pico-peanutGB etc.).
- Added a settings menu.
  - Main menu: press SELECT to open; adjust options without using in-game button combos.
  - In-game: press SELECT+START to open; from here you can also quit from the game.
- Switched to Fatfs R0.16.

## Fixes
- Improved FPS limiter: now uses a fixed timestep approach and initializes timing at the start of emulation, ensuring stable frame rate from the first frame and correct behavior when emulator is overloaded.
- Fruit Jam: Check VU-Meter toggle only once per frame in stead of multiple times per frame.
- Fruit Jam: In game SELECT + RIGHT to toggle VU-Meter now works properly. 
- Show correct buttonlabels in menus.
- removed wrappers for f_chdir en f_cwd, fixed in Fatfs R0.16. (there was a long standing issue with f_chdir and f_cwd not working with exFAT formatted SD cards.)

# v0.9 release notes

## Features
- Fruit Jam: overclock speed set to 378Mhz (was 340 Mhz)
- When (S)NES classic controller is connected, a W is shown next to the flash/PSRAM size

## Fixes

- Fixed not properly working SNES Classic/WII Pro I2C controller. (NES Classic I2C always worked fine). 
- Fruit Jam: Initialisation of (S)NES classic/WII Pro I2C controller will now take place after DAC is initialized. [#4](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/10). You need to plug-in the controller **after** the intro screen is shown. In case the controller is connected and the DAC fails to initialize, an error screen is shown.

# v0.8 release notes

- Game pad fixes for Genesis style USB controllers. See README.

# v0.7 release notes

- Added support for [Retro-Bit Genesis/Megadrive 8 button Arcade Pad with USB](https://www.retro-bit.com/controllers/genesis/#usb).
- Settings:
  - Version number added. When the version in settings.dat does not match, settings will be reset to defaults.

# v0.6 release notes

## Features

- Audio works now [#3](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/3), although quality is not good. You can toggle audio on/off with SELECT + RIGHT on the controller. This increases the emulation speed a bit.
- Metadata support added. Download the [metadata]((https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/GenesisPlusMetadata.zip)) from the releases below and extract the zip file to the root of your SD card. This adds game titles and box art to the menu. The screensaver also uses box art images.
- Support for [Adafruit Fruit Jam](https://www.adafruit.com/product/6200). This version keeps the monitor at a 60 Hz refresh rate and should work on every monitor. The Fruit Jam has a built-in speaker and a NeoPixel LED strip that can be used as a VU meter.
- [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107) support added.
- When PSRAM is available, roms are loaded from SD card to PSRAM instead of flash. This speads the loading time up a lot.
- Uses Pico SDK 2.2.0

## Fixes

- When SD card is prepared on MacOs, the additional metadata files created by MacOs caused several issues. This is fixed now. (Hopefully, because I cannot test this myself) 
- several small fixes and improvements.

# v0.5 release notes (this is a re-release)

- Releases now built with SDK 2.1.1
- Support added for Adafruit Metro RP2350 board. See README for more info. No RISCV support yet.
- Switched to SD card driver pico_fatfs from https://github.com/elehobica/pico_fatfs. This is required for the Adafruit Metro RP2350. Thanks to [elehobica](https://github.com/elehobica/pico_fatfs) for helping making it work for the Pimoroni Pico DV Demo board.
- Besides FAT32, SD cards can now also be formatted as exFAT.
- Nes controller PIO code updated by [@ManCloud](https://github.com/ManCloud). This fixes the NES controller issues on the Waveshare RP2040 - PiZero board. [#8](https://github.com/PicoPlus-devel/pico_shared/issues/8)
- Board configs are moved to pico_shared.

## Fixes
- Fixed Pico 2 W: Led blinking causes screen flicker and ioctl timeouts [#2](https://github.com/PicoPlus-devel/pico_shared/issues/2). Solved with in SDK 2.1.1
- WII classic controller: i2c bus instance (i2c0 / i2c1) not hardcoded anymore but configurable via CMakeLists.txt. 

## Features
- Releases now built with SDK 2.1.1
- Support added for Adafruit Metro RP2350 board. See README for more info. No RISCV support yet.
- Switched to SD card driver pico_fatfs https://github.com/elehobica/pico_fatfs. This is required for the Adafruit Metro RP2350. The Pimoroni Pico DV does not work with this updated version and still needs the old version. (see [https://github.com/elehobica/pico_fatfs/issues/7#issuecomment-2817953143](https://github.com/elehobica/pico_fatfs/issues/7#issuecomment-2817953143) ) Therefore, the old version is still included in the repository. (pico_shared/drivers/pio_fatfs) 
    This is configured in CMakeLists.txt file by setting USE_OLD_SDDRIVER to 1.
- Besides FAT32, SD cards can now also be formatted as exFAT.
- Nes controller PIO code updated by [@ManCloud](https://github.com/ManCloud). This fixes the NES controller issues on the Waveshare RP2040 - PiZero board. [#8](https://github.com/PicoPlus-devel/pico_shared/issues/8)

## Fixes
- Fixed Pico 2 W: Led blinking causes screen flicker and ioctl timeouts [#2](https://github.com/PicoPlus-devel/pico_shared/issues/2). Solved with in SDK 2.1.1
- WII classic controller: i2c bus instance (i2c0 / i2c1) not hardcoded anymore but configurable via CMakeLists.txt. 