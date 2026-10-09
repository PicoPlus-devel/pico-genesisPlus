#include <stdio.h>
#include <string.h>
#include <malloc.h> /* printSramHeadroom */
#include <unistd.h> /* sbrk; before the core headers, which #define uint */
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
#if HSTX
#include "romflash.h"
#include "progress_bar.h"
#endif
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
#include "gwmapper.h"
#include "scd.h"
#include "gwpier.h"
}
#if GENESIS_SEGACD
#include <malloc.h>
#include "pico/mutex.h"
#endif

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

extern "C" unsigned short button_state[3];
static void updateButtonState();
static void applyPadType();
#if GENESIS_SEGACD
static void saveCdBram();
#endif

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
/* Optional overclock, as in pico_snesPlus: the "Run CPU at high clock"
 * setting (Fruit Jam and Pico Plus 2 only) reboots into 504 MHz. 1.70 V is
 * what 504 MHz needs to stay stable; 1.60-1.65 V runs but may hard-fault in
 * heavy scenes. 504 / 4 is the same 126 MHz HSTX pixel clock, and
 * pico_shared slows the flash interface to match. Off by default. */
#define EMULATOR_MAX_CLOCKFREQ_KHZ 504000
#define MAX_VOLTAGE VREG_VOLTAGE_1_70
#define OVERCLOCK_SETTING (HW_CONFIG == 2 || HW_CONFIG == 8)
#endif
#ifndef OVERCLOCK_SETTING
#define OVERCLOCK_SETTING 0
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
    [MOPT_OVERCLOCK]                 = OVERCLOCK_SETTING, // 504 MHz, see EMULATOR_MAX_CLOCKFREQ_KHZ
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
    [MOPT_GENESIS_PAD]               = 1,  // 3 or 6 button pad
    [MOPT_NES_PALETTE]               = 0,  // NES only
    [MOPT_HSTX_CLOCK_FIX]            = HSTX && !CFG_TUH_RPI_PIO_USB, // Video Clock Fix; PIO-USB builds always have it (GENESIS_OVERCLOCK_HSTX_FIX)
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

#if GENESIS_SEGACD
/* Pier Solar keeps its saves in a 64 KB SPI EEPROM (port/gwpier.h), not in
   save RAM. Same .srm name, raw contents, the file PicoDrive writes. */
static void loadPierEeprom()
{
    UINT got = 0;

    buildSrmPath();
    FRESULT fr = f_open(&srmFile, srmPath, FA_READ);
    if (fr == FR_OK)
    {
        fr = f_read(&srmFile, gwpier_eeprom, GWPIER_EEPROM_SIZE, &got);
        f_close(&srmFile);
    }
    if (fr == FR_OK && got)
        printf("Cartridge EEPROM restored from %s\n", srmPath);
    else
        printf("No save file %s, cartridge EEPROM starts empty\n", srmPath);
    gwpier_eeprom_dirty = 0;
}

static void savePierEeprom()
{
    UINT put = 0;

    if (!gwpier_eeprom_dirty)
        return;
    buildSrmPath();
    f_mkdir(GAMESAVEDIR);
    FRESULT fr = f_open(&srmFile, srmPath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK)
    {
        fr = f_write(&srmFile, gwpier_eeprom, GWPIER_EEPROM_SIZE, &put);
        FRESULT closed = f_close(&srmFile);
        if (fr == FR_OK)
            fr = closed;
    }
    if (fr != FR_OK || put != GWPIER_EEPROM_SIZE)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Error writing save: %d", fr);
        printf("%s (%s)\n", ErrorMessage, srmPath);
        return; /* stays dirty: the next attempt tries again */
    }
    printf("Cartridge EEPROM saved to %s\n", srmPath);
    gwpier_eeprom_dirty = 0;
}
#endif

static void loadCartSram()
{
#if GENESIS_SEGACD
    if (gwpier_active)
    {
        loadPierEeprom();
        return;
    }
#endif
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
#if GENESIS_SEGACD
    if (gwpier_active)
    {
        savePierEeprom();
        return;
    }
#endif
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
#if GENESIS_SEGACD
    /* Keep core1's CD-DA prefetch off the card while this touches it (see
       gwcd_port_sd_lock). */
    const bool cdLock = gwcd_bus_mode != GWCD_BUS_CART;
    if (cdLock)
        gwcd_port_sd_lock();
#endif
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
    updateButtonState();
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
#if GENESIS_SEGACD
        saveCdBram();
#endif
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
        applyPadType(); // the Genesis pad setting may have changed
        // Reset next frame time for FPS limiter
        next_frame_time = 0;
    }
#if GENESIS_SEGACD
    if (cdLock)
        gwcd_port_sd_unlock();
#endif
    return count;
}

static DWORD prevButtons[2]{};

static int rapidFireMask[2]{};
static int rapidFireCounter = 0;

/* Button bits on the way from the pads to the core. The low byte uses the
   menu's layout for SELECT, START and the d-pad, so the GPIO pads' word can be
   masked straight into it; A B C X Y Z are the Genesis buttons. */
static constexpr int LEFT = 1 << 6;
static constexpr int RIGHT = 1 << 7;
static constexpr int UP = 1 << 4;
static constexpr int DOWN = 1 << 5;
static constexpr int SELECT = 1 << 2;
static constexpr int START = 1 << 3;
static constexpr int A = 1 << 0;
static constexpr int B = 1 << 1;
static constexpr int C = 1 << 8;
static constexpr int X = 1 << 9;
static constexpr int Y = 1 << 10;
static constexpr int Z = 1 << 11;
/* Button1 in the README (the menu's "back" button). It never reaches the core:
   it keeps START + Button1 on the same physical button on every pad, whichever
   Genesis button that is in a game. */
static constexpr int HOT1 = 1 << 12;

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

/* SELECT doubles as Genesis A for NES pads, which have only B and A for
   Genesis B and C. That covers every pad on the GPIO port, since a NES pad
   that does not identify itself is read as a SNES one (see nesPadButtons());
   on a real SNES pad it only repeats SNES A. The menu is unaffected, it reads
   the pads through its own code and keeps SELECT for itself.

   The SELECT bit is deliberately left in place, so every in-game SELECT+...
   hotkey keeps working. The button is only withheld while START is held down,
   so SELECT+START opens the settings menu without pressing it on the way out. */
static inline int selectDoublesAs(int bits, int button)
{
    return ((bits & SELECT) && !(bits & START)) ? (bits | button) : bits;
}

/* In-game layout. Genesis pads are used as they are. Every other pad maps by
   position: SNES A Y B play Genesis A B C, which puts Genesis B and C, the
   usual attack and jump buttons, on SNES Y and B (#40). L X R play X Y Z. The
   other pads follow from which of their buttons sit where SNES A Y B X L R do
   (XInput B X A Y LB RB, PlayStation Circle Square Cross Triangle L1 R1). NES
   pads have only B and A for Genesis B and C, with SELECT as A. */

/* Wii Classic and SNES Classic pads, wiipad_read() layout: bit0=A 1=B
   2=Select 3=Start 4-7=d-pad 8=X 9=Y 10=L 11=R. */
static inline int mapWiipadButtons(uint16_t w)
{
    int v = w & (SELECT | START | UP | DOWN | LEFT | RIGHT);
    if (w & (1u << 0))
        v |= A; // A
    if (w & (1u << 9))
        v |= B; // Y
    if (w & (1u << 1))
        v |= C | HOT1; // B
    if (w & (1u << 10))
        v |= X; // L
    if (w & (1u << 8))
        v |= Y; // X
    if (w & (1u << 11))
        v |= Z; // R
    return v;
}

static inline bool isGenesisPad(const char *name)
{
    return name && (strncmp(name, "Genesis", 7) == 0 || strcmp(name, "MDArcade") == 0);
}

/* One USB pad or keyboard. hid_app.cpp reports the bottom face button as A,
   the right one as B, the top one as X and the left one as Y; Genesis pads
   report their own names. */
static int mapUsbButtons(const io::GamePadState &gp)
{
    using Btn = io::GamePadState::Button;
    const uint32_t b = gp.buttons;
    const char *name = gp.GamePadName;
    int v = (b & Btn::LEFT ? LEFT : 0) |
            (b & Btn::RIGHT ? RIGHT : 0) |
            (b & Btn::UP ? UP : 0) |
            (b & Btn::DOWN ? DOWN : 0) |
            (b & Btn::SELECT ? SELECT : 0) |
            (b & Btn::START ? START : 0) |
            (b & Btn::A ? HOT1 : 0) |
            // Only Genesis pads, keyboard E and generic HID joysticks set these.
            (b & Btn::C ? C : 0) |
            (b & Btn::Z ? Z : 0);
    if (isGenesisPad(name))
    {
        v |= (b & Btn::A ? A : 0) | (b & Btn::B ? B : 0) |
             (b & Btn::X ? X : 0) | (b & Btn::Y ? Y : 0);
    }
    else if (name && strcmp(name, "Keyboard") == 0)
    {
        // Z X C = A B C and Q W E = X Y Z: keyboard rows, not pad positions.
        v |= (b & Btn::A ? A : 0) | (b & Btn::B ? B : 0) | (b & Btn::X ? C : 0) |
             (b & Btn::L ? X : 0) | (b & Btn::R ? Y : 0);
    }
    else if (name && strcmp(name, "Manta NES") == 0)
    {
        // AliExpress NES pad: A is NES B, B is NES A.
        v |= (b & Btn::A ? B : 0) | (b & Btn::B ? C : 0);
        v = selectDoublesAs(v, A);
    }
    else
    {
        v |= (b & Btn::B ? A : 0) | (b & Btn::Y ? B : 0) | (b & Btn::A ? C : 0) |
             (b & Btn::L ? X : 0) | (b & Btn::X ? Y : 0) | (b & Btn::R ? Z : 0);
    }
    return v;
}

#if NES_PIN_CLK != -1
/* One GPIO pad.

   The pad shifts its buttons out in the order bit0=B 1=Y 2=Select 3=Start
   4=Up 5=Down 6=Left 7=Right 8=A 9=X 10=L 11=R on a SNES pad, and bit0=A 1=B
   then the same Select/Start/d-pad on a NES one. Bits 2-7 therefore mean the
   same thing on both and already sit on the constants above, so they pass
   straight through as a mask.

   Bits 0 and 1 swap meaning, but the layout makes that harmless: bit1 (NES B,
   SNES Y) plays Genesis B and bit0 (NES A, SNES B) plays C on either pad, and
   SNES A X L R sit on bits 8-11, which a NES pad never sets. So every NES pad
   plays SELECT B A as Genesis A B C, including an aftermarket one that leaves
   the shift register's unused outputs floating and so cannot be told from a
   SNES pad (#28, #34).

   Only Button1 depends on which pad is on the wire: it is whatever the menu
   treats as "back", B on a proven SNES pad and bit1 (NES B, or SNES Y on an
   idle SNES pad) until then. */
static inline int nesPadButtons(int pad)
{
    const uint16_t ext = nespad_states_ext[pad];
    const bool snes = nespad_padtype[pad] == NESPAD_TYPE_SNES;
    int v = ext & (SELECT | START | UP | DOWN | LEFT | RIGHT); // same bits on both pads
    if (ext & (1u << 1))
        v |= B | (snes ? 0 : HOT1); // NES B, SNES Y
    if (ext & (1u << 0))
        v |= C | (snes ? HOT1 : 0); // NES A, SNES B
    if (ext & (1u << 8))
        v |= A; // SNES A
    if (ext & (1u << 10))
        v |= X; // SNES L
    if (ext & (1u << 9))
        v |= Y; // SNES X
    if (ext & (1u << 11))
        v |= Z; // SNES R
    return selectDoublesAs(v, A);
}
#endif

/* Refresh button_state[] (active low, M X Y Z S A C B R L D U) and run the
   in-game hotkeys. Called once per frame from ProcessAfterFrameIsRendered(),
   right after the pads have been polled: none of the sources changes anywhere
   else, so doing this on every pad read, as the core's callback would, only
   costs time - and a 6-button game reads the port several times per frame.
   It also keeps the hotkeys working while a game is not reading the pad. */
static void updateButtonState()
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
        int v = mapUsbButtons(gp);

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
            if (pushed & HOT1)
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
        // Mode (PAD_M) is never pressed: the pads' MODE and SELECT buttons are
        // the hotkey button, and hardly any game reads Mode.
        button_state[i] = (unsigned short)~(((v & LEFT) ? 1 << PAD_LEFT : 0) |
                                            ((v & RIGHT) ? 1 << PAD_RIGHT : 0) |
                                            ((v & UP) ? 1 << PAD_UP : 0) |
                                            ((v & DOWN) ? 1 << PAD_DOWN : 0) |
                                            ((v & START) ? 1 << PAD_S : 0) |
                                            ((v & A) ? 1 << PAD_A : 0) |
                                            ((v & B) ? 1 << PAD_B : 0) |
                                            ((v & C) ? 1 << PAD_C : 0) |
                                            ((v & X) ? 1 << PAD_X : 0) |
                                            ((v & Y) ? 1 << PAD_Y : 0) |
                                            ((v & Z) ? 1 << PAD_Z : 0));
    }
}

/* Core callback, run on every pad read. button_state[] is already current:
   updateButtonState() refreshes it once per frame. */
extern "C" void gwenesis_io_get_buttons()
{
}

/* Cartridge header, I/O support field ($190-$19F): '6' means the game knows
   the 6-button pad. The rom sits byte-swapped for the core's 16-bit fetches
   (see hdr8() in port/gwsram.c), hence the ^ 1. */
static bool romListsSixButtonPad(const unsigned char *rom)
{
    for (uint32_t off = 0x190; off < 0x1A0; off++)
    {
        if (rom[off ^ 1] == '6')
        {
            return true;
        }
    }
    return false;
}

static bool romSixButton = false;

/* Present 3- or 6-button pads per settings.flags.genesisPad: 0 = Auto (the
   cartridge header decides), 1 = 3 button, 2 = 6 button. 3 buttons is the safe
   choice for the rest: some 3-button games misread a 6-button pad, as they do
   on real hardware. */
static void applyPadType()
{
    bool six = settings.flags.genesisPad == 2 || (settings.flags.genesisPad == 0 && romSixButton);
    gwenesis_io_set_six_button(0, six);
    gwenesis_io_set_six_button(1, six);
    printf("Pads: %d button (setting %d, header %s)\n", six ? 6 : 3,
           settings.flags.genesisPad, romSixButton ? "lists 6 button" : "3 button");
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

#if GENESIS_SEGACD
/* The Sega CD part of the SELECT+DOWN perf line: where its time went since
   the last call -- the sub CPU executing, skipped in wait loops, or halted,
   and the main CPU skipped in wait loops. frames 0 only resets the counts.
   Kept out of emulate(), which runs from SRAM. */
static void __attribute__((noinline)) printCdStats(uint32_t frames, int is_pal)
{
    unsigned int sr, si, sh, mi;

    gwcd_stats_take(&sr, &si, &sh, &mi);
    if (!frames || !(gwcd_bus_mode & GWCD_BUS_SCD))
        return;
    uint64_t st = (uint64_t)sr + si + sh;
    uint64_t mt = (uint64_t)frames * (is_pal ? 313u : 262u) * 3420u;
    printf(", cd: sub run %u%% idle %u%% halted %u%%, main idle %u%%",
           (unsigned)(st ? sr * 100ull / st : 0), (unsigned)(st ? si * 100ull / st : 0),
           (unsigned)(st ? sh * 100ull / st : 0), (unsigned)(mi * 100ull / mt));
}
#endif

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
#if GENESIS_SEGACD
            printCdStats(dbgFrames, is_pal);
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
#if GENESIS_SEGACD
            printCdStats(0, is_pal);
#endif
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

/* ------------------------------------------------------------------ */
/* ROMs too large for PSRAM (romflash.h).                               */
/* ------------------------------------------------------------------ */

/* PSRAM part of a ROM split between flash and PSRAM, or null, and how much of
   the ROM the flash part holds. */
static uint8_t *romTail = nullptr;
static size_t romHeadLen = 0;

/* HSTX boards with PSRAM only. The flash write takes XIP away for hundreds of
   milliseconds at a time; HSTX scan-out on core1 is SRAM resident and rides
   that out, PicoDVI's (pico_lib/dvi) runs from flash and does not. */
#if HSTX

/* A ROM-to-flash write ends in a reboot: an erase holds interrupts off for
   hundreds of milliseconds at a time, which a PIO USB host does not survive.
   This marker in watchdog scratch[5] picks the cart straight back up after
   it, so the user still only chose it once. [4] holds the SDK's
   watchdog_enable magic, [6]/[7] are the bootloader handshake
   (FrensHelpers.cpp). The reboot is a watchdog_enable(), not a
   watchdog_reboot(): pico-bootLoader jumps straight back into the resident
   application only after a watchdog_enable reboot, and shows its menu after
   any other. The other effect of that magic, initAll() flashing the rom
   named in ROMINFOFILE, needs a board without PSRAM, and this path needs
   PSRAM. */
static constexpr int GEN_RESUME_SCRATCH = 5;
static constexpr uint32_t GEN_RESUME_MAGIC = 0x6E5F1A54u;

/* Decimal conversion for the flash status line, without pulling printf into
   a callback that runs between flash operations. */
static int u32ToDec(char *out, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do
    {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* Progress bar during a ROM-to-flash write. Colours are RGB555 literals so
   nothing is read from a palette in flash. The bar is built with
   PROGRESS_BAR_IN_SRAM=0: romflash.cpp puts QMI M0 back to a working timing
   inside every erase/program, so these calls always run against healthy
   flash, and SRAM is better spent on the emulator. */
#define PB_COL_BORDER 0x0000u /* black */
#define PB_COL_EMPTY 0x7FFFu  /* white */
#define PB_COL_FILL 0x03E0u   /* green */

static void romflashProgress(int phase, uint32_t done, uint32_t total)
{
    /* Erase is the long pole, so give it most of the bar. */
    uint32_t pct = total == 0 ? 0
                   : (phase == ROMFLASH_ERASE)
                       ? (uint32_t)((uint64_t)done * 60u / total)
                       : 60u + (uint32_t)((uint64_t)done * 40u / total);

    /* The write phase fires once per bounce buffer; redraw only on a move. */
    static uint32_t last = 0xFFFFFFFFu;
    if (pct == last && done != total)
        return;
    last = pct;

    char st[32];
    const char *what = (phase == ROMFLASH_ERASE) ? "Erasing " : "Writing ";
    int n = 0;
    while (what[n] && n < 12)
    {
        st[n] = what[n];
        n++;
    }
    n += u32ToDec(st + n, done / 1024u);
    st[n++] = '/';
    n += u32ToDec(st + n, total / 1024u);
    st[n++] = ' ';
    st[n++] = 'K';
    st[n++] = 'B';
    st[n] = 0;
    progress_bar_draw_status(st, PB_COL_EMPTY, PB_COL_BORDER);
    progress_bar_draw(pct, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
}

/* The menu hands back ROM_FILE_ADDR == 0 for a file too large to preload into
   PSRAM. Run such a cart from the flash region instead, with whatever does not
   fit there read into PSRAM. Writes the flash part first when the region does
   not already hold this exact file, then reboots (see GEN_RESUME_SCRATCH).
   Returns the image base, or 0 to go back to the menu -- with ErrorMessage set
   unless the user simply declined the write. */
static uintptr_t prepareOversizeRom(const char *path, size_t romSize, bool resumed)
{
    /* Anything the preload left there ("Cannot allocate ...") is not an error
       here: running this cart from flash is the plan. */
    ErrorMessage[0] = 0;

    if (romflash_capacity() == 0)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "ROM too large");
        return 0;
    }
    size_t headLen = romflash_head_len(romSize);

    /* On the launch we rebooted into, the record is proof of a write that was
       verified moments ago: re-checking would only cost time, and under
       ROMFLASH_FORCE_REWRITE it would rewrite and reboot forever. */
    if (!resumed && !romflash_holds(path, romSize))
    {
        const char *shortName = Frens::GetfileNameFromFullPath((char *)path);
        char sizeLine[40];
        snprintf(sizeLine, sizeof(sizeLine), "%u KB - takes about a minute",
                 (unsigned)(headLen / 1024));
        if (!menuConfirmPrompt("This game is too big for RAM and",
                               "must be written to flash first.", sizeLine))
        {
            printf("romflash: user declined the write\n");
            return 0;
        }
        menuNoticeScreen("Writing to flash memory", shortName,
                         "Do not power off.", "The console restarts when done.");
        progress_bar_draw(0, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
        if (!romflash_program(path, romSize, romflashProgress))
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Flash write failed");
            return 0;
        }
        printf("romflash: rebooting to restore USB, then resuming\n");
        watchdog_hw->scratch[GEN_RESUME_SCRATCH] = GEN_RESUME_MAGIC;
        watchdog_enable(1, 1);
        while (true)
            tight_loop_contents();
    }

    const unsigned char *head = romflash_image();
    if (headLen < romSize)
    {
        romTail = romflash_load_tail(path, romSize, headLen);
        if (!romTail)
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Not enough PSRAM for ROM");
            return 0;
        }
        romHeadLen = headLen;
    }
    return (uintptr_t)head;
}

/* On a PSRAM board the rom browser lists only files that fit in free PSRAM.
   With the flash region available, a ROM can be as large as the region's
   whole banks plus a PSRAM tail (keeping the preload's 512 KB margin), so tell
   the browser to list those too (maxOversizeRomSize, pico_shared). maxRomSize
   only feeds the menu's status line on these boards; raise it to match. */
static void raiseMaxRomSizeForFlash()
{
    if (!Frens::isPsramEnabled() || romflash_capacity() == 0)
        return;
    const size_t margin = 512 * 1024;
    size_t avail = Frens::GetAvailableMemory();
    size_t limit = romflash_head_len((size_t)-1) + (avail > margin ? avail - margin : 0);
    if (limit < romflash_capacity())
        limit = romflash_capacity();
    printf("romflash: ROMs up to %u KB can run (flash region + %u KB PSRAM free)\n",
           (unsigned)(limit / 1024), (unsigned)(avail / 1024));
    maxOversizeRomSize = (int)limit;
    if (limit > (size_t)maxRomSize)
        maxRomSize = (int)limit;
}
#endif /* HSTX */

extern char __StackLimit; /* end of the heap region (linker script) */

/* What the SRAM heap can still hand out: the free space inside the arena plus
   what sbrk has not claimed yet, up to __StackLimit. dumpHeapStats' "largest"
   (keepcost) is only the arena's top free block, so on a PSRAM board, where
   the arena grows on demand, it leaves out everything never claimed. Printed
   once the sound engine is up, i.e. at the game's peak. */
static void printSramHeadroom(const char *tag)
{
    struct mallinfo mi = mallinfo();
    size_t unclaimed = (size_t)(&__StackLimit - (char *)sbrk(0));
    printf("[heap] %-14s SRAM headroom=%uK (free in arena %uK + unclaimed %uK)\n", tag,
           (unsigned)((mi.fordblks + unclaimed) >> 10), (unsigned)(mi.fordblks >> 10),
           (unsigned)(unclaimed >> 10));
}

#if GENESIS_SEGACD
/* ------------------------------------------------------------------ */
/* Sega CD / Mega-CD and MD+ (port/scd.h)                              */
/*                                                                     */
/* A .cue or .chd picked in the rom browser is a Sega CD disc -- or,   */
/* with a Mega Drive rom of the same name next to it, an MD+ game      */
/* whose disc only holds its music. Either way the disc streams from   */
/* the SD card; every CD buffer lives in PSRAM, so boards without      */
/* PSRAM never offer these files.                                      */
/* ------------------------------------------------------------------ */

/* Platform hooks of port/scd.h. */
extern "C" void *gwcd_port_psram_alloc(size_t size)
{
    void *p = gwsram_port_psram_alloc(size); /* non-panicking */
    if (p)
        memset(p, 0, size);
    return p;
}

extern "C" void gwcd_port_psram_free(void *p)
{
    gwsram_port_psram_free(p);
}

/* FatFs is not reentrant, and on HSTX core1 prefetches CD audio while core0
   reads data sectors. In a CD session every FatFs call holds this, core1's
   only with a try (port/scd.h), and core0 holds it across its per-frame work
   (ProcessAfterFrameIsRendered), where the in-game menu and the hotkeys that
   save settings reach the card. Recursive: the backup RAM save nests inside
   that. */
static recursive_mutex_t cdSdMutex;

extern "C" void gwcd_port_sd_lock(void)
{
    recursive_mutex_enter_blocking(&cdSdMutex);
}

extern "C" void gwcd_port_sd_unlock(void)
{
    recursive_mutex_exit(&cdSdMutex);
}

extern "C" int gwcd_port_sd_trylock(void)
{
    return recursive_mutex_try_enter(&cdSdMutex, nullptr) ? 1 : 0;
}

/* libchdr's decoders need 6-10 KB of stack; core0 has 3 KB, core1 4 KB, and
   the SRAM heap cannot spare an 8 KB core1 stack the way pico-pcePlus does
   it. So the call runs on a PSRAM stack: thread mode switches to the process
   stack pointer (CONTROL.SPSEL) set to that memory for the duration of the
   call, while interrupt handlers -- HSTX scan-out on core1 -- keep using the
   main stack in SRAM. Only SPSEL is touched; the other CONTROL bits (FPCA
   in particular) are left as the call leaves them. */
static void __attribute__((naked, noinline)) runOnProcessStack(void *arg, void (*fn)(void *), void *top)
{
    __asm volatile(
        "push {r4, lr}        \n"
        "msr  psp, r2         \n"
        "mrs  r4, control     \n"
        "orr  r4, r4, #2      \n"
        "msr  control, r4     \n"
        "isb                  \n"
        "blx  r1              \n"
        "mrs  r4, control     \n"
        "bic  r4, r4, #2      \n"
        "msr  control, r4     \n"
        "isb                  \n"
        "pop  {r4, pc}        \n");
}

extern "C" void gwcd_port_big_stack_call(void (*fn)(void *), void *arg, void *stack, size_t size)
{
    if (!stack)
    {
        fn(arg);
        return;
    }
    runOnProcessStack(arg, fn, (void *)(((uintptr_t)stack + size) & ~(uintptr_t)7));
}

/* Free space at the top of the SRAM heap, as port/gwsram.c judges it: by
   the time the CD session starts every fixed emulator buffer is taken, so
   the top-most free block is what is left. */
extern "C" size_t gwcd_port_sram_free(void)
{
    struct mallinfo mi = mallinfo();
    return (size_t)mi.keepcost;
}

/* A .cue or .chd; with m3u also an .m3u playlist of a multi-disc game. */
static bool isDiscImage(const char *path, bool m3u = true)
{
    const char *dot = strrchr(path, '.');
    return dot && (strcasecmp(dot, ".cue") == 0 || strcasecmp(dot, ".chd") == 0 ||
                   (m3u && strcasecmp(dot, ".m3u") == 0));
}

/* Multi-disc games: while one runs, the settings menu's disk-swap entry
   (pico_shared's FDS hooks) is "Change disc", listed first. The shared
   visibility table is const, so for the session a copy with the entry
   switched on (in PSRAM: the menu is no hot path) stands in for it. */
static int8_t *cdDiscVisibility;

static int cdDiscIndex() { return gwcd_disc_index(); }
static int cdDiscCount() { return gwcd_disc_count(); }
static void cdDiscChange(int disc) { gwcd_disc_change(disc); }
static void cdDiscEject() {}
static void cdDiscName(int disc, char *buf, int size) { snprintf(buf, size, "Disc %d", disc + 1); }

static const MenuFdsHooks cdDiscHooks = {
    cdDiscIndex, cdDiscCount, cdDiscChange, cdDiscEject, "Change disc", cdDiscName,
};

static void offerDiscChange(bool on)
{
    g_settings_visibility = g_settings_visibility_md;
    menuSetFdsHooks(nullptr);
    gwcd_port_psram_free(cdDiscVisibility);
    cdDiscVisibility = nullptr;
    if (on && (cdDiscVisibility = (int8_t *)gwcd_port_psram_alloc(MOPT_COUNT)) != nullptr)
    {
        memcpy(cdDiscVisibility, g_settings_visibility_md, MOPT_COUNT);
        cdDiscVisibility[MOPT_FDS_DISK_SWAP] = 1;
        g_settings_visibility = cdDiscVisibility;
        menuSetFdsHooks(&cdDiscHooks);
    }
}

/* The console's 8 KB backup RAM: one file per BIOS region, shared by every
   game like the real thing, in the raw layout Genesis Plus GX and PicoDrive
   write (scd_U.brm, scd_E.brm, scd_J.brm), so it moves to and from a PC. */
static char brmPath[40];

static void buildBrmPath()
{
    snprintf(brmPath, sizeof(brmPath), GAMESAVEDIR "/scd_%c.brm",
             gwcd_bios_region() ? gwcd_bios_region() : 'U');
}

/* Both go through srmFile: a CD session has no cartridge RAM of its own. */
static void loadCdBram()
{
    UINT got = 0;

    buildBrmPath();
    gwcd_port_sd_lock();
    FRESULT fr = f_open(&srmFile, brmPath, FA_READ);
    if (fr == FR_OK)
    {
        fr = f_read(&srmFile, gwcd_bram(), (UINT)gwcd_bram_size(), &got);
        f_close(&srmFile);
    }
    gwcd_port_sd_unlock();
    if (fr == FR_OK && got == gwcd_bram_size())
        printf("Backup RAM restored from %s\n", brmPath);
    else
        printf("No backup RAM in %s, starting formatted\n", brmPath);
    gwcd_bram_clean();
}

static void saveCdBram()
{
    UINT put = 0;

    if (!(gwcd_bus_mode & GWCD_BUS_SCD) || !gwcd_bram() || !gwcd_bram_dirty())
        return;
    buildBrmPath();
    gwcd_port_sd_lock();
    f_mkdir(GAMESAVEDIR);
    FRESULT fr = f_open(&srmFile, brmPath, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr == FR_OK)
    {
        fr = f_write(&srmFile, gwcd_bram(), (UINT)gwcd_bram_size(), &put);
        FRESULT closed = f_close(&srmFile);
        if (fr == FR_OK)
            fr = closed;
    }
    gwcd_port_sd_unlock();
    if (fr != FR_OK || put != gwcd_bram_size())
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Error writing backup RAM: %d", fr);
        printf("%s (%s)\n", ErrorMessage, brmPath);
        return; /* stays dirty: the next attempt tries again */
    }
    printf("Backup RAM saved to %s\n", brmPath);
    gwcd_bram_clean();
}

/* After the reboot that follows writing an MD+ cartridge to flash, the flash
   record names the cartridge, not the disc it was picked through. The disc
   is the one in the same folder whose MD+ pairing (gwcd_mdplus_rom_path) is
   this cartridge; the rom browser hides such a cartridge, so the disc is
   the only way it can have been started. */
static bool findPairedDisc(const char *cartPath, char *out, size_t outSize)
{
    const size_t pairSize = FF_MAX_LFN + 8;
    const char *slash = strrchr(cartPath, '/');
    size_t dirLen = slash ? (size_t)(slash - cartPath) : 0;
    DIR *dir = (DIR *)gwcd_port_psram_alloc(sizeof(DIR));
    FILINFO *fno = (FILINFO *)gwcd_port_psram_alloc(sizeof(FILINFO));
    char *pair = (char *)gwcd_port_psram_alloc(pairSize);
    bool found = false;

    if (slash && dir && fno && pair && dirLen + 2 < outSize)
    {
        memcpy(out, cartPath, dirLen);
        out[dirLen] = 0;
        if (f_opendir(dir, dirLen ? out : "/") == FR_OK)
        {
            while (!found && f_readdir(dir, fno) == FR_OK && fno->fname[0])
            {
                if ((fno->fattrib & AM_DIR) || !isDiscImage(fno->fname, false))
                    continue;
                snprintf(out + dirLen, outSize - dirLen, "/%s", fno->fname);
                if (gwcd_disc_open(out) == 0)
                {
                    found = gwcd_mdplus_rom_path(out, pair, pairSize) && strcmp(pair, cartPath) == 0;
                    gwcd_disc_close();
                }
            }
            f_closedir(dir);
        }
    }
    gwcd_port_psram_free(pair);
    gwcd_port_psram_free(fno);
    gwcd_port_psram_free(dir);
    if (found)
        printf("romflash: %s goes with %s\n", cartPath, out);
    return found;
}

/* PSRAM a disc session needs beside its cartridge: the Sega CD's state
   (1.1 MB), the CHD index and cache, the CD-DA ring, the MD+ page and the
   Pier Solar buffers, with room to spare. */
static constexpr size_t CD_SESSION_PSRAM = 3u * 1024 * 1024;

/* One disc game, from the rom browser pick to the return to it.

   Three kinds of session, chosen by what sits next to the disc:
     - no cartridge: a Sega CD game, booted from the disc by the BIOS;
     - a cartridge and a data-only music disc: MD+, the cartridge plays its
       music from the disc through the MegaSD interface;
     - a cartridge and a Sega CD disc: "Mode 1", the cartridge boots with the
       Sega CD attached (Pier Solar's Enhanced Soundtrack Disc), MD+ included.
   `resumed` is set on the launch that follows the reboot after a flash
   write, as for any cartridge too large for PSRAM. */
static void runDiscGame(char *selectedRom, bool resumed)
{
    const size_t pathSize = FF_MAX_LFN + 8;
    char *cartPath = nullptr; /* the cartridge next to the disc (PSRAM) */
    char *biosPath = nullptr; /* (PSRAM) */
    const unsigned char *cart = nullptr;
    uint8_t *cartInPsram = nullptr; /* to free, unless the cart runs from flash */
    size_t cartSize = 0;
    bool mdplus, scd;

    /* The rom browser preloaded the (small) .cue into PSRAM; not needed. */
    if (Frens::isPsramEnabled() && ROM_FILE_ADDR)
    {
        Frens::f_free((void *)ROM_FILE_ADDR);
        ROM_FILE_ADDR = 0;
    }
    if (!Frens::isPsramEnabled())
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Sega CD needs PSRAM");
        return;
    }
    cartPath = (char *)gwcd_port_psram_alloc(pathSize);
    biosPath = (char *)gwcd_port_psram_alloc(pathSize);
    /* A .cue/.chd, or an .m3u: the disc set of a multi-disc game */
    if (!cartPath || !biosPath || gwcd_disc_open_set(selectedRom) != 0)
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Cannot read disc image");
        goto out;
    }

    mdplus = gwcd_mdplus_rom_path(gwcd_disc_path(), cartPath, pathSize);
    if (mdplus)
    {
        printf("MD+: cartridge %s\n", cartPath);
        cartSize = getSelectedRomSize(cartPath);
        if (cartSize + CD_SESSION_PSRAM <= Frens::GetAvailableMemory())
        {
            uint32_t crc = 0;
            char *load = biosPath; /* the loader clears the path on error */
            strcpy(load, cartPath);
            cartInPsram = (uint8_t *)Frens::flashromtoPsram(load, true, crc, 0);
            cart = cartInPsram;
        }
        else
        {
            /* Too large to share PSRAM with the disc: from the flash region,
               the tail (if any) in PSRAM. May write the flash and reboot. */
            cart = (const unsigned char *)prepareOversizeRom(cartPath, cartSize, resumed);
            if (!cart)
                goto out; /* declined, or ErrorMessage says why */
        }
        if (!cart || !isValidGenesisRom((uintptr_t)cart, cartSize, ErrorMessage, ERRORMESSAGESIZE))
        {
            if (!ErrorMessage[0])
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "MD+ rom could not be loaded");
            goto out;
        }
        /* A Sega CD disc under a cartridge: Mode 1 when there is a BIOS for
           the console the cartridge sets up. Without one it stays MD+. */
        scd = gwcd_disc_region() &&
              gwcd_bios_find(gwcd_disc_path(), gwcd_cart_region(cart, cartSize), "/bios", biosPath, pathSize);
        /* The save file is the cartridge's, not the disc's. */
        romName = cartPath;
    }
    else if (!gwcd_disc_region())
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Not a Sega CD disc");
        goto out;
    }
    else if (!gwcd_bios_find(gwcd_disc_path(), gwcd_disc_region(), "/bios", biosPath, pathSize))
    {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "No Sega CD BIOS in /bios/");
        goto out;
    }
    else
    {
        scd = true;
    }
    offerDiscChange(scd && gwcd_disc_count() > 1);

    do
    {
        abSwapped = 0;
        reset = resetGame = false;
        next_frame_time = 0;
        if (mdplus)
            gwmapper_set_storage(cart, romTail ? romHeadLen : cartSize, romTail);
        gwsram_detect(mdplus ? cart : nullptr, mdplus ? cartSize : 0);
        if (!init_emulator_mem())
        {
            snprintf(ErrorMessage, ERRORMESSAGESIZE, "Out of memory starting game");
            break;
        }
        if (mdplus)
            load_cartridge(cart, cartSize);
        if (scd)
        {
            size_t biosSize = 0;
            uint8_t *bios = gwcd_bios_load(biosPath, &biosSize);
            int rc = bios ? gwcd_scd_start(bios, biosSize, mdplus ? cart : nullptr, mdplus ? cartSize : 0) : -1;
            gwcd_port_psram_free(bios);
            if (rc != 0)
            {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, bios ? "Out of memory for Sega CD" : "Cannot read BIOS");
                gwcd_stop();
                free_emulator_mem();
                break;
            }
        }
        power_on();
        reset_emulation();
        if (scd)
        {
            gwcd_power_on();
            loadCdBram();
        }
        if (mdplus)
        {
            if ((scd ? gwcd_mdplus_attach(cart, cartSize) : gwcd_mdplus_start(cart, cartSize)) != 0)
            {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "Out of memory for MD+");
                gwcd_stop();
                free_emulator_mem();
                break;
            }
            romSixButton = romListsSixButtonPad(cart);
            loadCartSram();
        }
        else
        {
            romSixButton = false;
        }
        applyPadType(); /* after reset_emulation(), which resets the pads to 3 buttons */
        Frens::dumpHeapStats("cd game start");
        startAudioSinks(); /* must precede gwsnd_init */
        gwsnd_init(0 /* pal detected per frame */, HSTX);
        printSramHeadroom("cd running");
        emulate();
        /* core1's CD-DA prefetch stops with the sound engine: after that
           nothing else reads the card, and the session's buffers may go. */
        gwsnd_shutdown();
        if (mdplus)
            saveCartSram();
        saveCdBram();
        gwcd_stop();
        free_emulator_mem();
    } while (resetGame);

out:
    offerDiscChange(false);
    gwcd_disc_close();
    if (cartInPsram)
        Frens::f_free(cartInPsram);
    romflash_free_tail(romTail);
    romTail = nullptr;
    romHeadLen = 0;
    romName = selectedRom;
    gwcd_port_psram_free(biosPath);
    gwcd_port_psram_free(cartPath);
    gwmapper_set_storage(nullptr, 0, nullptr);
}
#endif /* GENESIS_SEGACD */

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
    // This emulator is always overclocked at 378 Mhz or higher. Where the
    // overclock setting exists, the settings menu stores the chosen clock in
    // FlashParams and reboots; pick it up here.
    vreg_voltage voltage = VOLTAGE;
#if OVERCLOCK_SETTING
    Frens::setOverclockLimits(EMULATOR_CLOCKFREQ_KHZ, EMULATOR_MAX_CLOCKFREQ_KHZ, VOLTAGE, MAX_VOLTAGE);
    const Frens::FlashParams *flashParams = (const Frens::FlashParams *)FLASHPARAM_ADDRESS;
    if (Frens::validateFlashParams(*flashParams))
    {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
    }
#else
    Frens::setOverclockLimits(CPUFreqKHz, CPUFreqKHz, VOLTAGE, VOLTAGE);
#endif
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);

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
#if HSTX
    raiseMaxRomSizeForFlash();
    bool resumedFromFlashWrite = false;
    if (watchdog_hw->scratch[GEN_RESUME_SCRATCH] == GEN_RESUME_MAGIC)
    {
        watchdog_hw->scratch[GEN_RESUME_SCRATCH] = 0;
        /* The path comes from the flash record, never from ROMINFOFILE: that
           file is written by every Frens emulator, so it routinely names
           another console's cart. */
        const char *rec = romflash_recorded_path();
        if (rec && rec[0])
        {
            strncpy(selectedRom, rec, sizeof(selectedRom) - 1);
            selectedRom[sizeof(selectedRom) - 1] = 0;
            resumedFromFlashWrite = true;
            showSplash = false;
            printf("romflash: resuming %s after the flash write\n", selectedRom);
        }
        else
        {
            printf("romflash: resume asked for, but the record is invalid\n");
        }
    }
#endif
    g_settings_visibility = g_settings_visibility_md;
    g_available_screen_modes = g_available_screen_modes_md;
    gwsnd_set_fill_query(sinkFillPermille);
#if GENESIS_SEGACD
    recursive_mutex_init(&cdSdMutex);
    /* Discs (.cue, .m3u playlists of multi-disc games, and .chd when built
       with CHD) only with PSRAM, where all their buffers live. */
    const char *menuExts = Frens::isPsramEnabled() ? ".md .bin .cue .m3u" GWCD_CHD_EXT : ".md .bin";
#else
    const char *menuExts = ".md .bin";
#endif
    while (true)
    {
        if (strlen(selectedRom) == 0 || reset == true)
        {
            menu("Pico-Genesis+", ErrorMessage, isFatalError, showSplash, menuExts, selectedRom);
        }
#if GENESIS_SEGACD
        if (resumedFromFlashWrite && Frens::isPsramEnabled() && !isDiscImage(selectedRom))
        {
            /* The flash write was for an MD+ cartridge: back to its disc. */
            char *disc = (char *)gwcd_port_psram_alloc(FF_MAX_LFN);
            if (disc && findPairedDisc(selectedRom, disc, FF_MAX_LFN))
                strcpy(selectedRom, disc);
            gwcd_port_psram_free(disc);
        }
        if (isDiscImage(selectedRom))
        {
            printf("Now playing (disc): %s\n", selectedRom);
            audio_enabled = settings.flags.audioEnabled;
            runDiscGame(selectedRom, resumedFromFlashWrite);
            resumedFromFlashWrite = false;
            if (ErrorMessage[0])
                printf("%s\n", ErrorMessage);
            selectedRom[0] = 0;
            showSplash = false;
            continue;
        }
#endif
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

        /* Normally the menu has preloaded the ROM into PSRAM (or, without
           PSRAM, flashed it). Zero with PSRAM present means it was too large
           to preload: run it from the flash region. */
        uintptr_t romAddr = ROM_FILE_ADDR;
#if HSTX
        if (!romAddr && Frens::isPsramEnabled() && strlen(selectedRom) > 0)
        {
            romAddr = prepareOversizeRom(selectedRom, romSize, resumedFromFlashWrite);
            if (!romAddr)
            {
                resumedFromFlashWrite = false;
                selectedRom[0] = 0;
                showSplash = false;
                continue;
            }
        }
        resumedFromFlashWrite = false;
#endif
        /* Every game, split or not: the firmware never reboots between games,
           and a previous game's split must not survive into this one. */
        gwmapper_set_storage((const unsigned char *)romAddr,
                             romTail ? romHeadLen : romSize, romTail);

        do
        {
            abSwapped = 0; // don't swap A and B buttons
            reset = resetGame = false;
            next_frame_time = 0; // Reset next frame time for FPS limiter
            if (!isValidGenesisRom(romAddr, romSize, ErrorMessage, ERRORMESSAGESIZE))
            {
                printf("%s: %s\n", ErrorMessage, selectedRom);
                reset = true;
                break;
            }
            printf("Starting game (%d KB rom) rom@%p\n", (int)(romSize / 1024),
                   (void *)romAddr);
            /* Must precede init_emulator_mem(), which allocates the buffer
               this sizes. */
            gwsram_detect((const unsigned char *)romAddr, romSize);
            if (!init_emulator_mem())
            {
                snprintf(ErrorMessage, 40, "Out of memory starting game");
                printf("%s\n", ErrorMessage);
                reset = true;
                break;
            }
            load_cartridge((const unsigned char *)romAddr, romSize);
            power_on();
            reset_emulation();
            romSixButton = romListsSixButtonPad((const unsigned char *)romAddr);
            applyPadType(); /* after reset_emulation(), which resets the pads to 3 buttons */
            loadCartSram();
            Frens::dumpHeapStats("game start"); /* peak usage, both heaps */
            startAudioSinks();                  /* must precede gwsnd_init */
            gwsnd_init(0 /* pal detected per frame */, HSTX);
            printSramHeadroom("game running");
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
        /* The PSRAM half of a flash/PSRAM split, for the same reason. The
           flash half is never freed. */
#if HSTX
        romflash_free_tail(romTail);
#endif
        romTail = nullptr;
        romHeadLen = 0;
        gwmapper_set_storage(nullptr, 0, nullptr);
        selectedRom[0] = 0;
        showSplash = false;
    }

    return 0;
}
