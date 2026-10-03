/*
scd/pico_int.h — the part of PicoDrive's pico/pico_int.h that the vendored
Sega CD sources (scd/cd/*.c) need, re-expressed on top of the gwenesis core.

The vendored files include "../pico_int.h" exactly as they do upstream, so
this file standing in its place is what keeps their diffs small. Everything
PicoDrive-specific they touch is mapped here:

  - the CD state structs (mcd_state & co.) are PicoDrive's, verbatim; the
    one instance lives in PSRAM (port/scd.c);
  - the Sek* CPU API maps onto the two gwenesis 68000 instances: `m68k`
    (main, master clocks, frame relative) and `s68k` (sub, 12.5 MHz cycles,
    run relative). See "cycle accounting" below and scd/PORTING.md;
  - Pico / PicoIn keep only the fields the CD code reads;
  - elprintf() is a no-op on the Pico and a filtered printf on the host.
*/
#ifndef SCD_PICO_INT_H
#define SCD_PICO_INT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m68k.h"   /* m68ki_cpu_core, the main CPU `m68k` */
#include "scd.h"    /* port/scd.h */

/* The 68000 core's macros.h and the CD code's genplus_macros.h both define
   these, identically in effect; the CD code's win in its files. */
#undef READ_BYTE
#undef WRITE_BYTE

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------ types --------------------------------- */

typedef uint8_t   u8;
typedef int8_t    s8;
typedef uint16_t  u16;
typedef int16_t   s16;
/* unsigned int, not uint32_t: the same width on both targets, but newlib's
   uint32_t is unsigned long, and the CD code's handlers must have exactly
   the type of the 68000 core's bus functions (unsigned int). */
typedef unsigned int u32;
typedef int       s32;
typedef uint64_t  u64;
typedef int64_t   s64;
typedef uintptr_t uptr;

#define PICO_INTERNAL
#define PICO_INTERNAL_ASM
#define NOINLINE __attribute__((noinline))

/* Both targets are little endian and keep 68000 memory as byte-swapped
   16-bit words, the same as gwenesis. */
#define MEM_BE2(a) ((a) ^ 1)
#define MEM_LE2(a) (a)
#define CPU_BE2(v) ((u32)((u64)(v) << 16) | ((u32)(v) >> 16))
#define CPU_LE4(v) (v)

/* Placement: the CD code's hot paths go to SRAM on the Pico. */
#include "gwenesis_port.h"

/* ------------------------------ logging -------------------------------- */

#define EL_INTS    0x00000004
#define EL_INTSW   0x00000010
#define EL_UIO     0x00000800
#define EL_CDPOLL  0x00002000
#define EL_CDREGS  0x00020000
#define EL_CDREG3  0x00040000
#define EL_CD      0x00400000
#define EL_STATUS  0x40000000
#define EL_ANOMALY 0x80000000

#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
extern unsigned int gwcd_log_mask;
#if defined(GWENESIS_HOST) && GWENESIS_HOST != 0
/* host harness profiling hook (GEN_SCD_PROFILE), port/scd.c */
extern void (*gwcd_s68k_sample)(unsigned int pc, unsigned int cycles);
extern void (*gwcd_m68k_sample)(unsigned int pc, unsigned int mclk);
extern unsigned int gwcd_host_s68k_runs; /* SekRunS68k calls */
#endif
#define elprintf(w, f, ...) do { \
    if ((w) & gwcd_log_mask) printf("scd: " f "\n", ##__VA_ARGS__); } while (0)
#else
#define elprintf(w, f, ...) do { } while (0)
#endif

/* ------------------------- Pico / PicoIn shims -------------------------- */

#define POPT_EN_MCD_PCM   (1 << 10)
#define POPT_EN_MCD_CDDA  (1 << 11)
#define POPT_EN_MCD_GFX   (1 << 15)
#define POPT_EN_MCD_RAMCART (1 << 16)

struct gwcd_pico_shim {
    struct { int pal; } m;
    unsigned int romsize;          /* 0 for a disc boot */
    const unsigned char *rom;      /* MD+: the cartridge, byte-swapped */
    struct { int changed; unsigned char *data; unsigned int start, end; } sv;
};
extern struct gwcd_pico_shim Pico;

struct gwcd_picoin_shim {
    unsigned int opt;
    void (*mcdTrayOpen)(void);
    void (*mcdTrayClose)(void);
};
extern struct gwcd_picoin_shim PicoIn;

extern void (*PicoResetHook)(void);

#define OSC_NTSC 53693175
#define OSC_PAL  53203424

/* ----------------------- CD state (PicoDrive) --------------------------- */

#define PCM_MIXBUF_LEN ((12500000 / 384) / 50 + 1)

struct mcd_pcm
{
	unsigned char control; // reg7
	unsigned char enabled; // reg8
	unsigned char cur_ch;
	unsigned char bank;
	unsigned int update_cycles;

	struct pcm_chan			// 08, size 0x10
	{
		unsigned char regs[8];
		unsigned int  addr;	// .08: played sample address
		int pad;
	} ch[8];
};

#define PCD_ST_S68K_RST     1
#define PCD_ST_S68K_SYNC    2
#define PCD_ST_S68K_SLEEP   4
#define PCD_ST_S68K_POLL   16
#define PCD_ST_M68K_POLL   32
#define PCD_ST_CDD_CMD     64
#define PCD_ST_S68K_IFL2   0x100

struct mcd_misc
{
  unsigned short hint_vector;
  unsigned char  busreq;          // not s68k_regs[1]
  unsigned char  s68k_pend_ints;
  unsigned int   state_flags;     // 04
  unsigned int   stopwatch_base_c;
  unsigned short m68k_poll_a;
  unsigned short m68k_poll_cnt;
  unsigned short s68k_poll_a;     // 10
  unsigned short s68k_poll_cnt;
  unsigned int   s68k_poll_clk;
  unsigned char  bcram_reg;       // 18: battery-backed RAM cart register
  unsigned char  dmna_ret_2m;
  unsigned char  need_sync;
  unsigned char  pad3;
  unsigned int   m68k_poll_clk;
  unsigned int   cdda_lba_offset; // 20
  int pad4[7];
};

typedef struct
{
  unsigned char bios[0x20000];			// 000000: 128K
  union {					// 020000: 512K
    unsigned char prg_ram[0x80000];
    unsigned char prg_ram_b[4][0x20000];
  };
  union {					// 0a0000: 256K
    struct {
      unsigned char word_ram2M[0x40000];
      unsigned char unused0[0x20000];
    };
    struct {
      unsigned char unused1[0x20000];
      unsigned char word_ram1M[2][0x20000];
    };
  };
  union {					// 100000: 64K
    unsigned char pcm_ram[0x10000];
    unsigned char pcm_ram_b[0x10][0x1000];
  };
  unsigned char s68k_regs[0x200];		// 110000: GA, not CPU regs
  unsigned char bram[0x2000];			// 110200: 8K
  struct mcd_misc m;				// 112200: misc
  struct mcd_pcm pcm;				// 112240:
  void *cdda_stream;
  int cdda_type;
  unsigned int cdda_frame_offs;
  int pcm_mixbuf[PCM_MIXBUF_LEN * 2];
  int pcm_mixpos;
  char pcm_mixbuf_dirty;
  char pcm_regs_dirty;
} mcd_state;

extern mcd_state *Pico_mcd;

#define PCDS_IEN1     (1<<1)
#define PCDS_IEN2     (1<<2)
#define PCDS_IEN3     (1<<3)
#define PCDS_IEN4     (1<<4)
#define PCDS_IEN5     (1<<5)
#define PCDS_IEN6     (1<<6)

enum pcd_event {
  PCD_EVENT_CDC,
  PCD_EVENT_TIMER3,
  PCD_EVENT_GFX,
  PCD_EVENT_DMA,
  PCD_EVENT_COUNT,
};
extern unsigned int pcd_event_times[PCD_EVENT_COUNT];

/* CD track types (pico/pico.h) */
enum cd_track_type
{
  CT_UNKNOWN = 0,
  CT_ISO = 1,	/* 2048 B/sector */
  CT_BIN = 2,	/* 2352 B/sector */
  CT_AUDIO = 8,
  CT_RAW = CT_AUDIO | 1,
  CT_CHD = CT_AUDIO | 2,
  CT_MP3 = CT_AUDIO | 3,
  CT_WAV = CT_AUDIO | 4,
};

#define USE_POLL_DETECT 1

/* compare cycles, handling overflows */
#define CYCLES_GT(a, b) ((int)((a) - (b)) > 0)
#define CYCLES_GE(a, b) ((int)((a) - (b)) >= 0)

/* ------------------------- cycle accounting ----------------------------- */
/*
Main CPU: PicoDrive counts 68000 cycles in a monotonic Pico.t.m68c_cnt. Here
the unit is the master clock (7 per 68000 cycle, what gwenesis' m68k.cycles
counts) and the count is gwcd_mclk_base + m68k.cycles, where the base moves
up by a frame at every frame end (the core rebases m68k.cycles to the frame).
Constants the CD code expresses in 68000 cycles are scaled with M68K_CYC().

Sub CPU: a monotonic SekCycleCntS68k in its own 12.5 MHz cycles, exactly as
PicoDrive. While s68k_run() executes, s68k.cycles counts up from 0 to
s68k.cycle_end; SekCycleCntS68k then holds the run's end point, as it does in
PicoDrive with Musashi, so SekCyclesDoneS68k() and SekEndRunS68k() keep their
meaning.
*/
extern unsigned int gwcd_mclk_base;
extern unsigned int SekCycleCntS68k;
extern unsigned int SekCycleAimS68k;
extern int gwcd_s68k_in_run;
extern m68ki_cpu_core s68k;

#define M68K_CYC(c) ((c) * 7)

#define SekCyclesDone()      (gwcd_mclk_base + m68k.cycles)
#define SekCyclesLeftS68k    (gwcd_s68k_in_run ? (int)(s68k.cycle_end - s68k.cycles) : 0)
#define SekCyclesDoneS68k()  (SekCycleCntS68k - SekCyclesLeftS68k)
#define SekPc                (m68k.pc)
#define SekPcS68k            (s68k.pc)
#define SekShouldInterrupt() (m68k.int_level > m68k.int_mask)
#define SekEndRun(after)     gwcd_m68k_end_run(M68K_CYC(after))
#define SekEndRunS68k(after) gwcd_s68k_end_run(after)

/* PicoDrive's "not polling" hint is set by DBcc: a counted loop is a delay,
   not a poll. The gwenesis core has the same hint already -- the idle-loop
   detection GPGX left in every DBcc handler clears cpu.poll.detected -- so
   "not polling" is !poll.detected, and "checked" sets it. */
/* Idle-loop detection in the sub CPU (port/scd_s68k_mem.h, s68kcpu.c) */
extern unsigned int gwcd_s68k_sidefx;
extern unsigned int gwcd_s68k_skipped;
void s68k_idle_new_run(void);
/* A sub CPU parked in a wait loop (port/scd.c): anything that could end the
   loop wakes it. */
extern int gwcd_s68k_parked;
void gwcd_s68k_unpark(void);
#define gwcd_s68k_wake() do { if (gwcd_s68k_parked) gwcd_s68k_unpark(); } while (0)
/* reads of values that change with time (stopwatch, PCM position) */
extern unsigned int gwcd_s68k_timeread;
/* ... and in the main CPU, in a Sega CD session (gwenesis/cpus/M68K/m68kcpu.c) */
extern int gwcd_m68k_idle;
extern unsigned int gwcd_m68k_skipped;

#define GWCD_NOT_POLLING(cpu)  (!(cpu).poll.detected)
#define GWCD_POLL_CHECKED(cpu) ((cpu).poll.detected = 1)

void gwcd_m68k_end_run(unsigned int after_mclk);

/* Where the CPU time goes (port/scd.c): sub-CPU cycles executed, skipped
   while the sub polls or sleeps, and while it is halted or in reset; main
   CPU master clocks skipped while it polls. Read-and-cleared by
   gwcd_stats_take(). */
struct gwcd_stats_t {
    unsigned int s68k_run, s68k_idle, s68k_halted, m68k_idle;
};
extern struct gwcd_stats_t gwcd_stats;
void gwcd_s68k_end_run(unsigned int after);

PICO_INTERNAL void SekInitS68k(void);
PICO_INTERNAL int  SekResetS68k(void);
PICO_INTERNAL int  SekInterruptS68k(int irq);
void SekInterruptClearS68k(int irq);

/* ----------------------------- prototypes ------------------------------- */

// cd/mcd.c
PICO_INTERNAL void PicoCreateMCD(unsigned char *bios_data, int bios_size);
PICO_INTERNAL void PicoInitMCD(void);
PICO_INTERNAL void PicoExitMCD(void);
PICO_INTERNAL void PicoPowerMCD(void);
PICO_INTERNAL int  PicoResetMCD(void);
PICO_INTERNAL void PicoMCDPrepare(void);
void pcd_event_schedule(unsigned int now, enum pcd_event event, int after);
void pcd_event_schedule_s68k(enum pcd_event event, int after);
void pcd_prepare_frame(void);
unsigned int pcd_cycles_m68k_to_s68k(unsigned int c);
void pcd_irq_s68k(int irq, int state);
int  pcd_sync_s68k(unsigned int m68k_target, int m68k_poll_sync);
void pcd_run_cpus(int m68k_cycles);
void pcd_soft_reset(void);
void pcd_state_loaded(void);

// cd/pcm.c
void pcd_pcm_sync(unsigned int to);
void pcd_pcm_update(s32 *buffer, int length, int stereo);
void pcd_pcm_write(unsigned int a, unsigned int d);
unsigned int pcd_pcm_read(unsigned int a);

// cd/cdc.c
void cdc_init(void);
void cdc_reset(void);
int  cdc_context_save(unsigned char *state);
int  cdc_context_load(unsigned char *state);
int  cdc_context_load_old(unsigned char *state);
void cdc_dma_update(void);
int  cdc_decoder_update(unsigned char header[4]);
void cdc_reg_w(unsigned char data);
unsigned char  cdc_reg_r(void);
unsigned short cdc_host_r(int sub);

// cd/cdd.c
void cdd_reset(void);
void cdd_play_audio(int index, int lba);
int cdd_context_save(unsigned char *state);
int cdd_context_load(unsigned char *state);
int cdd_context_load_old(unsigned char *state);
void cdd_read_data(unsigned char *dst);
void cdd_read_audio(unsigned int samples);
void cdd_update(void);
void cdd_process(void);
int cdd_unload(void);

// cd/gfx.c
void gfx_init(void);
void gfx_start(u32 base);
void gfx_update(unsigned int cycles);
int gfx_context_save(unsigned char *state);
int gfx_context_load(const unsigned char *state);

// cd/memory.c
extern u32 pcd_base_address;
PICO_INTERNAL void PicoMemSetupCD(void);
void pcd_state_loaded_mem(void);
u32 s68k_poll_detect(u32 a, u32 d);
u32 pcd_stopwatch_read(int sub);
void m68k_comm_check(u32 a);

// cd/misc.c
extern unsigned char formatted_bram[4*0x10];
PICO_INTERNAL_ASM void wram_2M_to_1M(unsigned char *m);
PICO_INTERNAL_ASM void wram_1M_to_2M(unsigned char *m);
PICO_INTERNAL_ASM void memcpy16bswap(unsigned short *dest, void *src, int count);

// sub 68000 (gwenesis/cpus/M68K/s68kcpu.c)
void s68k_run(unsigned int cycles);
void s68k_init(void);
void s68k_pulse_reset(void);
void s68k_set_irq(unsigned int int_level);
void s68k_set_int_ack_callback(int (*callback)(int int_level));

// CD audio (port/scd_audio.c; PicoDrive keeps these in pico/sound/sound.c)
void cdda_start_play(int lba_base, int lba_offset, int lb_len);
/* The drive's audio gate (s68k reg $36 bit 0 clear = playing) and the fader
   ($34-$35), published at every 75 Hz CDD update. */
void gwcd_cdda_drive_state(int playing, unsigned int fader);
/* Frame end: resample the frame's PCM output for the mixer. */
void gwcd_pcm_frame_end(int is_pal);

// MD+ (port/scd_msd.c)
void gwcd_msd_frame_end(int is_pal);


#ifdef __cplusplus
}
#endif

#endif /* SCD_PICO_INT_H */
