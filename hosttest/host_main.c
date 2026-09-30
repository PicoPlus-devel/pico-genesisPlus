/*
hosttest/host_main.c — headless Linux harness for the gwenesis core.

Runs the exact same core sources and frame loop (port/frame_loop.inc) as
the pico firmware, with the display in RGB565, and dumps:
  - PPM frames every N frames        -> <outdir>/frame_NNNNN.ppm
  - 44.1 kHz stereo mixed audio      -> <outdir>/mixed.wav (real resampler)
  - raw chip-rate mono per-chip WAVs -> <outdir>/ym.wav, <outdir>/psg.wav

Usage:
    gen_host <rom.md|.bin|.gen> <total-frames> <dump-every-N> [outdir]

Sequential-launch mode (reproduces "second game is broken" bugs, where
state survives in the core between games because the firmware starts a new
game without rebooting):

    GEN_FIRST_ROM=<rom> gen_host <rom2> <frames> <dump-every> [outdir]

runs <rom> to completion first, tears everything down exactly as the
firmware does (gwsnd_shutdown + free_emulator_mem), then launches <rom2>.
Its output must be byte-identical to launching <rom2> on its own.

Input injection (frame ranges, inclusive):
    GEN_PRESS_START="120:180"   hold START on pad 0
                                (several ranges: "120:125,300:305")
    GEN_PRESS_A="200:220"       hold A on pad 0
    GEN_PRESS_B / GEN_PRESS_C   likewise
    GEN_PRESS_X / _Y / _Z / _MODE
                                likewise; a game only sees these
                                with GEN_PAD=6

6-button pad:
    GEN_PAD=6                   present 6-button pads on both ports
                                (default 3, as upstream)
    GEN_PAD_SELFTEST=1          drive the TH protocol through the I/O
                                registers and check every read of a
                                6-button and a 3-button sequence, including
                                the ~1.5 ms reset, then exit (status 0 =
                                pass). No frames are run.

Split ROM storage (the firmware keeps a ROM too big for PSRAM as a head in
flash plus a tail in PSRAM, see romflash.cpp):
    GEN_SPLIT_BANKS=<n>         copy the first n 512 KB banks and the rest
                                into two separate allocations. The head is
                                exactly n banks, so ASan stops any fetch
                                that runs off its end. Output must be
                                byte-identical to the contiguous run.

Cartridge save RAM:
    GEN_SRM=<file>              load it before the run and write it after,
                                in the same flat 64 KB .srm layout the
                                firmware uses (main.cpp)
    GEN_SRAM_SELFTEST=1         write a pattern through the 68000 bus and
                                read it back through the CPU's own read
                                path, so detection and mapping can be
                                checked without driving a game's save menu.
                                Runs before the frame loop.

Convert PPMs: python3 hosttest/ppm2png.py <outdir>
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "gwenesis_bus.h"
#include "gwenesis_vdp.h"
#include "gwenesis_io.h"
#include "m68k.h"
#include "z80inst.h"
#include "ym2612.h"
#include "gwenesis_sn76489.h"

#include "gwsnd.h"
#include "buffers.h"
#include "gwsram.h"
#include "gwmapper.h"
#include "wav.h"

#include "frame_loop.inc"

#define FB_W 320
#define FB_H 240

static uint16_t framebuffer[FB_W * FB_H];

static wav_writer wav_mixed, wav_ym, wav_psg;

/* ------------------------- input injection ------------------------- */

extern unsigned short button_state[3];

typedef struct {
    int from, to, bit;
} press_range;

static press_range presses[32];
static int press_count;
static int current_frame_no;

/* "from:to", or several separated by commas: "120:125,300:305". */
static void parse_press(const char *env, int bit)
{
    const char *v = getenv(env);
    while (v && *v && press_count < (int)(sizeof presses / sizeof presses[0])) {
        int from = 0, to = 0;
        if (sscanf(v, "%d:%d", &from, &to) != 2)
            break;
        presses[press_count].from = from;
        presses[press_count].to = to;
        presses[press_count].bit = bit;
        press_count++;
        v = strchr(v, ',');
        if (v)
            v++;
    }
}

/* Set by pad_selftest(): the buttons it holds down on both pads, overriding
   the frame-range presses. */
static int pad_selftest_active;
static unsigned short pad_selftest_pressed;

/* Core callback: refresh button_state (active low, M X Y Z S A C B R L D U). */
void gwenesis_io_get_buttons(void)
{
    unsigned short pressed = 0;
    if (pad_selftest_active) {
        button_state[0] = button_state[1] = (unsigned short)~pad_selftest_pressed;
        button_state[2] = 0xffff;
        return;
    }
    for (int i = 0; i < press_count; i++) {
        if (current_frame_no >= presses[i].from && current_frame_no <= presses[i].to)
            pressed |= (unsigned short)(1u << presses[i].bit);
    }
    button_state[0] = (unsigned short)~pressed;
    button_state[1] = 0xffff;
    button_state[2] = 0xffff;
}

/* --------------------------- audio sinks --------------------------- */

static void audio_out(int16_t l, int16_t r)
{
    int16_t s[2] = {l, r};
    wav_write(&wav_mixed, s, 2);
}

static void frame_tap(const int16_t *ym, const int16_t *psg, int samples)
{
    wav_write(&wav_ym, ym, samples);
    wav_write(&wav_psg, psg, samples);
}

/* --------------------------- video dump ---------------------------- */

static void dump_ppm(const char *outdir, int frame)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/frame_%05d.ppm", outdir, frame);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", FB_W, FB_H);
    for (int i = 0; i < FB_W * FB_H; i++) {
        uint16_t px = framebuffer[i];
        uint8_t rgb[3];
        rgb[0] = (uint8_t)(((px >> 11) & 0x1f) << 3);
        rgb[1] = (uint8_t)(((px >> 5) & 0x3f) << 2);
        rgb[2] = (uint8_t)((px & 0x1f) << 3);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

/* ---------------------------- ROM load ----------------------------- */

static const unsigned char *load_rom(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0x200) {
        fclose(f);
        fprintf(stderr, "ROM too small\n");
        return NULL;
    }

    /* The core masks fetches with pow2ceil(size)-1, so back the image with
       a pow2-rounded buffer (+4 for the 32-bit fetch overhang). */
    size_t pot = 1;
    while (pot < (size_t)sz)
        pot <<= 1;
    unsigned char *rom = calloc(1, pot + 4);
    if (fread(rom, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(rom);
        fprintf(stderr, "short read\n");
        return NULL;
    }
    fclose(f);

    /* pico_shared byte-swaps Genesis ROMs at load; replicate. */
    for (long i = 0; i + 1 < sz; i += 2) {
        unsigned char t = rom[i];
        rom[i] = rom[i + 1];
        rom[i + 1] = t;
    }
    *size_out = (size_t)sz;
    return rom;
}

/* --------------------------- save RAM ------------------------------ */

/* The harness has no PSRAM, so port/gwsram.c's fallback finds nothing. The
   SRAM path never fails on a PC, so this is only ever reached in tests that
   deliberately force it. */
void *gwsram_port_psram_alloc(size_t size)
{
    (void)size;
    return NULL;
}

void gwsram_port_psram_free(void *p)
{
    (void)p;
}

/* The firmware's .srm layout: a flat 64 KB image of the $200000 page, file
   offset = address & 0xFFFF, 0xFF wherever nothing is backed. The interleave
   itself is gwsram_export/gwsram_import, the same code main.cpp streams
   through its chunk buffer — only the file I/O differs (stdio here, FatFs
   with a 3 KB stack there). */
#define SRM_FILE_SIZE 0x10000

static void srm_load(const char *path)
{
    uint8_t *flat;
    FILE *f;

    if (!gwsram_span)
        return;
    f = fopen(path, "rb");
    if (!f) {
        printf("srm: %s not present, cartridge RAM starts empty\n", path);
        return;
    }
    flat = malloc(SRM_FILE_SIZE);
    memset(flat, 0xFF, SRM_FILE_SIZE);
    if (fread(flat, 1, SRM_FILE_SIZE, f) == 0)
        printf("srm: %s is empty\n", path);
    fclose(f);

    if (!gwsram_ensure_buffer()) {
        printf("srm: no memory for cartridge RAM, %s not loaded\n", path);
        free(flat);
        return;
    }
    gwsram_import(0, flat + (gwsram_start & 0xFFFF), gwsram_span);
    free(flat);
    gwsram_dirty = 0;
    printf("srm: loaded %s\n", path);
}

static void srm_save(const char *path)
{
    uint8_t *flat;
    FILE *f;

    if (!gwsram_data || !gwsram_span || !gwsram_dirty) {
        printf("srm: nothing to save (dirty=%d)\n", gwsram_dirty);
        return;
    }
    flat = malloc(SRM_FILE_SIZE);
    memset(flat, 0xFF, SRM_FILE_SIZE);
    gwsram_export(0, flat + (gwsram_start & 0xFFFF), gwsram_span);

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "srm: cannot write %s\n", path);
        free(flat);
        return;
    }
    fwrite(flat, 1, SRM_FILE_SIZE, f);
    fclose(f);
    free(flat);
    gwsram_dirty = 0;
    printf("srm: wrote %s (%d bytes)\n", path, SRM_FILE_SIZE);
}

/* Drive save RAM through the 68000 bus the way a game would, and report
   whether it reads back. Returns non-zero on mismatch. */
unsigned int gwenesis_host_cpu_read8(unsigned int address);

static int srm_selftest(void)
{
    unsigned int addr, got;
    uint32_t i;
    int bad = 0;

    if (!gwsram_span) {
        printf("selftest: this cart has no save RAM\n");
        return 0;
    }
    /* A bankable cart hides save RAM behind $A130F1 until the game asks. */
    if (gwsram_bankable)
        m68k_write_memory_8(0xA130F1, 1);
    printf("selftest: buffer before first write: %s\n",
           gwsram_data ? "allocated" : "not yet (deferred)");

    for (i = 0; i < 8 && i * 2 < gwsram_span; i++) {
        addr = gwsram_start + i * 2 + (gwsram_packed ? (unsigned)gwsram_odd : 1u);
        m68k_write_memory_8(addr, 0x5A ^ i);
        /* Read back the way the 68000 does. m68k_read_memory_8() is the bus
           entry point, but the CPU takes a shortcut for the cartridge window,
           so checking only the bus would pass even with save RAM unreachable
           to every game. */
        got = gwenesis_host_cpu_read8(addr);
        if (got != (0x5Au ^ i)) {
            printf("selftest: %06x wrote %02x, CPU read %02x MISMATCH\n", addr,
                   0x5Au ^ i, got);
            bad++;
        }
        if (m68k_read_memory_8(addr) != got) {
            printf("selftest: %06x bus and CPU read paths disagree\n", addr);
            bad++;
        }
    }
    /* The half the chip does not drive must read back as open bus. */
    if (gwsram_packed) {
        addr = gwsram_start + (gwsram_odd ? 0u : 1u);
        m68k_write_memory_8(addr, 0x12);
        got = gwenesis_host_cpu_read8(addr);
        if (got != 0xFF) {
            printf("selftest: unbacked half at %06x read %02x, expected ff\n",
                   addr, got);
            bad++;
        }
    }
    printf("selftest: %s (dirty=%d)\n", bad ? "FAILED" : "ok", gwsram_dirty);
    return bad;
}

/* ----------------------- 6-button pad selftest ---------------------- */

/* Pressed-button bits, same positions as button_state (1 = pressed). */
enum { PB_U = 1 << 0, PB_D = 1 << 1, PB_L = 1 << 2, PB_R = 1 << 3,
       PB_B = 1 << 4, PB_C = 1 << 5, PB_A = 1 << 6, PB_S = 1 << 7,
       PB_Z = 1 << 8, PB_Y = 1 << 9, PB_X = 1 << 10, PB_M = 1 << 11 };

enum pad_read_kind { RD_HIGH, RD_LOW, RD_ID, RD_XYZ, RD_LOW_ONES };

/* What the pad drives on D0-D5 for one read, built bit by bit from the
   button names in the Sega 6-button documentation (1 = released). */
static unsigned expect_pad_bits(enum pad_read_kind kind, unsigned p)
{
#define REL(b) ((p & (b)) ? 0u : 1u)
    switch (kind) {
    case RD_HIGH:     /* ? 1 C B R L D U */
        return REL(PB_U) | REL(PB_D) << 1 | REL(PB_L) << 2 | REL(PB_R) << 3 |
               REL(PB_B) << 4 | REL(PB_C) << 5;
    case RD_LOW:      /* ? 0 S A 0 0 D U */
        return REL(PB_U) | REL(PB_D) << 1 | REL(PB_A) << 4 | REL(PB_S) << 5;
    case RD_ID:       /* ? 0 S A 0 0 0 0 */
        return REL(PB_A) << 4 | REL(PB_S) << 5;
    case RD_XYZ:      /* ? 1 C B M X Y Z */
        return REL(PB_Z) | REL(PB_Y) << 1 | REL(PB_X) << 2 | REL(PB_M) << 3 |
               REL(PB_B) << 4 | REL(PB_C) << 5;
    case RD_LOW_ONES: /* ? 0 S A 1 1 1 1 */
        return 0x0f | REL(PB_A) << 4 | REL(PB_S) << 5;
    }
#undef REL
    return 0;
}

static int pad_selftest_bad;

/* Write TH through the data port of `port` (0 or 1), read it back and check
   the result. TH is an output, so bit 6 reads back what was written. */
static void pad_step(const char *what, int port, int th, enum pad_read_kind kind)
{
    unsigned data = 0x03 + port * 2;
    gwenesis_io_write_ctrl(data, th ? 0x40 : 0x00);
    unsigned got = gwenesis_io_read_ctrl(data);
    unsigned want = (th ? 0x40u : 0u) | expect_pad_bits(kind, pad_selftest_pressed);
    if (got != want) {
        printf("padtest: %s: port %d TH=%d read %02x, expected %02x\n",
               what, port + 1, th, got, want);
        pad_selftest_bad++;
    }
}

/* One complete read-out, starting from an idle pad with TH high. On a
   6-button pad the third TH-low read is the ID, then X Y Z Mode. */
static void pad_sequence(const char *what, int port, int six)
{
    pad_step(what, port, 1, RD_HIGH);
    pad_step(what, port, 0, RD_LOW);
    pad_step(what, port, 1, RD_HIGH);
    pad_step(what, port, 0, RD_LOW);
    pad_step(what, port, 1, RD_HIGH);
    pad_step(what, port, 0, six ? RD_ID : RD_LOW);
    pad_step(what, port, 1, six ? RD_XYZ : RD_HIGH);
    pad_step(what, port, 0, six ? RD_LOW_ONES : RD_LOW);
    pad_step(what, port, 1, RD_HIGH);
}

static int pad_selftest(void)
{
    static const unsigned patterns[] = {
        0,
        PB_A | PB_C | PB_U | PB_X | PB_Z,
        PB_B | PB_S | PB_D | PB_R | PB_Y | PB_M,
        0xfff,
    };
    pad_selftest_active = 1;
    pad_selftest_bad = 0;

    for (int port = 0; port < 2; port++) {
        gwenesis_io_write_ctrl(0x09 + port * 2, 0x40); /* TH is an output */
        for (unsigned i = 0; i < sizeof patterns / sizeof patterns[0]; i++) {
            pad_selftest_pressed = (unsigned short)patterns[i];

            /* 3-button pad: the extra TH cycles change nothing. */
            gwenesis_io_set_six_button(port, 0);
            frame_counter = 100; scan_line = 10;
            pad_sequence("3-button", port, 0);

            gwenesis_io_set_six_button(port, 1);
            pad_sequence("6-button", port, 1);

            /* No pause: the count carries on past the read-out and the pad
               answers as a 3-button one until it resets. */
            scan_line += 20;
            pad_sequence("no reset after 20 lines", port, 0);

            /* A pause of more than ~1.5 ms restarts the count. */
            scan_line += 26;
            pad_sequence("reset after 26 lines", port, 1);

            /* Pause across the end of an NTSC frame: 262 - 250 + 5 = 17 lines,
               too short to reset. Continuing gives a 3-button answer. */
            gwenesis_vdp_status &= ~1;
            scan_line = 250;
            pad_sequence("NTSC frame end, before", port, 1);
            frame_counter++; scan_line = 5;
            pad_sequence("NTSC frame end, 17 lines", port, 0);
            /* Same from line 250 to line 30 of the next frame: 42 lines, reset. */
            scan_line = 250;
            pad_sequence("NTSC frame end, before (2)", port, 1);
            frame_counter++; scan_line = 30;
            pad_sequence("NTSC frame end, 42 lines", port, 1);

            /* PAL frames are 313 lines: stamp at line 250, next frame line 0
               is 63 lines later and resets, where NTSC arithmetic would give
               12 and not reset. */
            gwenesis_vdp_status |= 1;
            scan_line = 250;
            pad_sequence("PAL frame end, before", port, 1);
            frame_counter++; scan_line = 0;
            pad_sequence("PAL frame end, 63 lines", port, 1);
            gwenesis_vdp_status &= ~1;

            /* The reset also applies to reads: stop on the ID read, wait,
               and reading again without a write gives an ordinary TH-low
               read instead of the ID. */
            scan_line += 30;
            pad_step("halfway", port, 1, RD_HIGH);
            pad_step("halfway", port, 0, RD_LOW);
            pad_step("halfway", port, 1, RD_HIGH);
            pad_step("halfway", port, 0, RD_LOW);
            pad_step("halfway", port, 1, RD_HIGH);
            pad_step("halfway", port, 0, RD_ID);
            scan_line += 30;
            {
                unsigned data = 0x03 + port * 2;
                unsigned got = gwenesis_io_read_ctrl(data); /* TH still low */
                unsigned want = expect_pad_bits(RD_LOW, pad_selftest_pressed);
                if (got != want) {
                    printf("padtest: read after pause: port %d read %02x, expected %02x\n",
                           port + 1, got, want);
                    pad_selftest_bad++;
                }
            }
            /* Back to idle (TH high), then a full read-out after a pause. */
            pad_step("halfway, TH back high", port, 1, RD_HIGH);
            scan_line += 30;
            pad_sequence("after halfway pause", port, 1);

            gwenesis_io_set_six_button(port, 0);
            scan_line += 30;
        }
    }

    /* Reset must drop the pads back to 3 buttons. */
    gwenesis_io_set_six_button(0, 1);
    gwenesis_io_reset();
    gwenesis_io_write_ctrl(0x09, 0x40);
    pad_selftest_pressed = PB_A | PB_X;
    scan_line += 30;
    pad_sequence("after gwenesis_io_reset", 0, 0);

    pad_selftest_active = 0;
    printf("padtest: %s\n", pad_selftest_bad ? "FAILED" : "PASS");
    return pad_selftest_bad;
}

/* ------------------------------ main ------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <rom> <total-frames> <dump-every-N> [outdir]\n",
                argv[0]);
        return 2;
    }
    const char *rom_path = argv[1];
    int total_frames = atoi(argv[2]);
    int dump_every = atoi(argv[3]);
    const char *outdir = argc > 4 ? argv[4] : "out";
    mkdir(outdir, 0755);

    parse_press("GEN_PRESS_START", 7);
    parse_press("GEN_PRESS_A", 6);
    parse_press("GEN_PRESS_C", 5);
    parse_press("GEN_PRESS_B", 4);
    parse_press("GEN_PRESS_Z", 8);
    parse_press("GEN_PRESS_Y", 9);
    parse_press("GEN_PRESS_X", 10);
    parse_press("GEN_PRESS_MODE", 11);

    const char *pad_env = getenv("GEN_PAD");
    int six_button = pad_env && atoi(pad_env) == 6;

    /* Optional warm-up launch: run a different game first and tear it
       down, so this run starts from whatever state the core left behind. */
    const char *first_rom = getenv("GEN_FIRST_ROM");
    if (first_rom && *first_rom) {
        size_t first_size = 0;
        const unsigned char *first = load_rom(first_rom, &first_size);
        if (!first)
            return 1;
        printf("=== warm-up launch: %s ===\n", first_rom);
        gwsram_detect(first, first_size);
        if (!init_emulator_mem()) {
            fprintf(stderr, "out of memory\n");
            return 1;
        }
        load_cartridge(first, first_size);
        power_on();
        reset_emulation();
        gwenesis_io_set_six_button(0, six_button);
        gwenesis_io_set_six_button(1, six_button);
        gwsnd_init(0, 0);
        for (int f = 0; f < 300; f++) {
            current_frame_no = f;
            gwsnd_set_pal(gwenesis_frame_get_config());
            int m = (FB_H - screen_height) / 2;
            gwenesis_vdp_set_buffer(&framebuffer[m * FB_W]);
            gwenesis_frame_run(1);
        }
        /* Exactly the firmware's teardown order. */
        gwsnd_shutdown();
        free_emulator_mem();
        memset(framebuffer, 0, sizeof(framebuffer));
        free((void *)first);
        printf("=== warm-up done, now launching %s ===\n", rom_path);
    }

    size_t rom_size = 0;
    const unsigned char *rom = load_rom(rom_path, &rom_size);
    if (!rom)
        return 1;

    const char *split = getenv("GEN_SPLIT_BANKS");
    if (split && atoi(split) > 0) {
        size_t head_len = (size_t)atoi(split) * GWMAPPER_BANK_SIZE;
        if (head_len >= rom_size) {
            fprintf(stderr, "GEN_SPLIT_BANKS: %s banks is the whole rom\n", split);
            return 1;
        }
        /* The tail keeps the contiguous buffer's pow2 + 4 padding, so reads the
           mirror table sends past the image land in the same zeros as before;
           the head gets none. */
        size_t pot = 1;
        while (pot < rom_size)
            pot <<= 1;
        unsigned char *head = malloc(head_len);
        unsigned char *tail = calloc(1, pot - head_len + 4);
        memcpy(head, rom, head_len);
        memcpy(tail, rom + head_len, rom_size - head_len);
        free((void *)rom);
        gwmapper_set_storage(head, head_len, tail);
        rom = head;
    }

    gwsram_detect(rom, rom_size);
    if (!init_emulator_mem()) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/mixed.wav", outdir);
    wav_open(&wav_mixed, path, 44100, 2);
    snprintf(path, sizeof(path), "%s/ym.wav", outdir);
    wav_open(&wav_ym, path, GWENESIS_AUDIO_FREQ_NTSC, 1);
    snprintf(path, sizeof(path), "%s/psg.wav", outdir);
    wav_open(&wav_psg, path, GWENESIS_AUDIO_FREQ_NTSC, 1);

    load_cartridge(rom, rom_size);
    power_on();
    reset_emulation();
    gwenesis_io_set_six_button(0, six_button);
    gwenesis_io_set_six_button(1, six_button);
    if (six_button)
        printf("pads: 6 button\n");

    if (getenv("GEN_PAD_SELFTEST")) {
        int bad = pad_selftest();
        wav_close(&wav_mixed);
        wav_close(&wav_ym);
        wav_close(&wav_psg);
        free_emulator_mem();
        return bad ? 1 : 0;
    }

    const char *srm_path = getenv("GEN_SRM");
    if (srm_path && *srm_path)
        srm_load(srm_path);
    if (getenv("GEN_SRAM_SELFTEST"))
        srm_selftest();

    gwsnd_set_output(audio_out);
    gwsnd_set_frame_tap(frame_tap);
    gwsnd_init(0, 0);

    for (int frame = 0; frame < total_frames; frame++) {
        current_frame_no = frame;

        int is_pal = gwenesis_frame_get_config();
        gwsnd_set_pal(is_pal);
        int margin = (FB_H - screen_height) / 2;
        gwenesis_vdp_set_buffer(&framebuffer[margin * FB_W]);

        gwenesis_frame_run(1);

        if (dump_every > 0 && frame % dump_every == 0)
            dump_ppm(outdir, frame);
    }

    wav_close(&wav_mixed);
    wav_close(&wav_ym);
    wav_close(&wav_psg);
    if (srm_path && *srm_path)
        srm_save(srm_path);
    gwsnd_shutdown();
    free_emulator_mem();

    if (gwmapper_active())
        printf("mapper: %u bank writes\n", gwmapper_bank_writes);
    printf("ran %d frames, audio: %s/mixed.wav ym.wav psg.wav\n",
           total_frames, outdir);
    return 0;
}
