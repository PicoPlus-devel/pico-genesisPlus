/* pico-genesisPlus — ROMs too large for PSRAM, held in XIP flash (plus PSRAM).
 * See romflash.h for the layout and why the record lives in flash. The flash
 * write sequence is pico_snesPlus's, unchanged: it is the part that was hard
 * to get right, and it has been proven on the same boards. */

#include "romflash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "ff.h"

#include "FrensHelpers.h"
#include "crc32.h"
#if PICO_RP2350 && PSRAM_CS_PIN
#include "PicoPlusPsram.h"
#endif

/* Region: the top 8 MB of the board's 16 MB flash. Clear of the standalone
 * layout (app at 0x10000000), of the bootloader layout (FRENS_APP_BASE =
 * 0x10080000, see pico_shared/BootPartition.cmake) and of FlashParams, which
 * is placed immediately after the binary. */
#define ROMFLASH_BASE   0x10800000u
#define ROMFLASH_SIZE   (8u * 1024u * 1024u)
/* 64 KB, not one 4 KB sector, purely so the image below starts 64 KB-aligned:
 * flash_range_erase() only uses the 64 KB block-erase command on an aligned
 * range. The record itself only occupies the first sector. */
#define ROMFLASH_HDR    (64u * 1024u)
#define ROMFLASH_IMAGE  (ROMFLASH_BASE + ROMFLASH_HDR)
#define ROMFLASH_MAX    (ROMFLASH_SIZE - ROMFLASH_HDR)
#define ROMFLASH_MAGIC  0x31524E47u   /* "GNR1" -- pico_snesPlus uses "SNR1" */
/* The cartridge mapper's bank size (port/gwmapper.h): a split must fall on it. */
#define ROMFLASH_BANK   (512u * 1024u)

struct RomFlashRecord
{
    uint32_t magic;
    uint32_t base;      /* image base this record was written for */
    uint32_t size;      /* bytes of the whole ROM file */
    uint32_t crc;       /* crc32 over the flash image, as byte-swapped */
    uint16_t fdate;     /* f_stat of the source file when it was written */
    uint16_t ftime;
    uint32_t headLen;   /* bytes of the ROM held in flash; the rest is PSRAM */
    char     path[FF_MAX_LFN + 1];
};

static_assert(sizeof(RomFlashRecord) <= FLASH_SECTOR_SIZE,
              "romflash record must fit in one flash sector");

static const RomFlashRecord *record(void)
{
    return (const RomFlashRecord *)(uintptr_t)ROMFLASH_BASE;
}

/* The region needs a board that really carries 16 MB. Ask the chip rather than
 * trusting PICO_FLASH_SIZE_BYTES: CMakeLists.txt defines that as 16 MB for
 * every board, so a smaller part would happily be programmed at addresses that
 * alias back over the app. This must stay a check, not an assumption.
 *
 * The JEDEC ID (0xMMTTCC, CC = log2 of the capacity) is read off the chip once,
 * before the overclock, by pico_shared's storage_get_flash_capacity(), and
 * cached. That function is not in FrensHelpers.h; storage_get_flash_jedec_id()
 * is, and calling it keeps the former inlined -- an out-of-line call made the
 * linker keep an 80-byte SRAM copy of it. */
static uint32_t flash_capacity(void)
{
    uint32_t exp = Frens::storage_get_flash_jedec_id() & 0xFFu;
    return exp < 32u ? (1u << exp) : 0u;
}

static bool region_present(void)
{
    uint32_t capacity = flash_capacity();
    uint32_t need = (ROMFLASH_BASE - XIP_BASE) + ROMFLASH_SIZE;

    if (capacity < need) {
        printf("romflash: flash is %u MB, need %u MB for the ROM region\n",
               (unsigned)(capacity / (1024 * 1024)),
               (unsigned)(need / (1024 * 1024)));
        return false;
    }
    /* Belt and braces: never let the region collide with the linked image or
     * the FlashParams sector that follows it. */
    if ((uintptr_t)&__flash_binary_end + FLASH_SECTOR_SIZE > ROMFLASH_BASE) {
        printf("romflash: app image reaches %p, region starts at %08X\n",
               (void *)&__flash_binary_end, (unsigned)ROMFLASH_BASE);
        return false;
    }
    return true;
}

size_t romflash_capacity(void)
{
    return region_present() ? ROMFLASH_MAX : 0;
}

size_t romflash_head_len(size_t size)
{
    if (size <= ROMFLASH_MAX)
        return size;
    return (ROMFLASH_MAX / ROMFLASH_BANK) * ROMFLASH_BANK;
}

const uint8_t *romflash_image(void)
{
    return (const uint8_t *)(uintptr_t)ROMFLASH_IMAGE;
}

const char *romflash_recorded_path(void)
{
    if (!region_present()) return NULL;
    const RomFlashRecord *rec = record();
    if (rec->magic != ROMFLASH_MAGIC) return NULL;
    if (rec->base  != ROMFLASH_IMAGE) return NULL;
    if (rec->path[0] == 0)            return NULL;
    return rec->path;
}

/* crc32 over the image sitting in flash: a few tens of milliseconds for
 * 7.5 MB of XIP, paid once per launch. */
static uint32_t image_crc(size_t size)
{
    const uint8_t *p = romflash_image();
    uint32_t crc = 0;
    size_t done = 0;
    while (done < size) {
        UINT n = (UINT)((size - done > 65536u) ? 65536u : (size - done));
        crc = update_crc32(crc, p + done, n);
        done += n;
    }
    return crc;
}

bool romflash_holds(const char *path, size_t size)
{
    if (!region_present()) return false;

#if ROMFLASH_FORCE_REWRITE
    /* Testing build: always claim the region does not hold this cart, so the
     * write path (and its progress bar) runs on every launch. */
    (void)path; (void)size;
    printf("romflash: ROMFLASH_FORCE_REWRITE - ignoring any existing image\n");
    return false;
#endif

    const RomFlashRecord *rec = record();

    if (rec->magic   != ROMFLASH_MAGIC)                      return false;
    if (rec->base    != ROMFLASH_IMAGE)                      return false;
    if (rec->size    != (uint32_t)size)                      return false;
    if (rec->headLen != (uint32_t)romflash_head_len(size))   return false;
    if (strcasecmp(rec->path, path) != 0)                    return false;

    /* Size and timestamp catch the one thing the CRC cannot: the file on the
     * card was replaced by a different game under the same name. They also
     * cover the PSRAM tail, which is re-read from the card every launch. */
    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    bool stamped = (f_stat(path, fno) == FR_OK) &&
                   fno->fdate == rec->fdate && fno->ftime == rec->ftime;
    Frens::f_free(fno);
    if (!stamped) {
        printf("romflash: record is stale for %s\n", path);
        return false;
    }

    uint32_t crc = image_crc(rec->headLen);
    if (crc != rec->crc) {
        printf("romflash: image crc %08X != recorded %08X\n",
               (unsigned)crc, (unsigned)rec->crc);
        return false;
    }

    printf("romflash: %s already in flash at %08X (%u of %u KB)\n",
           path, (unsigned)ROMFLASH_IMAGE, (unsigned)(rec->headLen / 1024),
           (unsigned)(size / 1024));
    return true;
}

/* The core fetches 16-bit words little-endian, so the image is stored with
 * every byte pair swapped -- what the menu's PSRAM preload does too. Callers
 * only pass even offsets and lengths. */
static void swap_pairs(uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t t = p[i];
        p[i] = p[i + 1];
        p[i + 1] = t;
    }
}

/* --- XIP timing across a flash write -------------------------------------
 *
 * On RP2350 flash_range_erase/_program end in the bootrom's
 * flash_enter_cmd_xip(), which resets qmi_hw->m[0] to its default: a plain 03h
 * serial read at CLKDIV=4, RXDELAY=0. At 378 MHz sysclk that is ~94.5 MHz with
 * no read delay -- far outside what a 03h read tolerates -- so the very next
 * instruction fetched from flash is garbage. The SDK saves and restores only
 * QMI CS1 (the PSRAM); nothing puts M0 back, and pico_shared's
 * Frens::flashEraseSafe()/flashProgramSafe() only restore the divisor on
 * RP2040. Streaming a ROM from SD across ~1800 erase/program calls means going
 * back to FatFs, the SD driver and printf -- all in flash -- in between, so
 * after every operation drop M0 to a divisor a 03h serial read is comfortable
 * at (<= 50 MHz), and restore the tuned quad setup once at the end.
 *
 * The fixup must run from RAM and inside the same interrupts-off window as the
 * flash op, so that no flash fetch can happen in between. These three are the
 * only SRAM-resident code in this file. */
static uint32_t safe_m0_timing;   /* computed before the first write */
static uint32_t saved_m0_timing;  /* the tuned quad-read setup, captured  */
static uint32_t saved_m0_rfmt;    /* before the first erase and put back  */
static uint32_t saved_m0_rcmd;    /* after the last program               */

static inline uint32_t romflash_calc_timing(void)
{
    uint32_t hz  = clock_get_hz(clk_sys);
    uint32_t div = (hz + 49999999u) / 50000000u;   /* ceil -> <= 50 MHz */
    if (div < 2)   div = 2;
    if (div > 255) div = 255;
    /* MIN_DESELECT[15:11]=14, RXDELAY[10:8]=2, CLKDIV[7:0] */
    return 0x60000000u | (14u << 11) | (2u << 8) | div;
}

static void __no_inline_not_in_flash_func(romflash_erase)(uint32_t off, size_t n)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, n);
    qmi_hw->m[0].timing = safe_m0_timing;
    __compiler_memory_barrier();
    restore_interrupts(ints);
}

/* Put M0 back exactly as it was: whatever boot2 and setClocksAndStartStdio()
 * had programmed, including the quad-read format the bootrom replaced. */
static void __no_inline_not_in_flash_func(romflash_restore_xip)(void)
{
    uint32_t ints = save_and_disable_interrupts();
    qmi_hw->m[0].rfmt   = saved_m0_rfmt;
    qmi_hw->m[0].rcmd   = saved_m0_rcmd;
    qmi_hw->m[0].timing = saved_m0_timing;
    __compiler_memory_barrier();
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(romflash_program_page)(uint32_t off,
                                                                const uint8_t *data,
                                                                size_t n)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, data, n);
    qmi_hw->m[0].timing = safe_m0_timing;
    __compiler_memory_barrier();
    restore_interrupts(ints);
}

/* The program source must live in internal SRAM. Erase and program take the
 * QMI out of XIP mode, which stops the PSRAM aperture as well as flash, so a
 * Frens::f_malloc buffer (PSRAM whenever PSRAM is enabled -- which it always
 * is on the boards that reach this path) would be unreadable exactly when the
 * bootrom needs to read it. Plain malloc gives SRAM; it is freed again before
 * the game allocates anything. */
static uint8_t *alloc_sram_buffer(size_t *out_size)
{
    static const size_t sizes[] = { 64 * 1024, 32 * 1024, 16 * 1024,
                                    8 * 1024, FLASH_SECTOR_SIZE };
    for (size_t i = 0; i < count_of(sizes); i++) {
        uint8_t *p = (uint8_t *)malloc(sizes[i]);
        if (p) {
            *out_size = sizes[i];
            return p;
        }
    }
    *out_size = 0;
    return NULL;
}

bool romflash_program(const char *path, size_t size, romflash_progress_fn progress)
{
    if (!region_present()) return false;

    const size_t headLen = romflash_head_len(size);
    if (size == 0 || headLen == 0) {
        printf("romflash: nothing to write for %s\n", path);
        return false;
    }

    size_t bufsize = 0;
    uint8_t *buffer = alloc_sram_buffer(&bufsize);
    if (!buffer) {
        printf("romflash: no SRAM for a program buffer\n");
        return false;
    }

    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    if (f_stat(path, fno) != FR_OK) {
        printf("romflash: cannot stat %s\n", path);
        Frens::f_free(fno);
        free(buffer);
        return false;
    }
    uint16_t fdate = fno->fdate, ftime = fno->ftime;
    Frens::f_free(fno);

    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (f_open(fil, path, FA_READ) != FR_OK) {
        printf("romflash: cannot open %s\n", path);
        Frens::f_free(fil);
        free(buffer);
        return false;
    }

    safe_m0_timing  = romflash_calc_timing();
    saved_m0_timing = qmi_hw->m[0].timing;
    saved_m0_rfmt   = qmi_hw->m[0].rfmt;
    saved_m0_rcmd   = qmi_hw->m[0].rcmd;
    printf("romflash: writing %u of %u KB of %s to %08X "
           "(XIP drops to CLKDIV=%u for the duration)\n",
           (unsigned)(headLen / 1024), (unsigned)(size / 1024), path,
           (unsigned)ROMFLASH_IMAGE, (unsigned)(safe_m0_timing & 0xFF));

    /* Safety net: if anything below faults or stalls, come back up rather than
     * sitting on a frozen screen forever. Fed between operations. */
    watchdog_enable(8000, 1);

    /* Invalidate first: a power cut from here on leaves no record at all,
     * never a record that describes a half-written image. */
    romflash_erase(ROMFLASH_BASE - XIP_BASE, ROMFLASH_HDR);

    /* Erase the whole image range up front, in watchdog-sized batches, so
     * flash_range_erase() can use 64 KB block erases (the bounce buffer is
     * typically far smaller, and erasing per chunk would force 4 KB ones). */
    {
        const uint32_t image_off = ROMFLASH_IMAGE - XIP_BASE;
        const size_t   span      = (headLen + 0xFFFFu) & ~(size_t)0xFFFFu;
        for (size_t done = 0; done < span; ) {
            size_t n = span - done;
            if (n > 512u * 1024u) n = 512u * 1024u;
            romflash_erase(image_off + (uint32_t)done, n);
            watchdog_update();
            done += n;
            if (progress) progress(ROMFLASH_ERASE, (uint32_t)done, (uint32_t)span);
            printf("romflash: erased %u / %u KB\n",
                   (unsigned)(done / 1024), (unsigned)(span / 1024));
        }
    }

    bool     ok      = true;
    size_t   written = 0;
    uint32_t src_crc = 0;   /* CRC of what we meant to write, see below */
    while (written < headLen) {
        size_t want = headLen - written;
        if (want > bufsize) want = bufsize;

        UINT br = 0;
        if (f_read(fil, buffer, (UINT)want, &br) != FR_OK || br != want) {
            printf("romflash: read error at %u (%u of %u bytes)\n",
                   (unsigned)written, (unsigned)br, (unsigned)want);
            ok = false;
            break;
        }
        swap_pairs(buffer, br);

        /* flash_range_program wants whole pages; pad the tail of the last
         * chunk with the erased value. The range is already erased above. */
        size_t prog = (br + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);
        if (prog > br) memset(buffer + br, 0xFF, prog - br);

        /* CRC the source as it goes by rather than reading the image back at
         * the slow safe timing: this is what we meant to write, so the verify
         * below genuinely compares intent against what landed in flash. */
        src_crc = update_crc32(src_crc, buffer, br);

        uint32_t off = (ROMFLASH_IMAGE - XIP_BASE) + (uint32_t)written;
        romflash_program_page(off, buffer, prog);
        watchdog_update();

        written += br;
        if (progress) progress(ROMFLASH_WRITE, (uint32_t)written, (uint32_t)headLen);
        if ((written & 0xFFFFF) == 0 || written == headLen)
            printf("romflash: %u / %u KB\n",
                   (unsigned)(written / 1024), (unsigned)(headLen / 1024));
    }

    f_close(fil);
    Frens::f_free(fil);
    free(buffer);

    if (!ok || written != headLen) {
        printf("romflash: write failed (%u of %u bytes)\n",
               (unsigned)written, (unsigned)headLen);
        romflash_restore_xip();
        watchdog_disable();
        return false;
    }

    /* Build the record, but do not commit it until the image verifies. */
    RomFlashRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic   = ROMFLASH_MAGIC;
    rec.base    = ROMFLASH_IMAGE;
    rec.size    = (uint32_t)size;
    rec.crc     = src_crc;
    rec.fdate   = fdate;
    rec.ftime   = ftime;
    rec.headLen = (uint32_t)headLen;
    strncpy(rec.path, path, sizeof(rec.path) - 1);

    const size_t recpages = (sizeof(rec) + FLASH_PAGE_SIZE - 1) &
                            ~(size_t)(FLASH_PAGE_SIZE - 1);
    uint8_t *page = (uint8_t *)malloc(recpages);
    if (!page) {
        printf("romflash: no SRAM for the record page\n");
        romflash_restore_xip();
        watchdog_disable();
        return false;
    }
    memset(page, 0xFF, recpages);
    memcpy(page, &rec, sizeof(rec));
    romflash_program_page(ROMFLASH_BASE - XIP_BASE, page, recpages);
    free(page);

    /* Everything is written; put the tuned quad-read timing back so the verify
     * below runs at full XIP speed. */
    romflash_restore_xip();
    watchdog_disable();

    uint32_t back = image_crc(headLen);
    if (back != rec.crc) {
        /* Erase the record rather than leave one that lies. The next launch
         * then simply rewrites. */
        printf("romflash: verify FAILED (flash %08X != source %08X)\n",
               (unsigned)back, (unsigned)rec.crc);
        romflash_erase(ROMFLASH_BASE - XIP_BASE, ROMFLASH_HDR);
        romflash_restore_xip();
        return false;
    }

    printf("romflash: wrote and verified %u KB, crc %08X\n",
           (unsigned)(headLen / 1024), (unsigned)rec.crc);
    return true;
}

uint8_t *romflash_load_tail(const char *path, size_t size, size_t headLen)
{
#if PICO_RP2350 && PSRAM_CS_PIN
    if (!Frens::isPsramEnabled() || headLen >= size)
        return NULL;

    const size_t tailLen = size - headLen;
    /* PicoPlusPsram::Malloc, not Frens::f_malloc: the latter panics on
     * failure, and a 7 MB block can fail on a fragmented arena even when the
     * free total says it fits. */
    uint8_t *tail = (uint8_t *)PicoPlusPsram::getInstance().Malloc(tailLen);
    if (!tail) {
        printf("romflash: cannot allocate %u KB of PSRAM for the rest of the ROM "
               "(%u KB free)\n", (unsigned)(tailLen / 1024),
               (unsigned)(Frens::GetAvailableMemory() / 1024));
        return NULL;
    }

    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    bool ok = f_open(fil, path, FA_READ) == FR_OK;
    if (ok)
        ok = f_lseek(fil, (FSIZE_t)headLen) == FR_OK;
    size_t done = 0;
    /* Swap each chunk right after reading it, while it is still in the XIP
     * cache, rather than in a second pass over 7 MB of PSRAM. */
    while (ok && done < tailLen) {
        size_t want = tailLen - done;
        if (want > 16u * 1024u) want = 16u * 1024u;
        UINT br = 0;
        ok = f_read(fil, tail + done, (UINT)want, &br) == FR_OK && br == want;
        if (ok) {
            swap_pairs(tail + done, br);
            done += br;
        }
    }
    f_close(fil);
    Frens::f_free(fil);

    if (!ok) {
        printf("romflash: read error loading the ROM tail at %u of %u KB\n",
               (unsigned)(done / 1024), (unsigned)(tailLen / 1024));
        PicoPlusPsram::getInstance().Free(tail);
        return NULL;
    }
    printf("romflash: %u KB of the ROM loaded into PSRAM at %p\n",
           (unsigned)(tailLen / 1024), (void *)tail);
    return tail;
#else
    (void)path; (void)size; (void)headLen;
    return NULL;
#endif
}

void romflash_free_tail(uint8_t *tail)
{
#if PICO_RP2350 && PSRAM_CS_PIN
    if (tail)
        PicoPlusPsram::getInstance().Free(tail);
#else
    (void)tail;
#endif
}
