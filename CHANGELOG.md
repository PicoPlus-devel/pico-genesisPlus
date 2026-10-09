# CHANGELOG

**v0.19** changes the button layout in games for controllers other than Genesis ones.


# General Info


[Binaries for each configuration and PCB design are at the end of this page](#downloads___).

Only RP2350 (pico 2 based boards) supported. Works best with [Adafruit Fruit Jam](https://www.adafruit.com/product/6200)


[See the readme for how to install and wire up your board](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#getting-started)


> [!WARNING]  
> **Overclock Notice**  
>  
> **Only HSTX based boards like Raspberry Pi Pico 2, Pimoroni Pico Plus 2 (Both with PCB or breadboard), Adafruit Fruit Jam work on every monitor!**
> Boards with no HSTX use the PicoDVI driver, which due to the high overclock, sets the monitor refresh rate to **77.1 Hz**.
> Some monitors may **not support this refresh rate**, which can cause display or unsupported signal issues.  
> This can't be lowered using PicoDVI. See [#4](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/4)
>  
> If you experience problems, try using a **different monitor or TV**.  
>  
> **Note:** This limitation does **not** apply to **HSTX-based boards** (e.g., *Adafruit Fruit Jam*), where the monitor refresh rate can be set to **60 Hz**.
>
> Games also **run slower** on PicoDVI boards. See [Speed on PicoDVI boards](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#speed-on-picodvi-boards).

# v0.19 Release notes

## Changes

- **New button layout in games.** On controllers other than Genesis ones, Genesis B and C are now on the buttons most games use to attack and jump. On a SNES controller A, Y and B are Genesis A, B and C; on an XInput controller B, X and A; on a Dual Shock/Sense Circle, Square and Cross. NES controllers keep SELECT, B and A as Genesis A, B and C. X, Y and Z and the menus are unchanged. See [Buttons in a game](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#buttons-in-a-game). ([#40](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/40))

## Fixes

- A NES controller clone on the GPIO port that is not recognised as a NES controller now has its buttons in the same places as any other NES controller. ([#28](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/28))

# v0.18 Release notes

## What's new

- **Olimex RP2040-PICO-PC.** The emulator now runs on the [Olimex RP2040-PICO-PC](https://www.olimex.com/Products/MicroPython/PICO/RP2040-PICO-PC/) with a Raspberry Pi Pico 2: a 60 Hz HDMI picture, sound through HDMI and the board's audio jack at the same time, USB controllers on the USB-A port and a NES or SNES controller on the UEXT connector. See [Olimex RP2040-PICO-PC](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#olimex-rp2040-pico-pc). Contributed by [DnCraptor](https://github.com/DnCraptor).

## Fixes

- When started from [pico-bootLoader](https://github.com/PicoPlus-devel/pico-bootLoader) on a board without PSRAM, a game too large for the flash could still be picked, and writing it to flash could overwrite the bootloader.

# v0.17 Release notes

**v0.17** plays **MD+** games on the Adafruit Fruit Jam and similar boards, and games larger than 4 MB, such as **Super Street Fighter II**, even ones too large for PSRAM, such as *Demons of Asteborg*. It also adds **6 button controller** support. Some **Sega CD / Mega-CD** games run as well, others still have bugs and graphical artifacts. Most are too slow to be playable.

## What's new

- **MD+ games.** Cartridge games patched to play CD quality music, known as MD+, play well with their music on the same boards. Put the game and its disc image side by side with the same name and pick the disc image.
- **Pier Solar.** *Pier Solar and the Great Architects* now plays and saves its progress, and with its *Enhanced Soundtrack Disc* and a Sega CD BIOS it plays the enhanced soundtrack.
- **Games larger than 4 MB.** *Super Street Fighter II* now plays. It needs a board with PSRAM, or one whose flash has room for it. ([#21](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/21))
- **Games too large for PSRAM.** On a board with HSTX video, PSRAM and 16 MB of flash, such as the Adafruit Fruit Jam or a Pimoroni Pico Plus 2, games of up to about 15 MB now run, for example *Demons of Asteborg*. The first time such a game is started, the console asks before it writes the game to flash, which takes about a minute, and then restarts into the game. After that it starts in a few seconds. See [Large games](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#large-games).
- **Optional 504 MHz overclock** on the Adafruit Fruit Jam, and on a Pico 2 or Pimoroni Pico Plus 2 on a breadboard or the PicoNES PCB, in the settings menu. It is off by default and not advised; see [Overclocking](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#overclocking).
- **New setting: Video Clock Fix** (Pico 2, Pimoroni Pico Plus 2, PicoNES PCB, Adafruit Metro RP2350 and Murmulator M2). Turn it on if your TV or monitor shows small dots or lines in the picture. A USB controller can then no longer be used; use a NES, SNES or Wii controller instead. On the Adafruit Fruit Jam this is always on. See [Video Clock Fix](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#video-clock-fix).
- **6 button controllers.** Games such as *Super Street Fighter II* can now use the X, Y and Z buttons. The new **Genesis pad** setting chooses between a 3 and a 6 button controller; on **Auto**, the default, the game's cartridge decides. Some older games do not work with a 6 button controller, which is why they still get a 3 button one. ([#30](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/30))
- **New button layout in games.** Controllers other than Genesis ones are now mapped by the position of their buttons. On a SNES controller Y, B and A are Genesis A, B and C, and L, X and R are X, Y and Z. On a NES controller SELECT, B and A are Genesis A, B and C. The menus are unchanged. See [Buttons in a game](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#buttons-in-a-game) for all controllers. ([#32](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/32), [#35](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/35))
- **Sega CD / Mega-CD games.** On boards with HSTX video and PSRAM, such as the Adafruit Fruit Jam, some Sega CD games now run, others still have bugs and graphical artifacts. Most are too slow to be playable. Pick the game's `.cue` file in the menu; a Sega CD BIOS is needed. Games on more than one disc change discs from the settings menu, and saved games are kept on the SD card. See [Sega CD and MD+](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#sega-cd-and-md).
- **Interlace mode.** The two-player mode of *Sonic the Hedgehog 2* now shows the game instead of a blank screen. Every other line of its double-height picture is shown. ([#23](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/23))

## Fixes

- The music of every game after the first one since the board was switched on sounded slightly off. It now sounds the same every time.
- Fixed memory corruption in games that show a window on the left side of the screen, such as the pause menu of *Demons of Asteborg*.
- On boards without PSRAM, a game could crash right after it was written to flash, and with the 504 MHz overclock the board could hang while writing it.
- The B button of the AliExpress SNES USB controller works without pressing Y first. ([#26](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/26))
- A NES controller clone on the GPIO port that could not use its B button in games now has all three Genesis buttons. ([#34](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/34))
- In-game shortcuts such as SELECT + START also work while a game is not reading the controller.
- The controller test now closes when SELECT + UP is held for two seconds, instead of SELECT + START, which some 8BitDo wireless controllers use for themselves.
- The overclock setting always matches the speed the board runs at. It could show on while the board ran at the normal speed, or off while the board was still overclocked.
- All settings return to their defaults once after updating to this version.

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

# previous changes

See [HISTORY.md](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/HISTORY.md)

<a name="downloads___"></a>
## Downloads by configuration

Binaries for each configuration are listed below. Only RP2350 (Pico 2) boards are supported, and there are no risc-v binaries available.

A separate Pico 2 W binary is available for the breadboard and PicoNES PCB configuration. For the other configurations, use the Pico 2 binary on a Pico 2 W as well — the only thing you lose is the blinking led. The exception is the Olimex RP2040-PICO-PC, which has a binary for the Pico 2 only.


### Standalone boards

>[!NOTE]
> There is no binary for the WaveShare RP2350-PiZero board. See [#7](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/7)


| Board | Binary | Readme | |
|:--|:--|:--|:--|
| Adafruit Metro RP2350 | [picogenesisPlus_AdafruitMetroRP2350_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitMetroRP2350_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#adafruit-metro-rp2350) | |
| Adafruit Fruit Jam | [picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#adafruit-fruit-jam)| |
| Waveshare RP2350-PiZero | Unavailable [#7](https://github.com/PicoPlus-devel/pico-genesisPlus/issues/7) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#waveshare-rp2040rp2350-pizero-development-board)| [3-D Printed case](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#3d-printed-case-for-rp2040rp2350-pizero) |

### Breadboard

| Board | Binary | Readme |
|:--|:--|:--|
| Pico 2 | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard) |
| Pico 2 W (untested) | [picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard) |
| Pimoroni Pico Plus 2 | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard) |


### PicoNES PCB

Designed by John Edgar Park. See the [Custom PCBs section of the readme](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#picones-pcb).

| Board | Binary | Readme |
|:--|:--|:--|
| Pico 2 | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2) |
| Pico 2 W (untested) | [picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2) |
| Pimoroni Pico Plus 2 (PCB v2.6 and male headers) | [picogenesisPlus_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_AdafruitDVISD_pico2_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2) |

PCB [pico_nesPCB_v2.6.zip](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/pico_nesPCB_v2.6.zip)

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

Designed by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)). See the [Custom PCBs section of the readme](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#picones-mini-pcb).

| Board | Binary | Readme |
|:--|:--|:--|
| Waveshare RP2350-Zero | [picogenesisPlus_WaveShareRP2350ZeroWithPCB_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_WaveShareRP2350ZeroWithPCB_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-waveshare-rp2040rp2350-zero) |

PCB: [Gerber_PicoNES_Mini_PCB_v2.0.zip](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/Gerber_PicoNES_Mini_PCB_v2.0.zip)

3D-printed case design, also by Gavin Knight:
[https://www.thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536)

### PicoNES Micro PCB (Waveshare RP2350-USB-A)

Designed by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)). See the [Custom PCBs section of the readme](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#picones-micro-pcb).

[Binary](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_WaveShare2350USBA_arm_piousb.uf2)

PCB: [Gerber_PicoNES_Micro_v1.2.zip](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/Gerber_PicoNES_Micro_v1.2.zip)

[Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#pcb-with-waveshare-rp2350-usb-a)

[Build guide](https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/)

### Pimoroni Pico DV

| Board | Binary | Readme |
|:--|:--| :--|
| Pico 2/Pico 2 w | [picogenesisPlus_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_PimoroniDVI_pico2_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-for-pimoroni-pico-dv-demo-base) |
| Pimoroni Pico Plus 2 | [picogenesisPlus_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_PimoroniDVI_pico2_arm.uf2) | [Readme](https://github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-for-pimoroni-pico-dv-demo-base) |

> [!NOTE]
> On Pico W and Pico2 W, the CYW43 driver (used only for blinking the onboard LED) causes a DMA conflict with I2S audio on the Pimoroni Pico DV Demo Base, leading to emulator lock-ups. For now, no Pico W or Pico2 W binaries are provided; please use the Pico or Pico2 binaries instead.

### SpotPear HDMI (Untested)

This board has no setup section in the readme. Flash the binary below and wire the board according to its own [documentation](https://spotpear.com/index/product/detail/id/1207.html).

| Board | Binary |
|:--|:--|
| Pico 2/Pico 2 w | [picogenesisPlus_SpotpearHDMI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1 (Untested)

For more info about the Murmulator see this website: https://murmulator.ru/ and [#150](https://github.com/PicoPlus-devel/pico-infonesPlus/issues/150)

| Board | Binary |
|:--|:--|
| Murmulator M1 (with a Pico 2) | [picogenesisPlus_MurmulatorM1_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2 (untested)

For more info about the Murmulator see this website: https://murmulator.ru/ and [#150](https://github.com/PicoPlus-devel/pico-infonesPlus/issues/150)

| Board | Binary |
|:--|:--|
| Murmulator M2 | [picogenesisPlus_MurmulatorM2_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_MurmulatorM2_arm.uf2) |

### Olimex RP2040-PICO-PC

Contributed by [DnCraptor](https://github.com/DnCraptor). See the [Olimex RP2040-PICO-PC section of the readme](https://github.com/PicoPlus-devel/pico-genesisPlus/blob/main/README.md#olimex-rp2040-pico-pc).

| Board | Binary |
|:--|:--|
| Olimex RP2040-PICO-PC with a Pico 2 | [picogenesisPlus_OlimexPicoPC_arm.uf2](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/picogenesisPlus_OlimexPicoPC_arm.uf2) |

There is no Pico 2 W binary for this board.

### Other downloads

- Metadata: [GenesisPlusMetadata.zip](https://github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/GenesisPlusMetadata.zip)


Extract the zip file to the root folder of the SD card. Select a game in the menu and press START to show more information and box art. Works for most official released games. Screensaver shows floating random cover art.
