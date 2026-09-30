# Pico-genesisPlus

A Sega Genesis/Mega Drive emulator for the Raspberry Pi Pico 2 (RP2350). It plays roms from an SD card and puts the picture on your TV or monitor over HDMI. Connect a game controller, pick a game from the menu and play.

Based on [Gwenesis](https://github.com/bzhxx/gwenesis) by bzhxx.

## Getting started

1. **Flash the firmware.** Pick the `.uf2` for your board from the [supported boards](#supported-boards) table and download it from the [releases page](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest). Hold the BOOTSEL button while you connect the board to your computer, then copy the file to the drive that appears.
2. **Prepare an SD card.** Format it as FAT32 (recommended) or exFAT and copy your roms into a `/roms/MD` folder — that is where the menu opens, and it falls back to the root of the card when the folder is not there. Subfolders are fine, the menu lets you browse them. Needless to say, you must own the games you put on the card.
3. **Add box art (optional).** See [box art and game info](#box-art-and-game-info).
4. **Insert the card, connect a controller and switch the board on.** Browse the card, pick a game and play. Settings are saved on the card automatically. On a board without PSRAM the screen stays blank for a while when a game starts, because the rom is written to flash first — see [PSRAM](#psram).

Wiring depends on the board. The hardware is the same as for the NES emulator, so the setup instructions are in the pico-infonesPlus readme:

| Board | Setup instructions |
| ----- | ------------------ |
| Adafruit Fruit Jam | [Fruit Jam](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#adafruit-fruit-jam) |
| Pico 2 on a breadboard with Adafruit breakouts, or on the PicoNES PCB | [Adafruit hardware and breadboard](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard), [PicoNES PCB](#picones-pcb) |
| Adafruit Metro RP2350 | [Metro RP2350](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#adafruit-metro-rp2350) |
| Pimoroni Pico DV Demo Base | [Pimoroni Pico DV Demo Base](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-for-pimoroni-pico-dv-demo-base) |
| Pimoroni Pico Plus 2, wired the same as the Pico 2 above | [Adafruit hardware and breadboard](https:///github.com/PicoPlus-devel/pico-infonesPlus/blob/main/README.md#raspberry-pi-pico-or-pico-2-setup-with-adafruit-hardware-and-breadboard), [PicoNES PCB](#picones-pcb) (needs v2.6 with male headers) |
| PicoNES, PicoNES Mini or PicoNES Micro PCB | [Custom PCBs](#custom-pcbs) |



## Supported boards

Everything runs on the RP2350 (Pico 2) with the arm core. RP2040 boards and RISC-V builds are not supported.

Ready-made `.uf2` files for all of these are on the [releases page](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest).

| Board | Video output | Build command | Release binary |
| ----- | ------------ | ------------- | -------------- |
| Adafruit [Fruit Jam](https://www.adafruit.com/product/6200) — **recommended** | HSTX, 60 Hz | `./bld.sh -c8` | `picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2` |
| Pico 2 or Pimoroni Pico Plus 2 on a breadboard or on the [PicoNES PCB](#picones-pcb), with an [Adafruit DVI breakout](https://www.adafruit.com/product/4984) + microSD breakout | HSTX, 60 Hz | `./bld.sh -c2 -2` | `picogenesisPlus_AdafruitDVISD_pico2_arm.uf2` |
| Same, but with a Pico 2 W — *untested* | HSTX, 60 Hz | `./bld.sh -c2 -2 -w` | `picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2` |
| Adafruit Metro RP2350 | HSTX, 60 Hz | `./bld.sh -c5` | `picogenesisPlus_AdafruitMetroRP2350_arm.uf2` |
| Murmulator M2 — *untested* | HSTX, 60 Hz | `./bld.sh -c13` | `picogenesisPlus_MurmulatorM2_arm.uf2` |
| Pimoroni [Pico DV Demo Base](https://shop.pimoroni.com/products/pimoroni-pico-dv-demo-base?variant=39494203998291) | PicoDVI, 77.1 Hz, runs slower | `./bld.sh -c1 -2` | `picogenesisPlus_PimoroniDVI_pico2_arm.uf2` |
| Waveshare RP2350-Zero on the [PicoNES Mini PCB](#picones-mini-pcb) | PicoDVI, 77.1 Hz, runs slower | `./bld.sh -c6 -2` | `picogenesisPlus_WaveShareRP2350ZeroWithPCB_arm.uf2` |
| Waveshare RP2350-USB-A, on its own or on the [PicoNES Micro PCB](#picones-micro-pcb) | PicoDVI, 77.1 Hz, runs slower | `./bld.sh -c9` | `picogenesisPlus_WaveShare2350USBA_arm_piousb.uf2` |
| [Spotpear HDMI board](https://spotpear.com/index/product/detail/id/1207.html) — *untested* | PicoDVI, 77.1 Hz, runs slower | `./bld.sh -c10 -2` | `picogenesisPlus_SpotpearHDMI_pico2_arm.uf2` |
| Murmulator M1 — *untested* | PicoDVI, 77.1 Hz, runs slower | `./bld.sh -c12 -2` | `picogenesisPlus_MurmulatorM1_pico2_arm.uf2` |

Boards marked *untested* build and are released, but have not been tried on real hardware. They also have no setup section in the table above: wire the Spotpear board according to [its own documentation](https://spotpear.com/index/product/detail/id/1207.html), and for the Murmulator boards see [murmulator.ru](https://murmulator.ru/) and [#150](https:///github.com/PicoPlus-devel/pico-infonesPlus/issues/150).

> [!WARNING]
> **Only HSTX boards deliver proper 60 Hz output and universal monitor compatibility; non‑HSTX (PicoDVI) builds set the refresh rate to 77.1 Hz and may be rejected by some displays.**  
> The high refresh rate on non-HSTX boards is related to the high overclocking of the RP2350.
> This can't be lowered using PicoDVI. See [#4](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/4)
> If you experience problems, try using a **different monitor or TV**.  
> **Games also run slower on these boards**, see [Speed on PicoDVI boards](#speed-on-picodvi-boards).

### Speed on PicoDVI boards

Boards without HSTX use the PicoDVI driver to put the picture on screen, and making
that picture takes so much of the board's attention that the emulator does not get
enough left over. Games run slower than they should: the action, the music and the
sound all drag a little, and how noticeable it is depends on the game. Everything
else works the same as on any other board.

These are the boards it applies to:

| Board | Build command |
| ----- | ------------- |
| Pimoroni [Pico DV Demo Base](https://shop.pimoroni.com/products/pimoroni-pico-dv-demo-base?variant=39494203998291) | `./bld.sh -c1 -2` |
| Waveshare RP2350-Zero on the [PicoNES Mini PCB](#picones-mini-pcb) | `./bld.sh -c6 -2` |
| Waveshare RP2350-USB-A, on its own or on the [PicoNES Micro PCB](#picones-micro-pcb) | `./bld.sh -c9` |
| [Spotpear HDMI board](https://spotpear.com/index/product/detail/id/1207.html) | `./bld.sh -c10 -2` |
| Murmulator M1 | `./bld.sh -c12 -2` |

For games at full speed you want one of the HSTX boards from the table above: the
Adafruit Fruit Jam, a Pico 2 or Pimoroni Pico Plus 2 with an Adafruit DVI breakout
(also on the PicoNES PCB), the Adafruit Metro RP2350 or the Murmulator M2.

This is not something that can be tuned away. The board is already clocked as high
as it will go, so there is nothing left to hand to the emulator.

### PSRAM

PSRAM is worth having: the rom is loaded straight into it and the game starts the moment you pick it. Without PSRAM the rom is first written to flash, which takes several seconds (though [recently played games](#recently-played-games) can skip that).

**Without PSRAM, be patient after picking a game.** Writing the rom to flash takes a
while — a few seconds for a small game, considerably longer for a big one — and the
screen stays blank until it is done. The LED on the board flashes on and off the
whole time it is working, so as long as it keeps blinking the rom is still being
written and the board has not locked up. Leave it alone until the game appears; do
not switch the board off or reset it. Boards with no onboard LED (and a Pico 2 W)
cannot show this, so there the blank screen is all you get. Games in the
[recently played](#recently-played-games) list marked `[READY]` skip the wait
altogether.

It is detected at boot, so no separate binary is needed. You have it on the Fruit Jam and the Metro RP2350, on a Murmulator with a PSRAM chip fitted, and on a [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107) in any build that takes a Pico-shaped board — the breadboard/[PicoNES PCB](#picones-pcb) build (`-c2`), the Pimoroni Pico DV Demo Base (`-c1`) and the Spotpear board (`-c10`).

Roms that are too large for the memory the board has are left out of the list in the menu. Games larger than 8 MB are covered under [Large games](#large-games).

### Large games

Most games are 4 MB or smaller. A few are larger and switch between parts of the cartridge while they run. *Super Street Fighter II* (5 MB) is the only original cartridge of that kind; some newer homebrew games are considerably larger, such as *Demons of Asteborg* (15 MB).

- **Up to 8 MB**, such a game plays like any other on a board with PSRAM, or on a board without PSRAM whose flash has room for it.
- **Larger than 8 MB**, the game does not fit in PSRAM. On a board with HSTX video, PSRAM and 16 MB of flash, such as the Adafruit Fruit Jam or a Pimoroni Pico Plus 2 in the `-c2` build, it runs from flash instead, with the part that does not fit there held in PSRAM. The limit is about 15 MB. Boards that use the PicoDVI driver do not support this.

The first time such a game is started, the console asks before it writes the game to flash. Writing takes about a minute, a progress bar shows how far it is, and the console restarts and starts the game when it is done. Do not switch the board off while it is writing. After that the game starts in a few seconds, until another game of that size is started and takes its place. [pico-snesPlus](https:///github.com/PicoPlus-devel/pico-snesPlus) uses the same part of the flash for its largest games, so starting one of those replaces it as well.

### Other build configurations

`bld.sh` has a few more configurations that belong to related projects but are not supported here: `-c3` and `-c4` are RP2040 boards, `-c7` (Waveshare RP2350-PiZero) is disabled because of [#7](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/7), `-c11` is deprecated, and `-c14` (Adafruit Feather RP2350 with TLV320DAC3100) builds but has no release binary. Run `./bld.sh -h` for the full list of options.

### Several emulators on one board

The binaries above are standalone: one board, one emulator. With [pico-bootLoader](https:///github.com/PicoPlus-devel/pico-bootLoader) you can instead keep several emulators, and a *Doom* port, on the same board and pick one from an on-screen menu at power-on, without a computer. The bootloader and an SD card archive containing this emulator are on the [pico-bootLoader releases page](https:///github.com/PicoPlus-devel/pico-bootLoader/releases).

Started that way, the settings menu gains an extra item, **Return to emulator selection menu**, which takes you back to that boot menu. To build a bootloader version yourself, add `-b` to the build command, for example `./bld.sh -c8 -b`; the `.uf2` ends up in `releases_bl`.

## Custom PCBs

Three community PCB designs turn a supported board and its breakouts into a finished little console, each with an optional 3D-printed case. They are simply a neater way to build hardware this emulator already supports, so nothing changes in the firmware: flash the binary for that configuration and you are done.

| Design | Board it carries | Build | Gerber archive | Designed by |
| --- | --- | --- | --- | --- |
| [PicoNES](#picones-pcb) | Pico 2, Pico 2 W or Pimoroni Pico Plus 2 | `-c2` | `pico_nesPCB_v2.6.zip` | John Edgar Park |
| [PicoNES Mini](#picones-mini-pcb) | Waveshare RP2350-Zero | `-c6` | `Gerber_PicoNES_Mini_PCB_v2.0.zip` | Gavin Knight |
| [PicoNES Micro](#picones-micro-pcb) | Waveshare RP2350-USB-A | `-c9` | `Gerber_PicoNES_Micro_v1.2.zip` | Gavin Knight |

All three archives are attached to every [release](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest) of this project and also live in [pico_shared/PCB](pico_shared/PCB). Upload the zip as-is to a PCB manufacturer of your choice; [PCBWay](https://www.pcbway.com/) and JLCPCB are both good options.

The designs come from [pico-infonesPlus](https:///github.com/PicoPlus-devel/pico-infonesPlus) and kept their NES-flavoured names, but there is nothing NES-specific about them — they are DVI, microSD and controller wiring, and this emulator runs on them just as well.

> [!NOTE]
> Sellers on AliExpress have copied the PicoNES design and sell ready-made boards. For questions about those, contact the seller.

### PicoNES PCB

The original design, by [@johnedgarpark](https://twitter.com/johnedgarpark). It carries the Pico, the DVI and microSD breakouts and up to two NES controller ports. It is also the only one of the three that takes an interchangeable Pico-format board, which is what makes a Pimoroni Pico Plus 2 — and with it PSRAM — an option. The current design is **v2.6**.

<img width="480" alt="Populated PCB with a Pico plugged into the through-holes" src="https://github.com/user-attachments/assets/2bbc846d-56b1-4528-9899-01bc9b32ce11" />

#### Mounting the Pico

Design v2.6 added through-holes, so there are now two ways to fit the board:

| Mounting | Boards | Design version |
| --- | --- | --- |
| Soldered flat onto the PCB, no headers | Pico 2, Pico 2 W | any |
| Male headers plugged into the through-holes | Pico 2, Pico 2 W, Pimoroni Pico Plus 2 | v2.6 or later |

> [!IMPORTANT]
> A [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107) needs v2.6 **and** male headers. On v2.1 and older designs the board has to lie flat against the PCB, which the SP/CE connector on the back of the Pimoroni Pico Plus 2 prevents.

> [!NOTE]
> Soldering skills are required. Solder every connection from the Pico to the PCB, including the ones on the short right-hand side of the board — those are ground.

#### What you need

- One of the following, mounted as described above:
  * Raspberry Pi Pico 2 or Pico 2 W **without headers**, soldered flat.
  * Raspberry Pi Pico 2, Pico 2 W or [Pimoroni Pico Plus 2](https://shop.pimoroni.com/products/pimoroni-pico-plus-2?variant=42092668289107) **with male headers** soldered on ([these](https://a.co/d/dSNPuyo) fit), plugged into the through-holes.
- [Adafruit DVI Breakout Board — For HDMI Source Devices](https://www.adafruit.com/product/4984)
- [Adafruit Micro SD SPI or SDIO Card Breakout Board — 3V ONLY!](https://www.adafruit.com/product/4682)
- For NES controllers:
  * [one or two NES controller ports](https://www.zedlabz.com/products/controller-connector-port-for-nintendo-nes-console-7-pin-90-degree-replacement-2-pack-black-zedlabz)
  * [one or two NES controllers](https://www.amazon.com/s?k=NES+controller)
- [Micro USB to OTG Y-cable](https://a.co/d/b9t11rl) if you want to use a USB game controller — it powers the board and connects the controller at the same time.
- Micro USB power supply.
- Optional: an on/off switch, such as [this one](https://www.kiwi-electronics.com/en/spdt-slide-switch-410?search=KW-2467).

Two NES controllers give you a two-player setup; a USB controller for player 1 and a NES controller in either port for player 2 works just as well. Keep in mind that a NES controller has no C button — [SELECT stands in for it](#controllers-and-buttons) while a game runs.

> [!NOTE]
> You can also connect an SNES controller. The sockets speak the SNES protocol as well. The connectors differ, so a SNES pad needs a [SNES-to-NES adapter cable you make yourself](https:///github.com/PicoPlus-devel/pico-snesPlus/blob/main/snestonescontroller.md) — one per socket. There also are ready made cables, but hard to find at the moment.  Some ready made cables simply don't work as expected.

<img width="480" alt="Two-player setup with NES controllers" src="https://github.com/user-attachments/assets/d40ed98f-4632-4161-986a-732d35290fac" />

#### Which binary to flash

- Pico 2 **and** Pimoroni Pico Plus 2 — `picogenesisPlus_AdafruitDVISD_pico2_arm.uf2`
- Pico 2 W — `picogenesisPlus_AdafruitDVISD_pico2_w_arm.uf2` (untested on real hardware)

The Pimoroni Pico Plus 2 needs no separate build. The emulator reads the real flash size from the chip at boot and detects PSRAM at runtime, so the same `pico2` image adapts to whichever board is plugged in.

#### What the Pimoroni Pico Plus 2 adds

The Pimoroni Pico Plus 2 brings 8 MB of PSRAM and 16 MB of flash. The PSRAM is what you notice: roms are loaded into it and a game starts the moment you select it, instead of after the several seconds a plain Pico 2 needs to write the rom to its flash. It also lifts the limit on rom size — larger roms that a 4 MB Pico 2 has to leave out of the list will show up and play. See [PSRAM](#psram).

#### 3D printed case

Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)) designed an NES-like enclosure for this PCB: [thingiverse.com/thing:6689537](https://www.thingiverse.com/thing:6689537). The v2.0 design has a base, a power-switch part and a choice of two top covers — one with a button that reaches the BOOTSEL button so firmware can be updated without opening the case, one without. Print the files that match the PCB version you own; Gavin's Thingiverse page has the details.

> [!IMPORTANT]
> If the Pico is mounted with male headers, download the **latest** top cover. Headers raise the Pico, and only the newest cover leaves room for the USB cable — the older ones assume a Pico soldered flat onto the PCB.

<img width="480" alt="Top cover with a button for BOOTSEL" src="https://github.com/user-attachments/assets/3c8f8990-51b9-4873-9054-64bb2cd6c300" />

For the full photo gallery and assembly detail, see the [PCB section of the pico-infonesPlus documentation](https:///github.com/PicoPlus-devel/pico-infonesPlus#pcb-with-raspberry-pi-pico-or-pico-2-and-pimoroni-pico-plus-2).

### PicoNES Mini PCB

A smaller take on the same idea by Gavin Knight ([DynaMight1124](https://github.com/DynaMight1124)), built around a Waveshare RP2350-Zero and two NES controller ports. It uses cheaper but considerably harder to solder parts, so it is a more advanced project than the PicoNES — if you are unsure of your soldering, start with that one instead. The current design is **v2.0** (`Gerber_PicoNES_Mini_PCB_v2.0.zip`), which improved the SD slot and the components around the HDMI port.

Flash `picogenesisPlus_WaveShareRP2350ZeroWithPCB_arm.uf2`. The design also exists in an RP2040-Zero flavour, which this emulator cannot use — it is RP2350-only.

> [!NOTE]
> Good soldering skills are required, especially around the HDMI portion: plenty of flux, a fine tip and solder wick. The recommended order is the resistor arrays first, then the HDMI port, then the Pico or the microSD adaptor, and the NES ports last — they can be hard to push into the PCB.

The build guide and the full component list are on Instructables: <https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/>

<img width="480" alt="Soldered PicoNES Mini PCB" src="https://github.com/user-attachments/assets/13933b1d-af00-402e-a0a0-8456de4a82da" />

#### 3D printed case for the Mini

Also by Gavin Knight: [thingiverse.com/thing:7041536](https://www.thingiverse.com/thing:7041536). The same page still carries the older v1.0 PCB design files, gerber and BOM. Without a printer of your own, a local printing service or a professional one such as PCBWay or JLCPCB will produce it — the professional finishes are excellent.

<img width="480" alt="PicoNES Mini in its 3D-printed case" src="https://github.com/user-attachments/assets/732384bd-062d-43ca-97cb-a16a39607c41" />

### PicoNES Micro PCB

The smallest of the three, again by Gavin Knight: a Waveshare RP2350-USB-A board on a PCB barely larger than the USB port itself, with a single player controlling the console over USB. The current design is **v1.2** (`Gerber_PicoNES_Micro_v1.2.zip`).

Flash `picogenesisPlus_WaveShare2350USBA_arm_piousb.uf2`. The game controller plugs into the USB-A port; the USB-C port is for power and for flashing the firmware.

> [!NOTE]
> Because of the size, micro-soldering skills are required — the design uses 0603 SMD components. This is the most demanding of the three builds.

The build guide is on Instructables: <https://www.instructables.com/PicoNES-RaspberryPi-Pico-Based-NES-Emulator/>

<img width="480" alt="PicoNES Micro populated PCB, NES controller shown for scale" src="https://github.com/user-attachments/assets/59c8a31b-dc3e-47b0-8ffb-89e1eab2a75b" />

<img width="480" alt="PicoNES Micro in its 3D-printed case" src="https://github.com/user-attachments/assets/1d6051f2-1393-40e1-aad0-e39ffb7717a0" />

## Controllers and buttons

Supported controllers:

- Dual Shock/Dual Sense and PSClassic
- Xbox style controllers (XInput)
- Genesis Mini 1 and 2, and the [Retro-Bit 8 button Arcade Pad with USB](https://www.retro-bit.com/controllers/genesis/#usb)
- NES and SNES controllers on the GPIO port of a PCB or breadboard setup
- AliExpress NES and SNES USB controllers. On the SNES one you have to press Y once every time a game starts or the menu opens, otherwise the B button stays dead.
- Fruit Jam: SNES Classic and Wii Classic Pro controllers over I2C. Connect the controller to an [Adafruit Wii Nunchuck Breakout Adapter](https://www.adafruit.com/product/4836).
- USB keyboard

The three Genesis buttons are called Button1, Button2 and Button3 throughout this readme:

|     | (S)NES | Genesis | XInput | Dual Shock/Sense |
| --- | ------ | ------- | ------ | ---------------- |
| Button1 | B  |    A    |   A    |    X             |
| Button2 | A  |    B    |   B    |   Circle         |
| Button3 | X (SNES only)  |    C    |   Y    |   Triangle       |
| Select  | select | Mode (C on a 3 button controller) | Select | Select     |

### Controllers without a third button

Some controllers have no button that can reach the Genesis C button. For those, **SELECT doubles as C while a game runs**. SELECT keeps all its other jobs, and C is not sent while START is held, so SELECT + START still opens the settings menu.

This applies to the vintage NES controller on the NES/SNES GPIO port, which has only two buttons (A and B, plus Select, Start and the d-pad), and to the AliExpress NES USB controller. SNES controllers are unaffected either way: they have a real X button, on the GPIO port as well as over USB.

### NES and SNES pads on the GPIO port

The two sockets speak one protocol but the pads send their buttons in a different order, so the port works out for itself which one is plugged in. A NES pad says so on every read, and anything else is taken for a SNES pad — including a SNES pad behind a home-made adapter cable, which works fully from the first button press with no need to wake it up first. Both then get the mapping from the table above: on a NES pad B is Genesis A and A is Genesis B, and on a SNES pad B is Genesis A, A is Genesis B and X is Genesis C. SNES Y, L and R are not used, because the Genesis pad only has three buttons.

One caveat: a NES pad is recognised by grounding the shift register outputs it does not use, which is what an original Nintendo pad does, and most aftermarket ones with it. A clone that leaves them floating cannot be told from a SNES pad, and its B button will do nothing. Its A button and SELECT still work, so it stays usable. To check a pad, open **Settings > Controller Test**, press a button and look at the `Sent by pad:` line — a top digit of `F` means the pad identifies itself properly. ([#28](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/28))

## Menu

Gamepad buttons:
- UP/DOWN: next/previous item in the menu.
- LEFT/RIGHT: next/previous page.
- Button2: open folder, or start the selected game.
- Button1: back to the parent folder.
- START: show [box art and game info](#box-art-and-game-info).
- Button3: show the list of [recently played games](#recently-played-games).
- SELECT: open the settings menu. Here you can change things like the screen mode, scanlines, the game sound, [frame skip](#frame-skip), [usb drive mode](#usb-drive-mode), the framerate display, the menu colours, the overscan fix for the menus and settings specific to your board. The same menu can be opened while a game is running.

**Overscan fix in menu** is meant for TVs that cut off the edges of the picture: **Rows** leaves the top and bottom text rows of the menus blank, **Rows & columns** also leaves the first and last columns blank. The effect is shown while the setting is changed, and it applies to the menus only, not to the game picture. The color palette is shown only while one of the two menu color entries is selected, which leaves room for more entries on one page. In the settings menu, press SELECT on any setting to jump straight to the SAVE/CANCEL/DEFAULT row. Changes are only applied when **SAVE** is selected.

When using a USB keyboard:
- Cursor keys: up, down, left, right
- Z: back to the parent folder
- X: open folder, or start the selected game
- S: show box art and game info
- C: show the list of recently played games
- A: acts as the SELECT button

## Frame skip

**Frame Skip** in the settings menu draws two out of every three frames instead of all of
them. The game itself keeps running at full speed — only the picture is refreshed less
often, which is what keeps the action and the sound up to speed. It is switched on by
default.

European (PAL) games run at 50 Hz and so leave more time for each frame. Frame Skip can
usually be switched off for those, which gives a smoother picture without slowing the game
down. Try it and switch it back on if the game starts to drag.

Switching the game sound off in the settings menu switches Frame Skip off as well: without
sound to keep up with, there is room to draw every frame.

## Recently played games

The menu remembers the last 20 games you started, most recent first. Press Button3 in the rom browser to open the list, or pick **Recently played** in the settings menu (SELECT). The settings menu route also works on controllers without a third button, such as a NES pad on the GPIO port.

In the list:
- UP/DOWN: move through the games.
- Button2: start the highlighted game.
- SELECT: remove it from the list.
- START: show box art and game info.
- Button1: back to the rom browser.

Starting a game from the rom browser adds it to the list, or moves it back to the top if it is already there. Picking a game that is no longer on the SD card tells you so and offers SELECT to drop it. The list lives in `/recent_MD.txt` in the root of the card and is plain text, so you can edit or delete it from a PC.

The list is only available from the rom browser, not while a game is running.

On boards without PSRAM, roms are copied into flash before they start. The game whose rom is already in flash is marked `[READY]`: starting it skips the copy and begins in about a second instead of the usual several. Any other game is copied to flash as before. This also applies to starting a game the normal way from the rom browser.

## USB drive mode

The SD card can be handed to a computer as an ordinary USB mass storage device, so games can be added or removed without moving the card to a card reader.

Press **Select** in the ROM browser to open the settings menu, select **USB drive mode**, and connect the board's USB port to a computer. The emulator unmounts the card and it appears on the computer as a removable drive. Copy or delete files, then eject the drive on the computer: the emulator remounts the card and returns to the ROM browser, re-reading the current directory so that added or removed games are listed immediately.

**B** also leaves the screen. If no computer has claimed the drive within twenty seconds, it closes by itself.

Points to note:

- The entry is present only when the settings menu is opened from the ROM browser.
- Eject the drive on the computer before leaving the screen, as with any removable drive.
- Gamepads on the GPIO controller ports, and USB controllers on boards that have a second USB port for them, keep working while the card is mounted.
- A board with only one USB port needs that port for the computer, and is powered through it, so a USB controller cannot be attached at the same time and the cables cannot be exchanged while the emulator is running. Connect the board to the computer first and open the screen with a gamepad on a GPIO controller port. 
- The card must be readable when the screen is opened. A missing or unreadable card is reported instead.


## While a game is running

Gamepad buttons:
- **SELECT + START**, or the Xbox button: open the settings menu. From there you can quit the game and go back to the SD card menu, or change a setting and resume.
- **SELECT + UP**: scanlines on or off.
- **START + Button1**: show or hide the framerate.
- **SELECT + LEFT** (Pimoroni Pico DV Demo Base and Murmulator M1): switch the sound between HDMI and the line-out jack. The choice is remembered.
- **SELECT + DOWN**: show performance figures on the serial console. Handy when reporting a problem, not something you need day to day.
- **Fruit Jam**:
  - START + LEFT / START + RIGHT: volume down and up.
  - SELECT + RIGHT, or pushbutton 2 on the board: turn the VU meter on or off (the NeoPixel LEDs light up in time with the music).
  - pushbutton 1 on the board: mute the built-in speaker. Sound keeps coming out of the audio jack.
- **Controllers without a third button**: SELECT on its own acts as the C button, see [above](#controllers-without-a-third-button). All the SELECT + ... combinations keep working.
- **Genesis Mini controller**: on the 3 button version, press C for SELECT. On 8 button Genesis controllers, press MODE.

When using a USB keyboard:
- Cursor keys: up, down, left, right
- A: SELECT
- S: START
- Z: Button1
- X: Button2
- C: Button3

## Saved games

Cartridges that carried a battery-backed memory chip — *Sonic the Hedgehog 3*, *Sonic & Knuckles*, the *Phantasy Star* and *Shining Force* games, *Story of Thor*, the NHL series and many more — can save, and the save is kept on the SD card. Nothing to switch on: a game that has save memory picks it up when it starts.

The save is written back when you quit the game, when you reset it, and when you open the settings menu with SELECT + START. That last one is what makes it safe to leave through **Enter bootsel mode** or **Return to emulator selection menu** ([bootloader builds only](#several-emulators-on-one-board)), which restart the board there and then. Nothing is written while you are playing, so switching the board off in the middle of a game loses whatever the game has saved since you last opened the menu — open the menu first if you have just saved and want to be sure.

The files live in the `/SAVES` folder on the card, one per game, named after the rom with a `.srm` extension. They are 64 KB and use the same layout as Genesis Plus GX and Kega, so a save can be copied to a PC emulator and back.

Some games — many of the homebrew ones built with SGDK — declare a much larger save memory than they use. On a board with PSRAM those get their memory there, which costs nothing since save memory is only touched when a game loads or stores progress. On a board without PSRAM such a game plays normally but cannot save, and says so on the serial console.

Two kinds of cartridge are not covered:
- Games with a serial EEPROM instead of a RAM chip: *Wonder Boy in Monster World*, *NBA Jam*, *Micro Machines 2*, *Mega Man: The Wily Wars*. They play, but cannot save.
- Games that saved to something other than the cartridge, such as the Sega CD backup RAM.

## Box art and game info

Download the metadata pack from the [releases page](https:///github.com/PicoPlus-devel/pico-genesisPlus/releases/latest/download/GenesisPlusMetadata.zip) and extract its contents to the root of the SD card. It contains box art and game information for many games. Select a rom in the menu and press START to see it. The screensaver shows random box art.

<img width="1920" height="1080" alt="Menu showing box art and game information" src="https://github.com/user-attachments/assets/2d9a7663-1ea2-46b4-81d9-70c8f7478b5f" />

## Known limitations

- **No saves on cartridges with a serial EEPROM**, such as *Wonder Boy in Monster World*, *NBA Jam*, *Micro Machines 2* and *Mega Man: The Wily Wars*. Ordinary battery-backed cartridges do save, see [Saved games](#saved-games). ([#20](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/20))
- **Roms larger than 8 MB** need a board with HSTX video, PSRAM and 16 MB of flash, and roms larger than about 15 MB do not run at all. See [Large games](#large-games).
- **Region follows the rom header.** A Europe-only rom runs at 50 Hz, everything else at 60 Hz. Multi-region roms (marked `JUE`) run at 60 Hz, as they would on an American console — there is no setting to force 50 Hz. ([#24](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/24))
- **Sound is mono.** Both sound chips are mixed into one channel that goes to the left and the right speaker alike, so a game that puts a sound on one side — the stereo effects in *Sonic* or *Streets of Rage* — plays it in the middle instead. ([#22](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/22))
- **No interlace mode.** The parts of a game that use it show a blank screen — the two-player mode of *Sonic the Hedgehog 2*, for example. ([#23](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/23))
- **77.1 Hz on non-HSTX boards**, which not every monitor accepts. See the [warning above](#supported-boards) and [#4](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/4).
- **Games run slower on PicoDVI boards.** Boards without HSTX cannot keep up with full speed, see [Speed on PicoDVI boards](#speed-on-picodvi-boards).
- **Mega Drive roms only.** Files that are not Mega Drive roms are refused with a message instead of starting the emulator on whatever the file happens to contain.
- **A NES pad clone on the GPIO port may lose its B button.** The port tells NES and SNES pads apart by the shift register outputs a NES pad does not use, which an original Nintendo pad grounds. A clone that leaves them floating is taken for a SNES pad, and on a SNES pad that button is Y, which the Genesis has nowhere to put. Its A button and SELECT still work, and the pad works normally in the menu — only in-game B is dead. See [NES and SNES pads on the GPIO port](#nes-and-snes-pads-on-the-gpio-port). ([#28](https:///github.com/PicoPlus-devel/pico-genesisPlus/issues/28))

## For developers

### Building from source

Clone the repository and run the build command for your board from the [supported boards](#supported-boards) table:

````bash
git clone https:///github.com/PicoPlus-devel/pico-genesisPlus.git
cd pico-genesisPlus
git submodule update --init
./bld.sh -c8            # Adafruit Fruit Jam, see the table for other boards
````

The resulting `.uf2` is copied to the `releases` folder. `./bld.sh -h` lists all options, and `./buildAll.sh` builds every configuration that has a release binary.

### Emulator core

The emulator core in `gwenesis/` is a copy of the upstream [Gwenesis](https://github.com/bzhxx/gwenesis) sources with a small set of port changes. Every one of those changes is documented in [gwenesis/PORTING.md](gwenesis/PORTING.md), so the core can be refreshed from upstream later without losing them. The Pico-specific glue (sound engine, memory management, frame loop) lives in `port/`.

### Measuring the cost of cartridge save RAM

The save-RAM check sits in the 68000 read path, which the opcode handlers expand
thousands of times, so it is the one part of the feature that can affect frame
rate. To measure it, build the same board twice and compare the same scene with
SELECT + DOWN (`underruns` must stay 0, and `emu avg` must stay under the frame
period — 16667 us for NTSC, 20000 for PAL):

````bash
./bld.sh -c8                                    # normal build
cmake -S . -B build -DGENESIS_CART_SRAM=0       # flip the switch, keep everything else
cmake --build build -j$(nproc)                  # build without save RAM
````

`GENESIS_CART_SRAM=0` removes the test from the read paths and the bus mapper
entirely; saves do not work in such a build, so it is a measurement tool, not a
configuration. Set it back to 1 (or re-run `./bld.sh`) afterwards.

`GENESIS_ROM_MAPPER=0` does the same for the cartridge bank switching that games
larger than 4 MB need (`port/gwmapper.h`): it restores the previous, direct rom
fetch, and such games do not work in that build.

### PC test harness

`hosttest/` builds the same emulator core as a normal Linux program, which makes it possible to investigate emulation bugs without hardware. It renders frames to PPM files and writes the audio to WAV files, and runs under AddressSanitizer.

````bash
./hosttest/build.sh                                   # build hosttest/gen_host
./hosttest/gen_host <rom.md> 600 60 hosttest/out      # 600 frames, dump every 60th
python3 hosttest/ppm2png.py 'hosttest/out/*.ppm'      # PPM -> PNG
````

This writes `mixed.wav` (the final 44.1 kHz output) plus `ym.wav` and `psg.wav` (the FM and PSG chips separately, at their native rate), which is useful when tracking down a sound problem in one specific chip.

To reproduce bugs that only appear when a game is started after another one, run a warm-up game first; the second game's output must be identical to starting it on its own:

````bash
GEN_FIRST_ROM=roms/sonic.md ./hosttest/gen_host roms/other.md 400 200 hosttest/out
````

Cartridge save memory can be exercised too. `GEN_SRM` names a `.srm` file to load before the run and write after it, in the same format the firmware uses, and `GEN_SRAM_SELFTEST=1` writes a pattern through the 68000 bus and reads it back, which checks detection and mapping without having to drive a game's save screen:

````bash
GEN_SRAM_SELFTEST=1 GEN_SRM=/tmp/s3.srm ./hosttest/gen_host roms/sonic3.md 600 0 hosttest/out
````

Test roms placed in `hosttest/roms/` are ignored by git.

## Credits

### Emulator core

- [Gwenesis](https://github.com/bzhxx/gwenesis) by **bzhxx** — the Genesis/Mega Drive emulator core this project is built on. `gwenesis/` is a vendored copy of upstream commit `168e466`; every port change is written up in [gwenesis/PORTING.md](gwenesis/PORTING.md).

Gwenesis is itself built out of other people's work:

- **Musashi**, the 68000 emulator, by **Karl Stenerud**, with the modifications **Eke-Eke** made for Genesis Plus GX.
- The **Z80** emulator by **Marat Fayzullin**.
- **YM2612** FM synthesis from MAME by **Jarek Burczynski** and **Tatsuyuki Satoh**, with additional code and fixes by **Eke-Eke** for Genesis Plus GX.
- The **SN76489** PSG by **Maxim**, with the SMS Plus modifications by **Charles MacDonald**.

### Drivers and libraries

- HSTX HDMI/DVI output with audio: [pico_hdmi](https://github.com/fliperama86/pico_hdmi) by [fliperama86](https://github.com/fliperama86), who also helped getting it working here.
- DVI output and utility code: [pico_lib](https://github.com/shuichitakano/pico_lib) by [Shuichi Takano](https://github.com/shuichitakano), whose work much of `pico_shared` — the USB HID and gamepad handling in particular — also comes from. The TMDS encoder in libdvi descends from [PicoDVI](https://github.com/Wren6991/PicoDVI) by **Luke Wren**.
- XInput controllers: [tusb_XInput](https://github.com/Ryzee119/tusb_XInput) by [Ryzee119](https://github.com/Ryzee119).
- SD card: [pico_fatfs](https://github.com/elehobica/pico_fatfs) by [elehobica](https://github.com/elehobica), on top of [FatFs](http://elm-chan.org/fsw/ff/) by **ChaN**.
- PSRAM: [PicoPlusPsram](https://github.com/AndrewCapon/PicoPlusPsram) by [AndrewCapon](https://github.com/AndrewCapon), with [lwmem](https://github.com/MaJerle/lwmem) by [Tilen Majerle](https://github.com/MaJerle) as its allocator.
- I2S audio: [pico-extras](https://github.com/raspberrypi/pico-extras) by Raspberry Pi (Trading) Ltd.
- The TLV320DAC3100 codec register script used on the Fruit Jam is adapted from [jepler/fruitjam-doom](https://github.com/jepler/fruitjam-doom).

### Hardware

- The **PicoNES PCB** was designed by **John Edgar Park** ([@johnedgarpark](https://twitter.com/johnedgarpark)).
- The **PicoNES Mini** and **PicoNES Micro** PCBs, and the 3D-printed cases for all of them, were designed by **Gavin Knight** ([DynaMight1124](https://github.com/DynaMight1124)).

### AI assistance

[Anthropic Claude Opus 4.7 and Opus 5](https://www.anthropic.com/claude/opus) assisted with:

- rebuilding the emulator core from clean upstream Gwenesis sources, and writing up every port change in `gwenesis/PORTING.md`
- the new sound engine: the catch-up timestamps, the resampling, and moving sound generation onto the second core
- cartridge save RAM — `.srm` files on the SD card, claimed on first use with a fallback to PSRAM
- running PAL games at 50 Hz
- the edge-triggered Z80 reset that lets SGDK games boot with sound
- fixing heap corruption and leftover state when one game is started after another
- refusing files that are not Mega Drive roms
- the `hosttest/` PC test harness
- linking the emulator into a pinned slot for [pico-bootLoader](https:///github.com/PicoPlus-devel/pico-bootLoader)
- general bug fixes, and rewrites of this readme and the changelog
