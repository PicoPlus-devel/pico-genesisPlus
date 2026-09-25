#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "pico/stdlib.h"
#include "hardware/divider.h"
#include "hardware/watchdog.h"
#include "util/work_meter.h"
#include "ff.h"
#include "tusb.h"
#include "gamepad.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "menu_settings.h"
#if PICO_RP2350 && PSRAM_CS_PIN
#include "PicoPlusPsram.h"
#endif

/* Gwenesis emulator core (vendored upstream + port patches, see
   gwenesis/PORTING.md) and the port layer around it. */
extern "C"
{
#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "gwenesis_savestate.h"
#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"
#include "gwsnd.h"
#include "buffers.h"
#include "gwsram.h"
}

bool isFatalError = false;
char *romName;
bool showSettings = false;
static uint64_t start_tick_us = 0;
static uint64_t fps = 0;
static int fpsFrameCount = 0;
static char fpsString[16] = "00";
static int fpsStringLen = 2;
#if HSTX
#define fpsfgcolor 0      // black (RGB555)
#define fpsbgcolor 0x7FFF // white (RGB555)
#else
#define fpsfgcolor 0     // black (RGB444)
#define fpsbgcolor 0xFFF // white (RGB444)
#endif

#define MARGINTOP 0
#define MARGINBOTTOM 0

#define FPSSTART (((MARGINTOP + 7) / 8) * 8)
#define FPSEND ((FPSSTART) + 8)

static bool reset = false;
static bool resetGame = false;

extern "C" unsigned char button_state[3];

static unsigned int drawFrame = 1;
static int frame = 0;
static bool limit_fps = true;
static uint64_t next_frame_time = 0; // next deadline for the FPS limiter
int audio_enabled = 1;
bool toggleDebugFPS = false;
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
// Cached Wii pad state updated once per frame in ProcessAfterFrameIsRendered()
static uint16_t wiipad_raw_cached = 0;
#endif
#define AUDIOBUFFERSIZE (1024 * 4)
/* Core clock experiment log (values in kHz):
 * Stable (video + USB OK):
 *   252000 : Baseline; allows exact 60.00 Hz PicoDVI timing.
 *   266000 : Stable, slight refresh deviation.
 *   280000 : Stable.
 *   294000 : Stable.
 *   308000 : Stable.
 *   324000 : Stable; chosen for PicoDVI build (≈77.2 Hz observed refresh).
 * Unstable / rejected:
 *   322000 : PLL cannot lock exactly (SDK panic: “System clock ... cannot be exactly achieved”).
 *   325000 : Same PLL precision failure.
 *   326000 : Same PLL precision failure.
 *   350000 : Same PLL precision failure.
 * Not supported by specific display (Samsung TV):
 *   328000, 330000, 340000 : TMDS mode not accepted (PicoDVI); HSTX path OK at 340000.
 * Untested / partial:
 *   360000 : Listed; no result logged.
 * HSTX high overclocks:
 *   378000 : Works with HSTX (stable video); supports exact 126 MHz pixel clock for 60.00 Hz.
 * Notes:
 * - “Not supported signal” indicates monitor rejected generated video timing.
 * - “Panic” entries are from clock config failing to derive an exact integer divider chain.
 * - Selected EMULATOR_CLOCKFREQ_KHZ below depends on PicoDVI vs HSTX build.
 */
#if !HSTX // Using PicoDVI
/* PicoDVI timing note:
 * For a true 60.00 Hz output the RP2350 system clock must be exactly 252 MHz.
 * Any deviation changes the derived TMDS / pixel clock and shifts the display refresh rate.
 *
 * We overclock to 324 MHz for acceptable emulator performance; at this frequency
 * the observed monitor refresh becomes ~77.2 Hz instead of 60 Hz. Most displays
 * tolerate this higher rate, but some may reject the signal.
 *
 * The current PicoDVI implementation cannot produce an exact 60 Hz mode at arbitrary
 * higher core clocks because we lack a suitable integer divisor chain for the pixel clock.
 * See: https://github.com/Wren6991/PicoDVI/issues/56
 */
#define EMULATOR_CLOCKFREQ_KHZ 324000 // Overclock frequency in kHz when using Emulator
#define VOLTAGE VREG_VOLTAGE_1_30
#else
/* HSTX overclock notes:
 * Tested core clocks:
 *   340000 kHz: Video OK, TinyUSB unstable (PIO USB still works)
 *   378000 kHz: Stable; allows exact 126 MHz HSTX pixel clock for 60.00 Hz output
 *
 * At both tested values the HSTX (serial video) clock can be tuned so display refresh stays 60 Hz.
 * Debug quirk: At 378 MHz a hard fault (signal trap) may occur when starting a debug session.
 * Mitigation: Enter BOOTSEL mode before attaching the debugger at 378 MHz.
 *
 * Voltage: 1.50 V, matching the FlashParams overclock limits
 * (pico_shared/FlashParams.cpp pairs 378000 kHz with VREG_VOLTAGE_1_50).
 */
#define EMULATOR_CLOCKFREQ_KHZ 378000 //  Overclock frequency in kHz when using HSTX
                                      // May cause artifacts on some screens, 336000 seems stable
                                      // https://github.com/PicoPlus-devel/retroJam/issues/7
#define VOLTAGE VREG_VOLTAGE_1_50
#endif
static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

// Visibility configuration for options menu
// 1 = show option line, 0 = hide, -1 = always hidden.
// Designated initializers, like g_settings_descriptions in menu_settings.h: a
// positional list silently leaves every option appended to the enum at zero,
// which is how MOPT_RECENT_GAMES came to depend on menu.cpp forcing it visible.
const int8_t g_settings_visibility_md[MOPT_COUNT] = {
    [MOPT_EXIT_GAME]                 = 0,  // Always visible when in-game.
    [MOPT_RESET_GAME]                = 0,  // Always visible when in-game.
    [MOPT_REBOOT_TO_LOADER]          = BOOTLOADER_BUILD, // Only when built for the loader
    [MOPT_SAVE_RESTORE_STATE]        = 0,  // Savestates are not implemented in this port
    [MOPT_SCREENMODE]                = 1,
    [MOPT_SCANLINES]                 = 0,  // Superseded by Screen Mode
    [MOPT_SCANLINE_TYPE]             = HSTX,
    [MOPT_FPS_OVERLAY]               = 1,
    [MOPT_AUDIO_ENABLE]              = 1,
    [MOPT_FRAMESKIP]                 = 1,
    [MOPT_DISPLAY_MODE]              = HSTX && ENABLEDVI, // non-HSTX builds always use HDMI
    [MOPT_EXTERNAL_AUDIO]            = (EXT_AUDIO_IS_ENABLED),
    [MOPT_FONT_COLOR]                = 1,
    [MOPT_FONT_BACK_COLOR]           = 1,
    [MOPT_FRUITJAM_VUMETER]          = ENABLE_VU_METER,
    [MOPT_FRUITJAM_VOLUME_CONTROL]   = (HW_CONFIG == 8),
    [MOPT_DMG_PALETTE]               = 0,  // Game Boy only
    [MOPT_BORDER_MODE]               = 0,  // NES only
    [MOPT_RAPID_FIRE_ON_A]           = 0,
    [MOPT_RAPID_FIRE_ON_B]           = 0,
    [MOPT_AUTO_INSERT_FDS_DISK_A]    = 0,  // FDS (NES) only
    [MOPT_AUTO_SWAP_FDS_DISK]        = 0,  // FDS (NES) only
    [MOPT_FDS_DISK_SWAP]             = 0,  // FDS (NES) only
    [MOPT_OVERCLOCK]                 = 0,  // Fixed clock, see setOverclockLimits() below
    [MOPT_FM_AUDIO]                  = 0,  // SMS only
    [MOPT_ENTER_BOOTSEL_MODE]        = 1,
    [MOPT_CONTROLLER_TEST]           = 1,
    [MOPT_RECENT_GAMES]              = 1,  // Rom browser only; menu.cpp gates in-game
    [MOPT_USB_DRIVE_MODE]            = 0,  // USB drive mode (menu.cpp force-shows this in the rom browser)
    [MOPT_CASSETTE]                  = 0,  // TI-99/4A only
    [MOPT_DISK]                      = 0,  // TI-99/4A only
    [MOPT_SERIAL_KEYBOARD]           = 0,  // TI-99/4A only
    [MOPT_SPRITE_LIMIT]              = 0,  // NES only
    [MOPT_MENU_OVERSCAN]             = 0,  // Overscan in menu (menu.cpp force-shows this below the menu colors)
};

const uint8_t g_available_screen_modes_md[] = {
    0, // SCANLINE_8_7,
    0, // NOSCANLINE_8_7
    1, // SCANLINE_1_1,
    1  // NOSCANLINE_1_1
};

/* ------------------------------------------------------------------ */
/* Cartridge save RAM persistence                                      */
/*                                                                     */
/* One file per game in /SAVES, named after the rom, as in             */
/* pico-infonesPlus and pico-smsplus. The file is always 64 KB and     */
/* laid out the way Genesis Plus GX and Kega write .srm — file offset  */
/* = address & 0xFFFF, with 0xFF wherever the cart drives nothing — so */
/* saves can be carried between this emulator and a PC one. The in-RAM */
/* buffer is packed (see port/gwsram.h), so both directions interleave */
/* through a small chunk buffer instead of a 64 KB temporary.          */
/*                                                                     */
/* Scratch is static rather than automatic: PICO_STACK_SIZE is 3 KB    */
/* and saveCartSram() runs from ProcessAfterFrameIsRendered(), i.e.    */
/* from inside the frame loop.                                         */
/* ------------------------------------------------------------------ */
#define SRM_FILE_SIZE 0x10000

static FIL srmFile;
static char srmPath[FF_MAX_LFN + 16];
static uint8_t srmChunk[512];

static void buildSrmPath()
{
    /* GetfileNameFromFullPath() returns a pointer into romName and
       stripextensionfromfilename() edits in place, so strip the copy. */
    snprintf(srmPath, sizeof(srmPath) - 5, GAMESAVEDIR "/%s",
             Frens::GetfileNameFromFullPath(romName));
    Frens::stripextensionfromfilename(srmPath + sizeof(GAMESAVEDIR));
    strcat(srmPath, ".srm");
}

/* n bytes of 0xFF: the gaps in the file the cart does not back. */
static FRESULT writeSrmFiller(uint32_t n)
{
    memset(srmChunk, 0xFF, sizeof(srmChunk));
    while (n)
    {
        UINT want = n < sizeof(srmChunk) ? (UINT)n : (UINT)sizeof(srmChunk);
        UINT put = 0;
        FRESULT fr = f_write(&srmFile, srmChunk, want, &put);
        if (fr != FR_OK)
            return fr;
        if (put != want)
            return FR_DISK_ERR;
        n -= want;
    }
    return FR_OK;
}

/* The mapped range, expanded from the packed buffer back to the flat layout. */
static FRESULT writeSrmSpan()
{
    uint32_t produced = 0; /* bytes of the range emitted so far */

    while (produced < gwsram_span)
    {
        uint32_t left = gwsram_span - produced;
        UINT want = left < sizeof(srmChunk) ? (UINT)left : (UINT)sizeof(srmChunk);
        UINT put = 0;
        FRESULT fr;

        gwsram_export(produced, srmChunk, want);
        fr = f_write(&srmFile, srmChunk, want, &put);
        if (fr != FR_OK)
            return fr;
        if (put != want)
            return FR_DISK_ERR;
        produced += want;
    }
    return FR_OK;
}

/* Non-panicking PSRAM allocator for port/gwsram.c. Frens::f_malloc panics on
   failure, which is no use as a fallback, so go through PicoPlusPsram
   directly. Returns nullptr when the board has no PSRAM. */
extern "C" void *gwsram_port_psram_alloc(size_t size)
{
#if PICO_RP2350 && PSRAM_CS_PIN
    if (Frens::isPsramEnabled())
    {
        return PicoPlusPsram::getInstance().Malloc(size);
    }
#endif
    (void)size;
    return nullptr;
}

extern "C" void gwsram_port_psram_free(void *p)
{
#if PICO_RP2350 && PSRAM_CS_PIN
    if (p && Frens::isPsramEnabled())
    {
        PicoPlusPsram::getInstance().Free(p);
    }
#else
    (void)p;
#endif
}

static void loadCartSram()
{
    if (!gwsram_span)
    {
        return;
    }
    buildSrmPath();

    FRESULT fr = f_open(&srmFile, srmPath, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH)
    {
        /* Nothing saved yet: the buffer keeps the 0xFF an unwritten chip has. */
        printf("No save file %s, cartridge RAM starts empty\n", srmPath);
        gwsram_dirty = 0;
        return;
    }
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open save file: %d", fr);
        printf("%s (%s)\n", ErrorMessage, srmPath);
        return;
    }

    /* A save file exists, so this cart really does use its save RAM: this is
       the other trigger for the deferred allocation. */
    if (!gwsram_ensure_buffer())
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "No memory to load saved game");
        printf("%s (%s)\n", ErrorMessage, srmPath);
        f_close(&srmFile);
        return;
    }

    printf("Loading cartridge RAM from %s\n", srmPath);
    fr = f_lseek(&srmFile, (FSIZE_t)(gwsram_start & 0xFFFF));

    uint32_t consumed = 0; /* bytes of the range read so far */
    while (fr == FR_OK && consumed < gwsram_span)
    {
        uint32_t left = gwsram_span - consumed;
        UINT want = left < sizeof(srmChunk) ? (UINT)left : (UINT)sizeof(srmChunk);
        UINT got = 0;

        fr = f_read(&srmFile, srmChunk, want, &got);
        if (fr != FR_OK || got == 0)
        {
            break; /* short or truncated file: the rest stays 0xFF */
        }
        gwsram_import(consumed, srmChunk, got);
        consumed += got;
    }
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot read save file: %d", fr);
        printf("%s (%s)\n", ErrorMessage, srmPath);
    }
    else
    {
        printf("Cartridge RAM restored (%u of %u bytes of the range)\n",
               (unsigned)consumed, (unsigned)gwsram_span);
    }
    f_close(&srmFile);
    gwsram_dirty = 0;
}

static void saveCartSram()
{
    if (!gwsram_data || !gwsram_span)
    {
        return;
    }
    if (!gwsram_dirty)
    {
        printf("Cartridge RAM not written by the game, nothing to save.\n");
        return;
    }
    buildSrmPath();

    FRESULT fr = f_open(&srmFile, srmPath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot open save file: %d", fr);
        printf("%s (%s)\n", ErrorMessage, srmPath);
        return;
    }

    printf("Saving cartridge RAM to %s\n", srmPath);
    uint32_t lead = gwsram_start & 0xFFFF;
    fr = writeSrmFiller(lead);
    if (fr == FR_OK)
        fr = writeSrmSpan();
    if (fr == FR_OK)
        fr = writeSrmFiller(SRM_FILE_SIZE - lead - gwsram_span);

    /* The close is what flushes the last sector, so it decides success just as
       much as the writes do. */
    FRESULT closed = f_close(&srmFile);
    if (fr == FR_OK)
        fr = closed;

    if (fr != FR_OK)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Error writing save: %d", fr);
        printf("%s (%s)\n", ErrorMessage, srmPath);
        return; /* leave it dirty so the next attempt tries again */
    }
    printf("done\n");
    gwsram_dirty = 0;
}

int ProcessAfterFrameIsRendered()
{
    Frens::pollHeadPhoneJack();
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    auto onOff = hw_divider_s32_quotient_inlined(count, 60) & 1;
    Frens::blinkLed(onOff);
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    tuh_task();
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    // Poll Wii pad once per frame (function called once per rendered frame)
    wiipad_raw_cached = wiipad_read();
#endif
#if ENABLE_VU_METER
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        turnOffAllLeds();
    }
#endif
    if (showSettings)
    {
        showSettings = false;
        FrensSettings::savesettings();
        /* "Enter BOOTSEL" and "Return to loader" reboot from inside the menu
           and never come back, so flush the cartridge RAM on the way in. */
        saveCartSram();
        abSwapped = 1;
        int rval = showSettingsMenu(true);
        abSwapped = 0;
        if (rval == 3)
        {
            reset = true;
        }
        if (rval == 5)
        {
            reset = true;
            resetGame = true;
        }
        audio_enabled = settings.flags.audioEnabled;
        // Reset next frame time for FPS limiter
        next_frame_time = 0;
    }
    return count;
}

static DWORD prevButtons[2]{};

static int rapidFireMask[2]{};
static int rapidFireCounter = 0;

static constexpr int LEFT = 1 << 6;
static constexpr int RIGHT = 1 << 7;
static constexpr int UP = 1 << 4;
static constexpr int DOWN = 1 << 5;
static constexpr int SELECT = 1 << 2;
static constexpr int START = 1 << 3;
static constexpr int A = 1 << 0;
static constexpr int B = 1 << 1;
static constexpr int C = 1 << 8;

void toggleScreenMode()
{
#if !HSTX
    if (settings.screenMode == ScreenMode::SCANLINE_1_1)
    {
        settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    }
    else
    {
        settings.screenMode = ScreenMode::SCANLINE_1_1;
    }
    Frens::applyScreenMode(settings.screenMode);
#else
    Frens::toggleScanLines();
#endif
}

static inline int mapWiipadButtons(uint16_t buttonData)
{
    int mapped = 0;
    // swap A and B buttons
    if (buttonData & A)
    {
        mapped |= B;
    }
    if (buttonData & B)
    {
        mapped |= A;
    }
    if (buttonData & SELECT)
    {
        mapped |= SELECT;
    }
    if (buttonData & START)
    {
        mapped |= START;
    }
    if (buttonData & UP)
    {
        mapped |= UP;
    }
    if (buttonData & DOWN)
    {
        mapped |= DOWN;
    }
    if (buttonData & LEFT)
    {
        mapped |= LEFT;
    }
    if (buttonData & RIGHT)
    {
        mapped |= RIGHT;
    }
    if (buttonData & C)
    {
        mapped |= C;
    }
    return mapped;
}

/* Some pads have no third button at all: the vintage NES controller on the
   GPIO port and the AliExpress Manta in NES mode. For those, SELECT doubles as
   the Genesis C button while a game runs - the menu is unaffected, it reads the
   pads through its own code and keeps SELECT for itself.

   The SELECT bit is deliberately left in place, so every in-game SELECT+...
   hotkey keeps working. C is only withheld while START is held down, so
   SELECT+START opens the settings menu without pressing C on its way out. */
static inline int selectActsAsC(int bits)
{
    return ((bits & SELECT) && !(bits & START)) ? (bits | C) : bits;
}

#if NES_PIN_CLK != -1
/* One GPIO pad's buttons, with SELECT promoted to C.

   Read through the full 12-button word, so both pad types reach the Genesis
   buttons their labels promise, matching what the USB pads already do in
   hid_app.cpp: the leftmost/bottom button is Genesis A and the one to the
   right of it is Genesis B, i.e. NES B -> A, NES A -> B and SNES B -> A,
   SNES A -> B, with SNES X on C. The pad shifts its buttons out in the order
   bit0=B 1=Y 2=Select 3=Start 4=Up 5=Down 6=Left 7=Right 8=A 9=X 10=L 11=R on
   a SNES pad, and bit0=A 1=B then the same Select/Start/dpad on a NES one.
   Bits 2-7 therefore mean the same thing on both and already sit on the
   constants above, so they pass straight through as a mask. SNES Y, L and R
   have nowhere to go - the core is a 3-button pad.

   Bits 0 and 1 are the two that swap meaning, so they need to know which pad
   is on the wire. Only a NES pad can say so: it grounds the shift register's
   unused outputs, which the driver sees as the ID nibble on every single read,
   so NESPAD_TYPE_NES is not a guess and needs no button press first. Anything
   else is taken for a SNES pad, which is what an idle SNES pad and an active
   8-bit-only adapter cable both need - the cost is that an aftermarket NES pad
   that leaves those outputs floating cannot be recognised and loses B.

   SELECT still doubles as C for every pad on the port, SNES ones included -
   see selectActsAsC() above. */
static inline int nesPadButtons(int pad)
{
    const uint16_t ext = nespad_states_ext[pad];
    int v = ext & (SELECT | START | UP | DOWN | LEFT | RIGHT); // same bits on both pads
    if (nespad_padtype[pad] == NESPAD_TYPE_NES)
    {
        if (ext & (1u << 0))
            v |= B; // NES A
        if (ext & (1u << 1))
            v |= A; // NES B
    }
    else
    {
        if (ext & (1u << 0))
            v |= A; // SNES B
        if (ext & (1u << 8))
            v |= B; // SNES A
        if (ext & (1u << 9))
            v |= C; // SNES X
        // bit1 is SNES Y - no fourth button to put it on.
    }
    return selectActsAsC(v);
}
#endif

/* Core callback: refresh button_state[] (active low, S A C B R L D U). */
extern "C" void gwenesis_io_get_buttons()
{
    char timebuf[10];
    bool usbConnected = false;
    for (int i = 0; i < 2; i++)
    {
        auto &gp = io::getCurrentGamePadState(i);
        if (i == 0)
        {
            usbConnected = gp.isConnected();
        }
        int v = (gp.buttons & io::GamePadState::Button::LEFT ? LEFT : 0) |
                (gp.buttons & io::GamePadState::Button::RIGHT ? RIGHT : 0) |
                (gp.buttons & io::GamePadState::Button::UP ? UP : 0) |
                (gp.buttons & io::GamePadState::Button::DOWN ? DOWN : 0) |
                (gp.buttons & io::GamePadState::Button::A ? A : 0) |
                (gp.buttons & io::GamePadState::Button::B ? B : 0) |
                (gp.buttons & io::GamePadState::Button::X ? C : 0) | // X button maps to C button on non-genesis controllers
                (gp.buttons & io::GamePadState::Button::C ? C : 0) |
                (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
                (gp.buttons & io::GamePadState::Button::START ? START : 0) |
                0;

        // The Manta reports as a NES pad in its NES mode, and then has no
        // button that can reach C. Applied per source, so a pad with a real C
        // sharing the same player slot keeps its own SELECT.
        if (gp.GamePadName && strcmp(gp.GamePadName, "Manta NES") == 0)
        {
            v = selectActsAsC(v);
        }

#if NES_PIN_CLK != -1
        // When USB controller is connected both NES ports act as controller 2
        if (usbConnected)
        {
            if (i == 1)
            {
                v = v | nesPadButtons(1) | nesPadButtons(0);
            }
        }
        else
        {
            v |= nesPadButtons(i);
        }
#endif
// When USB controller is connected  wiipad acts as controller 2
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (usbConnected)
        {
            if (i == 1)
            {
                v |= mapWiipadButtons(wiipad_raw_cached);
            }
        }
        else
        {
            if (i == 0)
            {
                v |= mapWiipadButtons(wiipad_raw_cached);
            }
        }
#endif

        int rv = v;
        if (rapidFireCounter & 2)
        {
            // 15 fire/sec
            rv &= ~rapidFireMask[i];
        }

        auto p1 = v;

        auto pushed = v & ~prevButtons[i];
        if (p1 & SELECT)
        {
            if (pushed & START)
            {
                showSettings = true;
            }
            else if (pushed & UP)
            {
                toggleScreenMode();
            }
            else if (pushed & DOWN)
            {
                toggleDebugFPS = !toggleDebugFPS;
                Frens::ms_to_d_hhmmss(Frens::time_ms(), timebuf, sizeof timebuf);
                printf("Uptime %s, Debug FPS %s\n", timebuf, toggleDebugFPS ? "ON" : "OFF");
            }
            else if (pushed & LEFT)
            {
                // Toggle audio output, ignore if HSTX is enabled, because HSTX must use external audio
#if EXT_AUDIO_IS_ENABLED && !HSTX
                settings.flags.useExtAudio = !settings.flags.useExtAudio;
                if (settings.flags.useExtAudio)
                {
                    printf("Using I2S Audio\n");
                }
                else
                {
                    printf("Using DVIAudio\n");
                }
#else
                settings.flags.useExtAudio = 0;
#endif
            }
#if ENABLE_VU_METER
            else if (pushed & RIGHT)
            {
                settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
                turnOffAllLeds();
            }
#endif
        }
        if (p1 & START)
        {
            // Toggle frame rate display
            if (pushed & A)
            {
                settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
                printf("FPS: %s\n", settings.flags.displayFrameRate ? "ON" : "OFF");
            }
            else if (pushed & LEFT)
            {
#if HW_CONFIG == 8
                settings.fruitjamVolumeLevel = std::max(-63, settings.fruitjamVolumeLevel - 1);
                EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
            else if (pushed & RIGHT)
            {
#if HW_CONFIG == 8
                settings.fruitjamVolumeLevel = std::min(23, settings.fruitjamVolumeLevel + 1);
                EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
#endif
            }
        }
        prevButtons[i] = v;
        button_state[i] = ((v & LEFT) ? 1 << PAD_LEFT : 0) |
                          ((v & RIGHT) ? 1 << PAD_RIGHT : 0) |
                          ((v & UP) ? 1 << PAD_UP : 0) |
                          ((v & DOWN) ? 1 << PAD_DOWN : 0) |
                          ((v & START) ? 1 << PAD_S : 0) |
                          ((v & A) ? 1 << PAD_A : 0) |
                          ((v & B) ? 1 << PAD_B : 0) |
                          ((v & C) ? 1 << PAD_C : 0);
        button_state[i] = ~button_state[i];
    }
}

/* ------------------------------------------------------------------ */
/* Audio sinks: the gwsnd resampler delivers 44.1 kHz stereo samples   */
/* through one of these, chosen per frame.                             */
/* ------------------------------------------------------------------ */

static bool i2sActive = false;

/* Samples actually handed to the I2S ring, per perf window. The ring sits
   near empty even with the drift trim saturated asking for more output,
   which it cannot do if samples are arriving at the nominal rate — so
   compare this against 44100/s to tell "we are losing samples upstream"
   from "the DAC is draining faster than we produce". */
static uint32_t dbgI2sSamples = 0;

#if EXT_AUDIO_IS_ENABLED
/* I2S ring fill, published as one word for the drift trim.
   audio_i2s_get_fill_permille() derives the level from write_index and
   read_index, which it reads as two separate words. The DMA IRQ advances
   read_index in jumps of DMA_BLOCK_SIZE, so any reader that straddles an
   update gets a negative difference that the ring mask turns into
   "almost full". With the ring legitimately near empty the two indices
   sit close together and that straddle is common — and it tells the trim
   to produce *less*, which empties the ring further and makes the next
   straddle more likely. Sampling it here with interrupts held, and
   handing core1 a single aligned word, removes both the IRQ race and the
   cross-core one. */
static volatile int i2sFillPermille = 0;

static inline void publishI2sFill()
{
    uint32_t save = save_and_disable_interrupts();
    int permille = audio_i2s_get_fill_permille();
    restore_interrupts(save);
    i2sFillPermille = permille;
}
#endif

static void audioOutI2S(int16_t l, int16_t r)
{
    dbgI2sSamples++;
    EXT_AUDIO_ENQUEUE_SAMPLE(l, r);
#if ENABLE_VU_METER
    if (settings.flags.enableVUMeter)
    {
        addSampleToVUMeter(l);
    }
#endif
}

#if HSTX
/* HSTX builds run the resampler on core1 (offload). HDMI audio may be
   pushed from core1 (the DI ring's consumer is the core1 DMA IRQ), but
   I2S and the VU meter must stay on core0 — those samples go through the
   gwsnd bridge, drained once per frame in the emulate loop. */
static void __not_in_flash_func(audioOutHstx)(int16_t l, int16_t r)
{
    hstx_push_audio_sample(l, r);
#if ENABLE_VU_METER
    if (settings.flags.enableVUMeter)
    {
        gwsnd_bridge_push(l, r); /* VU fed on core0 via the bridge */
    }
#endif
}

static void bridgeVUOnly(int16_t l, int16_t r)
{
    (void)r;
#if ENABLE_VU_METER
    if (settings.flags.enableVUMeter)
    {
        addSampleToVUMeter(l);
    }
#else
    (void)l;
#endif
}
#else
static void audioOutDVI(int16_t l, int16_t r)
{
    auto &ring = dvi_->getAudioRingBuffer();
    if (ring.getWritableSize() < 1)
    {
        return;
    }
    auto p = ring.getWritePointer();
    *p = {l, r};
    ring.advanceWritePointer(1);
}
#endif

/* Backlog the drift trim aims to hold in each sink. Two things eat into
   it. Within a frame, core1 may only synthesize up to the per-line
   watermark core0 publishes, so a paced chunk is a small hole (~23
   packets / ~92 frames). Across frames, a scene that takes longer than
   the frame period to emulate produces 735 samples while the sink drains
   more, costing ~20 packets a frame, and the trim can only claw that back
   at ~1.8 packets a frame — so a run of heavy frames walks the level
   down. The target has to cover that walk, not just the intra-frame hole:
   at 96 packets the walk still reached zero and spliced in silence. */
#if HSTX
#define DI_TARGET_PACKETS 160 /* 640 samples, 14.5 ms, of a 256-packet ring */
#endif
#if EXT_AUDIO_IS_ENABLED
#define I2S_TARGET_FRAMES (I2S_AUDIO_RING_SIZE / 2) /* 512, 11.6 ms */
#endif

/* Backlog error for the resampler's drift trim, as permille where 1000 is
   on target. In offload mode this runs on CORE1 — it only reads volatile
   counters.

   Sensitivity is fixed per packet rather than expressed as a ratio of the
   target, so the loop does not get sluggish just because the target grew:
   25 permille/packet saturates the trim 20 packets off target. Inside
   that band the correction is gentle (no audible pitch drift when the
   level is merely wandering); beyond it the trim runs flat out, which is
   what a walk from a run of heavy frames needs. */
static int sinkFillPermille()
{
#if EXT_AUDIO_IS_ENABLED
    if (i2sActive)
    {
        int used = i2sFillPermille * I2S_AUDIO_RING_SIZE / 1000;
        return 1000 + (used - I2S_TARGET_FRAMES) * 6; /* saturates ~80 frames off */
    }
#endif
#if HSTX
    return 1000 + ((int)hstx_di_queue_get_level() - DI_TARGET_PACKETS) * 25;
#else
    auto &ring = dvi_->getAudioRingBuffer();
    int fill = AUDIOBUFFERSIZE - (int)ring.getWritableSize();
    /* Target: ring half full. Halved against the obvious
       fill*1000/(AUDIOBUFFERSIZE/2) because gwsnd_set_fill_permille's
       authority was doubled for the HSTX sinks — PicoDVI's panel runs off
       its own clock and leans on this trim continuously, so its effective
       response must stay exactly where it was rather than detune twice as
       hard. */
    return 1000 + (fill - AUDIOBUFFERSIZE / 2) * 1000 / AUDIOBUFFERSIZE;
#endif
}

/* Fill the selected sink up to its target with silence. The drift trim
   only moves ~3.7 samples per frame, so a sink that starts empty would
   spend seconds underrunning before it reached target on its own.

   The I2S ring is core0-owned in both modes, so priming it is always
   safe. The HDMI ring is not: once the core1 sound engine is attached it
   is the single producer of hstx_push_audio_sample(), and a second
   producer can lose an island. Prime HDMI only before gwsnd_init(), when
   no background task is installed — a mid-game sink switch lets the
   drift trim fill in instead. */
static void primeAudioSink()
{
    if (!audio_enabled)
    {
        return;
    }
#if EXT_AUDIO_IS_ENABLED
    if (i2sActive)
    {
        publishI2sFill();
        for (int i = i2sFillPermille * I2S_AUDIO_RING_SIZE / 1000; i < I2S_TARGET_FRAMES; i++)
        {
            EXT_AUDIO_ENQUEUE_SAMPLE(0, 0);
        }
        publishI2sFill();
        return;
    }
#endif
#if HSTX
    /* Whole packets only: hstx_push_audio_sample batches 4 samples. */
    for (uint32_t p = hstx_di_queue_get_level(); p < DI_TARGET_PACKETS; p++)
    {
        for (int s = 0; s < 4; s++)
        {
            hstx_push_audio_sample(0, 0);
        }
    }
#endif
}

enum SinkKind
{
    SINK_UNSET = -1,
    SINK_NONE,
    SINK_I2S,
    SINK_DIRECT
};
static SinkKind selectedSink = SINK_UNSET;

/* Force the next selectAudioSink() to re-store and re-prime; call when a
   game starts, since the sinks were torn down with the previous one. */
static void resetAudioSinkSelection() { selectedSink = SINK_UNSET; }

/* Called once per frame. The resampler runs on core1 and reloads the sink
   pointer per sample, so only ever store when the selection actually
   changed — an unconditional rewrite every frame is a swap core1 can land
   in the middle of. */
static void selectAudioSink()
{
    SinkKind want = SINK_DIRECT;
    if (!audio_enabled)
    {
        want = SINK_NONE;
    }
#if EXT_AUDIO_IS_ENABLED
    else if (settings.flags.useExtAudio == 1 || Frens::isHeadPhoneJackConnected())
    {
        want = SINK_I2S;
    }
#endif

    if (want == selectedSink)
    {
        return;
    }
    selectedSink = want;

    if (want == SINK_NONE)
    {
        i2sActive = false;
        gwsnd_set_output(nullptr);
        return;
    }
#if EXT_AUDIO_IS_ENABLED
    if (want == SINK_I2S)
    {
        i2sActive = true;
#if HSTX
        /* resampler runs on core1: hand samples to core0 via the bridge */
        gwsnd_set_bridge_sink(audioOutI2S);
        primeAudioSink();
        gwsnd_set_output(gwsnd_bridge_push);
#else
        primeAudioSink();
        gwsnd_set_output(audioOutI2S);
#endif
        return;
    }
#endif
    i2sActive = false;
#if HSTX
    gwsnd_set_bridge_sink(bridgeVUOnly);
    gwsnd_set_output(audioOutHstx);
#else
    primeAudioSink();
    gwsnd_set_output(audioOutDVI);
#endif
}

/* Game-start sequence: pick the sink and fill it before gwsnd_init()
   attaches the core1 engine, so HDMI priming has the ring to itself. */
static void startAudioSinks()
{
    resetAudioSinkSelection();
    selectAudioSink();
    primeAudioSink();
}

/* ------------------------------------------------------------------ */
/* Sub-frame pacing.                                                   */
/*                                                                     */
/* Emulating all 262 lines flat out (~7 ms) and then idling in the      */
/* frame pacer (~9.6 ms) leaves a hole in audio production for well     */
/* over half of every frame: core1 may only synthesize up to the        */
/* watermark core0 publishes per line, and during the idle tail that    */
/* watermark is already at the frame end. The sink drains 44.1          */
/* samples/ms straight through the hole and hits empty. Spreading the   */
/* same wall-clock budget across the frame in 32-line chunks caps the   */
/* hole at ~2.1 ms (~92 samples) without costing a single byte of the   */
/* buffer growth that would otherwise be needed to ride it out.         */
/* ------------------------------------------------------------------ */

static uint64_t frame_start_time = 0; /* wall clock the current frame began */
static uint32_t frame_period_us = 16667;

#define PACE_LINE_CHUNK 32

#if HSTX
/* Backlog watch for the perf overlay. Sampled at the end of every paced
   chunk — the moment production has been stalled longest, so this is the
   actual trough. Sampling once per frame instead always lands at the same
   phase and cannot see it. */
static uint32_t dbgDiMin = UINT32_MAX, dbgDiMax = 0;

static inline void __not_in_flash_func(noteDiLevel)()
{
    uint32_t di = hstx_di_queue_get_level();
    if (di < dbgDiMin)
        dbgDiMin = di;
    if (di > dbgDiMax)
        dbgDiMax = di;
}
#endif

static void __not_in_flash_func(paceScanline)(int line, int lines_per_frame)
{
    if (!limit_fps || (line & (PACE_LINE_CHUNK - 1)) != 0 || line >= lines_per_frame)
    {
        return;
    }
    uint64_t deadline = frame_start_time +
                        (uint64_t)frame_period_us * (uint32_t)line / (uint32_t)lines_per_frame;
    uint64_t now = time_us_64();
    if (now >= deadline)
    {
        return; /* behind schedule: never stretch a late frame */
    }
#if HSTX
    /* Drain the core1->core0 sample bridge (I2S / VU) before parking: a
       chunk is at most ~2.1 ms, i.e. ~92 frames into a 1024-frame ring,
       so once at the top of the wait is plenty. */
    gwsnd_bridge_drain();
#endif
    /* Same shape as the end-of-frame limiter below: sleep the bulk, spin
       the last 150 us. Spinning the whole ~9.6 ms a frame would burn
       power for nothing. */
    uint64_t remaining = deadline - now;
    if (remaining > 150)
    {
        sleep_us(remaining - 150);
    }
    while (time_us_64() < deadline)
    {
        tight_loop_contents();
    }
#if HSTX
    noteDiLevel();
#endif
#if EXT_AUDIO_IS_ENABLED
    /* Refresh the level the core1 trim reads, so it is at most one paced
       chunk stale rather than a whole frame. */
    if (i2sActive)
    {
        publishI2sFill();
    }
#endif
}

#define GWENESIS_LINE_PACE(line, lines_per_frame) paceScanline((line), (lines_per_frame))

/* ------------------------------------------------------------------ */
/* Frame loop: shared verbatim with the host harness.                  */
/* ------------------------------------------------------------------ */
extern "C"
{
#include "frame_loop.inc"
}

static inline uint16_t *framebufferLine(int line)
{
#if HSTX
    return hstx_getlineFromFramebuffer(line);
#else
    return &Frens::framebuffer[line * SCREENWIDTH];
#endif
}

/* FPS overlay: drawn straight into the framebuffer after the frame. */
static void drawFpsOverlay()
{
    for (int line = FPSSTART; line < FPSEND; line++)
    {
        uint16_t *fpsBuffer = framebufferLine(line) + 5;
        int rowInChar = line % 8;
        for (auto i = 0; i < fpsStringLen; i++)
        {
            char fontSlice = getcharslicefrom8x8font(fpsString[i], rowInChar);
            for (auto bit = 0; bit < 8; bit++)
            {
                *fpsBuffer++ = (fontSlice & 1) ? fpsfgcolor : fpsbgcolor;
                fontSlice >>= 1;
            }
        }
    }
}

void __not_in_flash_func(emulate)()
{
    bool firstLoop = true;
    int old_screen_width = 0;
    int old_screen_height = 0;
    char tbuf[32];

    /* Perf diagnostics, printed once per second while the SELECT+DOWN
       debug toggle is on: core work time per frame (emulation vs whole
       pre-pacing loop), sound FIFO high-water/drops, DI queue min/max and
       underruns. The DI level is sampled every frame and reported as a
       range — a single instantaneous read once per second lands at a
       random point on the sawtooth and says nothing. */
    uint32_t dbgEmuSum = 0, dbgEmuMax = 0, dbgTotSum = 0, dbgTotMax = 0, dbgFrames = 0;
    uint64_t dbgLastPrint = time_us_64();
#if HSTX
    uint32_t dbgUnderrunBase = hstx_di_queue_get_underrun_count();
    dbgDiMin = UINT32_MAX;
    dbgDiMax = 0;
#endif

    while (!reset)
    {
        uint64_t t_frame0 = time_us_64();
        int is_pal = gwenesis_frame_get_config();
        gwsnd_set_pal(is_pal);

        /* Pace off the FPS limiter's own deadline so sub-frame chunks and
           the end-of-frame wait share one timebase; on the first frame (and
           after a catch-up jump) fall back to now, which makes every chunk
           deadline already past and the pacing a no-op for that frame. */
        frame_period_us = is_pal ? 20000 : 16667;
        frame_start_time = (next_frame_time > frame_period_us)
                               ? next_frame_time - frame_period_us
                               : t_frame0;
        if (firstLoop || old_screen_height != screen_height || old_screen_width != screen_width)
        {
            printf("Uptime %s, is_pal %d, screen_width: %d, screen_height: %d, audio_enabled: %d, frameskip: %d\n",
                   Frens::ms_to_d_hhmmss(Frens::time_ms(), tbuf, sizeof tbuf), is_pal, screen_width, screen_height,
                   settings.flags.audioEnabled, settings.flags.frameSkip);
            firstLoop = false;
            old_screen_height = screen_height;
            old_screen_width = screen_width;
        }

        /* Vertical centering: the framebuffer is 320x240, NTSC games are
           224 lines. The core renders directly into the framebuffer. */
        int margin = (SCREENHEIGHT - screen_height) / 2;
        if (margin < 0)
        {
            margin = 0;
        }
        gwenesis_vdp_set_buffer(framebufferLine(margin));

        /* Frameskip: render every frame unless enabled (then skip 1 of 3). */
        drawFrame = !settings.flags.frameSkip || (frame % 3 != 0);

        selectAudioSink();

        uint64_t t_emu0 = time_us_64();
        gwenesis_frame_run(drawFrame);
        uint32_t emu_us = (uint32_t)(time_us_64() - t_emu0);

        if (drawFrame && settings.flags.displayFrameRate)
        {
            drawFpsOverlay();
        }

#if HSTX
        /* Final sweep of the core1->core0 sample bridge (I2S / VU); the
           bulk is drained incrementally per scanline by gwsnd_line_tick. */
        gwsnd_bridge_drain();
#endif

        ProcessAfterFrameIsRendered();
        frame++;
        rapidFireCounter++;

        /* Heap-corruption watch: report the first frame in which any
           emulator buffer's guard word is clobbered, then stop checking
           so the log stays readable. */
        static bool guardReported = false;
        if (!guardReported && check_emulator_mem("in-game") > 0)
        {
            printf("  (frame %d, scan_line %d, screen %dx%d)\n", frame, scan_line,
                   screen_width, screen_height);
            guardReported = true;
        }

        uint32_t tot_us = (uint32_t)(time_us_64() - t_frame0);
        dbgEmuSum += emu_us;
        dbgTotSum += tot_us;
        if (emu_us > dbgEmuMax)
            dbgEmuMax = emu_us;
        if (tot_us > dbgTotMax)
            dbgTotMax = tot_us;
        dbgFrames++;
#if HSTX
        /* Covers the inter-frame gap; paceScanline covers within a frame. */
        noteDiLevel();
#endif
#if EXT_AUDIO_IS_ENABLED
        if (i2sActive)
        {
            publishI2sFill();
        }
#endif
        if (toggleDebugFPS && (time_us_64() - dbgLastPrint) >= 1000000)
        {
            printf("perf: frames=%u emu avg=%u max=%u us, loop avg=%u max=%u us",
                   dbgFrames, dbgEmuSum / dbgFrames, dbgEmuMax,
                   dbgTotSum / dbgFrames, dbgTotMax);
#if GWSND_OFFLOAD
            printf(", fifo_hw=%u drops=%u", gwsnd_stats_fifo_highwater(),
                   gwsnd_stats_drops());
#endif
#if HSTX
            uint32_t underruns = hstx_di_queue_get_underrun_count();
            printf(", di=%u/%u underruns=%u resync=%d",
                   (unsigned)(dbgDiMin == UINT32_MAX ? 0 : dbgDiMin), (unsigned)dbgDiMax,
                   (unsigned)(underruns - dbgUnderrunBase), get_video_output_resync_count());
            dbgUnderrunBase = underruns;
#endif
#if EXT_AUDIO_IS_ENABLED
            if (i2sActive)
            {
                /* i2s_in is samples/s reaching the ring: ~44100 means the
                   loss is downstream (DAC too fast), well under means we
                   are not producing/delivering them in the first place. */
                printf(", i2s=%u%% i2s_in=%u", (unsigned)(i2sFillPermille / 10),
                       (unsigned)dbgI2sSamples);
            }
            dbgI2sSamples = 0;
#endif
#if GWSND_OFFLOAD
            printf(" bridge_drops=%u", gwsnd_stats_bridge_drops());
#endif
            printf("\n");
            dbgEmuSum = dbgEmuMax = dbgTotSum = dbgTotMax = dbgFrames = 0;
#if HSTX
            dbgDiMin = UINT32_MAX;
            dbgDiMax = 0;
#endif
            dbgLastPrint = time_us_64();
        }
        else if (!toggleDebugFPS && dbgFrames >= 600)
        {
            /* keep the accumulators fresh so enabling the toggle shows
               recent numbers, not an average since game start */
            dbgEmuSum = dbgEmuMax = dbgTotSum = dbgTotMax = dbgFrames = 0;
#if HSTX
            dbgDiMin = UINT32_MAX;
            dbgDiMax = 0;
            dbgUnderrunBase = hstx_di_queue_get_underrun_count();
#endif
            dbgLastPrint = time_us_64();
        }

        if (limit_fps)
        {
            // Same period the sub-frame pacer divides up, so the last
            // chunk deadline and this one cannot disagree.
            const uint64_t frame_period = frame_period_us; // 50Hz or 60Hz
            const uint64_t now = time_us_64();

            // Initialize first deadline
            if (next_frame_time == 0)
                next_frame_time = now + frame_period;

            // Wait if ahead of schedule
            if (now < next_frame_time)
            {
                uint64_t remaining = next_frame_time - now;
                if (remaining > 150)
                    sleep_us(remaining - 150);
                while (time_us_64() < next_frame_time)
                {
                    tight_loop_contents();
                }
                // Advance by exactly one frame period
                next_frame_time += frame_period;
            }
            else
            {
                // Late: advance until the deadline is in the future (catch up without drifting)
                do
                {
                    next_frame_time += frame_period;
                } while (now >= next_frame_time);
            }
        }

        // calculate framerate
        if (settings.flags.displayFrameRate)
        {
            fpsFrameCount++;
            uint64_t tick_us = Frens::time_us() - start_tick_us;
            if (tick_us > 1000000)
            {
                fps = fpsFrameCount;
                start_tick_us = Frens::time_us();
                fpsFrameCount = 0;
            }
            int nchars = 0;
            fpsString[nchars++] = '0' + (fps / 10) % 10;
            fpsString[nchars++] = '0' + (fps % 10);

#if HSTX
            // Append the HSTX auto-resync count so display glitches are visible.
            fpsString[nchars++] = ' ';
            fpsString[nchars++] = 'R';
            int resync = get_video_output_resync_count();
            char digits[10];
            int nd = 0;
            do
            {
                digits[nd++] = '0' + (resync % 10);
                resync /= 10;
            } while (resync > 0 && nd < (int)sizeof(digits));
            while (nd > 0 && nchars < (int)sizeof(fpsString))
            {
                fpsString[nchars++] = digits[--nd];
            }
#endif
            fpsStringLen = nchars;
        }
    }
}

/* Size of the currently selected ROM file (the core needs it to build its
   address mask; pico_shared only exports the data pointer). */
static size_t getSelectedRomSize(const char *path)
{
    FILINFO fno;
    if (f_stat(path, &fno) == FR_OK)
    {
        return (size_t)fno.fsize;
    }
    printf("f_stat(%s) failed, assuming 4MB rom\n", path);
    return 4 * 1024 * 1024;
}

/* Reject anything that is not a plausible Mega Drive image before handing it
   to the core. The menu only filters on extension (".md .bin") and both are
   generic enough to match unrelated files, so without this a wrong pick sends
   the 68000 off to execute junk from a garbage reset vector.

   Reads go through the image as the loader left it: every 16-bit word is
   byte-swapped for the core's little-endian fetches, so the byte at file
   offset N lives at [N ^ 1]. */
static bool isValidGenesisRom(uintptr_t addr, size_t size, char *err, size_t errSize)
{
    /* flashromtoPsram() returns nullptr on a read error or when the file does
       not fit in PSRAM, and the menu has no way to report that back — catch it
       here instead of letting the core fetch from address 0. */
    if (addr == 0)
    {
        snprintf(err, errSize, "ROM could not be loaded");
        return false;
    }
    /* Vector table (0x000..0x0FF) plus header (0x100..0x1FF): below that there
       is nothing to run, and set_region() would read past the allocation. */
    if (size < 0x200)
    {
        snprintf(err, errSize, "Not a Genesis ROM (too small)");
        return false;
    }
    /* 68000 fetches are 16-bit and every cart image is word-sized. An odd size
       also means the loader's byte-swap had a byte with no pair, so refuse
       rather than run on a possibly damaged heap. */
    if (size & 1)
    {
        snprintf(err, errSize, "Not a Genesis ROM (odd size)");
        return false;
    }

    const unsigned char *rom = (const unsigned char *)addr;

    /* Console name at 0x100: "SEGA MEGA DRIVE ", "SEGA GENESIS    ", ... */
    if (rom[0x100 ^ 1] == 'S' && rom[0x101 ^ 1] == 'E' &&
        rom[0x102 ^ 1] == 'G' && rom[0x103 ^ 1] == 'A')
    {
        return true;
    }

    /* Hacks and homebrew sometimes wipe the console name, so fall back on the
       vector table: the initial PC (big-endian longword at 0x004) must be an
       even address pointing at cartridge space past the header. */
    uint32_t pc = ((uint32_t)rom[0x04 ^ 1] << 24) | ((uint32_t)rom[0x05 ^ 1] << 16) |
                  ((uint32_t)rom[0x06 ^ 1] << 8) | (uint32_t)rom[0x07 ^ 1];
    if ((pc & 1) == 0 && pc >= 0x200 && pc < size)
    {
        printf("No SEGA header, but reset vector 0x%06x is sane: accepting\n", (unsigned)pc);
        return true;
    }

    snprintf(err, errSize, "Not a Genesis ROM");
    return false;
}

/// @brief
/// Start emulator.
/// @return
int main()
{
#if !defined(PICO_RP2350)
#error "This code is for RP2350 only"
#endif
    char selectedRom[FF_MAX_LFN];
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;
    // This emulator is always overclocked at 378 Mhz or higher
    Frens::setOverclockLimits(CPUFreqKHz,  CPUFreqKHz, VOLTAGE, VOLTAGE);
    Frens::setClocksAndStartStdio(CPUFreqKHz, VOLTAGE);

    printf("==========================================================================================\n");
    printf("Pico-Genesis+ %s\n", SWVERSION);
    printf("Build date: %s\n", __DATE__);
    printf("Build time: %s\n", __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
#if HSTX
    printf("HSTX freq: %d kHz\n", clock_get_hz(clk_hstx) / 1000);
#endif
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");
    printf("Starting up...\n");
    FrensSettings::initSettings(FrensSettings::emulators::GENESIS);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, MARGINTOP, MARGINBOTTOM, AUDIOBUFFERSIZE, true, true);
#if !HSTX
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);
#endif
    bool showSplash = true;
    g_settings_visibility = g_settings_visibility_md;
    g_available_screen_modes = g_available_screen_modes_md;
    gwsnd_set_fill_query(sinkFillPermille);
    while (true)
    {
        if (strlen(selectedRom) == 0 || reset == true)
        {
            menu("Pico-Genesis+", ErrorMessage, isFatalError, showSplash, ".md .bin", selectedRom);
        }
#if !HSTX
        if (settings.screenMode != ScreenMode::SCANLINE_1_1 && settings.screenMode != ScreenMode::NOSCANLINE_1_1)
        {
            settings.screenMode = ScreenMode::SCANLINE_1_1;
            FrensSettings::savesettings();
        }
        scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);
#endif

        audio_enabled = settings.flags.audioEnabled;
        size_t romSize = getSelectedRomSize(selectedRom);

        do
        {
            abSwapped = 0; // don't swap A and B buttons
            reset = resetGame = false;
            next_frame_time = 0; // Reset next frame time for FPS limiter
            if (!isValidGenesisRom(ROM_FILE_ADDR, romSize, ErrorMessage, ERRORMESSAGESIZE))
            {
                printf("%s: %s\n", ErrorMessage, selectedRom);
                reset = true;
                break;
            }
            printf("Starting game (%d KB rom) rom@%p\n", (int)(romSize / 1024),
                   (void *)ROM_FILE_ADDR);
            /* Must precede init_emulator_mem(), which allocates the buffer
               this sizes. */
            gwsram_detect((const unsigned char *)ROM_FILE_ADDR, romSize);
            if (!init_emulator_mem())
            {
                snprintf(ErrorMessage, 40, "Out of memory starting game");
                printf("%s\n", ErrorMessage);
                reset = true;
                break;
            }
            load_cartridge((const unsigned char *)ROM_FILE_ADDR, romSize);
            power_on();
            reset_emulation();
            loadCartSram();
            Frens::dumpHeapStats("game start"); /* peak usage, both heaps */
            startAudioSinks();                  /* must precede gwsnd_init */
            gwsnd_init(0 /* pal detected per frame */, HSTX);
            emulate();
            /* Covers both leaving the game and resetting it: the loop below
               re-enters and reloads the file. */
            saveCartSram();
            gwsnd_shutdown();
            free_emulator_mem();
        } while (resetGame);

        /* Release the ROM before returning to the menu. Holding it while
           the menu allocates (RomLister, artwork) leaves those blocks
           sitting above it in PSRAM, so freeing it later — inside
           loadRomInPsRam, immediately before allocating the next one —
           can leave no contiguous room for a larger ROM and f_malloc
           panics. Freeing here keeps the arena defragmented across games.
           Only valid with PSRAM: without it ROM_FILE_ADDR points into XIP
           flash, which must never be passed to free(). */
        if (Frens::isPsramEnabled() && ROM_FILE_ADDR)
        {
            Frens::f_free((void *)ROM_FILE_ADDR);
            ROM_FILE_ADDR = 0;
        }
        selectedRom[0] = 0;
        showSplash = false;
    }

    return 0;
}
