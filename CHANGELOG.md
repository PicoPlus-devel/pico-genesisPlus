# CHANGELOG

**v0.17** plays games larger than 4 MB, such as **Super Street Fighter II**, and on the Adafruit Fruit Jam and similar boards even games too large for PSRAM, such as *Demons of Asteborg*. It also adds **6 button controller** support.


# General Info


[Binaries for each configuration and PCB design are at the end of this page](#downloads___).

Only RP2350 (pico 2 based boards) supported. Works best with [Adafruit Fruit Jam](https://www.adafruit.com/product/6200)


[See the readme for how to install and wire up your board](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#getting-started)


> [!WARNING]  
> **Overclock Notice**  
>  
> **Only HSTX based boards like Raspberry Pi Pico 2, Pimoroni Pico Plus 2 (Both with PCB or breadboard), Adafruit Fruit Jam work on every monitor!**
> Boards with no HSTX use the PicoDVI driver, which due to the high overclock, sets the monitor refresh rate to **77.1 Hz**.
> Some monitors may **not support this refresh rate**, which can cause display or unsupported signal issues.  
> This can't be lowered using PicoDVI. See [#4](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/4)
>  
> If you experience problems, try using a **different monitor or TV**.  
>  
> **Note:** This limitation does **not** apply to **HSTX-based boards** (e.g., *Adafruit Fruit Jam*), where the monitor refresh rate can be set to **60 Hz**.
>
> Games also **run slower** on PicoDVI boards. See [Performance](#performance) below.

# v0.17 Release notes

## What's new

- **Games larger than 4 MB.** *Super Street Fighter II* now plays. It needs a board with PSRAM, or one whose flash has room for it. ([#21](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/21))
- **Games too large for PSRAM.** On a board with HSTX video, PSRAM and 16 MB of flash, such as the Adafruit Fruit Jam or a Pimoroni Pico Plus 2, games of up to about 15 MB now run, for example *Demons of Asteborg*. The first time such a game is started, the console asks before it writes the game to flash, which takes about a minute, and then restarts into the game. After that it starts in a few seconds. See [Large games](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#large-games).
- **6 button controllers.** Games such as *Super Street Fighter II* can now use the X, Y and Z buttons. The new **Genesis pad** setting chooses between a 3 and a 6 button controller; on **Auto**, the default, the game's cartridge decides. Some older games do not work with a 6 button controller, which is why they still get a 3 button one. ([#30](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/30))
- **New button layout in games.** Controllers other than Genesis ones are now mapped by the position of their buttons. On a SNES controller Y, B and A are Genesis A, B and C, and L, X and R are X, Y and Z. On a NES controller SELECT, B and A are Genesis A, B and C. The menus are unchanged. See [Buttons in a game](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#buttons-in-a-game) for all controllers. ([#32](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/32), [#35](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/35))

## Fixes

- Fixed memory corruption in games that show a window on the left side of the screen, such as the pause menu of *Demons of Asteborg*.
- The B button of the AliExpress SNES USB controller works without pressing Y first. ([#26](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/26))
- A NES controller clone on the GPIO port that could not use its B button in games now has all three Genesis buttons. ([#34](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/34))
- In-game shortcuts such as SELECT + START also work while a game is not reading the controller.

# v0.16 Release notes

## What's new

- **Overscan fix in menu.** A new setting for TVs that cut off the edges of the screen. It leaves the top and bottom rows of the menus blank, and optionally the first and last columns as well. The change is shown right away in the settings menu.
- **More options on one page in the settings menu.** The color palette is now only shown while one of the menu color options is selected, which leaves room for more options on screen.
- **Quicker saving in the settings menu.** Press SELECT on any setting to jump straight to the SAVE/CANCEL/DEFAULT row.

## Fixes

- **Controller test screen** shows the controller outline and the list of controllers correctly again.

# v0.15 Release notes

## Copy games over USB

Adding a game used to mean powering the board down, digging the microSD card out, finding a card reader and putting it all back. **USB drive mode** removes that: it hands the card to a computer as an ordinary USB drive while the board stays where it is.

Press **Select** in the ROM browser, choose **USB drive mode**, and connect the board's USB port to a computer. The card appears as a removable drive. Copy or delete files, then eject the drive on the computer; the emulator returns to the ROM browser with the new list of games already read in. **B** leaves the screen too, and if no computer turns up within twenty seconds it closes by itself.

Worth knowing:

- It is offered in the ROM browser only.
- Eject the drive on the computer before leaving, as with any USB stick.
- Controllers on a GPIO port, or on a second USB port, keep working while the card is mounted.
- A board with only one USB port needs it for the computer, which powers the board through it, so use a gamepad on the GPIO port there. 

## Fixes

- Fixed a potential race condition when initializing PSRAM.

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
  [Speed on PicoDVI boards](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#speed-on-picodvi-boards)
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
  [#11](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/11).
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
  [Custom PCBs](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#custom-pcbs)
  section covering mounting, parts, which binary to flash and the 3D-printed cases.

## Known limitations

- Cartridges with a serial EEPROM instead of a save memory chip still cannot save:
  *Wonder Boy in Monster World*, *NBA Jam*, *Micro Machines 2* and *Mega Man: The
  Wily Wars*. Ordinary battery-backed cartridges do save, see
  [Saved games](#saved-games) above.
  ([#20](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/20))
- Roms larger than 4 MB do not work: they need bank switching that is not emulated,
  so a game such as *Super Street Fighter II* breaks once it reaches past the first
  4 MB. On a board with PSRAM such a rom does fit in memory and will start.
  ([#21](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/21))
- Sound is mono: both sound chips are mixed into one channel that goes to the left
  and the right speaker alike, so the stereo effects in games such as *Sonic* and
  *Streets of Rage* play in the middle.
  ([#22](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/22))
- Games that use interlace mode are still not supported; for example the two-player
  levels of *Sonic the Hedgehog 2* show a blank screen.
  ([#23](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/23))
- The region comes from the rom itself. A Europe-only rom runs at 50 Hz, everything
  else at 60 Hz; there is no setting to force one or the other.
  ([#24](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/24))
- Non-HSTX (PicoDVI) boards still run the display at 77.1 Hz, see
  [#4](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/4).
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
  ([#28](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/28))

## For developers

- Every difference from the upstream Gwenesis core is documented in
  `gwenesis/PORTING.md`, so the core can be updated from upstream again.
- New PC test harness in `hosttest/`: the same emulator core built for Linux, which
  makes it possible to find emulation bugs without hardware.

# previous changes

See [HISTORY.md](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/HISTORY.md)

<a name="downloads___"></a>
## Downloads by configuration

Binaries for each configuration are listed below. Only RP2350 (Pico 2) boards are supported, and there are no risc-v binaries available.

A separate Pico 2 W binary is available for the breadboard and PicoNES PCB configuration. For the other configurations, use the Pico 2 binary on a Pico 2 W as well — the only thing you lose is the blinking led.


### Standalone boards

>[!NOTE]
> There is no binary for the WaveShare RP2350-PiZero board. See [#7](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/7)


| Board | Binary | Readme | |
|:--|:--|:--|:--|
| Adafruit Metro RP2350 | [picogenesisPlus_AdafruitMetroRP2350_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitMetroRP2350_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#adafruit-metro-rp2350) | |
| Adafruit Fruit Jam | [picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#adafruit-fruit-jam)| |
| Waveshare RP2350-PiZero | Unavailable [#7](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/7) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#waveshare-rp2040rp2350-pizero-development-board)| [3-D Printed case](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#3d-printed-case-for-rp2040rp2350-pizero) |

### Breadboard

| Board | Binary | Readme |
|:--|:--|:--|
| Pico 2 | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard) |
| Pico 2 W (untested) | [picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard) |
| Pimoroni Pico Plus 2 | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard) |


### PicoNES PCB

Designed by John Edgar Park. See the [Custom PCBs section of the readme](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#picones-pcb).

| Board | Binary | Readme |
|:--|:--|:--|
| Pico 2 | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2) |
| Pico 2 W (untested) | [picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2) |
| Pimoroni Pico Plus 2 (PCB v2.6 and male headers) | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2) |

PCB [pico_nesPCB_v2.6.zip](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/pico_nesPCB_v2.6.zip)

Design v2.6 has through-holes, so the Pico can be mounted on male headers. That is the only way to use a Pimoroni Pico Plus 2, which brings 8 MB of PSRAM: games then start immediately instead of after the flash copy.

3D-printed case designs for PCB, by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)):

[https://www.thingiverse.com/thing:6689537](https://www.thingiverse.com/thing:6689537). 
For the latest two player PCB 2.0, you need:

- Top_v2.0_with_Bootsel_Button.stl. This allows for software upgrades without removing the cover. (*)
- Base_v2.0.stl
- Power_Switch.stl.
(*) in case you don't want to access the bootsel button on the Pico, you can choose Top_v2.0.stl

> [!IMPORTANT]
> If the Pico is mounted on male headers, download the **latest** top cover. Headers raise the Pico, and only the newest cover leaves room for the USB cable.

### PicoNES Mini PCB (Waveshare RP2350-Zero, PCB required)

Designed by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)). See the [Custom PCBs section of the readme](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#picones-mini-pcb).

| Board | Binary | Readme |
|:--|:--|:--|
| Waveshare RP2350-Zero | [picogenesisPlus_WaveShareRP2350ZeroWithPCB_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_WaveShareRP2350ZeroWithPCB_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-waveshare-rp2040rp2350-zero) |

PCB: [Gerber_PicoNES_Mini_PCB_v2.0.zip](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/Gerber_PicoNES_Mini_PCB_v2.0.zip)

3D-printed case design, also by Gavin Knight:
[https://www.thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536)

### PicoNES Micro PCB (Waveshare RP2350-USB-A)

Designed by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)). See the [Custom PCBs section of the readme](https:///github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#picones-micro-pcb).

[Binary](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_WaveShare2350USBA_arm_piousb.uf2)

PCB: [Gerber_PicoNES_Micro_v1.2.zip](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/Gerber_PicoNES_Micro_v1.2.zip)

[Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-waveshare-rp2350-usb-a)

[Build guide](https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/)

### Pimoroni Pico DV

| Board | Binary | Readme |
|:--|:--| :--|
| Pico 2/Pico 2 w | [picogenesisPlus_PimoroniDVI_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_PimoroniDVI_pico2_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-for-pimoroni-pico-dv-demo-base) |
| Pimoroni Pico Plus 2 | [picogenesisPlus_PimoroniDVI_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_PimoroniDVI_pico2_arm.uf2) | [Readme](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-for-pimoroni-pico-dv-demo-base) |

> [!NOTE]
> On Pico W and Pico2 W, the CYW43 driver (used only for blinking the onboard LED) causes a DMA conflict with I2S audio on the Pimoroni Pico DV Demo Base, leading to emulator lock-ups. For now, no Pico W or Pico2 W binaries are provided; please use the Pico or Pico2 binaries instead.

### SpotPear HDMI (Untested)

This board has no setup section in the readme. Flash the binary below and wire the board according to its own [documentation](https://spotpear.com/index/product/detail/id/1207.html).

| Board | Binary |
|:--|:--|
| Pico 2/Pico 2 w | [picogenesisPlus_SpotpearHDMI_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1 (Untested)

For more info about the Murmulator see this website: https://murmulator.ru/ and [#150](https:///github.com/PicoPlus-devel/pico-infonesPlus/issues/150)

| Board | Binary |
|:--|:--|
| Murmulator M1 (with a Pico 2) | [picogenesisPlus_MurmulatorM1_pico2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2 (untested)

For more info about the Murmulator see this website: https://murmulator.ru/ and [#150](https:///github.com/PicoPlus-devel/pico-infonesPlus/issues/150)

| Board | Binary |
|:--|:--|
| Murmulator M2 | [picogenesisPlus_MurmulatorM2_arm.uf2](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_MurmulatorM2_arm.uf2) |

### Other downloads

- Metadata: [GenesisPlusMetadata.zip](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/GenesisPlusMetadata.zip)


Extract the zip file to the root folder of the SD card. Select a game in the menu and press START to show more information and box art. Works for most official released games. Screensaver shows floating random cover art.
